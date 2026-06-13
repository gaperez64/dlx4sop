#!/usr/bin/env python3

import pathlib
import subprocess
import sys


def run_stats(
    exe: pathlib.Path,
    source_root: pathlib.Path,
    args: list[str],
    expected_name: str,
) -> None:
    qsop = source_root / "tests" / "golden" / "labelled_raw.qsop"
    expected = source_root / "tests" / "golden" / expected_name
    completed = subprocess.run(
        [str(exe), *args, str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"sop-stats failed\n{completed.stderr}")
    expected_text = expected.read_text()
    if completed.stdout != expected_text:
        raise AssertionError(
            f"stats output mismatch for {expected_name}\n"
            f"expected:\n{expected_text}\n"
            f"actual:\n{completed.stdout}\n"
        )


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_sop_stats.py SOP_STATS SOURCE_ROOT", file=sys.stderr)
        return 2

    exe = pathlib.Path(sys.argv[1])
    source_root = pathlib.Path(sys.argv[2])
    run_stats(exe, source_root, [], "stats_labelled.text")
    run_stats(exe, source_root, ["--json"], "stats_labelled.json")
    run_stats(exe, source_root, ["--format", "json"], "stats_labelled.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
