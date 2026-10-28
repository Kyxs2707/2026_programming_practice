"""每次 PR 检查 B01、B02 的 Release 规模增长并保存完整证据。"""

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
MIN_SECONDS = 0.1
CALIBRATION_TARGET_SECONDS = 0.5
MAX_VARIATION = 0.2
MAX_RETRIES = 2
BUDGET_SECONDS = 180
MAX_INT64 = (1 << 63) - 1
spec = importlib.util.spec_from_file_location("myb_audit_reproduction", ROOT / "tests/audit/20261027/reproduce.py")
reproduction = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reproduction)


class BudgetExceeded(Exception):
    """表示本性能项目的有限测量预算已用完。"""


class MeasurementFailure(Exception):
    """携带已完成轮次，避免失败后丢失原始记录。"""


class Deadline:
    def __init__(self, seconds, clock=time.monotonic):
        self.clock = clock
        self.end = clock() + seconds

    def remaining(self):
        remaining = self.end - self.clock()
        if remaining <= 0:
            raise BudgetExceeded("performance budget exhausted")
        return remaining


def write_report(path, report):
    """保留异常数值的文本表示，生成符合 JSON 标准的证据。"""
    def safe(value):
        if isinstance(value, float) and not math.isfinite(value):
            return repr(value)
        if isinstance(value, dict):
            return {key: safe(item) for key, item in value.items()}
        if isinstance(value, (list, tuple)):
            return [safe(item) for item in value]
        return value
    path = Path(path)
    path.write_bytes((json.dumps(safe(report), ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8"))


def run_process(exe, arguments, cwd, timeout):
    command = [str(exe), *map(str, arguments)]
    started = time.perf_counter()
    row = {"command": command, "cwd": str(cwd), "timeout_seconds": timeout,
           "started_at": datetime.now(timezone.utc).isoformat()}
    try:
        process = subprocess.run(command, cwd=cwd, input=b"", capture_output=True, timeout=timeout)
        row.update(exit=process.returncode, stdout_hex=process.stdout.hex(), stderr_hex=process.stderr.hex())
        try:
            row.update(stdout=process.stdout.decode("utf-8", errors="strict"),
                       stderr=process.stderr.decode("utf-8", errors="strict"))
        except UnicodeDecodeError as error:
            row.update(error="invalid output UTF-8", detail=str(error))
    except subprocess.TimeoutExpired as error:
        row.update(error="timeout", stdout_hex=(error.stdout or b"").hex(), stderr_hex=(error.stderr or b"").hex())
    except OSError as error:
        row.update(error="process launch failed", detail=str(error))
    row["seconds"] = time.perf_counter() - started
    return row


def valid_duration(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and value > 0


def validate_run(row, expected):
    if row.get("error"):
        return row["error"]
    if not valid_duration(row.get("seconds")):
        return "duration must be finite and positive"
    if row.get("exit") != 0 or row.get("stdout") != expected or row.get("stderr") != "":
        return "exit/stdout/stderr mismatch"
    return None


def sample_statistics(row, rounds=5):
    """按中位数之差和正常执行波动检查一组原始样本。"""
    iterations = row.get("iterations")
    if not isinstance(iterations, int) or isinstance(iterations, bool) or not 0 < iterations <= MAX_INT64:
        return {"status": "failed", "reason": "missing or invalid work count"}
    checks, executions = row.get("check_runs", []), row.get("execution_runs", [])
    if rounds < 5 or len(checks) != rounds or len(executions) != rounds:
        return {"status": "failed", "reason": "missing measurement rounds"}
    for samples, expected in ((checks, ""), (executions, f"{iterations}\n")):
        for sample in samples:
            problem = validate_run(sample, expected)
            if problem:
                return {"status": "failed", "reason": problem}
    check_median = statistics.median(item["seconds"] for item in checks)
    run_median = statistics.median(item["seconds"] for item in executions)
    if not valid_duration(check_median) or not valid_duration(run_median):
        return {"status": "failed", "reason": "nonfinite derived median"}
    estimate = run_median - check_median
    variation = (max(item["seconds"] for item in executions) - min(item["seconds"] for item in executions)) / run_median
    if not math.isfinite(estimate) or not math.isfinite(variation):
        return {"status": "failed", "reason": "nonfinite derived timing"}
    problems = []
    if estimate < MIN_SECONDS:
        problems.append("estimated execution is below 100 ms")
    if variation > MAX_VARIATION:
        problems.append("execution variation exceeds 20%")
    return {"status": "inconclusive" if problems else "passed", "reason": "; ".join(problems),
            "check_median_seconds": check_median, "run_median_seconds": run_median,
            "estimated_execution_seconds": estimate, "variation": variation}


def case_keys(case):
    if case == "B01":
        return [(target, scale) for target in ("builtin", "user") for scale in (0, 1000, 5000)]
    if case == "B02":
        return [("struct", scale) for scale in (256, 512, 1024)]
    raise ValueError(f"unknown performance case: {case}")


def combined_status(statuses):
    statuses = list(statuses)
    if not statuses or any(status not in ("passed", "failed", "inconclusive") for status in statuses):
        return "failed"
    if "failed" in statuses:
        return "failed"
    return "inconclusive" if "inconclusive" in statuses else "passed"


def inspect_rows(case, rows, rounds=5):
    keys = [(row.get("target"), row.get("scale")) for row in rows]
    if len(keys) != len(set(keys)) or set(keys) != set(case_keys(case)):
        return {"case": case, "status": "failed", "reason": "missing, duplicate or unexpected scales", "rows": rows}
    if len({row.get("iterations") for row in rows}) != 1:
        return {"case": case, "status": "failed", "reason": "work counts differ between scales", "rows": rows}
    inspected = [dict(row, statistics=sample_statistics(row, rounds)) for row in rows]
    return {"case": case, "status": combined_status(row["statistics"]["status"] for row in inspected), "rows": inspected}


def evaluate_case(case, rows, rounds=5):
    result = inspect_rows(case, rows, rounds)
    if result["status"] != "passed":
        return result
    estimates = {(row["target"], row["scale"]): row["statistics"]["estimated_execution_seconds"] for row in result["rows"]}
    targets, low, high, limit = (("builtin", "user"), 0, 5000, 1.5) if case == "B01" else (("struct",), 256, 1024, 6.0)
    result["ratios"] = {target: estimates[target, high] / estimates[target, low] for target in targets}
    result["limit"] = limit
    if any(not math.isfinite(value) or value > limit for value in result["ratios"].values()):
        result.update(status="failed", reason="scale growth exceeds limit")
    return result


def exit_status(cases, required=("B01", "B02")):
    names = [row.get("case") for row in cases]
    if len(names) != len(set(names)) or set(names) != set(required):
        return 1
    status = combined_status(row.get("status") for row in cases)
    return {"passed": 0, "failed": 1, "inconclusive": 2}[status]


def measure_versions(versions, script, iterations, *, rounds=5, timeout=30,
                     executor=run_process, deadline=None):
    """同一输入预热后按轮交换先后顺序，保留各版本全部执行结果。"""
    measured = {label: {"warmup_runs": [], "check_runs": [], "execution_runs": []} for label in versions}
    def execute_one(label, check, destination):
        remaining = deadline.remaining() if deadline else timeout
        arguments = ["--check", script] if check else [script]
        try:
            row = executor(versions[label], arguments, ROOT, min(timeout, remaining))
        except subprocess.TimeoutExpired as error:
            row = {"error": "timeout", "detail": str(error)}
        except OSError as error:
            row = {"error": "process launch failed", "detail": str(error)}
        row = dict(row, version=label)
        destination.append(row)
        problem = validate_run(row, "" if check else f"{iterations}\n")
        if problem:
            raise MeasurementFailure(problem)
    try:
        for label in versions:
            execute_one(label, False, measured[label]["warmup_runs"])
        for index in range(rounds):
            labels = list(versions)
            if index % 2:
                labels.reverse()
            for label in labels:
                execute_one(label, True, measured[label]["check_runs"])
                execute_one(label, False, measured[label]["execution_runs"])
        return measured
    except (MeasurementFailure, BudgetExceeded) as error:
        error.records = measured
        raise


def source_record(case, target, scale, iterations, directory):
    source = reproduction.dispatch_source(scale, iterations, target) if case == "B01" else reproduction.struct_source(scale, iterations)
    content = source.encode("utf-8")
    path = directory / f"{case}_{target}_{scale}.myb"
    path.write_bytes(content)
    return {"target": target, "scale": scale, "iterations": iterations, "script": str(path),
            "source": source, "source_sha256": hashlib.sha256(content).hexdigest(), "source_bytes": len(content)}


def collect_case(case, exe, output, *, rounds=5, calls=300000, constructions=300,
                 timeout=30, budget_seconds=BUDGET_SECONDS, executor=run_process, clock=time.monotonic):
    deadline = Deadline(budget_seconds, clock)
    result = {"case": case, "status": "inconclusive", "budget_seconds": budget_seconds, "attempts": []}
    iterations = calls if case == "B01" else constructions
    targets, minimum = (("builtin", "user"), 0) if case == "B01" else (("struct",), 256)
    minimum_keys = [(target, minimum) for target in targets]
    keys = minimum_keys + [key for key in case_keys(case) if key not in minimum_keys]
    for attempt_number in range(MAX_RETRIES + 1):
        directory = Path(output) / case / f"attempt-{attempt_number}"
        directory.mkdir(parents=True, exist_ok=True)
        attempt = {"iterations": iterations, "rows": []}
        result["attempts"].append(attempt)
        try:
            for position, (target, scale) in enumerate(keys):
                deadline.remaining()
                row = source_record(case, target, scale, iterations, directory)
                attempt["rows"].append(row)
                try:
                    row.update(measure_versions({"current": exe}, Path(row["script"]), iterations,
                                                rounds=rounds, timeout=timeout, executor=executor, deadline=deadline)["current"])
                except (MeasurementFailure, BudgetExceeded) as error:
                    row.update(getattr(error, "records", {}).get("current", {}))
                    raise
                deadline.remaining()
                row["statistics"] = sample_statistics(row, rounds)
                if row["statistics"]["status"] == "failed":
                    raise MeasurementFailure(row["statistics"]["reason"])
                if position + 1 == len(minimum_keys) and any(item["statistics"]["status"] != "passed" for item in attempt["rows"]):
                    break
            complete = len(attempt["rows"]) == len(keys)
            decision = evaluate_case(case, attempt["rows"], rounds) if complete else {"status": "inconclusive", "reason": "minimum scale requires calibration"}
            attempt["evaluation"] = decision
            if decision["status"] != "inconclusive":
                result.update(status=decision["status"], evaluation=decision)
                return result
            estimates = [item["statistics"]["estimated_execution_seconds"] for item in attempt["rows"]]
            shortest = min(estimates)
            if attempt_number == MAX_RETRIES:
                break
            # 固定绝对干扰可能在超过最短时长后仍占较高比例，重新校准时统一扩大工作量。
            factor = min(1000, max(2, math.ceil(CALIBRATION_TARGET_SECONDS / max(shortest, 0.001))))
            if iterations > MAX_INT64 // factor:
                raise MeasurementFailure("calibrated work count exceeds Int64")
            attempt["recalibration"] = {"factor": factor, "next_iterations": iterations * factor}
            iterations *= factor
        except BudgetExceeded as error:
            result.update(status="inconclusive", reason=str(error))
            return result
        except (MeasurementFailure, ValueError, OSError) as error:
            result.update(status="failed", reason=str(error))
            return result
    result["reason"] = "calibration or stability retry limit exhausted"
    return result


def run_suite(exe, output, **options):
    result = []
    for case in ("B01", "B02"):
        try:
            result.append(collect_case(case, exe, output, **options))
        except Exception as error:
            result.append({"case": case, "status": "failed", "reason": f"{type(error).__name__}: {error}"})
    return result


def metadata(executables):
    def revision():
        try:
            return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True, timeout=5).strip()
        except (OSError, subprocess.SubprocessError):
            return "unavailable"
    described = {}
    for label, exe in executables.items():
        exe = Path(exe).resolve()
        described[label] = {"path": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
                            "dlls": [{"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()} for path in sorted(exe.parent.glob("*.dll"))]}
    return {"schema_version": 1, "policy_version": 1, "audit_date": "2026-10-27",
            "driver_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "generator_sha256": hashlib.sha256((ROOT / "tests/audit/20261027/reproduce.py").read_bytes()).hexdigest(),
            "started_at": datetime.now(timezone.utc).isoformat(),
            "commit": revision(), "platform": platform.platform(),
            "processor": os.environ.get("PROCESSOR_IDENTIFIER", platform.processor()), "python": sys.version,
            "clock": vars(time.get_clock_info("perf_counter")), "executables": described,
            "thresholds": {"B01_growth": 1.5, "B02_growth": 6, "minimum_estimated_seconds": MIN_SECONDS,
                           "calibration_target_seconds": CALIBRATION_TARGET_SECONDS,
                           "maximum_variation": MAX_VARIATION, "maximum_retries": MAX_RETRIES,
                           "case_budget_seconds": BUDGET_SECONDS}}


def fresh_output(path):
    path = Path(path).resolve()
    if path.exists() and (not path.is_dir() or any(path.iterdir())):
        raise ValueError(f"output directory must be new or empty: {path}")
    path.mkdir(parents=True, exist_ok=True)
    return path


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interpreter", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--calls", type=int, default=300000)
    parser.add_argument("--constructions", type=int, default=300)
    parser.add_argument("--timeout", type=float, default=30)
    try:
        args = parser.parse_args(arguments)
        if args.rounds < 5 or not 0 < args.calls <= MAX_INT64 or not 0 < args.constructions <= MAX_INT64 or not valid_duration(args.timeout):
            parser.error("require rounds >= 5, positive Int64 work counts and a finite positive timeout")
        exe = args.interpreter.resolve()
        if not exe.is_file():
            parser.error(f"missing interpreter: {exe}")
        output = fresh_output(args.output)
        report = metadata({"current": exe})
        report["arguments"] = {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()}
        report["cases"] = run_suite(exe, output, rounds=args.rounds, calls=args.calls,
                                    constructions=args.constructions, timeout=args.timeout)
        report["exit"] = exit_status(report["cases"])
        report["finished_at"] = datetime.now(timezone.utc).isoformat()
        write_report(output / "results.json", report)
        for case in report["cases"]:
            print(json.dumps({"case": case["case"], "status": case["status"]}), flush=True)
        return report["exit"]
    except (ValueError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 2
    except SystemExit as error:
        return error.code


if __name__ == "__main__":
    raise SystemExit(main())
