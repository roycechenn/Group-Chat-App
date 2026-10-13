"""Standard-library fixtures for testing the actual C executables."""

import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SERVER = Path(os.environ.get("GROUP_CHAT_SERVER", ROOT / "build/server"))
CLIENT = Path(os.environ.get("GROUP_CHAT_CLIENT", ROOT / "build/client"))
TIMEOUT = 3.0
DONE = b"\x01\n"
MAX_TEXT = 1016


def chat(text):
    return b"\x00" + text + b"\n"


def broadcast(text, ip="127.0.0.1", port=1234):
    return b"\x00" + socket.inet_aton(ip) + struct.pack("!H", port) + text + b"\n"


def formatted(text, ip="127.0.0.1", port=1234):
    return f"{ip:<15}{port:<10}".encode() + text + b"\n"


def listener():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    sock.listen(128)
    sock.settimeout(TIMEOUT)
    return sock


def unused_port():
    with listener() as sock:
        return sock.getsockname()[1]


class Process:
    """File-backed output avoids pipe deadlocks; cleanup reaps every child."""

    def __init__(self, executable, args):
        if not executable.is_file():
            raise AssertionError(f"Missing executable {executable}. Build the project first.")
        self.stdout = tempfile.TemporaryFile()
        self.stderr = tempfile.TemporaryFile()
        self._closed = False
        try:
            self.child = subprocess.Popen(
                [str(executable), *map(str, args)],
                stdout=self.stdout,
                stderr=self.stderr,
                start_new_session=True,
            )
        except BaseException:
            self.stdout.close()
            self.stderr.close()
            raise

    def output(self):
        self.stdout.seek(0)
        self.stderr.seek(0)
        return self.stdout.read(), self.stderr.read()

    def diagnostic(self):
        out, err = self.output()
        return (
            f"exit={self.child.poll()}\n"
            f"stdout tail:\n{out[-1000:].decode(errors='replace')}\n"
            f"stderr tail:\n{err[-5000:].decode(errors='replace')}"
        )

    def alive(self):
        if self.child.poll() is not None:
            raise AssertionError("Process unexpectedly exited:\n" + self.diagnostic())

    def wait(self, timeout=TIMEOUT):
        try:
            return self.child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            raise AssertionError(f"Process did not exit within {timeout}s:\n" + self.diagnostic()) from None

    def close(self):
        if self._closed:
            return
        self._closed = True
        try:
            if self.child.poll() is None:
                # Popen handles the exit/signal race; these binaries spawn no children.
                self.child.terminate()
                try:
                    self.child.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    self.child.kill()
                    self.child.wait(timeout=1)
        finally:
            self.stdout.close()
            self.stderr.close()


