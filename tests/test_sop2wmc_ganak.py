#!/usr/bin/env python3
"""Optional smoke test: cross-check sop2wmc residue counts against Ganak.

Gated behind the `ganak_tests` meson option. Skips (exit 77) when no `ganak`
binary is on PATH so the gate can be enabled without forcing an install.
"""

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

SKIP = 77


def run(cmd, **kwargs):
    return subprocess.run(
        cmd, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kwargs
    )


def reference_counts(sop_solve: pathlib.Path, qsop: pathlib.Path):
    result = run([str(sop_solve), "--format", "residue-vector", str(qsop)])
    if result.returncode != 0:
        raise AssertionError(f"sop-solve failed on {qsop}:\n{result.stderr}")
    for line in result.stdout.splitlines():
        if line.startswith("counts"):
            return [int(tok) for tok in line.split()[1:]]
    raise AssertionError(f"no counts line in sop-solve output:\n{result.stdout}")


def parse_ganak_count(text: str) -> int:
    """Extract the model count from Ganak's output across known formats."""
    patterns = [
        r"^c s exact arb int (\d+)",
        r"^c s exact double int (\d+)",
        r"^s mc (\d+)",
        r"^s (\d+)$",
    ]
    for line in text.splitlines():
        for pattern in patterns:
            match = re.match(pattern, line.strip())
            if match:
                return int(match.group(1))
    raise AssertionError(f"could not parse model count from ganak output:\n{text}")


def ganak_residue_counts(sop2wmc, ganak, qsop, r):
    counts = []
    with tempfile.TemporaryDirectory() as tmp:
        for k in range(r):
            cnf = pathlib.Path(tmp) / f"residue_{k}.cnf"
            exported = run([str(sop2wmc), "--residue", str(k), "-o", str(cnf), str(qsop)])
            if exported.returncode != 0:
                raise AssertionError(f"sop2wmc --residue {k} failed:\n{exported.stderr}")
            counted = run([str(ganak), str(cnf)])
            counts.append(parse_ganak_count(counted.stdout))
    return counts


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: test_sop2wmc_ganak.py SOP2WMC SOP_SOLVE SOURCE_ROOT", file=sys.stderr)
        return 2
    ganak = shutil.which("ganak")
    if ganak is None:
        print("ganak not found on PATH; skipping", file=sys.stderr)
        return SKIP

    sop2wmc = pathlib.Path(sys.argv[1])
    sop_solve = pathlib.Path(sys.argv[2])
    source_root = pathlib.Path(sys.argv[3])

    for name in ("solve_sign_path.qsop", "solve_signed_edge.qsop"):
        qsop = source_root / "tests" / "golden" / name
        expected = reference_counts(sop_solve, qsop)
        actual = ganak_residue_counts(sop2wmc, pathlib.Path(ganak), qsop, len(expected))
        if actual != expected:
            raise AssertionError(f"{name}: ganak counts {actual} != sop-solve {expected}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
