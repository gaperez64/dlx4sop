#!/usr/bin/env python3

import argparse
import collections
import json
import pathlib
import sys
from typing import Iterable, TextIO

from render_scoreboard import format_ns
from summarize_qasm_report import markdown_escape


AMPLITUDE_ABS_TOL = 1e-8


def read_jsonl(path: pathlib.Path) -> list[dict]:
    records = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip():
            continue
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"{path}:{line_number}: invalid JSONL row") from exc
    return records


def labelled_path(text: str) -> tuple[str, pathlib.Path]:
    if "=" in text:
        label, path_text = text.split("=", 1)
        if not label:
            raise argparse.ArgumentTypeError("label before '=' must be non-empty")
        return label, pathlib.Path(path_text)
    path = pathlib.Path(text)
    return path.stem, path


def result_key(record: dict) -> tuple[str, str, str, str, str]:
    return (
        str(record.get("source") or "unknown"),
        str(record.get("source_relative_path") or ""),
        str(record.get("case") or ""),
        str(record.get("input") or ""),
        str(record.get("output") or ""),
    )


def solver_config(record: dict) -> str:
    backend = str(record.get("backend") or "")
    if backend == "branch":
        return f"branch --branch-heuristic {record.get('branch_heuristic') or 'split'}"
    if backend == "rankwidth":
        return (
            f"rankwidth --rankwidth-generate {record.get('rankwidth_decomposition') or 'left-deep'} "
            f"--rankwidth-mode {record.get('rankwidth_mode') or 'count-table'}"
        )
    if backend == "treewidth":
        return f"treewidth --treewidth-order {record.get('treewidth_order') or 'min-fill'}"
    return backend


def status(record: dict) -> str:
    return str(record.get("status") or "ok")


def numeric(record: dict, key: str) -> int:
    value = record.get(key)
    return int(value) if isinstance(value, int) else 0


def amplitude(record: dict) -> complex | None:
    real = record.get("amplitude_real")
    imag = record.get("amplitude_imag")
    if isinstance(real, (int, float)) and isinstance(imag, (int, float)):
        return complex(float(real), float(imag))
    return None


def cap_value(record: dict, key: str) -> str:
    if key not in record:
        return "not recorded"
    value = record.get(key)
    return "none" if value is None else str(value)


def speedup_text(native_ns: int, solver_ns: int) -> str:
    if native_ns <= 0 or solver_ns <= 0:
        return "n/a"
    return f"{native_ns / solver_ns:.2f}x"


def native_error(record: dict) -> str:
    return str(record.get("error") or "")


def summarize(
    solver_sets: Iterable[tuple[str, list[dict]]],
    native_sets: Iterable[tuple[str, list[dict]]],
) -> list[dict]:
    native_by_tier_and_key: dict[tuple[str, tuple[str, str, str, str, str]], list[dict]] = {}
    for tier, records in native_sets:
        for record in records:
            native_by_tier_and_key.setdefault((tier, result_key(record)), []).append(record)

    grouped: dict[tuple[str, str, str], dict] = {}
    for tier, solver_records in solver_sets:
        for solver in solver_records:
            matches = native_by_tier_and_key.get((tier, result_key(solver)), [])
            if not matches:
                continue
            config = solver_config(solver)
            solver_ok = status(solver) == "ok"
            solver_elapsed = numeric(solver, "solve_elapsed_ns")
            for native in matches:
                engine = str(native.get("engine") or "unknown")
                key = (tier, config, engine)
                entry = grouped.setdefault(
                    key,
                    {
                        "tier": tier,
                        "solver_config": config,
                        "engine": engine,
                        "matched": 0,
                        "both_ok": 0,
                        "solver_ok_native_skip": 0,
                        "native_ok_solver_skip": 0,
                        "both_skip": 0,
                        "solver_elapsed_ns": 0,
                        "native_elapsed_ns": 0,
                        "amplitude_checked": 0,
                        "amplitude_mismatches": 0,
                        "amplitude_abs_error_sum": 0.0,
                        "amplitude_max_abs_error": 0.0,
                        "max_boundary_qubits": 0,
                        "qubit_caps": collections.Counter(),
                        "timeouts": collections.Counter(),
                        "memory_caps": collections.Counter(),
                        "native_errors": collections.Counter(),
                    },
                )
                native_ok = status(native) == "ok"
                entry["matched"] += 1
                entry["qubit_caps"][cap_value(native, "qubit_cap")] += 1
                entry["timeouts"][cap_value(native, "timeout_seconds")] += 1
                entry["memory_caps"][cap_value(native, "memory_limit_mib")] += 1
                if isinstance(native.get("qubits"), int):
                    entry["max_boundary_qubits"] = max(entry["max_boundary_qubits"], int(native["qubits"]))
                if solver_ok and native_ok:
                    entry["both_ok"] += 1
                    entry["solver_elapsed_ns"] += solver_elapsed
                    entry["native_elapsed_ns"] += numeric(native, "elapsed_ns")
                    solver_amplitude = amplitude(solver)
                    native_amplitude = amplitude(native)
                    if solver_amplitude is not None and native_amplitude is not None:
                        entry["amplitude_checked"] += 1
                        error = abs(solver_amplitude - native_amplitude)
                        entry["amplitude_abs_error_sum"] += error
                        entry["amplitude_max_abs_error"] = max(
                            entry["amplitude_max_abs_error"], error
                        )
                        if error > AMPLITUDE_ABS_TOL:
                            entry["amplitude_mismatches"] += 1
                elif solver_ok:
                    entry["solver_ok_native_skip"] += 1
                    error = native_error(native)
                    if error:
                        entry["native_errors"][error] += 1
                elif native_ok:
                    entry["native_ok_solver_skip"] += 1
                else:
                    entry["both_skip"] += 1
                    error = native_error(native)
                    if error:
                        entry["native_errors"][error] += 1

    return [grouped[key] for key in sorted(grouped)]


