#!/usr/bin/env python3
"""Cross-check bounded-rankwidth family verdicts, counts, and amplitudes.

Usage:
    python3 tests/test_rankwidth_family_crosscheck.py \
        scripts/gen_rankwidth_family.py scripts/bench_sop_local.py build/sop-solve
"""

from __future__ import annotations

import cmath
import json
import math
import pathlib
import subprocess
import sys
import tempfile


DIRECT_BACKENDS = {
    "treewidth": [
        "--backend", "treewidth",
        "--treewidth-order", "min-fill-max-degree",
        "--max-vars", "128",
    ],
    "rankwidth:best": [
        "--backend", "rankwidth",
        "--rankwidth-generate", "best",
        "--rankwidth-mode", "count-table",
        "--max-vars", "128",
    ],
    "rankwidth:best:fourier": [
        "--backend", "rankwidth",
        "--rankwidth-generate", "best",
        "--rankwidth-mode", "fourier",
        "--max-vars", "128",
    ],
}


def _run(cmd: list[str], *, timeout: float = 30.0) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def _amplitude(modulus: int, norm_h: int, counts: list[int]) -> complex:
    omega = cmath.exp(2j * math.pi / modulus)
    if modulus % 2 == 0 and len(counts) == modulus:
        half = modulus // 2
        total = sum(
            (counts[residue] - counts[residue + half]) * (omega**residue)
            for residue in range(half)
        )
    else:
        total = sum(count * (omega**residue) for residue, count in enumerate(counts))
    return total * (2.0 ** (-norm_h / 2.0))


def _parse_stats(text: str) -> dict:
    parsed: dict = {"stats": {}}
    for line in text.splitlines():
        if ": " not in line:
            continue
        key, value = line.split(": ", 1)
        if key == "result_counts":
            parsed["counts"] = [int(part) for part in value.split()]
        elif key == "result_modulus":
            parsed["modulus"] = int(value)
        elif key == "result_norm_h":
            parsed["norm_h"] = int(value)
        elif key == "result_probability":
            parsed["probability"] = float(value)
        else:
            try:
                parsed["stats"][key] = int(value)
            except ValueError:
                parsed["stats"][key] = value
    missing = [key for key in ("counts", "modulus", "norm_h", "probability") if key not in parsed]
    if missing:
        raise AssertionError(f"missing solver fields {missing}\n{text}")
    parsed["amplitude"] = _amplitude(parsed["modulus"], parsed["norm_h"], parsed["counts"])
    return parsed


def _assert_close(label: str, actual: complex, expected: complex, tol: float = 1e-9) -> None:
    if abs(actual - expected) > tol:
        raise AssertionError(f"{label}: {actual!r} != {expected!r}")


def _assert_probability_consistent(label: str, parsed: dict) -> None:
    expected = abs(parsed["amplitude"]) ** 2
    observed = parsed["probability"]
    tolerance = max(1e-9, abs(expected) * 1e-10)
    if abs(observed - expected) > tolerance:
        raise AssertionError(
            f"{label}: probability {observed} != |amplitude|^2 {expected}"
        )


