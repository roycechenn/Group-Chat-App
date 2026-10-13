# Chat test suite

These are black-box tests of the compiled server and client. They use Python's
standard library, real loopback TCP sockets, and a mock server; no pip packages
or Linux-only assignment tester executables are needed.

The suite contains **53 test methods**, with additional parameterized boundary
cases. It targets the existing binary protocol and the repairs described in
[the bug-fix guide](../docs/bug-fix-guide.md). It does not change application code.

## Build and run

Use Python 3.8 or newer, CMake 3.22 or newer, and a POSIX system such as Linux or
macOS. GCC or Clang is needed for the sanitizer option.

From the project root:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGROUP_CHAT_ENABLE_SANITIZERS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

CTest registers four suite entries: `chat_server`, `chat_client`, `chat_cli`, and
`chat_integration`. Each entry runs multiple Python test methods and writes a
JSON report under `build/test-results/`. Python is
optional for building the application, but required for these tests; CMake
prints a message if Python was not found and the tests could not be registered.
`BUILD_TESTING=OFF` disables their registration explicitly.

Sanitizers catch out-of-bounds writes that might otherwise seem to work. A crash
or sanitizer report is a failure, even in a test that expects invalid input to
be rejected. An orderly nonzero exit is expected for client protocol errors.

You can also run the Python runner directly:

```sh
python3 tests/run_tests.py
python3 tests/run_tests.py --suite server
python3 tests/run_tests.py --suite client
python3 tests/run_tests.py --suite cli
python3 tests/run_tests.py --suite integration
```

For a different build directory:

```sh
python3 tests/run_tests.py \
  --server build/debug/server \
  --client build/debug/client
```

List cases without running them, focus on one bug, or save a machine-readable
report:

```sh
python3 tests/run_tests.py --list
python3 tests/run_tests.py --suite server --match 1024_byte_input
python3 tests/run_tests.py --suite client --match maximum_valid_frame
python3 tests/run_tests.py --report build/test-results.json
```

`GROUP_CHAT_SERVER` and `GROUP_CHAT_CLIENT` environment variables can also choose
the executables. Explicit runner options override those variables.

## Coverage

| File | Methods | Behavior checked |
| --- | ---: | --- |
| `test_server.py` | 23 | Sender identity, echo and broadcast, empty text, fragmentation, combined frames, partial tails, length boundaries, four concurrent senders, identical recipient ordering, 100 clients, capacity rejection, completion ordering, duplicate and split completion, slot reuse, reset isolation, malformed frames, stale completion counts, slow readers |
| `test_client.py` | 18 | Requested random-message count and hex format, zero messages, concurrent receiving, exact stdout/log formatting, embedded newline bytes in binary headers, fragmented frames, log truncation and open failure, completion parsing, size limits, malformed types, truncated EOF, connection refusal |
| `test_cli.py` | 8 | Missing/extra arguments, occupied ports, invalid IPv4 addresses, port ranges, numeric suffixes, nonnumeric and negative counts, integer overflow, rejection before connecting |
| `test_integration.py` | 4 | Actual server/client echoes and shutdown, zero-message completion, three actual clients against the actual server, identical ordered streams from a mock relay |

The concurrent-sender test checks total-order agreement and each sender's own
message order. It does not invent a wall-clock ordering between simultaneous
sends from different connections.

## Policies these tests enforce

Some checks define the repaired behavior more precisely than the school tests:

- The maximum complete server broadcast is 1024 bytes, so chat text is limited
  to 1016 bytes. An incoming chat frame may therefore be at most 1018 bytes.
- An oversized or malformed sender connection is closed while healthy server
  connections remain usable.
- The client reports malformed, oversized, or truncated frames with an orderly
  nonzero exit instead of crashing, spinning, or silently accepting them.
- A completion vote belongs to a currently connected client and is removed on
  disconnect. Duplicate votes do not count twice.
- Ports must be in 1–65535. Client capacity must be positive. Message count may
  be zero, but cannot be negative or outside the implementation's integer range.
- Numeric arguments must be parsed completely; values such as `1oops` are invalid.
- A slow receiver may be disconnected. It must not prevent responsive clients
  from receiving messages or new connections from being served when space permits.

If you deliberately change the protocol or remove assignment-style completion
for the interactive chat version, update these expectations along with the code.

## Reading failures

Known bugs are ordinary tests, not skipped tests or expected failures. The suite
is intentionally not all green on the starting implementation. Keep the failing
cases while you repair the code, so they become regression checks afterward.

The initial sanitizer run passed **33 of 53 methods**. See
[BASELINE.md](BASELINE.md) for the per-suite results and failure categories.

Start with:

```sh
python3 tests/run_tests.py --suite server --match 1024_byte_input
python3 tests/run_tests.py --suite client --match maximum_valid_frame
```

Then work on `unknown_type`, `connection_reset`,
`disconnected_completion_vote`, and `slow_reader` in that order. Use the same
`--match` option for each name. Finally run all suites to catch regressions.

The runner prints the number of passing and failing **methods**. Unittest may
also print several failed subcases for one method, such as multiple invalid
ports; those are counted once in the method summary and JSON report.

## Isolation and timing

Each test launches its own processes, uses temporary log files and ephemeral
loopback ports, and cleans up sockets and child processes even when an assertion
fails. Startup broadcasts acknowledge acceptance of the intended peers without
consuming a hidden readiness slot. Short quiet-period checks and disconnect
settling delays are used where the protocol has no acknowledgment.

Most socket operations and process waits have a three-second timeout. The
slow-reader test floods about 17 MB and has a bounded eight-second sender wait.
Each CTest suite also has a 120-second outer timeout. Output is stored in
temporary files rather than pipes so verbose clients cannot deadlock the test
harness by filling a pipe.

Tests need permission to bind local sockets. A sandbox reporting `Operation not
permitted` is an environment restriction, not evidence of an application bug.
The stress test depends on OS scheduling and socket buffering; investigate an
isolated failure before treating it as a confirmed regression.

This suite covers the specified behaviors and reviewed failures. It is not an
exhaustive fuzzer, a benchmark, or a proof that all possible network failures
are handled. EINTR injection, allocation failures, and unimplemented chat
features require additional tests when those behaviors are introduced.
