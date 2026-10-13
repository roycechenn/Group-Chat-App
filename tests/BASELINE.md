# Starting test results

Baseline recorded on **2026-10-12**, using macOS, Apple Clang, a Debug CMake
build, and AddressSanitizer plus UndefinedBehaviorSanitizer. Application source
was left unchanged.

| Suite | Passing methods | Failing methods | Total |
| --- | ---: | ---: | ---: |
| Server | 15 | 8 | 23 |
| Client | 10 | 8 | 18 |
| Command line | 4 | 4 | 8 |
| Integration | 4 | 0 | 4 |
| **Total** | **33** | **20** | **53** |

CTest counts four suite entries, so it reports one passing suite and three
failing suites. The table counts the individual Python test methods. Several
methods include multiple parameterized subcases.

The failing methods expose:

- Server and client buffer overflows and missing size rejection.
- Unknown types and invalid completion frames not being rejected.
- Truncated client frames at EOF being accepted without an error.
- A client connection reset terminating the server.
- Completion votes surviving disconnects and causing premature shutdown.
- A slow receiver disrupting the server's broadcast loop.
- Invalid ports and counts reaching networking or allocation instead of being
  rejected during argument validation.

All four integration methods pass, including the actual server with three
actual clients and a controlled multi-client relay. Normal framing, ordering,
logging, and completion tests also pass.

This is a snapshot of the starting implementation, not an expected-failure
configuration. Keep the failing tests enabled and make them pass as you apply
the repairs. Run the suite again for current results; CTest writes detailed JSON
reports under the selected build directory's `test-results/` folder.