def _solve(sop_solve: pathlib.Path, qsop: pathlib.Path, label: str, args: list[str]) -> dict:
    cmd = [
        str(sop_solve),
        *args,
        "--format", "stats",
        "--include-result",
        "--include-probability",
        str(qsop),
    ]
    result = _run(cmd)
    if result.returncode != 0:
        raise AssertionError(
            f"{label} failed on {qsop.name} with exit {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    parsed = _parse_stats(result.stdout)
    _assert_probability_consistent(f"{qsop.name} {label}", parsed)
    return parsed


def _materialize_corpus(gen_tool: pathlib.Path, out_dir: pathlib.Path) -> list[pathlib.Path]:
    result = _run(
        [
            sys.executable,
            str(gen_tool),
            "--heights", "1,2",
            "--blowups", "2",
            "--materialize-dir", str(out_dir),
        ]
    )
    if result.returncode != 0:
        raise AssertionError(
            f"generator failed with exit {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    qsops = sorted((out_dir / "tier-rankwidth").glob("*.qsop"))
    if len(qsops) != 2:
        raise AssertionError(f"expected 2 generated cases, found {len(qsops)}")
    return qsops


def _crosscheck_direct(sop_solve: pathlib.Path, qsops: list[pathlib.Path]) -> dict[str, dict]:
    baselines: dict[str, dict] = {}
    for qsop in qsops:
        solved = {
            label: _solve(sop_solve, qsop, label, args)
            for label, args in DIRECT_BACKENDS.items()
        }
        baseline = solved["treewidth"]
        for label, parsed in solved.items():
            if parsed["counts"] != baseline["counts"]:
                raise AssertionError(
                    f"{qsop.name} {label} counts differ from treewidth\n"
                    f"{parsed['counts']}\n{baseline['counts']}"
                )
            _assert_close(f"{qsop.name} {label} amplitude", parsed["amplitude"], baseline["amplitude"])
        baselines[qsop.stem] = baseline
    return baselines


def _run_local_bench(
    bench_tool: pathlib.Path,
    sop_solve: pathlib.Path,
    corpus_dir: pathlib.Path,
    out_path: pathlib.Path,
) -> list[dict]:
    cmd = [
        sys.executable,
        str(bench_tool),
        "--sop-solve", str(sop_solve),
        "--corpus-dir", str(corpus_dir),
        "--tier", "tier-rankwidth",
        "--backend", "treewidth",
        "--backend", "rankwidth:best",
        "--backend", "rankwidth:best:fourier",
        "--timeout", "10",
        "--max-vars", "128",
        "--out", str(out_path),
        "--quiet",
    ]
    result = _run(cmd, timeout=60)
    if result.returncode != 0:
        raise AssertionError(
            f"bench_sop_local failed with exit {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return [json.loads(line) for line in out_path.read_text(encoding="utf-8").splitlines() if line]


def _crosscheck_bench_rows(rows: list[dict], baselines: dict[str, dict]) -> None:
    grouped: dict[str, list[dict]] = {}
    for row in rows:
        grouped.setdefault(row["instance_id"], []).append(row)
    if set(grouped) != set(baselines):
        raise AssertionError(f"unexpected benchmark instances: {sorted(grouped)}")

    for instance_id, instance_rows in grouped.items():
        by_backend = {row["backend"]: row for row in instance_rows}
        if set(by_backend) != set(DIRECT_BACKENDS):
            raise AssertionError(f"{instance_id}: missing benchmark backends {sorted(by_backend)}")
        for backend, row in by_backend.items():
            if row["status"] != "ok":
                raise AssertionError(f"{instance_id} {backend}: status {row['status']}")
            if "amplitude_real" not in row or "amplitude_imag" not in row:
                raise AssertionError(f"{instance_id} {backend}: missing amplitude fields")

        hashes = {row["counts_hash"] for row in instance_rows}
        if len(hashes) != 1:
            raise AssertionError(f"{instance_id}: benchmark counts_hash mismatch {hashes}")

        expected = baselines[instance_id]["amplitude"]
        for backend, row in by_backend.items():
            observed = complex(row["amplitude_real"], row["amplitude_imag"])
            _assert_close(f"{instance_id} {backend} benchmark amplitude", observed, expected)

        fourier = by_backend["rankwidth:best:fourier"]
        stats = fourier["stats"]
        if stats.get("rankwidth_mode") != "fourier":
            raise AssertionError(f"{instance_id}: Fourier benchmark row missing rankwidth_mode")
        for key in ("rankwidth_dense_table_forecast", "rankwidth_dense_even_join_forecast"):
            if not isinstance(stats.get(key), int) or stats[key] <= 0:
                raise AssertionError(f"{instance_id}: missing positive stats[{key}]")
            if fourier.get(key) != stats[key]:
                raise AssertionError(f"{instance_id}: top-level {key} does not match stats")
        if fourier.get("rankwidth_fourier_join_events", 0) <= 0:
            raise AssertionError(f"{instance_id}: missing Fourier join trace events")


def run_tests(gen_tool: pathlib.Path, bench_tool: pathlib.Path, sop_solve: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = pathlib.Path(tmp)
        corpus_dir = tmp_dir / "rankwidth-corpus"
        qsops = _materialize_corpus(gen_tool, corpus_dir)
        baselines = _crosscheck_direct(sop_solve, qsops)
        rows = _run_local_bench(bench_tool, sop_solve, corpus_dir, tmp_dir / "bench.jsonl")
        _crosscheck_bench_rows(rows, baselines)


def main() -> int:
    if len(sys.argv) != 4:
        print(
            "usage: test_rankwidth_family_crosscheck.py "
            "<gen_rankwidth_family.py> <bench_sop_local.py> <sop-solve>",
            file=sys.stderr,
        )
        return 1
    gen_tool = pathlib.Path(sys.argv[1])
    bench_tool = pathlib.Path(sys.argv[2])
    sop_solve = pathlib.Path(sys.argv[3])
    for path in (gen_tool, bench_tool, sop_solve):
        if not path.exists():
            print(f"error: {path} not found", file=sys.stderr)
            return 1
    run_tests(gen_tool, bench_tool, sop_solve)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
