#!/usr/bin/env python3
"""Exercise the analyzer gate's rejection and stable-key boundaries."""

import importlib.util
import json
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/check-analysis.py"
spec = importlib.util.spec_from_file_location("analysis_gate", SCRIPT)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class GateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.out = self.root / "out"
        self.out.mkdir()
        self.report = self.out / "sample.analyzed"
        self.baseline = self.root / "baseline.json"
        self.finding = {
            "check_name": "core.NullDereference",
            "description": "Dereference of null pointer",
            "issue_context": "sample",
            "location": {"file": 0, "line": 12, "col": 3},
        }
        self.entry = {
            "translation_unit": "sample.c",
            "checker": "core.NullDereference",
            "file": "header.h",
            "function": "sample",
            "message": "Dereference of null pointer",
            "count": 1,
            "reason": "Fixture exercises reviewed diagnostic matching.",
        }
        self.write_report([self.finding])
        self.write_baseline([self.entry])

    def write_report(self, findings):
        self.report.write_bytes(plistlib.dumps({
            "clang_version": "fixture",
            "files": [str(self.root / "header.h")],
            "diagnostics": findings,
        }))

    def write_baseline(self, entries, arch="x86_64"):
        self.baseline.write_text(json.dumps({
            "schema": 1, "architecture": arch, "diagnostics": entries,
        }))

    def run_gate(self):
        return subprocess.run([
            sys.executable, str(SCRIPT), "--root", str(self.root),
            "--out", str(self.out), "--arch", "x86_64",
            "--baseline", str(self.baseline),
            "--inventory", str(self.out / "inventory.json"), str(self.report),
        ], text=True, capture_output=True)

    def assert_rejected(self, expected=1):
        result = self.run_gate()
        self.assertEqual(result.returncode, expected, result.stderr)
        self.assertNotIn("static analysis: clean", result.stdout + result.stderr)

    def test_reviewed_header_diagnostic_survives_line_movement(self):
        self.finding["location"]["line"] = 400
        self.write_report([self.finding])
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("1 baselined diagnostics, 0 unexpected", result.stdout)

    def test_empty_report_is_valid(self):
        self.write_report([])
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("0 baselined diagnostics", result.stdout)

    def test_unreviewed_diagnostic_fails(self):
        self.write_baseline([])
        self.assert_rejected()

    def test_additional_identical_occurrence_fails(self):
        self.write_report([self.finding, self.finding])
        self.assert_rejected()

    def test_different_translation_unit_does_not_match(self):
        self.entry["translation_unit"] = "other.c"
        self.write_baseline([self.entry])
        self.assert_rejected()

    def test_different_message_does_not_match(self):
        self.finding["description"] = "A new finding"
        self.write_report([self.finding])
        self.assert_rejected()

    def test_missing_report_fails(self):
        self.report.unlink()
        self.assert_rejected(2)

    def test_old_empty_stamp_fails(self):
        self.report.write_bytes(b"")
        self.assert_rejected(2)

    def test_malformed_report_fails(self):
        self.report.write_bytes(plistlib.dumps({"diagnostics": []}))
        self.assert_rejected(2)

    def test_invalid_file_index_fails(self):
        self.finding["location"]["file"] = -1
        self.write_report([self.finding])
        self.assert_rejected(2)

    def test_wrong_architecture_fails(self):
        self.write_baseline([self.entry], "aarch64")
        self.assert_rejected(2)

    def test_missing_reason_fails(self):
        self.entry["reason"] = " "
        self.write_baseline([self.entry])
        self.assert_rejected(2)

    def test_duplicate_baseline_fails(self):
        self.write_baseline([self.entry, self.entry])
        self.assert_rejected(2)

    def test_invalid_count_fails(self):
        self.entry["count"] = True
        self.write_baseline([self.entry])
        self.assert_rejected(2)


if __name__ == "__main__":
    unittest.main()
