"""交替测量两份 Release，并严格检查两配置的入口诊断修复。"""

import argparse
from datetime import datetime, timezone
import importlib.util
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("myb_check_performance_shared", ROOT / "tools/check_performance.py")
performance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(performance)


def evaluate_comparison(case, before_rows, after_rows, rounds=5):
    """修复后的规模增长与大规模旧版耗时比例必须同时满足。"""
    before = performance.inspect_rows(case, before_rows, rounds)
    after = performance.evaluate_case(case, after_rows, rounds)
    result = {"case": case, "before": before, "after": after,
              "status": performance.combined_status((before["status"], after["status"]))}
    if result["status"] != "passed":
        return result
    old = {(row["target"], row["scale"]): row["statistics"]["estimated_execution_seconds"] for row in before["rows"]}
    new = {(row["target"], row["scale"]): row["statistics"]["estimated_execution_seconds"] for row in after["rows"]}
    targets, scale = (("builtin", "user"), 5000) if case == "B01" else (("struct",), 1024)
    result["after_before_ratios"] = {target: new[target, scale] / old[target, scale] for target in targets}
    result["after_before_limit"] = 0.25
    if any(value > 0.25 for value in result["after_before_ratios"].values()):
        result.update(status="failed", reason="large-case time exceeds 25% of reference")
    return result


def collect_comparison(case, before, after, output, *, rounds=5, calls=1000000,
                       constructions=3000, timeout=180, executor=performance.run_process):
    directory = Path(output) / case
    directory.mkdir(parents=True, exist_ok=True)
    iterations = calls if case == "B01" else constructions
    rows = {"before": [], "after": []}
    try:
        for target, scale in performance.case_keys(case):
            source = performance.source_record(case, target, scale, iterations, directory)
            for version in rows:
                rows[version].append(dict(source))
            try:
                measured = performance.measure_versions({"before": before, "after": after}, Path(source["script"]), iterations,
                                                        rounds=rounds, timeout=timeout, executor=executor)
            except performance.MeasurementFailure as error:
                for version, recorded in error.records.items():
                    rows[version][-1].update(recorded)
                raise
            for version, recorded in measured.items():
                rows[version][-1].update(recorded)
        return evaluate_comparison(case, rows["before"], rows["after"], rounds)
    except (performance.MeasurementFailure, ValueError, OSError) as error:
        return {"case": case, "status": "failed", "reason": str(error), "rows": rows}


def check_entry(exe, output, *, timeout=30, executor=performance.run_process):
    directory = Path(output)
    directory.mkdir(parents=True, exist_ok=True)
    missing = directory / "missing source.myb"
    if missing.exists():
        return {"status": "failed", "reason": "entry case requires a missing source file"}
    try:
        row = executor(exe, [missing.name], directory, timeout)
    except (subprocess.TimeoutExpired, OSError) as error:
        return {"status": "failed", "reason": str(error)}
    absolute = str(missing.resolve()).replace("/", "\\")
    stderr = row.get("stderr", "")
    passed = (not row.get("error") and performance.valid_duration(row.get("seconds")) and
              row.get("exit") == 2 and row.get("stdout") == "" and
              absolute in stderr.replace("/", "\\") and
              "entry I/O error [IO]" in stderr and stderr.count("\n") == 1 and
              "\r" not in stderr and not re.search(r":\d+:\d+:", stderr) and
              "call chain" not in stderr)
    return {"status": "passed" if passed else "failed", "expected_absolute_path": absolute, "run": row}


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--after-debug", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--calls", type=int, default=1000000)
    parser.add_argument("--constructions", type=int, default=3000)
    parser.add_argument("--timeout", type=float, default=180)
    try:
        args = parser.parse_args(arguments)
        if args.rounds < 5 or not 0 < args.calls <= performance.MAX_INT64 or not 0 < args.constructions <= performance.MAX_INT64 or not performance.valid_duration(args.timeout):
            parser.error("require rounds >= 5, positive Int64 work counts and a finite positive timeout")
        executables = {"before": args.before.resolve(), "after": args.after.resolve(), "after-debug": args.after_debug.resolve()}
        for path in executables.values():
            if not path.is_file():
                parser.error(f"missing interpreter: {path}")
        output = performance.fresh_output(args.output)
        report = performance.metadata(executables)
        # 严格对照由调用方同步调整循环数；180 秒总预算只属于每次 PR 的驱动。
        report["thresholds"].pop("case_budget_seconds")
        report["thresholds"]["maximum_retries"] = 0
        report["thresholds"]["process_timeout_seconds"] = args.timeout
        report["thresholds"]["maximum_after_before_ratio"] = 0.25
        report["arguments"] = {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()}
        report["cases"] = []
        for case in ("B01", "B02"):
            try:
                row = collect_comparison(case, executables["before"], executables["after"], output,
                                         rounds=args.rounds, calls=args.calls, constructions=args.constructions, timeout=args.timeout)
            except Exception as error:
                row = {"case": case, "status": "failed", "reason": f"{type(error).__name__}: {error}"}
            report["cases"].append(row)
            performance.write_report(output / "results.json", report)
        for label, configuration in (("after", "release"), ("after-debug", "debug")):
            try:
                row = check_entry(executables[label], output / f"entry-{configuration}", timeout=args.timeout)
            except Exception as error:
                row = {"status": "failed", "reason": f"{type(error).__name__}: {error}"}
            report["cases"].append(dict(row, case=f"B03-{configuration}"))
            performance.write_report(output / "results.json", report)
        report["exit"] = performance.exit_status(report["cases"], required=("B01", "B02", "B03-release", "B03-debug"))
        report["finished_at"] = datetime.now(timezone.utc).isoformat()
        performance.write_report(output / "results.json", report)
        for case in report["cases"]:
            print(f"{case['case']}: {case['status']}", flush=True)
        return report["exit"]
    except (ValueError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 2
    except SystemExit as error:
        return error.code


if __name__ == "__main__":
    raise SystemExit(main())
