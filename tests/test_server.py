"""Black-box tests of server framing, ordering, lifecycle, and regressions."""

import socket
import struct
import threading
import time

from support import ChatTest, DONE, MAX_TEXT, Server, chat


class ServerTests(ChatTest):
    def test_one_client_receives_its_own_message_and_address(self):
        server, (peer,) = self.server(1)
        peer.send(chat(b"hello"))
        self.assertEqual(peer.frame(), peer.expected(b"hello"))
        server.process.alive()

    def test_empty_payload(self):
        _, (peer,) = self.server(1)
        peer.send(chat(b""))
        self.assertEqual(peer.frame(), peer.expected(b""))

    def test_fragmented_chat_frame_waits_for_newline(self):
        _, (peer,) = self.server(1)
        for fragment in (b"\x00", b"hel", b"lo"):
            peer.send(fragment)
            peer.quiet(0.03)
        peer.send(b"\n")
        self.assertEqual(peer.frame(), peer.expected(b"hello"))

    def test_combined_frames_and_partial_tail(self):
        _, (peer,) = self.server(1)
        peer.send(chat(b"one") + chat(b"two") + b"\x00thr")
        self.assertEqual(peer.frame(), peer.expected(b"one"))
        self.assertEqual(peer.frame(), peer.expected(b"two"))
        peer.quiet(0.03)
        peer.send(b"ee\n")
        self.assertEqual(peer.frame(), peer.expected(b"three"))

    def test_payload_at_and_below_limit(self):
        _, (peer,) = self.server(1)
        for size in (1, 20, MAX_TEXT - 1, MAX_TEXT):
            with self.subTest(text_bytes=size):
                text = b"A" * size
                peer.send(chat(text))
                self.assertEqual(peer.frame(), peer.expected(text))

    def test_broadcast_reaches_every_client_including_sender(self):
        _, peers = self.server(4)
        for sender in peers:
            sender.send(chat(b"hello everyone"))
            for recipient in peers:
                self.assertEqual(recipient.frame(), sender.expected(b"hello everyone"))

    def test_concurrent_senders_have_identical_global_order(self):
        _, peers = self.server(4)
        start = threading.Barrier(len(peers))
        errors = []

        def send_messages(index, peer):
            try:
                start.wait(timeout=3)
                peer.send(b"".join(chat(f"{index}:{n:03}".encode()) for n in range(30)))
            except Exception as error:
                errors.append(error)

        threads = [threading.Thread(target=send_messages, args=(i, peer), daemon=True)
                   for i, peer in enumerate(peers)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=4)
            self.assertFalse(thread.is_alive(), "Sender thread hung")
        self.assertFalse(errors, str(errors))
        sequences = [[peer.frame() for _ in range(120)] for peer in peers]
        for sequence in sequences[1:]:
            self.assertEqual(sequence, sequences[0])
        for index in range(4):
            expected = [peers[index].expected(f"{index}:{n:03}".encode()) for n in range(30)]
            actual = [frame for frame in sequences[0] if frame[7:].startswith(f"{index}:".encode())]
            self.assertEqual(actual, expected)

    def test_one_hundred_connected_clients(self):
        _, peers = self.server(100)
        peers[-1].send(chat(b"100 clients"))
        for peer in peers:
            self.assertEqual(peer.frame(), peers[-1].expected(b"100 clients"))

    def test_full_server_rejects_extra_connection(self):
        server, (peer,) = self.server(1)
        extra = server.connect()
        extra.closed()
        peer.send(chat(b"still here"))
        self.assertEqual(peer.frame(), peer.expected(b"still here"))

    def test_shutdown_after_all_clients_finish(self):
        server, peers = self.server(3)
        peers[0].send(DONE)
        peers[1].send(DONE)
        for peer in peers:
            peer.quiet()
        peers[2].send(DONE)
        for peer in peers:
            self.assertEqual(peer.frame(), DONE)
        self.clean_exit(server.process)

    def test_duplicate_done_does_not_count_twice(self):
        server, (a, b) = self.server(2)
        a.send(DONE + DONE)
        a.quiet()
        b.quiet()
        b.send(DONE)
        self.assertEqual(a.frame(), DONE)
        self.assertEqual(b.frame(), DONE)
        self.clean_exit(server.process)

    def test_fragmented_done(self):
        server, (peer,) = self.server(1)
        peer.send(b"\x01")
        peer.quiet()
        peer.send(b"\n")
        self.assertEqual(peer.frame(), DONE)
        self.clean_exit(server.process)

    def test_shutdown_follows_all_earlier_messages(self):
        server, (peer,) = self.server(1)
        texts = [f"message-{i}".encode() for i in range(100)]
        peer.send(b"".join(map(chat, texts)) + DONE)
        for text in texts:
            self.assertEqual(peer.frame(), peer.expected(text))
        self.assertEqual(peer.frame(), DONE)
        self.clean_exit(server.process)

    def test_graceful_disconnect_allows_slot_reuse(self):
        server, (a, b) = self.server(2)
        a.close()
        time.sleep(0.05)
        b.send(chat(b"after disconnect"))
        self.assertEqual(b.frame(), b.expected(b"after disconnect"))
        c = server.connect()
        server.ready([b, c])

    def test_partial_frame_is_discarded_when_slot_is_reused(self):
        server, (a, b) = self.server(2)
        a.send(b"\x00unfinished")
        a.quiet(0.03)
        a.close()
        time.sleep(0.05)
        b.send(chat(b"barrier"))
        self.assertEqual(b.frame(), b.expected(b"barrier"))
        c = server.connect()
        server.ready([b, c])

    def assert_bad_sender_isolated(self, wire):
        server, (bad, healthy) = self.server(2)
        bad.send(wire)
        bad.closed()
        server.process.alive()
        healthy.send(chat(b"survived"))
        self.assertEqual(healthy.frame(), healthy.expected(b"survived"))

    def test_one_byte_over_payload_limit_is_rejected(self):
        self.assert_bad_sender_isolated(chat(b"A" * (MAX_TEXT + 1)))

    def test_1024_byte_input_cannot_overflow_broadcast_buffer(self):
        self.assert_bad_sender_isolated(chat(b"A" * 1022))

    def test_unterminated_oversized_frame_is_rejected(self):
        self.assert_bad_sender_isolated(b"\x00" + b"A" * 1023)

    def test_unknown_type_is_rejected(self):
        self.assert_bad_sender_isolated(b"\x02\n")

    def test_invalid_done_frame_is_rejected(self):
        self.assert_bad_sender_isolated(b"\x01x\n")

    def test_connection_reset_only_removes_that_client(self):
        server, (bad, healthy) = self.server(2)
        bad.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        bad.close()
        time.sleep(0.05)
        server.process.alive()
        healthy.send(chat(b"survived reset"))
        self.assertEqual(healthy.frame(), healthy.expected(b"survived reset"))

    def test_disconnected_completion_vote_is_not_reused(self):
        server, (a, b) = self.server(2)
        a.send(DONE)
        a.quiet()
        a.close()
        time.sleep(0.05)
        b.send(chat(b"barrier"))
        self.assertEqual(b.frame(), b.expected(b"barrier"))
        c = server.connect()
        server.ready([b, c])
        c.send(DONE)
        b.quiet()
        server.process.alive()
        b.send(DONE)
        self.assertEqual(b.frame(), DONE)
        self.assertEqual(c.frame(), DONE)
        self.clean_exit(server.process)

    def test_slow_reader_does_not_stop_other_clients(self):
        server = Server(self, 3)
        fast = server.connect()
        slow = server.connect(receive_buffer=1024)
        server.ready([fast, slow])
        stop = threading.Event()
        fast.sock.settimeout(1)
        errors = []

        def drain():
            while not stop.is_set():
                try:
                    if not fast.sock.recv(65536):
                        return
                except socket.timeout:
                    continue
                except OSError:
                    return

        def flood():
            try:
                batch = chat(b"A" * 900) * 32
                for _ in range(600):  # About 17 MB: enough to pressure send buffers.
                    if stop.is_set():
                        return
                    fast.send(batch)
            except OSError as error:
                errors.append(error)

        threads = [threading.Thread(target=drain, daemon=True), threading.Thread(target=flood, daemon=True)]
        for thread in threads:
            thread.start()
        try:
            threads[1].join(timeout=8)
            server.process.alive()
            observer = server.connect()
            observer.send(chat(b"responsive"))
            deadline = time.monotonic() + 3
            while True:
                frame = observer.frame(timeout=max(0.01, deadline - time.monotonic()))
                if frame == observer.expected(b"responsive"):
                    break
                self.assertLess(time.monotonic(), deadline, "Server stopped serving the responsive client")
            self.assertFalse(errors, f"Responsive sender lost its connection: {errors}")
        finally:
            stop.set()
            fast.close()
            for thread in threads:
                thread.join(timeout=2)
                self.assertFalse(thread.is_alive(), "Stress-test thread did not stop")
