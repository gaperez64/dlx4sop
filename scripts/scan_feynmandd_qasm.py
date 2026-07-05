#!/usr/bin/env python3

import argparse
import collections
import json
import pathlib
import subprocess
import sys


def qasm_files(root: pathlib.Path, include_invalid: bool) -> list[pathlib.Path]:
    files = sorted(root.rglob("*.qasm"))
    if include_invalid:
        return files
    return [path for path in files if "invalid" not in path.parts]


def classify(stderr: str) -> str:
    if "dynamic or classical OpenQASM features" in stderr:
        return "dynamic_classical"
    if "unsupported OpenQASM operation 'gate'" in stderr:
        return "unsupported_gate_definition"
    if "unsupported OpenQASM operation" in stderr:
        return "unsupported_gate"
    if "unsupported " in stderr and " angle" in stderr:
        return "unsupported_angle"
    if "statements must end with ';'" in stderr or "operation 'gate'" in stderr:
        return "unsupported_gate_definition"
    return "other_error"


def scan(qasm2sop: pathlib.Path, root: pathlib.Path, include_invalid: bool, limit: int | None) -> dict:
    files = qasm_files(root, include_invalid)
    if limit is not None:
        files = files[:limit]

    results = []
    counts: collections.Counter[str] = collections.Counter()
    for path in files:
        completed = subprocess.run(
            [str(qasm2sop), str(path)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        relpath = str(path.relative_to(root))
        if completed.returncode == 0:
            counts["ok"] += 1
            results.append({"path": relpath, "status": "ok"})
            continue

        kind = classify(completed.stderr)
        counts[kind] += 1
        diagnostic = completed.stderr.strip().splitlines()[-1] if completed.stderr.strip() else ""
        results.append({"path": relpath, "status": kind, "diagnostic": diagnostic})

    return {
        "root": str(root),
        "files": len(files),
        "counts": dict(sorted(counts.items())),
        "failures": [result for result in results if result["status"] != "ok"],
    }


def write_text(report: dict, max_failures: int) -> None:
    print(f"root: {report['root']}")
    print(f"files: {report['files']}")
    for key, value in report["counts"].items():
        print(f"{key}: {value}")

    failures = report["failures"][:max_failures]
    if failures:
        print("failures:")
        for failure in failures:
            print(f"- {failure['status']}: {failure['path']}: {failure['diagnostic']}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Scan FeynmanDD QASM files through qasm2sop.")
    parser.add_argument("qasm2sop", type=pathlib.Path)
    parser.add_argument("root", type=pathlib.Path, help="FeynmanDD checkout or corpus root.")
    parser.add_argument("--include-invalid", action="store_true")
    parser.add_argument("--limit", type=int)
    parser.add_argument("--format", choices=("text", "json"), default="text")
    parser.add_argument("--max-failures", type=int, default=20)
    args = parser.parse_args(argv)
    if args.limit is not None and args.limit < 0:
        parser.error("--limit must be non-negative")
    if args.max_failures < 0:
        parser.error("--max-failures must be non-negative")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    report = scan(args.qasm2sop, args.root, args.include_invalid, args.limit)
    if args.format == "json":
        print(json.dumps(report, sort_keys=True, indent=2))
    else:
        write_text(report, args.max_failures)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
