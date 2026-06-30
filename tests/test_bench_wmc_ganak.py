#!/usr/bin/env python3
"""Smoke tests for bench_wmc_ganak metadata parsing."""

import importlib.util
import pathlib
import sys


def load_tool(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("bench_wmc_ganak", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_bench_wmc_ganak.py BENCH_WMC_GANAK", file=sys.stderr)
        return 2
    tool = load_tool(pathlib.Path(sys.argv[1]))
    cnf_text = """p cnf 12 18
c sop2wmc encoding=amp-block r=8 nvars=8 nedges=8 format=qsop-sign norm_h=0
c block count=2 covered_edges=8 residual_edges=0 nvars_after=8
c block sign-parity index=0 a_size=2 b_size=3
c block sign-parity index=1 a_size=4 b_size=2
c amplitude_factor 1+0i
"""
    got = tool.parse_wmc_metadata(cnf_text)
    expected = {
        "wmc_export_encoding": "amp-block",
        "wmc_original_nvars": 8,
        "wmc_original_edges": 8,
        "wmc_norm_h": 0,
        "wmc_block_count": 2,
        "wmc_block_edges": 8,
        "wmc_residual_edges": 0,
        "wmc_active_vars": 8,
        "wmc_block_max_a_size": 4,
        "wmc_block_max_b_size": 3,
    }
    for key, value in expected.items():
        if got.get(key) != value:
            raise AssertionError(f"{key}: got {got.get(key)!r}, expected {value!r}")

    fallback = tool.parse_wmc_metadata(
        "c sop2wmc encoding=amp-soft r=8 nvars=2 nedges=1 format=qsop-sign norm_h=4\n"
    )
    if fallback.get("wmc_export_encoding") != "amp-soft":
        raise AssertionError(f"bad fallback metadata: {fallback}")
    if fallback.get("wmc_block_count") != 0 or fallback.get("wmc_block_edges") != 0:
        raise AssertionError(f"fallback should have zero block counters: {fallback}")

    zero_residual = tool.parse_wmc_metadata(
        "c sop2wmc encoding=amp-and r=16 nvars=40 nedges=37 format=qsop-sign norm_h=60\n"
        "c preprocess nvars_after=0 pairs_after=0\n"
    )
    if not tool.is_zero_residual_wmc(zero_residual):
        raise AssertionError(f"zero-residual WMC row not detected: {zero_residual}")
    naturally_empty = tool.parse_wmc_metadata(
        "c sop2wmc encoding=amp-and r=8 nvars=0 nedges=0 format=qsop-sign norm_h=4\n"
    )
    if not tool.is_zero_residual_wmc(naturally_empty):
        raise AssertionError(f"empty WMC row not detected: {naturally_empty}")
    if tool.is_zero_residual_wmc(expected):
        raise AssertionError(f"nonzero active WMC row misdetected: {expected}")
    if tool.normalize_amplitude(16 + 0j, 8) != 1 + 0j:
        raise AssertionError("WMC amplitude normalization mismatch")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