def serializable_row(row: dict) -> dict:
    out = dict(row)
    for key in ("qubit_caps", "timeouts", "memory_caps", "native_errors"):
        out[key] = dict(out[key])
    out["qsop_speedup_vs_native"] = speedup_text(out["native_elapsed_ns"], out["solver_elapsed_ns"])
    out["amplitude_mean_abs_error"] = (
        out["amplitude_abs_error_sum"] / out["amplitude_checked"]
        if out["amplitude_checked"]
        else 0.0
    )
    return out


def most_common(counter: collections.Counter) -> str:
    return counter.most_common(1)[0][0] if counter else ""


def write_markdown(rows: list[dict], file: TextIO) -> None:
    print("# Native Solver Comparison\n", file=file)
    print(
        "Rows join QSOP solver benchmark JSONL with native simulator JSONL on "
        "`source`, `source_relative_path`, `case`, `input`, and `output`. "
        "The speedup column is native elapsed time divided by QSOP solve time "
        "over rows where both engines completed. Amplitude mismatch columns are "
        "computed for completed rows where both JSONL records include amplitudes.",
        file=file,
    )
    print("", file=file)
    print(
        "| Tier | QSOP solver | Native engine | Both OK / matched | QSOP solve time | "
        "Native time | QSOP speedup | Amplitude checked | Mismatches | Mean amplitude error | "
        "Max amplitude error | "
        "Max boundary qubits | Qubit cap | Timeout | Memory cap | Main native skip reason |",
        file=file,
    )
    print(
        "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
        file=file,
    )
    for row in rows:
        reason = most_common(row["native_errors"])
        mean_error = (
            row["amplitude_abs_error_sum"] / row["amplitude_checked"]
            if row["amplitude_checked"]
            else 0.0
        )
        print(
            f"| {markdown_escape(row['tier'])} | `{markdown_escape(row['solver_config'])}` | "
            f"`{markdown_escape(row['engine'])}` | {row['both_ok']} / {row['matched']} | "
            f"{format_ns(row['solver_elapsed_ns'])} | {format_ns(row['native_elapsed_ns'])} | "
            f"{speedup_text(row['native_elapsed_ns'], row['solver_elapsed_ns'])} | "
            f"{row['amplitude_checked']} | {row['amplitude_mismatches']} | "
            f"{mean_error:.3g} | {row['amplitude_max_abs_error']:.3g} | "
            f"{row['max_boundary_qubits']} | {markdown_escape(most_common(row['qubit_caps']))} | "
            f"{markdown_escape(most_common(row['timeouts']))} | "
            f"{markdown_escape(most_common(row['memory_caps']))} | {markdown_escape(reason)} |",
            file=file,
        )


def write_json(rows: list[dict], file: TextIO) -> None:
    print(json.dumps([serializable_row(row) for row in rows], indent=2, sort_keys=True), file=file)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Join QSOP solver benchmark JSONL with native simulator JSONL."
    )
    parser.add_argument("--solver-jsonl", action="append", type=labelled_path, required=True)
    parser.add_argument("--native-jsonl", action="append", type=labelled_path, required=True)
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        solver_sets = [(label, read_jsonl(path)) for label, path in args.solver_jsonl]
        native_sets = [(label, read_jsonl(path)) for label, path in args.native_jsonl]
        rows = summarize(solver_sets, native_sets)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if args.format == "json":
        write_json(rows, sys.stdout)
    else:
        write_markdown(rows, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
