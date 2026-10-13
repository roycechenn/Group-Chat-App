# Group Chat

A TCP broadcast server and concurrent test client, built to explore socket
programming, message framing, and connection management.

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
- Coordinates shutdown when the expected clients report that they are finished.

## Technology

The current implementation is **C**: the source files use `.c` extensions and
CMake configures the project with `LANGUAGES C`. It uses POSIX sockets and threads,
with CMake as the build system. A future C++ conversion would require changes to
the source and build configuration.

## Build

Requirements:

- CMake 3.22 or newer.
- A C compiler, such as Clang or GCC.
- A POSIX environment with sockets, threads, and `getentropy()` support.

The project was originally written for Linux. The current source also builds on
macOS. The supplied assignment test executables run on Linux.

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

The server's client count currently controls both the maximum number of active
connections and the number of completion messages needed for shutdown. Additional
connections are closed when all slots are occupied.

Use a numeric IPv4 address for the client. The server binds to all local IPv4
interfaces. Each client truncates its log file when it starts.

## Protocol

Type values are binary bytes: `0x00` and `0x01`, rather than the text characters
`"0"` and `"1"`.

| Direction | Frame |
| --- | --- |
| Client to server: chat | `[type: 1 byte = 0][text][newline]` |
| Server to clients: chat | `[type: 1 byte = 0][IPv4: 4 bytes][port: 2 bytes][text][newline]` |
| Completion / shutdown | `[type: 1 byte = 1][newline]` |

The address and port use network byte order. When parsing a server broadcast,
newline detection starts after its seven-byte header, because address bytes can
also contain `0x0A`.

The intended frame limit is 1024 bytes. A server broadcast therefore has room for
1016 bytes of text after accounting for its header and final newline. Payloads
must not contain newlines under this protocol. Current length validation needs
repair; the generated test messages are much smaller than this limit.

## Project structure

```text
CMakeLists.txt          Build configuration
src/server.c           TCP server and broadcast event loop
src/client.c           Concurrent random-message test client
include/server.h       Per-client server state
include/client.h       Client function declarations
bin/                   Linux assignment test executables
docs/assignment.md     Original assignment and tester instructions
docs/bug-fix-guide.md  Guided fixes for the current implementation
```

## Development and testing

Build with warnings and memory checks:

```sh
cmake -S . -B build/debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build/debug
```

Run the demo with `build/debug/server` and `build/debug/client` to use the
instrumented binaries. Sanitizer availability depends on the compiler and
platform.

The original Linux tester commands are documented in
[the assignment reference](docs/assignment.md#grading). There is currently no
project-owned automated test suite or CTest integration.

## Known issues

A local review verified normal broadcasts, fragmented and combined frames,
consistent ordering among connected recipients, and normal completion. It also
identified these issues:

- Large messages can overflow buffers in both the server and client.
- A connection reset can terminate the entire server.
- Slow readers can interrupt broadcasts or terminate the server.
- Reused client slots can retain stale completion counts and cause early shutdown.
- Unknown message types can leave the client parser spinning indefinitely.
- Numeric arguments and thread creation results need validation.

See [the bug-fix guide](docs/bug-fix-guide.md) for explanations, suggested changes,
and ways to check each fix.

## Roadmap

- Repair frame validation, connection error handling, and output buffering.
- Add repeatable integration tests for boundary sizes and connection failures.
- Add an interactive terminal client with nicknames and join/leave notices.
- Support `/users` and `/quit`.
- Let the server run independently of a fixed client count.
- Add graceful signal handling and a recorded demo.
