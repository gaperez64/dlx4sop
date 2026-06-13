#!/usr/bin/env python3

import pathlib
import subprocess
import sys


BACKENDS = ["components", "brute-force", "branch"]


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
        if len(parts) == 5 and parts[:2] == ["p", "qsop"]:
            return int(parts[3])
    raise AssertionError(f"missing QSOP header:\n{qsop}")


def parse_stats(text: str) -> dict[str, int | str]:
    stats: dict[str, int | str] = {}
    for line in text.splitlines():
        if not line:
            continue
        key, value = line.split(": ", 1)
        stats[key] = value if key == "backend" else int(value)
    return stats


def assert_backend_agreement(sop_solve: pathlib.Path, case: str, boundary: str, qsop: str) -> None:
    expected = solve(sop_solve, qsop, "components", "residue-vector")
    for backend in BACKENDS:
        actual = solve(sop_solve, qsop, backend, "residue-vector")
        if actual != expected:
            raise AssertionError(
                f"{case} {boundary}: {backend} disagrees with components\n"
                f"expected:\n{expected}\nactual:\n{actual}"
            )


def assert_stats_invariants(sop_solve: pathlib.Path, case: str, boundary: str, qsop: str) -> None:
    nvars = qsop_nvars(qsop)
    stats = {
        backend: parse_stats(solve(sop_solve, qsop, backend, "stats")) for backend in BACKENDS
    }

    brute_leaves = stats["brute-force"]["leaf_assignments"]
    if brute_leaves != 1 << nvars:
        raise AssertionError(
            f"{case} {boundary}: brute-force leaves {brute_leaves} do not match 2^{nvars}"
        )

    branch_leaves = stats["branch"]["leaf_assignments"]
    if branch_leaves > brute_leaves:
        raise AssertionError(
            f"{case} {boundary}: branch leaves {branch_leaves} exceed brute {brute_leaves}"
        )

    component_leaves = stats["components"]["leaf_assignments"]
    if component_leaves > brute_leaves:
        raise AssertionError(
            f"{case} {boundary}: components leaves {component_leaves} exceed brute {brute_leaves}"
        )

    components = stats["components"]["components"]
    cache_hits = stats["components"]["cache_hits"]
    cache_misses = stats["components"]["cache_misses"]
    if cache_hits + cache_misses != components:
        raise AssertionError(
            f"{case} {boundary}: cache hits + misses do not match component count"
        )

    if case == "repeated_components" and cache_hits < 1:
        raise AssertionError(f"{case} {boundary}: expected a component cache hit")


def run_corpus(qasm2sop: pathlib.Path, sop_solve: pathlib.Path) -> None:
    cases = [
        (
            "qymera_ghz",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[4];
            h q[0];
            cx q[0], q[1];
            cx q[1], q[2];
            cx q[2], q[3];
            """,
            [("0000", "0000"), ("0000", "1111"), ("0000", "0011")],
        ),
        (
            "qymera_uniform_superposition",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[4];
            h q;
            """,
            [("0000", "0000"), ("0000", "1111"), ("1010", "0101")],
        ),
        (
            "repeated_components",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[2];
            h q;
            t q;
            h q;
            """,
            [("00", "00"), ("11", "11")],
        ),
        (
            "entangled_axis_chain",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[3];
            h q;
            rx(pi/4) q[0];
            ry(-pi/2) q[1];
            crz(pi/4) q[0], q[2];
            cx q[0], q[1];
            cy q[1], q[2];
            """,
            [("000", "000"), ("001", "111"), ("101", "010")],
        ),
        (
            "register_pair_mix",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg a[2];
            qreg b[2];
            h a;
            sx b;
            cp(pi/4) a, b;
            cx a, b;
            sxdg b;
            """,
            [("0000", "0000"), ("1010", "1111")],
        ),
    ]

    for case, qasm, boundaries in cases:
        for input_bits, output_bits in boundaries:
            boundary = f"{input_bits}->{output_bits}"
            qsop = import_qasm(qasm2sop, qasm, input_bits, output_bits)
            assert_backend_agreement(sop_solve, case, boundary, qsop)
            assert_stats_invariants(sop_solve, case, boundary, qsop)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_qasm_solver_corpus.py QASM2SOP SOP_SOLVE", file=sys.stderr)
        return 2

    run_corpus(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
