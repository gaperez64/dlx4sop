#!/usr/bin/env python3

import pathlib
import subprocess
import sys


def run_cli_paths(exe: pathlib.Path) -> None:
    help_result = subprocess.run(
        [str(exe), "--help"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if help_result.returncode != 0 or "qgraph" not in help_result.stdout:
        raise AssertionError(f"unexpected --help result:\n{help_result.stdout}\n{help_result.stderr}")

    invalid = subprocess.run(
        [str(exe), "-"],
        input="{",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if invalid.returncode == 0 or "invalid qgraph JSON" not in invalid.stderr:
        raise AssertionError(f"unexpected invalid JSON result:\n{invalid.stderr}")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_qgraph2qasm.py QGRAPH2QASM", file=sys.stderr)
        return 2
    run_cli_paths(pathlib.Path(sys.argv[1]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
