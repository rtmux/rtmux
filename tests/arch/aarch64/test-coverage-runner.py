#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Reject false-positive coverage evidence, including truncated/contradictory runs."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location('coverage', ROOT / 'scripts/check-pi3-coverage.py')
COVERAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COVERAGE)
RUNNER = COVERAGE.runner


class CoverageTests(unittest.TestCase):
    def validate(self, data, actual=0, **kwargs):
        return RUNNER.validate_output('fixture', data, actual, require_summary=True, **kwargs)

    def test_control_characters_from_tcflow(self):
        result = self.validate(b'\x13\x11  PASS  tcflow\r\n=== Results: 1 tests, 1 passed, 0 failed ===\r\n')
        self.assertEqual(result['passed_assertions'], ['tcflow'])

    def test_banner_or_truncated_output(self):
        for data in (b'=== RTMux Syscall Conformance Test Suite ===', b'  PASS  first\n'):
            with self.assertRaises(RuntimeError):
                self.validate(data)

    def test_failure_despite_zero_exit(self):
        with self.assertRaises(RuntimeError):
            self.validate(b'=== Results: 2 tests, 1 passed, 1 failed ===')

    def test_missing_assertions(self):
        with self.assertRaises(RuntimeError):
            self.validate(b'  PASS  one\n=== Results: 2 tests, 2 passed, 0 failed ===')

    def test_nonzero_exit_despite_success_text(self):
        with self.assertRaises(RuntimeError):
            self.validate(b'  PASS  one\n=== Results: 1 tests, 1 passed, 0 failed ===', actual=1)

    def test_trace_control_records_are_not_syscall_coverage(self):
        control = b'tracelogger: buf 0 ev 0 header=0x401 d=0,0,0\ntracelogger: PASS\n'
        with self.assertRaises(RuntimeError):
            RUNNER.validate_output('trace', control, 0, required_trace_classes=[2])
        kcall = b'tracelogger: buf 0 ev 1 header=0x881 d=1,2,3\n'
        r = RUNNER.validate_output('trace', control + kcall, 0, required_trace_classes=[2])
        self.assertEqual(r['trace_class_counts'], {'2': 1})

    def test_only_upstream_skip_allowed(self):
        forbidden = COVERAGE.conformance()['forbidden']
        data = b'  PASS  one\n=== Results: 1 tests, 1 passed, 0 failed ===\n'
        self.validate(data + b'  SKIP  spawn_concurrent (SMP: open defect Q32)\n', forbidden=forbidden)
        for skip in (b'  SKIP  irq controller absent\n', b'  SKIP  disconnect allocation failure\n'):
            with self.assertRaises(RuntimeError):
                self.validate(data + skip, forbidden=forbidden)


if __name__ == '__main__':
    unittest.main()
