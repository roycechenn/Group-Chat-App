"""Validation must reject bad arguments before allocating state or connecting."""

import socket
import tempfile
import time
from pathlib import Path

from support import CLIENT, SERVER, ChatTest, listener, unused_port


class CommandLineTests(ChatTest):
    def test_server_requires_exact_argument_count(self):
        for args in ([], [8000], [8000, 1, "extra"]):
            with self.subTest(args=args):
                self.rejected(self.process(SERVER, args))

    def test_client_requires_exact_argument_count(self):
        for args in ([], ["127.0.0.1", 8000, 1], ["127.0.0.1", 8000, 1, "log", "extra"]):
            with self.subTest(args=args):
                self.rejected(self.process(CLIENT, args))

    def test_server_rejects_invalid_ports(self):
        for port in ("0", "-1", "65536", "abc", f"{unused_port()}oops", "9999999999999999999999"):
            with self.subTest(port=port):
                process = self.process(SERVER, [port, 1])
                try:
                    self.rejected(process, timeout=1)
                finally:
                    process.close()

    def test_server_rejects_invalid_client_counts(self):
        for count in ("0", "-1", "abc", "1oops", "2147483648"):
            with self.subTest(count=count):
                process = self.process(SERVER, [unused_port(), count])
                try:
                    self.rejected(process, timeout=1)
                finally:
                    process.close()

    def test_occupied_server_port_reports_error(self):
        with listener() as host:
            self.rejected(self.process(SERVER, [host.getsockname()[1], 1]))

    def assert_client_validation(self, ip=None, port_transform=None, count="0"):
        with listener() as host, tempfile.TemporaryDirectory(prefix="group-chat-cli-test-") as directory:
            host.settimeout(0.05)
            port = host.getsockname()[1]
            args = [ip or "127.0.0.1", port_transform(port) if port_transform else port,
                    count, Path(directory) / "client.log"]
            process = self.process(CLIENT, args)
            try:
                deadline = time.monotonic() + 1
                while time.monotonic() < deadline:
                    try:
                        sock, _ = host.accept()
                    except socket.timeout:
                        if process.child.poll() is not None:
                            self.rejected(process)
                            self.assertNotIn(b"connect:", process.output()[1].lower(),
                                             "Invalid arguments reached connect() instead of validation")
                            return
                    else:
                        sock.close()
                        self.fail("Client connected before rejecting invalid arguments")
                self.fail("Client did not reject invalid arguments promptly")
            finally:
                process.close()

    def test_client_rejects_invalid_ipv4_addresses(self):
        for ip in ("not-an-ip", "localhost", "256.0.0.1", "127.0.0.1oops"):
            with self.subTest(ip=ip):
                self.assert_client_validation(ip=ip)

    def test_client_rejects_invalid_ports_before_connecting(self):
        # These transformed ports wrap or partially parse to the live mock port
        # with atoi(), proving validation rather than relying on connection refusal.
        transforms = [lambda p: str(p + 65536), lambda p: str(p - 65536),
                      lambda p: f"{p}oops", lambda p: "0", lambda p: "abc"]
        for transform in transforms:
            with self.subTest(port=transform(12345)):
                self.assert_client_validation(port_transform=transform)

    def test_client_rejects_invalid_message_counts_before_connecting(self):
        for count in ("-1", "abc", "1oops", "2147483648"):
            with self.subTest(count=count):
                self.assert_client_validation(count=count)
