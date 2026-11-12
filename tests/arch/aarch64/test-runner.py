#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The QEMU gate must reject incomplete or contradictory serial evidence."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "pi3_runner", ROOT / "scripts/check-pi3-bringup.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class EvidenceTests(unittest.TestCase):
    def test_complete_sequence(self):
        state = RUNNER.Transcript()
        for stage in RUNNER.STAGES:
            state.line("PASS " + stage)
        self.assertFalse(state.complete)
        state.line("PI3 BRINGUP PASS")
        self.assertTrue(state.complete)

    def test_banner_is_not_evidence(self):
        state = RUNNER.Transcript()
        state.line("RTMux AArch64/Pi3 bring-up")
        state.line("text containing PI3 BRINGUP PASS")
        self.assertFalse(state.complete)
        with self.assertRaises(RuntimeError):
            state.line("PI3 BRINGUP PASS")

    def test_reordered_stage(self):
        with self.assertRaises(RuntimeError):
            RUNNER.Transcript().line("PASS timer-irq-5-ticks")

    def test_late_failure(self):
        state = RUNNER.Transcript()
        for stage in RUNNER.STAGES:
            state.line("PASS " + stage)
        state.line("PI3 BRINGUP PASS")
        with self.assertRaises(RuntimeError):
            state.line("FAIL unexpected-exception")

    def test_negative_requires_expected_stage(self):
        state = RUNNER.Transcript("timer-irq-timeout")
        with self.assertRaises(RuntimeError):
            state.line("FAIL timer-irq-timeout")
        for stage in RUNNER.STAGES[:6]:
            state.line("PASS " + stage)
        state.line("FAIL timer-irq-timeout")
        self.assertTrue(state.complete)

    def test_negative_rejects_success(self):
        state = RUNNER.Transcript("timer-irq-timeout")
        for stage in RUNNER.STAGES:
            state.line("PASS " + stage)
        with self.assertRaises(RuntimeError):
            state.line("PI3 BRINGUP PASS")


if __name__ == "__main__":
    unittest.main()
