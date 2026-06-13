#!/usr/bin/env python3

import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_scan_mqt_bench.py SCAN_MQT_BENCH QASM2SOP", file=sys.stderr)
        return 2

    scanner = pathlib.Path(sys.argv[1])
    qasm2sop = pathlib.Path(sys.argv[2])

    help_result = subprocess.run(
        [str(scanner), "--help"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if help_result.returncode != 0 or "MQT Bench" not in help_result.stdout:
        raise AssertionError(f"unexpected --help result:\n{help_result.stdout}\n{help_result.stderr}")

    missing_source = subprocess.run(
        [str(scanner), str(qasm2sop), "--mqt-source", "/tmp/dlx4sop-missing-mqt-bench", "--limit", "1"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if missing_source.returncode == 0 or "MQT Bench source path does not exist" not in missing_source.stderr:
        raise AssertionError(f"unexpected missing-source result:\n{missing_source.stderr}")

    bad_sizes = subprocess.run(
        [str(scanner), str(qasm2sop), "--sizes", "0"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if bad_sizes.returncode == 0 or "sizes must be positive" not in bad_sizes.stderr:
        raise AssertionError(f"unexpected bad-sizes result:\n{bad_sizes.stderr}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
