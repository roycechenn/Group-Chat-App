#!/usr/bin/env python3
"""Run unittest cases against selected server/client binaries; no pip packages."""

import argparse
import json
import os
from pathlib import Path
import sys
import unittest


class MethodResult(unittest.TextTestResult):
    """Count parent methods once even when several parameter subcases fail."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.passed_ids = set()
        self.failed_ids = set()

    def addSuccess(self, test):
        self.passed_ids.add(test.id())
        super().addSuccess(test)

    def addFailure(self, test, err):
        self.failed_ids.add(test.id())
        super().addFailure(test, err)

    def addError(self, test, err):
        self.failed_ids.add(test.id())
        super().addError(test, err)

    def addSubTest(self, test, subtest, err):
        if err is not None:
            self.failed_ids.add(test.id())
        super().addSubTest(test, subtest, err)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, help="Server executable (default: build/server)")
    parser.add_argument("--client", type=Path, help="Client executable (default: build/client)")
    parser.add_argument("--suite", choices=("all", "server", "client", "cli", "integration"), default="all")
    parser.add_argument("--list", action="store_true", help="List cases without opening sockets")
    parser.add_argument("--match", help="Run only methods whose names contain this text")
    parser.add_argument("--report", type=Path, help="Write a JSON result report")
    args = parser.parse_args()
    for name in ("server", "client"):
        value = getattr(args, name)
        if value:
            os.environ[f"GROUP_CHAT_{name.upper()}"] = str(value.resolve())
    tests_dir = Path(__file__).resolve().parent
    pattern = "test_*.py" if args.suite == "all" else f"test_{args.suite}.py"
    if args.match:
        unittest.defaultTestLoader.testNamePatterns = [f"*{args.match}*"]
    suite = unittest.defaultTestLoader.discover(str(tests_dir), pattern=pattern)
    if suite.countTestCases() == 0:
        parser.error("No test methods matched the selection")
    if args.list:
        def cases(group):
            for entry in group:
                if isinstance(entry, unittest.TestSuite):
                    yield from cases(entry)
                else:
                    yield entry
        for case in cases(suite):
            print(case.id())
        print(f"\n{suite.countTestCases()} test methods (some include additional boundary subcases).")
        return 0
    result = unittest.TextTestRunner(verbosity=2, resultclass=MethodResult).run(suite)
    print(f"\nMethod summary: {len(result.passed_ids)} passed, {len(result.failed_ids)} failed, {result.testsRun} total.", flush=True)
    if args.report:
        report = {
            "suite": args.suite,
            "total": result.testsRun,
            "passed": len(result.passed_ids),
            "failed": len(result.failed_ids),
            "passed_tests": sorted(result.passed_ids),
            "failed_tests": sorted(result.failed_ids),
            "details": [{"test": str(test), "traceback": detail}
                        for test, detail in result.failures + result.errors],
        }
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
