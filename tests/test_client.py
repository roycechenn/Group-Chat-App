"""Run the actual client against a controlled, binary-protocol mock server."""

import re
import socket
import tempfile
import time
from pathlib import Path

from support import CLIENT, ChatTest, DONE, MAX_TEXT, broadcast, formatted, unused_port


class ClientTests(ChatTest):
    def test_zero_messages_sends_only_done(self):
        process, peer, log = self.mock_client(0)
        self.assertEqual(peer.frame(), DONE)
        self.finish_mock(process, peer)
        self.assertEqual(log.read_bytes(), b"")

    def test_generates_requested_number_of_hex_messages(self):
        process, peer, _ = self.mock_client(50)
        for _ in range(50):
            frame = peer.frame()
            self.assertEqual(frame[0], 0)
            self.assertIsNotNone(re.fullmatch(rb"[0-9A-F]{20}\n", frame[1:]))
        self.assertEqual(peer.frame(), DONE)
        peer.quiet()
        self.finish_mock(process, peer)

    def test_received_messages_match_stdout_and_log(self):
        process, peer, log = self.mock_client(0)
        self.assertEqual(peer.frame(), DONE)
        texts = [b"hello", b"", b"a second message"]
        peer.send(b"".join(broadcast(text) for text in texts) + DONE)
        self.clean_exit(process)
        expected = b"".join(map(formatted, texts))
        self.assertEqual(process.output()[0], expected)
        self.assertEqual(log.read_bytes(), expected)

    def test_binary_header_newlines_are_not_delimiters(self):
        process, peer, log = self.mock_client(0)
        self.assertEqual(peer.frame(), DONE)
        ip, port = "10.0.0.10", 0x0A0A
        peer.send(broadcast(b"header contains newlines", ip, port) + DONE)
        self.clean_exit(process)
        expected = formatted(b"header contains newlines", ip, port)
        self.assertEqual(process.output()[0], expected)
        self.assertEqual(log.read_bytes(), expected)

    def test_fragmented_header_and_payload(self):
        process, peer, log = self.mock_client(0)
        self.assertEqual(peer.frame(), DONE)
        wire = broadcast(b"fragmented")
        for fragment in (wire[:1], wire[1:4], wire[4:7], wire[7:-1]):
            peer.send(fragment)
            time.sleep(0.02)
            process.alive()
            self.assertEqual(log.read_bytes(), b"")
        peer.send(wire[-1:] + DONE)
        self.clean_exit(process)
        self.assertEqual(log.read_bytes(), formatted(b"fragmented"))

    def test_fragmented_shutdown_waits_for_newline(self):
        process, peer, _ = self.mock_client(0)
        self.assertEqual(peer.frame(), DONE)
        peer.send(b"\x01")
        time.sleep(0.05)
        process.alive()
        peer.send(b"\n")
        self.clean_exit(process)

    def test_existing_log_is_truncated(self):
        with tempfile.TemporaryDirectory(prefix="group-chat-log-test-") as directory:
            log = Path(directory) / "messages.log"
            log.write_bytes(b"old content\n")
            process, peer, _ = self.mock_client(0, log)
            self.assertEqual(peer.frame(), DONE)
            peer.send(broadcast(b"new content") + DONE)
            self.clean_exit(process)
            self.assertEqual(log.read_bytes(), formatted(b"new content"))

    def test_unwritable_log_reports_error(self):
        with tempfile.TemporaryDirectory(prefix="group-chat-log-test-") as directory:
            # Opening a directory for writing reliably fails, including as root.
            process, peer, _ = self.mock_client(0, Path(directory))
            self.rejected(process)

    def test_receives_while_outgoing_stream_is_not_drained(self):
        # A 22 MB outgoing stream and tiny receive window force write pressure.
        process, peer, log = self.mock_client(1000000, receive_buffer=1024)
        self.assertEqual(peer.frame()[0], 0)
        peer.send(broadcast(b"read thread is active"))
        expected = formatted(b"read thread is active")
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            process.alive()
            if log.exists() and log.read_bytes() == expected:
                break
            time.sleep(0.01)
        self.assertEqual(log.read_bytes(), expected, "Receive thread did not log while sends were backpressured")
        # Cleanup stops this deliberate load; normal completion is tested separately.

    def test_maximum_valid_frame_formats_without_overflow(self):
        process, peer, log = self.mock_client(0)
        self.assertEqual(peer.frame(), DONE)
        text = b"A" * MAX_TEXT
        peer.send(broadcast(text) + DONE)
        self.clean_exit(process)
        self.assertEqual(log.read_bytes(), formatted(text))
        self.assertEqual(process.output()[0], formatted(text))

    def test_one_byte_oversized_frame_is_rejected(self):
        process, peer, _ = self.mock_client(0)
        self.assertEqual(peer.frame(), DONE)
        peer.send(broadcast(b"A" * (MAX_TEXT + 1)))
        self.rejected(process)

    def test_large_frame_cannot_overflow_payload_buffer(self):
        process, peer, _ = self.mock_client(0)
        self.assertEqual(peer.frame(), DONE)
        peer.send(broadcast(b"A" * 1500))
        self.rejected(process)

    def test_unterminated_oversized_frame_is_rejected(self):
        process, peer, _ = self.mock_client(0)
        self.assertEqual(peer.frame(), DONE)
        peer.send(broadcast(b"A" * 1500)[:-1])
        self.rejected(process)

    def test_unknown_type_is_rejected_without_spinning(self):
        process, peer, _ = self.mock_client(0)
        self.assertEqual(peer.frame(), DONE)
        peer.send(b"\x02\n" + DONE)
        self.rejected(process)

    def test_invalid_shutdown_frame_is_rejected(self):
        process, peer, _ = self.mock_client(0)
        self.assertEqual(peer.frame(), DONE)
        peer.send(b"\x01x")
        self.rejected(process)

    def test_truncated_header_at_eof_reports_error(self):
        process, peer, _ = self.mock_client(0)
        self.assertEqual(peer.frame(), DONE)
        peer.send(b"\x00\x7f\x00")
        peer.sock.shutdown(socket.SHUT_WR)
        self.rejected(process)

    def test_truncated_payload_at_eof_reports_error(self):
        process, peer, _ = self.mock_client(0)
        self.assertEqual(peer.frame(), DONE)
        peer.send(broadcast(b"unfinished")[:-1])
        peer.sock.shutdown(socket.SHUT_WR)
        self.rejected(process)

    def test_server_connection_refused_reports_error(self):
        # A bound but non-listening socket can stall connect() on macOS; close
        # the port reservation before testing a genuinely absent server.
        with tempfile.TemporaryDirectory(prefix="group-chat-refused-test-") as directory:
            process = self.process(CLIENT, ["127.0.0.1", unused_port(), 0,
                                            Path(directory) / "client.log"])
            self.rejected(process)
