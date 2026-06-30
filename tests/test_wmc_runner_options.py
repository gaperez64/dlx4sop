#!/usr/bin/env python3
"""Smoke tests for WMC tuning defaults in benchmark runners."""

import importlib.util
import pathlib
import sys


def load_module(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_wmc_runner_options.py RUN_CORPUS_BENCHMARKS BENCH", file=sys.stderr)
        return 2

    run_corpus = load_module("run_corpus_benchmarks", pathlib.Path(sys.argv[1]))
    bench = load_module("bench", pathlib.Path(sys.argv[2]))

    args = run_corpus.parse_args(["--skip-scoreboard"])
    if args.wmc_preprocess != "peel2-safe":
        raise AssertionError(f"unexpected WMC preprocess default: {args.wmc_preprocess!r}")
    if args.cgroup_memory_limit_mib != 2048:
        raise AssertionError(f"unexpected cgroup memory cap default: {args.cgroup_memory_limit_mib!r}")
    if args.memory_limit_mib is not None:
        raise AssertionError(f"address-space cap should be opt-in, got {args.memory_limit_mib!r}")
    if args.solver_memory_limit_mib is not None:
        raise AssertionError(
            f"direct solver cap should be opt-in, got {args.solver_memory_limit_mib!r}"
        )
    if args.ganak_memory_limit_mib is not None:
        raise AssertionError(
            f"Ganak cap should be opt-in, got {args.ganak_memory_limit_mib!r}"
        )
    if args.wmc_qasm2sop_timeout != 30.0:
        raise AssertionError(f"unexpected WMC qasm2sop timeout: {args.wmc_qasm2sop_timeout!r}")
    if args.wmc_block_min_side != 2 or args.wmc_block_min_savings != 1:
        raise AssertionError(
            f"unexpected amp-block defaults: {args.wmc_block_min_side}, "
            f"{args.wmc_block_min_savings}"
        )
    common = run_corpus.wmc_sop2wmc_extra_args(args)
    if common != ["--wmc-preprocess", "peel2-safe"]:
        raise AssertionError(f"bad common WMC args: {common}")
    block = run_corpus.wmc_sop2wmc_extra_args(args, block=True)
    expected_block = [
        "--wmc-preprocess", "peel2-safe",
        "--wmc-block-min-side", "2",
        "--wmc-block-min-savings", "1",
    ]
    if block != expected_block:
        raise AssertionError(f"bad block WMC args: {block}")

    parser = bench.build_parser()
    ganak_args = parser.parse_args(["ganak"])
    cmd = []
    bench.append_wmc_tuning_args(cmd, ganak_args, block=True)
    if cmd != expected_block:
        raise AssertionError(f"bad bench.py WMC args: {cmd}")

    none_args = run_corpus.parse_args(["--wmc-preprocess", "none", "--wmc-block-min-side", "4"])
    none_block = run_corpus.wmc_sop2wmc_extra_args(none_args, block=True)
    expected_none = [
        "--wmc-preprocess", "none",
        "--wmc-block-min-side", "4",
        "--wmc-block-min-savings", "1",
    ]
    if none_block != expected_none:
        raise AssertionError(f"bad explicit-none WMC args: {none_block}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
