"""验证性能判据、有限重测、完整结果和退出状态。"""

import contextlib
import importlib.util
import io
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]


def temporary_directory():
    directory = ROOT / "build/driver-performance-temp"
    directory.mkdir(parents=True, exist_ok=True)
    return tempfile.TemporaryDirectory(prefix="myb-performance-", dir=directory)


def load_tool(name, path):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def record(seconds, stdout=""):
    return {"seconds": seconds, "exit": 0, "stdout": stdout, "stderr": ""}


def rows(case, *, bad=False):
    result = []
    targets = ["builtin", "user"] if case == "B01" else ["struct"]
    scales = [0, 1000, 5000] if case == "B01" else [256, 512, 1024]
    for target in targets:
        for scale in scales:
            elapsed = (0.2 if not bad else 0.2 * (1 + scale / 1000)) if case == "B01" else 0.2 * (scale / 256) ** (2 if bad else 1)
            result.append({"target": target, "scale": scale, "iterations": 17,
                           "check_runs": [record(0.02) for _ in range(5)],
                           "execution_runs": [record(elapsed + 0.02, "17\n") for _ in range(5)]})
    return result


class FakeRunner:
    """按输入程序产生可重复的耗时，推进同一预算时钟。"""

    def __init__(self, mode="normal"):
        self.now = 0.0
        self.mode = mode
        self.calls = []
        self.executions = 0

    def clock(self):
        return self.now

    def __call__(self, exe, arguments, cwd, timeout):
        self.calls.append((str(exe), list(map(str, arguments)), timeout))
        script = Path(arguments[-1])
        source = script.read_text(encoding="utf-8")
        iterations = int(re.search(r"while \(i < (\d+)\)", source)[1])
        is_struct = source.startswith("struct Record")
        if self.mode == "timeout" and not is_struct:
            raise subprocess.TimeoutExpired(str(exe), timeout)
        if arguments[0] == "--check":
            elapsed, output = 0.02, ""
        else:
            self.executions += 1
            scale = len(re.findall(r"f\d+: Int64;", source)) / 256 if is_struct else 1
            elapsed = 0.02 + iterations / (2000 if is_struct else 5000) * scale
            if self.mode == "unstable":
                elapsed *= 1.5 if self.executions % 2 else 0.5
            if self.mode == "fixed_interference" and self.executions % 2:
                elapsed += 0.08
            if self.mode == "nonfinite":
                elapsed = math.nan
            output = f"{iterations}\n"
        self.now += elapsed if math.isfinite(elapsed) else 0.1
        return record(elapsed, output)


