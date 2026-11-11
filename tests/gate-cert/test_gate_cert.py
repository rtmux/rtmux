#!/usr/bin/env python3
"""Hermetic tests for scripts/gate_cert.py write and verify.

Every case builds its certificates in a private temporary directory (passed via
--certs or $GATE_CERT_DIR), so the run neither reads nor writes build/certs/ and
does not depend on whether the working tree is clean.  verify must accept a
complete, current, clean set and reject a stale commit, a dirty flag, a missing
certificate, and malformed certificates.
"""

import copy
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
GATE_CERT = SCRIPTS / "gate_cert.py"
sys.path.insert(0, str(SCRIPTS))
import gate_cert  # noqa: E402

HEAD = gate_cert.out(["git", "rev-parse", "HEAD"])
STALE = "0" * 40


def good(gate):
    return {
        "gate": gate, "tool": "python3", "tool_version": "Python 3",
        "commit": HEAD, "dirty": False, "timestamp_utc": "2026-01-01T00:00:00Z",
        "assertions": [f"{gate}: fixture assertion"], "result": "ok",
    }


def run(*args, env=None):
    e = dict(os.environ)
    e.pop("GATE_CERT_DIR", None)
    e.update(env or {})
    return subprocess.run([sys.executable, str(GATE_CERT), *args], text=True,
                          capture_output=True, env=e)


class VerifyTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)
        for gate in gate_cert.GATES:
            self.put(gate, good(gate))

    def put(self, gate, cert):
        (self.dir / f"{gate}.json").write_text(json.dumps(cert))

    def mutate(self, name, **changes):
        cert = copy.deepcopy(good(name))
        for key, value in changes.items():
            if value is None:
                cert.pop(key)
            else:
                cert[key] = value
        self.put(name, cert)

    def verify(self, *extra):
        return run("verify", "--certs", str(self.dir), *extra)

    def assertRejects(self, needle, *extra):
        r = self.verify(*extra)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn(needle, r.stderr)

    def test_complete_set_passes(self):
        r = self.verify()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("gate_cert: ok", r.stdout)

    def test_stale_commit_fails(self):
        self.mutate("check-temporal", commit=STALE)
        self.assertRejects("check-temporal: commit 000000000000 != HEAD")
        self.assertRejects("!= HEAD", "--allow-dirty")

    def test_dirty_flag_fails_unless_allowed(self):
        self.mutate("check-cap-smt", dirty=True)
        self.assertRejects("check-cap-smt: certificate was produced on a dirty tree")
        self.assertEqual(self.verify("--allow-dirty").returncode, 0)

    def test_missing_dirty_flag_is_not_clean(self):
        self.mutate("check-cap-smt", dirty=None)
        self.assertRejects("check-cap-smt: missing field 'dirty'")

    def test_missing_certificate_fails(self):
        (self.dir / "check-ipc-cbmc.json").unlink()
        self.assertRejects("check-ipc-cbmc: no readable certificate")

    def test_unparsable_certificate_fails(self):
        (self.dir / "check-assumptions.json").write_text("{not json")
        self.assertRejects("check-assumptions: no readable certificate")

    def test_empty_directory_reports_every_gate(self):
        for p in self.dir.glob("*.json"):
            p.unlink()
        r = self.verify()
        self.assertEqual(r.returncode, 1)
        for gate in gate_cert.GATES:
            self.assertIn(f"{gate}: no readable certificate", r.stderr)

    def test_malformed_certificates_fail(self):
        self.mutate("check-assumptions", assertions=[])
        self.assertRejects("check-assumptions: empty or malformed assertion list")
        self.mutate("check-assumptions", assertions=["ok", ""])
        self.assertRejects("check-assumptions: empty or malformed assertion list")
        self.mutate("check-assumptions")
        self.mutate("check-evidence-claims", result="")
        self.assertRejects("check-evidence-claims: empty result")
        self.mutate("check-evidence-claims")
        self.mutate("check-formal-claims", gate="check-temporal")
        self.assertRejects("check-formal-claims: certificate names gate")
        self.mutate("check-formal-claims")
        self.mutate("check-capability-law", tool=None)
        self.assertRejects("check-capability-law: missing field 'tool'")


class WriteTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)
        self.env = {"GATE_CERT_DIR": str(self.dir)}

    def test_assertions_come_from_the_gate_file(self):
        f = self.dir / "a.assert"
        f.write_text("fact one\nfact two\nresult: all good\n")
        r = run("write", "check-assumptions", "--tool", "python3",
                "--assertions-file", str(f), env=self.env)
        self.assertEqual(r.returncode, 0, r.stderr)
        cert = json.loads((self.dir / "check-assumptions.json").read_text())
        self.assertEqual(cert["assertions"], ["fact one", "fact two"])
        self.assertEqual(cert["result"], "all good")
        self.assertEqual(cert["commit"], HEAD)

    def test_empty_or_resultless_gate_file_writes_nothing(self):
        f = self.dir / "a.assert"
        f.write_text("result: ok\n")
        r = run("write", "check-assumptions", "--tool", "python3",
                "--assertions-file", str(f), env=self.env)
        self.assertEqual(r.returncode, 1)
        f.write_text("a fact\n")
        r = run("write", "check-assumptions", "--tool", "python3",
                "--assertions-file", str(f), env=self.env)
        self.assertEqual(r.returncode, 1)
        self.assertFalse((self.dir / "check-assumptions.json").exists())

    def test_needs_requires_a_current_subgate_certificate(self):
        args = ["write", "check-capability-law", "--tool", "python3",
                "--assertion", "a fact", "--result", "ok",
                "--needs", "check-cap-smt"]
        self.assertEqual(run(*args, env=self.env).returncode, 1)
        (self.dir / "check-cap-smt.json").write_text(
            json.dumps(dict(good("check-cap-smt"), commit=STALE)))
        self.assertEqual(run(*args, env=self.env).returncode, 1)
        (self.dir / "check-cap-smt.json").write_text(json.dumps(good("check-cap-smt")))
        self.assertEqual(run(*args, env=self.env).returncode, 0)
        cert = json.loads((self.dir / "check-capability-law.json").read_text())
        self.assertTrue(any("check-cap-smt" in a for a in cert["assertions"]))

    def test_unknown_gate_is_rejected(self):
        r = run("write", "check-nope", "--tool", "python3", "--assertion", "x",
                "--result", "ok", env=self.env)
        self.assertEqual(r.returncode, 1)

    def test_emit_appends_only_when_gate_assertions_is_set(self):
        f = self.dir / "e.assert"
        old = os.environ.pop("GATE_ASSERTIONS", None)
        try:
            gate_cert.emit("dropped")
            self.assertFalse(f.exists())
            os.environ["GATE_ASSERTIONS"] = str(f)
            gate_cert.emit("kept")
            gate_cert.emit("done", result=True)
        finally:
            os.environ.pop("GATE_ASSERTIONS", None)
            if old is not None:
                os.environ["GATE_ASSERTIONS"] = old
        self.assertEqual(f.read_text(), "kept\nresult: done\n")


if __name__ == "__main__":
    unittest.main(verbosity=1)
