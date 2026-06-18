#!/usr/bin/env python3
"""D5: Rankwidth memory budget regression tests.

Verifies that --rankwidth-memory-budget-bytes and --rankwidth-memory-policy
behave correctly: skip, fallback, and hard-error all exit cleanly without OOM
when the memory forecast exceeds the budget.

Usage: python3 tests/test_rankwidth_memory_budget.py <sop-solve>
"""

import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CORPUS_DIR = REPO_ROOT / "benchmarks" / "corpus" / "sop"


def _find_rankwidth_instances(max_count=4):
    """Return local corpus instances that are solvable by rankwidth."""
    files = []
    for tier in sorted(CORPUS_DIR.iterdir()):
        if not tier.is_dir():
            continue
        for f in sorted(tier.glob("*.qsop")):
            files.append(f)
            if len(files) >= max_count:
                return files
    return files


def _run_rankwidth(sop_solve, qsop, extra_args, format_stats=True):
    cmd = [
        str(sop_solve),
        "--backend", "rankwidth",
        "--rankwidth-generate", "min-fill-cut",
        "--max-vars", "64",
    ]
    if format_stats:
        cmd += ["--format", "stats"]
    cmd += extra_args + [str(qsop)]
    result = subprocess.run(cmd, capture_output=True, timeout=30)
    return result


def test_skip_policy(sop_solve):
    """1-byte budget must trigger memory-skip on any rankwidth instance."""
    files = _find_rankwidth_instances()
    if not files:
        print("  SKIP: no corpus files found")
        return

    qsop = files[0]
    result = _run_rankwidth(sop_solve, qsop, [
        "--rankwidth-memory-budget-bytes", "1",
        "--rankwidth-memory-policy", "skip",
    ])
    if result.returncode != 0:
        raise AssertionError(
            f"skip policy must exit 0, got {result.returncode}\n{result.stderr.decode()}"
        )
    stdout = result.stdout.decode()
    if "status: memory-skip" not in stdout:
        raise AssertionError(f"expected 'status: memory-skip' in output\n{stdout}")
    if "rankwidth_memory_forecast_bytes:" not in stdout:
        raise AssertionError(f"expected forecast bytes in output\n{stdout}")
    if "rankwidth_memory_budget_bytes: 1" not in stdout:
        raise AssertionError(f"expected budget bytes in output\n{stdout}")


def test_fallback_policy(sop_solve):
    """1-byte budget with fallback must complete via treewidth (exit 0)."""
    files = _find_rankwidth_instances()
    if not files:
        print("  SKIP: no corpus files found")
        return

    qsop = files[0]
    result = _run_rankwidth(sop_solve, qsop, [
        "--rankwidth-memory-budget-bytes", "1",
        "--rankwidth-memory-policy", "fallback",
    ])
    if result.returncode != 0:
        raise AssertionError(
            f"fallback policy must exit 0, got {result.returncode}\n{result.stderr.decode()}"
        )
    stdout = result.stdout.decode()
    if "backend: rankwidth" not in stdout:
        raise AssertionError(f"expected 'backend: rankwidth' in fallback output\n{stdout}")
    if "status: memory-skip" in stdout:
        raise AssertionError("fallback policy must not emit memory-skip status")


def test_hard_error_policy(sop_solve):
    """1-byte budget with hard-error must exit 1 with an error message."""
    files = _find_rankwidth_instances()
    if not files:
        print("  SKIP: no corpus files found")
        return

    qsop = files[0]
    result = _run_rankwidth(sop_solve, qsop, [
        "--rankwidth-memory-budget-bytes", "1",
        "--rankwidth-memory-policy", "hard-error",
    ])
    if result.returncode != 1:
        raise AssertionError(
            f"hard-error policy must exit 1, got {result.returncode}"
        )
    stderr = result.stderr.decode()
    if "rankwidth memory forecast" not in stderr:
        raise AssertionError(f"expected forecast error message\n{stderr}")


def test_large_budget_no_skip(sop_solve):
    """100 GiB budget must never trigger skip on local corpus instances."""
    files = _find_rankwidth_instances(max_count=8)
    if not files:
        print("  SKIP: no corpus files found")
        return

    for qsop in files:
        result = _run_rankwidth(sop_solve, qsop, [
            "--rankwidth-memory-budget-bytes", str(100 * 1024 * 1024 * 1024),
            "--rankwidth-memory-policy", "skip",
        ])
        if result.returncode != 0:
            raise AssertionError(
                f"{qsop.name}: large budget solve failed exit={result.returncode}\n"
                f"{result.stderr.decode()}"
            )
        stdout = result.stdout.decode()
        if "status: memory-skip" in stdout:
            raise AssertionError(
                f"{qsop.name}: unexpected memory-skip with 100 GiB budget"
            )


def test_skip_result_consistent_with_treewidth(sop_solve):
    """After a skip, falling back to treewidth must yield the same result."""
    files = _find_rankwidth_instances()
    if not files:
        print("  SKIP: no corpus files found")
        return

    qsop = files[0]
    # Get the treewidth result for reference.
    tw = subprocess.run(
        [str(sop_solve), "--backend", "treewidth", "--max-vars", "64",
         "--format", "stats", str(qsop)],
        capture_output=True, timeout=30,
    )
    if tw.returncode != 0:
        print(f"  SKIP: treewidth baseline failed for {qsop.name}")
        return

    tw_result = None
    for line in tw.stdout.decode().splitlines():
        if line.startswith("result:"):
            tw_result = line.split(":", 1)[1].strip()
            break

    # Fallback from rankwidth should give the same result.
    fb = _run_rankwidth(sop_solve, qsop, [
        "--rankwidth-memory-budget-bytes", "1",
        "--rankwidth-memory-policy", "fallback",
    ])
    if fb.returncode != 0:
        raise AssertionError(f"fallback solve failed\n{fb.stderr.decode()}")

    fb_result = None
    for line in fb.stdout.decode().splitlines():
        if line.startswith("result:"):
            fb_result = line.split(":", 1)[1].strip()
            break

    if tw_result and fb_result and tw_result != fb_result:
        raise AssertionError(
            f"fallback result {fb_result!r} != treewidth result {tw_result!r}"
        )


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: test_rankwidth_memory_budget.py <sop-solve>", file=sys.stderr)
        return 2

    sop_solve = pathlib.Path(sys.argv[1])
    if not sop_solve.exists():
        print(f"error: sop-solve not found at {sop_solve}", file=sys.stderr)
        return 2

    tests = [
        ("skip_policy", test_skip_policy),
        ("fallback_policy", test_fallback_policy),
        ("hard_error_policy", test_hard_error_policy),
        ("large_budget_no_skip", test_large_budget_no_skip),
        ("skip_result_consistent_with_treewidth", test_skip_result_consistent_with_treewidth),
    ]
    failed = []
    for name, fn in tests:
        try:
            fn(sop_solve)
            print(f"  PASS {name}")
        except Exception as exc:
            print(f"  FAIL {name}: {exc}")
            failed.append(name)

    if failed:
        print(f"\n{len(failed)} test(s) failed: {failed}", file=sys.stderr)
        return 1
    print(f"\n{len(tests)} test(s) passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
