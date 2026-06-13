#!/usr/bin/env python3

import pathlib
import subprocess
import sys


def run_check(exe: pathlib.Path, source_root: pathlib.Path, name: str) -> None:
    raw = source_root / "tests" / "golden" / f"{name}_raw.qsop"
    expected = source_root / "tests" / "golden" / f"{name}_expected.qsop"
    completed = subprocess.run(
        [str(exe), str(raw)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"{name}: sop-check failed\n{completed.stderr}")
    expected_text = expected.read_text()
    if completed.stdout != expected_text:
        raise AssertionError(
            f"{name}: canonical output mismatch\n"
            f"expected:\n{expected_text}\n"
            f"actual:\n{completed.stdout}\n"
        )


def run_invalid_sign_header(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    invalid = source_root / "tests" / "golden" / "invalid_sign_coeff.qsop"
    completed = subprocess.run(
        [str(exe), str(invalid)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode == 0:
        raise AssertionError("invalid sign coefficient unexpectedly parsed")
    if "qsop-sign accepts only sign coefficient" not in completed.stderr:
        raise AssertionError(f"unexpected diagnostic:\n{completed.stderr}")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_sop_check.py SOP_CHECK SOURCE_ROOT", file=sys.stderr)
        return 2

    exe = pathlib.Path(sys.argv[1])
    source_root = pathlib.Path(sys.argv[2])
    run_check(exe, source_root, "sign")
    run_check(exe, source_root, "labelled")
    run_invalid_sign_header(exe, source_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