class PerformanceTests(unittest.TestCase):
    def setUp(self):
        path = ROOT / "tools/check_performance.py"
        self.assertTrue(path.is_file(), "性能回归驱动尚未实现")
        self.tool = load_tool("myb_check_performance", path)
        self.directory = temporary_directory()
        self.addCleanup(self.directory.cleanup)
        self.output = Path(self.directory.name)

    def test_correct_linear_samples_pass(self):
        for case in ("B01", "B02"):
            self.assertEqual(self.tool.evaluate_case(case, rows(case))["status"], "passed")

    def test_old_linear_dispatch_and_quadratic_fields_fail(self):
        for case in ("B01", "B02"):
            self.assertEqual(self.tool.evaluate_case(case, rows(case, bad=True))["status"], "failed")

    def test_missing_duplicate_empty_and_zero_iterations_fail(self):
        for case in ("B01", "B02"):
            valid = rows(case)
            for values in ([], valid[:-1], valid + [valid[0]]):
                self.assertEqual(self.tool.evaluate_case(case, values)["status"], "failed")
            valid[0]["iterations"] = 0
            self.assertEqual(self.tool.evaluate_case(case, valid)["status"], "failed")

    def test_mixed_work_counts_fail(self):
        values = rows("B01")
        values[0]["iterations"] = 18
        self.assertEqual(self.tool.evaluate_case("B01", values)["status"], "failed")

    def test_wrong_output_and_nonfinite_or_nonpositive_times_fail(self):
        for value in (math.nan, math.inf, -math.inf, 0, -1):
            values = rows("B01")
            values[0]["execution_runs"][0]["seconds"] = value
            self.assertEqual(self.tool.evaluate_case("B01", values)["status"], "failed")
        for key, value in (("exit", 1), ("stdout", "wrong\n"), ("stderr", "error")):
            values = rows("B02")
            values[0]["execution_runs"][0][key] = value
            self.assertEqual(self.tool.evaluate_case("B02", values)["status"], "failed")

    def test_short_negative_estimates_and_noise_need_remeasurement(self):
        for seconds in (0.01, 0.05):
            values = rows("B02")
            values[0]["execution_runs"] = [record(seconds, "17\n") for _ in range(5)]
            self.assertEqual(self.tool.evaluate_case("B02", values)["status"], "inconclusive")
        values = rows("B01")
        values[0]["execution_runs"][0]["seconds"] = 1
        self.assertEqual(self.tool.evaluate_case("B01", values)["status"], "inconclusive")

    def test_fewer_than_five_samples_fail(self):
        values = rows("B01")
        values[0]["execution_runs"].pop()
        self.assertEqual(self.tool.evaluate_case("B01", values)["status"], "failed")

    def test_nonfinite_derived_statistics_fail(self):
        values = rows("B01")
        for row in values:
            row["check_runs"] = [record(1e307) for _ in range(6)]
            row["execution_runs"] = [record(1e308, "17\n") for _ in range(6)]
        self.assertEqual(self.tool.evaluate_case("B01", values, rounds=6)["status"], "failed")

    def test_process_executor_preserves_invalid_utf8_and_timeout_bytes(self):
        process = subprocess.CompletedProcess(["myb.exe"], 0, b"\xff", b"")
        with mock.patch.object(self.tool.subprocess, "run", return_value=process):
            result = self.tool.run_process("myb.exe", [], self.output, 1)
        self.assertEqual(result["stdout_hex"], "ff")
        self.assertIsNotNone(self.tool.validate_run(result, ""))
        error = subprocess.TimeoutExpired("myb.exe", 1, output=b"before\n", stderr=b"partial")
        with mock.patch.object(self.tool.subprocess, "run", side_effect=error):
            result = self.tool.run_process("myb.exe", [], self.output, 1)
        self.assertEqual(result["error"], "timeout")
        self.assertEqual(result["stdout_hex"], b"before\n".hex())

    def test_aggregate_requires_both_cases_and_propagates_either_failure(self):
        for cases, expected in (([], 1), ([{"case": "B01", "status": "passed"}], 1),
                                ([{"case": "B01", "status": "passed"}] * 2, 1)):
            self.assertEqual(self.tool.exit_status(cases), expected)
        for failed in ("B01", "B02"):
            cases = [{"case": name, "status": "failed" if name == failed else "passed"} for name in ("B01", "B02")]
            self.assertEqual(self.tool.exit_status(cases), 1)
        self.assertEqual(self.tool.exit_status([{"case": "B01", "status": "inconclusive"}, {"case": "B02", "status": "passed"}]), 2)

    def test_real_collector_calibrates_uniform_counts_and_keeps_attempts(self):
        runner = FakeRunner()
        result = self.tool.collect_case("B02", self.output / "myb.exe", self.output,
                                        constructions=100, executor=runner, clock=runner.clock)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(len(result["attempts"]), 2)
        for attempt in result["attempts"]:
            self.assertEqual(len({item["iterations"] for item in attempt["rows"]}), 1)
        self.assertTrue(all("source_sha256" in item for item in result["attempts"][-1]["rows"]))

    def test_retries_are_bounded_and_all_raw_attempts_remain(self):
        runner = FakeRunner("unstable")
        result = self.tool.collect_case("B01", self.output / "myb.exe", self.output,
                                        calls=1000, executor=runner, clock=runner.clock)
        self.assertEqual(result["status"], "inconclusive")
        self.assertEqual(len(result["attempts"]), 3)
        self.assertTrue(all(attempt["rows"] for attempt in result["attempts"]))

    def test_fixed_interference_recalibrates_all_scales_to_longer_work(self):
        runner = FakeRunner("fixed_interference")
        result = self.tool.collect_case("B01", self.output / "myb.exe", self.output,
                                        calls=1000, executor=runner, clock=runner.clock)
        first = result["attempts"][0]
        self.assertTrue(all(row["statistics"]["estimated_execution_seconds"] >= 0.1 for row in first["rows"]))
        self.assertTrue(any(row["statistics"]["variation"] > 0.2 for row in first["rows"]))
        self.assertEqual(result["status"], "passed")
        self.assertEqual(len(result["attempts"]), 2)
        final = result["attempts"][-1]
        self.assertGreaterEqual(final["iterations"], 2 * first["iterations"])
        self.assertEqual(len(final["rows"]), 6)
        self.assertEqual({row["iterations"] for row in final["rows"]}, {final["iterations"]})
        self.assertTrue(all(row["statistics"]["variation"] <= 0.2 for row in final["rows"]))

    def test_timeout_does_not_prevent_other_case_evidence(self):
        runner = FakeRunner("timeout")
        result = self.tool.run_suite(self.output / "myb.exe", self.output,
                                     calls=1000, constructions=400,
                                     executor=runner, clock=runner.clock)
        self.assertEqual([item["case"] for item in result], ["B01", "B02"])
        self.assertEqual(result[0]["status"], "failed")
        self.assertEqual(result[1]["status"], "passed")

    def test_budget_exhaustion_is_not_success(self):
        runner = FakeRunner()
        result = self.tool.collect_case("B01", self.output / "myb.exe", self.output,
                                        calls=1000, budget_seconds=0.01,
                                        executor=runner, clock=runner.clock)
        self.assertEqual(result["status"], "inconclusive")
        self.assertIn("budget", result["reason"])

    def test_budget_is_checked_after_the_last_sample(self):
        runner = FakeRunner()
        result = self.tool.collect_case("B02", self.output / "myb.exe", self.output,
                                        constructions=400, budget_seconds=9.055,
                                        executor=runner, clock=runner.clock)
        self.assertEqual(result["status"], "inconclusive")
        self.assertIn("budget", result["reason"])

    def test_nonfinite_actual_record_fails_and_json_remains_readable(self):
        runner = FakeRunner("nonfinite")
        result = self.tool.run_suite(self.output / "myb.exe", self.output,
                                     executor=runner, clock=runner.clock)
        self.assertTrue(all(item["status"] == "failed" for item in result))
        self.tool.write_report(self.output / "results.json", {"cases": result})
        self.assertEqual(len(json.loads((self.output / "results.json").read_text(encoding="utf-8"))["cases"]), 2)

    def test_cli_preserves_both_results_and_refuses_existing_evidence(self):
        exe = self.output / "myb.exe"
        exe.write_bytes(b"unused executable")
        cases = [{"case": "B01", "status": "failed"}, {"case": "B02", "status": "passed"}]
        destination = self.output / "evidence"
        with mock.patch.object(self.tool, "run_suite", return_value=cases), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(self.tool.main(["--interpreter", str(exe), "--output", str(destination)]), 1)
            report = json.loads((destination / "results.json").read_text(encoding="utf-8"))
            self.assertEqual(report["cases"], cases)
            self.assertIn("sha256", report["executables"]["current"])
            self.assertEqual(report["policy_version"], 1)
            self.assertEqual(len(report["generator_sha256"]), 64)
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(self.tool.main(["--interpreter", str(exe), "--output", str(destination)]), 2)