class Peer:
    """Parse binary headers before looking for a newline in message text."""

    def __init__(self, sock, from_server=True):
        self.sock = sock
        self.from_server = from_server
        self.buffer = bytearray()
        sock.settimeout(TIMEOUT)

    def send(self, data):
        self.sock.sendall(data)

    def frame(self, timeout=TIMEOUT):
        deadline = time.monotonic() + timeout
        while True:
            if self.buffer:
                kind = self.buffer[0]
                if kind == 1 and len(self.buffer) >= 2:
                    if self.buffer[1] != 10:
                        raise AssertionError("Invalid completion frame from executable")
                    result = bytes(self.buffer[:2])
                    del self.buffer[:2]
                    return result
                if kind == 0:
                    header = 7 if self.from_server else 1
                    end = self.buffer.find(b"\n", header)
                    if end != -1:
                        result = bytes(self.buffer[:end + 1])
                        del self.buffer[:end + 1]
                        if len(result) > 1024:
                            raise AssertionError(f"Executable sent an oversized {len(result)}-byte frame")
                        return result
                if kind not in (0, 1):
                    raise AssertionError(f"Unexpected message type {kind}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError("Timed out waiting for a complete frame")
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                raise AssertionError("Timed out waiting for a complete frame") from None
            if not chunk:
                raise AssertionError(f"Peer closed before a complete frame; buffered={bytes(self.buffer[:40])!r}")
            self.buffer.extend(chunk)

    def quiet(self, timeout=0.15):
        if self.buffer:
            raise AssertionError(f"Unexpected buffered bytes: {bytes(self.buffer[:40])!r}")
        self.sock.settimeout(timeout)
        try:
            data = self.sock.recv(1)
        except socket.timeout:
            return
        raise AssertionError(f"Expected connection to remain open and quiet; received {data!r}")

    def closed(self, timeout=TIMEOUT):
        if self.buffer:
            raise AssertionError("Expected rejection, but executable sent a frame")
        self.sock.settimeout(timeout)
        try:
            data = self.sock.recv(1)
        except ConnectionResetError:
            return
        except socket.timeout:
            raise AssertionError("Invalid connection was not closed promptly") from None
        if data:
            raise AssertionError(f"Expected connection to close, received {data!r}")

    def expected(self, text):
        ip, port = self.sock.getsockname()
        return broadcast(text, ip, port)

    def close(self):
        self.sock.close()


class Server:
    def __init__(self, case, capacity):
        self.case = case
        self.port = unused_port()
        self.process = case.process(SERVER, [self.port, capacity])
        self.peers = []

    def connect(self, receive_buffer=None):
        deadline = time.monotonic() + TIMEOUT
        while True:
            self.process.alive()
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            if receive_buffer is not None:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
            sock.settimeout(TIMEOUT)
            try:
                sock.connect(("127.0.0.1", self.port))
            except ConnectionRefusedError:
                sock.close()
                if time.monotonic() >= deadline:
                    raise AssertionError("Server did not start listening:\n" + self.process.diagnostic()) from None
                time.sleep(0.01)
                continue
            except BaseException:
                sock.close()
                raise
            peer = Peer(sock)
            self.case.addCleanup(peer.close)
            self.peers.append(peer)
            return peer

    def ready(self, peers=None):
        """A real broadcast acknowledges that every intended peer was accepted."""
        peers = self.peers if peers is None else peers
        sender = peers[-1]
        sender.send(chat(b"__test_ready__"))
        expected = sender.expected(b"__test_ready__")
        for peer in peers:
            self.case.assertEqual(peer.frame(), expected)


class ChatTest(unittest.TestCase):
    def process(self, executable, args):
        process = Process(executable, args)
        self.addCleanup(process.close)
        return process

    def server(self, count):
        server = Server(self, count)
        peers = [server.connect() for _ in range(count)]
        server.ready(peers)
        return server, peers

    def mock_client(self, messages=0, log_path=None, receive_buffer=None):
        host = listener()
        self.addCleanup(host.close)
        if receive_buffer is not None:
            host.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        directory = tempfile.TemporaryDirectory(prefix="group-chat-tests-")
        self.addCleanup(directory.cleanup)
        log = Path(log_path) if log_path is not None else Path(directory.name) / "client.log"
        process = self.process(CLIENT, ["127.0.0.1", host.getsockname()[1], messages, log])
        try:
            sock, _ = host.accept()
        except socket.timeout:
            raise AssertionError("Client did not connect:\n" + process.diagnostic()) from None
        peer = Peer(sock, from_server=False)
        self.addCleanup(peer.close)
        return process, peer, log

    def clean_exit(self, process):
        self.assertEqual(process.wait(), 0, process.diagnostic())
        self.assert_no_sanitizer_error(process)

    def rejected(self, process, timeout=TIMEOUT):
        self.assertGreater(process.wait(timeout), 0, "Expected a reported error, not success or a crash:\n" + process.diagnostic())
        self.assert_no_sanitizer_error(process)

    def assert_no_sanitizer_error(self, process):
        _, stderr = process.output()
        self.assertIsNone(re.search(rb"ERROR: AddressSanitizer|runtime error:|SUMMARY: .*Sanitizer", stderr), process.diagnostic())

    def finish_mock(self, process, peer):
        peer.send(DONE)
        self.clean_exit(process)
