#!/usr/bin/env python3

import pathlib
import json
import subprocess
import sys


BACKENDS = ["branch", "treewidth", "rankwidth"]


def run(cmd: list[str], *, input_text: str | None = None) -> str:
    completed = subprocess.run(
        cmd,
        input=input_text,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"command failed: {cmd}\n{completed.stderr}")
    return completed.stdout


def import_qasm(qasm2sop: pathlib.Path, qasm: str, input_bits: str, output_bits: str) -> str:
    return run(
        [str(qasm2sop), "--input", input_bits, "--output", output_bits, "-"],
        input_text=qasm,
    )


def solve(sop_solve: pathlib.Path, qsop: str, backend: str, fmt: str) -> str:
    return run(
        [str(sop_solve), "--backend", backend, "--format", fmt, "-"],
        input_text=qsop,
    )


def qsop_nvars(qsop: str) -> int:
    for line in qsop.splitlines():
        parts = line.split()
        if len(parts) == 5 and parts[:2] == ["p", "qsop-sign"]:
            return int(parts[3])
    raise AssertionError(f"missing QSOP header:\n{qsop}")


STRING_STATS = {
    "backend",
    "solve_mode",
    "solve_mode_kernel",
    "treewidth_order",
    "rankwidth_mode",
    "rankwidth_decomposition",
}


def parse_stats(text: str) -> dict[str, int | str]:
    stats: dict[str, int | str] = {}
    for line in text.splitlines():
        if not line:
            continue
        key, value = line.split(": ", 1)
        stats[key] = value if key in STRING_STATS else int(value)
    return stats


def assert_backend_agreement(sop_solve: pathlib.Path, case: str, boundary: str, qsop: str) -> None:
    expected = solve(sop_solve, qsop, "branch", "residue-vector")
    for backend in BACKENDS:
        actual = solve(sop_solve, qsop, backend, "residue-vector")
        if actual != expected:
            raise AssertionError(
                f"{case} {boundary}: {backend} disagrees with branch\n"
                f"expected:\n{expected}\nactual:\n{actual}"
            )


def assert_stats_invariants(
    sop_solve: pathlib.Path,
    case: str,
    boundary: str,
    qsop: str,
) -> None:
    nvars = qsop_nvars(qsop)
    stats = {
        backend: parse_stats(solve(sop_solve, qsop, backend, "stats")) for backend in BACKENDS
    }

    branch_leaves = stats["branch"]["leaf_assignments"]
    if branch_leaves > 1 << nvars:
        raise AssertionError(
            f"{case} {boundary}: branch leaves {branch_leaves} exceed 2^{nvars}"
        )
    branch_nodes = stats["branch"]["search_nodes"]
    branch_cache_hits = stats["branch"]["cache_hits"]
    branch_cache_misses = stats["branch"]["cache_misses"]
    if branch_cache_hits + branch_cache_misses != branch_nodes:
        raise AssertionError(
            f"{case} {boundary}: branch cache hits + misses do not match search nodes"
        )

    treewidth_width = stats["treewidth"]["decomposition_width"]
    if treewidth_width > nvars:
        raise AssertionError(
            f"{case} {boundary}: treewidth width {treewidth_width} exceeds nvars {nvars}"
        )
    if stats["treewidth"].get("treewidth_order") != "min-fill":
        raise AssertionError(f"{case} {boundary}: unexpected treewidth order")


def load_cases(path: pathlib.Path) -> list[dict]:
    return json.loads(path.read_text())


def case_qasm(case: dict) -> str:
    return "\n".join(case["qasm_lines"]) + "\n"


def run_corpus(qasm2sop: pathlib.Path, sop_solve: pathlib.Path, manifest: pathlib.Path) -> None:
    for case_data in load_cases(manifest):
        case = case_data["name"]
        qasm = case_qasm(case_data)
        for input_bits, output_bits in case_data["boundaries"]:
            boundary = f"{input_bits}->{output_bits}"
            qsop = import_qasm(qasm2sop, qasm, input_bits, output_bits)
            assert_backend_agreement(sop_solve, case, boundary, qsop)
            assert_stats_invariants(
                sop_solve,
                case,
                boundary,
                qsop,
            )


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print("usage: test_qasm_solver_corpus.py QASM2SOP SOP_SOLVE [MANIFEST]", file=sys.stderr)
        return 2

    default_manifest = pathlib.Path(__file__).with_name("qasm_solver_corpus.json")
    manifest = pathlib.Path(sys.argv[3]) if len(sys.argv) == 4 else default_manifest
    run_corpus(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