class FixComparisonTests(unittest.TestCase):
    def setUp(self):
        path = ROOT / "tests/audit/20261027/verify_fixes.py"
        self.assertTrue(path.is_file(), "严格对照驱动尚未实现")
        self.tool = load_tool("myb_verify_fixes", path)

    def test_unchanged_old_version_fails_both_regressions(self):
        for case in ("B01", "B02"):
            old = rows(case, bad=True)
            self.assertEqual(self.tool.evaluate_comparison(case, old, old)["status"], "failed")

    def test_fixed_scaling_and_large_case_speedup_must_both_pass(self):
        for case in ("B01", "B02"):
            before, after = rows(case, bad=True), rows(case)
            for item in before:
                for sample in item["execution_runs"]:
                    sample["seconds"] = 4 * (sample["seconds"] - 0.02) + 0.02
            self.assertEqual(self.tool.evaluate_comparison(case, before, after)["status"], "passed")
            self.assertEqual(self.tool.evaluate_comparison(case, after, after)["status"], "failed")

    def test_invalid_reference_and_missing_rows_cannot_pass(self):
        for case in ("B01", "B02"):
            self.assertEqual(self.tool.evaluate_comparison(case, [], rows(case))["status"], "failed")

    def test_entry_checks_absolute_path_exit_and_location(self):
        with temporary_directory() as directory:
            cwd = Path(directory)
            path = str(cwd / "missing source.myb").replace("/", "\\")
            good = {"seconds": 0.02, "exit": 2, "stdout": "", "stderr": f"myb: entry I/O error [IO]: {path} (OS 2)\n"}
            runner = mock.Mock(return_value=good)
            self.assertEqual(self.tool.check_entry(cwd / "myb.exe", cwd, executor=runner)["status"], "passed")
            for stderr in ("missing source.myb\n", good["stderr"] + "call chain\n", good["stderr"].replace(" (OS", ":1:1: (OS")):
                runner.return_value = dict(good, stderr=stderr)
                self.assertEqual(self.tool.check_entry(cwd / "myb.exe", cwd, executor=runner)["status"], "failed")

    def test_versions_share_source_and_runs_interleave(self):
        helper = self.tool.performance
        runner = FakeRunner()
        with temporary_directory() as directory:
            root = Path(directory)
            source = root / "program.myb"
            source.write_text(helper.reproduction.dispatch_source(0, 1000, "builtin"), encoding="utf-8")
            measured = helper.measure_versions({"before": root / "old.exe", "after": root / "new.exe"}, source, 1000,
                                               rounds=5, timeout=30, executor=runner)
            self.assertEqual(set(measured), {"before", "after"})
            self.assertEqual(len(measured["after"]["execution_runs"]), 5)
            self.assertEqual({call[1][-1] for call in runner.calls}, {str(source)})
            executed = [Path(call[0]).name for call in runner.calls if call[1][0] != "--check"]
            self.assertEqual(executed, ["old.exe", "new.exe", "old.exe", "new.exe", "new.exe", "old.exe",
                                        "old.exe", "new.exe", "new.exe", "old.exe", "old.exe", "new.exe"])

    def test_cli_requires_debug_and_keeps_failed_performance_and_entries(self):
        with temporary_directory() as directory:
            root = Path(directory)
            paths = [root / name for name in ("old.exe", "new.exe", "debug.exe")]
            for path in paths:
                path.write_bytes(b"unused")
            arguments = ["--before", str(paths[0]), "--after", str(paths[1]), "--after-debug", str(paths[2]), "--output", str(root / "results")]
            def comparison(case, *args, **kwargs):
                return {"case": case, "status": "failed" if case == "B01" else "passed"}
            with mock.patch.object(self.tool, "collect_comparison", side_effect=comparison), mock.patch.object(self.tool, "check_entry", return_value={"status": "passed"}) as entries, contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(self.tool.main(arguments), 1)
                self.assertEqual(entries.call_count, 2)
                report = json.loads((root / "results/results.json").read_text(encoding="utf-8"))
                self.assertEqual({row["case"] for row in report["cases"]}, {"B01", "B02", "B03-release", "B03-debug"})
                self.assertNotIn("case_budget_seconds", report["thresholds"])
                self.assertEqual(report["thresholds"]["maximum_retries"], 0)


if __name__ == "__main__":
    unittest.main()
