"""复现 2026-10-27 审计的性能问题和入口诊断差异。

在仓库根目录执行：
    python tests/audit/20261027/reproduce.py
计时串行执行；结果校验成功后保存各轮原始耗时。退出 0 表示复现过程完成。
"""

import argparse
import hashlib
import json
import os
import pathlib
import platform
import statistics
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]


def execute(exe, arguments, cwd, timeout):
    command = [str(exe), *map(str, arguments)]
    started = time.perf_counter()
    result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=timeout)
    return {
        "command": command,
        "cwd": str(cwd),
        "seconds": time.perf_counter() - started,
        "exit": result.returncode,
        "stdout": result.stdout.decode("utf-8", errors="strict"),
        "stderr": result.stderr.decode("utf-8", errors="strict"),
    }


def require_success(result, expected):
    if result["exit"] != 0 or result["stderr"] or result["stdout"] != expected:
        raise RuntimeError(json.dumps(result, ensure_ascii=False))


def measure(exe, script, expected, rounds, timeout):
    require_success(execute(exe, [script], ROOT, timeout), expected)
    checks, executions = [], []
    for _ in range(rounds):
        check = execute(exe, ["--check", script], ROOT, timeout)
        require_success(check, "")
        checks.append(check)
        execution = execute(exe, [script], ROOT, timeout)
        require_success(execution, expected)
        executions.append(execution)
    check_median = statistics.median(item["seconds"] for item in checks)
    run_median = statistics.median(item["seconds"] for item in executions)
    return {
        "script": str(script),
        "check_runs": checks,
        "execution_runs": executions,
        "check_median_seconds": check_median,
        "run_median_seconds": run_median,
        "estimated_execution_seconds": run_median - check_median,
    }


def dispatch_source(count, calls, target):
    declarations = "\n".join(f"fn unused_{i}() {{}}" for i in range(count))
    hot = "fn hot() { return 1; }\n" if target == "user" else ""
    call = "hot()" if target == "user" else "len(values)"
    return declarations + "\n" + hot + f"""let values = [1];
let i = 0;
let total = 0;
while (i < {calls}) {{
    total = total + {call};
    i = i + 1;
}}
print(total);
"""


def struct_source(count, constructions):
    fields = "\n".join(f"f{i:04d}: Int64;" for i in range(count))
    values = ", ".join(f"f{i:04d}: 1" for i in range(count))
    return f"""struct Record {{
{fields}
}}
let i = 0;
let total = 0;
while (i < {constructions}) {{
    let value = Record {{ {values} }};
    total = total + value.f0000;
    i = i + 1;
}}
print(total);
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=["all", "dispatch", "struct", "entry"], default="all")
    parser.add_argument("--configurations", choices=["debug", "release"], nargs="+", default=["release"])
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--calls", type=int, default=300000)
    parser.add_argument("--constructions", type=int, default=300)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "build/bug-audit-20261027/reproduction")
    args = parser.parse_args()
    if min(args.rounds, args.calls, args.constructions, args.timeout) <= 0:
        parser.error("rounds, calls, constructions and timeout must be positive")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {
        "date": "2026-10-27",
        "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "platform": platform.platform(),
        "processor": os.environ.get("PROCESSOR_IDENTIFIER", platform.processor()),
        "python": sys.version,
        "arguments": {key: str(value) if isinstance(value, pathlib.Path) else value for key, value in vars(args).items()},
        "executables": {},
        "observations": [],
    }

    def save(row):
        report["observations"].append(row)
        (output / "results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        summary = {key: value for key, value in row.items() if key not in ["check_runs", "execution_runs"]}
        print(json.dumps(summary, ensure_ascii=False), flush=True)

    for configuration in args.configurations:
        exe = ROOT / "build" / configuration / "myb.exe"
        if not exe.is_file():
            parser.error(f"missing interpreter: {exe}")
        report["executables"][configuration] = {"path": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest()}
        if args.case in ["all", "dispatch"]:
            for target in ["builtin", "user"]:
                for count in [0, 1000, 5000]:
                    script = output / f"dispatch_{configuration}_{target}_{count}.myb"
                    script.write_text(dispatch_source(count, args.calls, target), encoding="utf-8")
                    row = measure(exe, script, f"{args.calls}\n", args.rounds, args.timeout)
                    save(dict(case="dispatch", configuration=configuration, target=target,
                              unused_functions=count, calls=args.calls, **row))
        if args.case in ["all", "struct"]:
            for count in [64, 128, 256, 512, 1024]:
                script = output / f"struct_{configuration}_{count}.myb"
                script.write_text(struct_source(count, args.constructions), encoding="utf-8")
                row = measure(exe, script, f"{args.constructions}\n", args.rounds, args.timeout)
                save(dict(case="struct", configuration=configuration, fields=count,
                          constructions=args.constructions, **row))
        if args.case in ["all", "entry"]:
            entry_cwd = output / "entry"
            entry_cwd.mkdir(exist_ok=True)
            missing = entry_cwd / "missing source.myb"
            if missing.exists():
                raise RuntimeError(f"test requires a missing file: {missing}")
            row = execute(exe, [missing.name], entry_cwd, args.timeout)
            if row["exit"] != 2 or row["stdout"]:
                raise RuntimeError(json.dumps(row, ensure_ascii=False))
            native_path = str(missing).replace("/", "\\")
            save(dict(case="entry", configuration=configuration, expected_absolute_path=native_path,
                      absolute_path_present=native_path in row["stderr"].replace("/", "\\"), **row))
    print(f"Evidence: {output / 'results.json'}", flush=True)


if __name__ == "__main__":
    main()
