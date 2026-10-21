# Group Chat

A TCP broadcast server and concurrent test client written in C11, built to
explore socket programming, message framing, and connection management.

This project started as a school assignment and is being developed into a
personal terminal chat application. The current client generates random messages;
interactive typing, nicknames, and chat commands are planned features.

## Current features

- Accepts multiple IPv4 TCP connections using a `select()` event loop.
- Broadcasts messages to connected clients, including the sender.
- Adds the sender's IP address and port to each broadcast.
- Buffers incoming data to handle messages split across TCP reads or combined in
  a single read.
- Sends and receives concurrently in the client using POSIX threads.
- Prints received messages and saves them to a log file.
- Shares parsing, encoding, connection handling, and argument validation through
  the `chat_core` library.
- Rejects malformed and oversized frames, and isolates individual connection
  failures.
- Queues outgoing data per client, handles partial writes, and disconnects
  receivers whose output queues fill up.
- Coordinates shutdown when the expected clients report that they are finished,
  with a five-second deadline for flushing output queues.

## Technology

The implementation uses **C11**, POSIX sockets, POSIX threads, and CMake. The
server uses nonblocking sockets with `select()`; the client uses a sending thread
while its main thread receives and logs messages.

## Build

Requirements:

- CMake 3.22 or newer.
- A C11 compiler, such as Clang or GCC.
- A POSIX environment with sockets, threads, and `getentropy()` support.
- Python 3.8 or newer to run the automated tests; it is optional when building
  only the application.

The project was originally written for Linux. The latest verified build and test
run used macOS. The supplied assignment test executables under `bin/` run on Linux.

From the project directory:

```sh
cmake -S . -B build
cmake --build build
```

This creates `build/server` and `build/client`.

## Run a local demo

Start a server in one terminal:

```sh
./build/server 8000 2
```

In another terminal, start two clients:

```sh
./build/client 127.0.0.1 8000 100 /tmp/group-chat-client-a.log &
./build/client 127.0.0.1 8000 100 /tmp/group-chat-client-b.log &
wait
```

Each client sends 100 messages containing 20 random hexadecimal characters. It
prints and logs broadcasts received while connected. Messages sent before another
client connects are not replayed, so clients may have different log contents.

After both clients report completion, the server sends a shutdown message and
exits. `Ctrl+C` stops a process manually; coordinated signal handling is planned.

### Command-line arguments

```text
server <port> <expected-client-count>
client <server-ipv4-address> <port> <message-count> <log-file-path>
```

Ports must be in the range 1–65535, the expected client count must be 1–100, and
the message count must be between zero and `INT_MAX`.

The server's client count currently controls both the maximum number of active
connections and the number of completion messages needed for shutdown. Additional
connections are closed when all slots are occupied. Duplicate completion messages
count once per connection. A disconnected client's completion vote is removed,
so the server may wait for a replacement client before the group can finish.

Use a numeric IPv4 address for the client. The server binds to all local IPv4
interfaces. Each client truncates its log file when it starts.

## Protocol

Type values are binary bytes: `0x00` and `0x01`, rather than the text characters
`"0"` and `"1"`.

| Direction               | Frame                                                             |
| ----------------------- | ----------------------------------------------------------------- |
| Client to server: chat  | `[type: 1 byte = 0][text][newline]`                               |
| Server to clients: chat | `[type: 1 byte = 0][IPv4: 4 bytes][port: 2 bytes][text][newline]` |
| Completion / shutdown   | `[type: 1 byte = 1][newline]`                                     |

The address and port use network byte order. When parsing a server broadcast,
newline detection starts after its seven-byte header, because address bytes can
also contain `0x0A`.

Frame limits are enforced by the shared protocol module:

| Content                                              | Maximum size |
| ---------------------------------------------------- | -----------: |
| Chat text                                            |   1016 bytes |
| Client chat frame: type, text, newline               |   1018 bytes |
| Server broadcast: type, address, port, text, newline |   1024 bytes |
| Completion / shutdown frame                          |      2 bytes |

Payloads must not contain newlines under this protocol. Unknown types, malformed
completion frames, and oversized messages are rejected. The client also reports
truncated frames when the server closes the connection.

## Project structure

```text
CMakeLists.txt          Executables, shared library, sanitizers, and CTest
include/protocol.h     Message types, frame limits, and parser/encoder API
include/connection.h   Socket state, input buffers, and output queues
include/config.h       Server/client configuration and argument parsing API
src/protocol.c         Pure frame parsing and encoding
src/connection.c       Receiving, output buffering, sending, and cleanup
src/config.c           Checked command-line argument parsing
src/server.c           Connections, broadcast ordering, and group completion
src/client.c           Random generation, receiving, printing, and logging
tests/                 Python integration and regression tests
bin/                   Original Linux assignment test executables
```

## Development and testing

Build with warnings and memory checks:

```sh
cmake -S . -B build/debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DGROUP_CHAT_ENABLE_SANITIZERS=ON
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

The build enables `-Wall`, `-Wextra`, and `-Wpedantic`. The sanitizer option adds
AddressSanitizer and UndefinedBehaviorSanitizer with GCC or Clang. Run the demo
with `build/debug/server` and `build/debug/client` to use the instrumented binaries.
Sanitizer availability depends on the compiler and platform.

The project-owned suite contains **53 test methods** for server behavior, client
behavior, command-line validation, and end-to-end integration. It uses only
Python's standard library. CTest registers four suite entries; each runs multiple
Python methods and writes a JSON report under `build/debug/test-results/`.

To run one regression test against the debug build:

```sh
python3 tests/run_tests.py \
  --server build/debug/server \
  --client build/debug/client \
  --suite server \
  --match 1024_byte_input
```

Python must be detected during CMake configuration for the suites to be
registered. Use `-DBUILD_TESTING=OFF` when tests are not needed.

## Roadmap

- Add a separate interactive terminal client while retaining the message generator
  for testing.
- Add nicknames and join/leave notices.
- Support `/users` and `/quit`.
- Support a server mode that runs independently of a fixed client count.
- Add graceful handling of interrupt and termination signals.
- Add CI builds and tests on Linux and macOS.
- Record a short demo of the completed networking milestone.
