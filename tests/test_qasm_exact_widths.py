#!/usr/bin/env python3
"""Exact treewidth/rankwidth support-graph width smoke test.

Imports the first case+boundary from the core QASM solver corpus, pipes it
through sop-stats --exact-widths, and asserts the exact-width fields come
back sane.

Usage: python3 tests/test_qasm_exact_widths.py QASM2SOP SOP_STATS
"""

import json
import pathlib
import subprocess
import sys


def run(cmd: list[str], *, input_text: str | None = None) -> str:
    completed = subprocess.run(
        cmd, input=input_text, check=False,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"command failed: {cmd}\n{completed.stderr}")
    return completed.stdout


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_qasm_exact_widths.py QASM2SOP SOP_STATS", file=sys.stderr)
        return 2

    qasm2sop = pathlib.Path(sys.argv[1])
    sop_stats = pathlib.Path(sys.argv[2])

    manifest = pathlib.Path(__file__).with_name("qasm_solver_corpus.json")
    case = json.loads(manifest.read_text())[0]
    qasm = "\n".join(case["qasm_lines"]) + "\n"
    input_bits, output_bits = case["boundaries"][0]

    qsop = run(
        [str(qasm2sop), "--input", input_bits, "--output", output_bits, "-"],
        input_text=qasm,
    )
    stats_text = run(
        [str(sop_stats), "--format", "json", "--exact-widths",
         "--exact-width-max-vars", "12", "-"],
        input_text=qsop,
    )
    stats = json.loads(stats_text)

    if not stats.get("exact_widths_available"):
        raise AssertionError(f"expected exact widths to be available:\n{stats_text}")
    for key in ("exact_treewidth", "exact_rankwidth", "min_fill_width", "prefix_cut_rank"):
        value = stats.get(key)
        if not isinstance(value, int) or value < 0:
            raise AssertionError(f"{key}: expected a non-negative int, got {value!r}\n{stats_text}")
    # Oum-Seymour: rankwidth(G) <= treewidth(G) + 1.
    if stats["exact_rankwidth"] > stats["exact_treewidth"] + 1:
        raise AssertionError(
            f"exact_rankwidth {stats['exact_rankwidth']} unexpectedly exceeds "
            f"exact_treewidth+1 {stats['exact_treewidth'] + 1}\n{stats_text}"
        )
    print(f"exact_treewidth={stats['exact_treewidth']} exact_rankwidth={stats['exact_rankwidth']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
