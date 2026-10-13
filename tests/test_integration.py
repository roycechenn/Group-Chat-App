"""Exercise both compiled programs together, and several clients concurrently."""

import tempfile
from pathlib import Path

from support import CLIENT, ChatTest, DONE, Peer, Server, broadcast, formatted, listener


class IntegrationTests(ChatTest):
    def test_real_server_with_three_real_clients(self):
        # The controller keeps shutdown pending and observes the complete stream.
        # Each actual client sees a suffix beginning when the server accepted it.
        server = Server(self, 4)
        controller = server.connect()
        server.ready([controller])
        with tempfile.TemporaryDirectory(prefix="group-chat-integration-") as directory:
            logs = [Path(directory) / f"client-{i}.log" for i in range(3)]
            processes = [self.process(CLIENT, ["127.0.0.1", server.port, 100, log]) for log in logs]
            frames = [controller.frame() for _ in range(300)]
            self.assertTrue(all(frame[0] == 0 for frame in frames))
            ports = {int.from_bytes(frame[5:7], "big") for frame in frames}
            self.assertEqual(len(ports), 3)
            for port in ports:
                self.assertEqual(sum(int.from_bytes(frame[5:7], "big") == port for frame in frames), 100)
            expected_lines = [formatted(frame[7:-1], "127.0.0.1", int.from_bytes(frame[5:7], "big"))
                              for frame in frames]
            controller.send(DONE)
            self.assertEqual(controller.frame(), DONE)
            self.clean_exit(server.process)
            for process, log in zip(processes, logs):
                self.clean_exit(process)
                content = log.read_bytes()
                count = len(content.splitlines())
                self.assertGreaterEqual(count, 100)
                self.assertLessEqual(count, 300)
                self.assertEqual(content, b"".join(expected_lines[-count:]))
                self.assertEqual(process.output()[0], content)

    def test_real_server_and_client_complete_and_log_every_echo(self):
        server = Server(self, 2)
        # Keep a controller connected to acknowledge startup and observe echoes.
        ready = server.connect()
        server.ready([ready])
        with tempfile.TemporaryDirectory(prefix="group-chat-integration-") as directory:
            log = Path(directory) / "client.log"
            process = self.process(CLIENT, ["127.0.0.1", server.port, 100, log])
            for _ in range(100):
                self.assertEqual(ready.frame()[0], 0)
            ready.send(DONE)
            self.assertEqual(ready.frame(), DONE)
            self.clean_exit(process)
            self.clean_exit(server.process)
            lines = log.read_bytes().splitlines()
            self.assertEqual(len(lines), 100)
            self.assertEqual(process.output()[0], log.read_bytes())
            for line in lines:
                self.assertEqual(line[:15].strip(), b"127.0.0.1")
                self.assertTrue(line[15:25].strip().isdigit())
                self.assertRegex(line[25:], rb"^[0-9A-F]{20}$")

    def test_three_real_clients_receive_identical_mock_broadcasts(self):
        # Accept all clients before relaying: startup timing cannot drop early
        # broadcasts, and a single deterministic relay defines the total order.
        with listener() as host, tempfile.TemporaryDirectory(prefix="group-chat-integration-") as directory:
            processes = [self.process(CLIENT, ["127.0.0.1", host.getsockname()[1], 30,
                                              Path(directory) / f"client-{i}.log"])
                         for i in range(3)]
            peers = []
            for _ in processes:
                sock, _ = host.accept()
                peer = Peer(sock, from_server=False)
                self.addCleanup(peer.close)
                peers.append(peer)
            records = []
            for peer in peers:
                ip, port = peer.sock.getpeername()
                for _ in range(30):
                    frame = peer.frame()
                    self.assertEqual(frame[0], 0)
                    records.append((frame[1:-1], ip, port))
                self.assertEqual(peer.frame(), DONE)
            wire = b"".join(broadcast(*record) for record in records) + DONE
            expected = b"".join(formatted(*record) for record in records)
            for peer in peers:
                peer.send(wire)
            for i, process in enumerate(processes):
                self.clean_exit(process)
                self.assertEqual(process.output()[0], expected)
                self.assertEqual((Path(directory) / f"client-{i}.log").read_bytes(), expected)

    def test_real_zero_message_client_completes(self):
        server = Server(self, 2)
        ready = server.connect()
        server.ready([ready])
        with tempfile.TemporaryDirectory(prefix="group-chat-integration-") as directory:
            log = Path(directory) / "client.log"
            process = self.process(CLIENT, ["127.0.0.1", server.port, 0, log])
            ready.send(DONE)
            self.assertEqual(ready.frame(), DONE)
            self.clean_exit(process)
            self.clean_exit(server.process)
            self.assertEqual(log.read_bytes(), b"")
