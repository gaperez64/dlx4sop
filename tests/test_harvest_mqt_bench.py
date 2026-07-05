#!/usr/bin/env python3
"""Smoke tests for scripts/harvest_mqt_bench.py."""

import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile

TOOLS_DIR = pathlib.Path(__file__).resolve().parent.parent / "scripts"


def _load_tool():
    path = TOOLS_DIR / "harvest_mqt_bench.py"
    spec = importlib.util.spec_from_file_location("harvest_mqt_bench", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["harvest_mqt_bench"] = module
    spec.loader.exec_module(module)
    return module


def test_tier_bounds_coverage(tool, tmp):
    bounds = tool.TIER_BOUNDS
    for expected in ("0-32", "33-64", "65-128", "129-256", "257-512-sample"):
        if expected not in bounds:
            raise AssertionError(f"missing tier {expected!r} in TIER_BOUNDS")
    lo, hi = bounds["0-32"]
    if lo != 0 or hi != 32:
        raise AssertionError(f"0-32 bounds wrong: {lo}-{hi}")
    lo, hi = bounds["33-64"]
    if lo != 33 or hi != 64:
        raise AssertionError(f"33-64 bounds wrong: {lo}-{hi}")


def test_tier_for_nvars(tool, tmp):
    cases = [
        (0, "0-32"), (32, "0-32"),
        (33, "33-64"), (64, "33-64"),
        (65, "65-128"), (128, "65-128"),
        (256, "129-256"),
        (512, "257-512-sample"),
        (600, None),
    ]
    for nvars, expected in cases:
        got = tool._tier_for_nvars(nvars)
        if got != expected:
            raise AssertionError(f"_tier_for_nvars({nvars}) = {got!r}, expected {expected!r}")


def test_zero_and_one_boundaries(tool, tmp):
    if tool._zero_boundary(4) != "0000":
        raise AssertionError("zero_boundary(4) != '0000'")
    if tool._one_boundary(3) != "111":
        raise AssertionError("one_boundary(3) != '111'")
    if len(tool._zero_boundary(8)) != 8:
        raise AssertionError("zero_boundary(8) wrong length")
    if len(tool._one_boundary(8)) != 8:
        raise AssertionError("one_boundary(8) wrong length")


def test_sha256(tool, tmp):
    h = tool._sha256(b"hello")
    if len(h) != 64:
        raise AssertionError(f"sha256 wrong length: {len(h)}")
    if h != tool._sha256(b"hello"):
        raise AssertionError("sha256 not deterministic")
    if h == tool._sha256(b"world"):
        raise AssertionError("sha256 collision")


def test_probe_import_missing_binary(tool, tmp):
    result = tool._probe_import(b"OPENQASM 2.0;", tmp / "nonexistent", timeout=5.0)
    if result["status"] != "error":
        raise AssertionError(f"expected error for missing binary, got {result['status']!r}")


def test_main_no_mqt_exits_cleanly(tool, tmp):
    out_dir = tmp / "mqt-manifests"
    result = subprocess.run(
        [sys.executable, str(TOOLS_DIR / "harvest_mqt_bench.py"),
         "--output-dir", str(out_dir),
         "--target-tier", "33-64",
         "--size", "4",
         "--opt-level", "1"],
        capture_output=True,
        timeout=15,
    )
    if result.returncode not in (0, 1, 2):
        raise AssertionError(f"unexpected exit code: {result.returncode}")


def test_harvest_summary_structure(tool, tmp):
    out_dir = tmp / "manifests"
    out_dir.mkdir()
    for tier in ("0-32", "33-64", "65-128", "129-256", "257-512-sample"):
        (out_dir / f"tier-{tier}.json").write_text("[]", encoding="utf-8")
    (out_dir / "unsupported.jsonl").write_text("", encoding="utf-8")
    summary = {
        "seed": 1234,
        "target_tiers": ["33-64"],
        "sizes": [4],
        "opt_levels": [1],
        "total_candidates": 0,
        "unsupported_count": 0,
        "per_tier": {"33-64": 0},
    }
    (out_dir / "harvest-summary.json").write_text(json.dumps(summary), encoding="utf-8")
    data = json.loads((out_dir / "harvest-summary.json").read_text())
    if data["seed"] != 1234:
        raise AssertionError("seed mismatch")
    if "per_tier" not in data:
        raise AssertionError("missing per_tier")
    if "total_candidates" not in data:
        raise AssertionError("missing total_candidates")


def test_source_url_constant(tool, tmp):
    if "munich-quantum-toolkit" not in tool.SOURCE_URL:
        raise AssertionError(f"SOURCE_URL missing expected domain: {tool.SOURCE_URL!r}")


def main() -> int:
    tool = _load_tool()
    tests = [
        ("tier_bounds_coverage", test_tier_bounds_coverage),
        ("tier_for_nvars", test_tier_for_nvars),
        ("zero_and_one_boundaries", test_zero_and_one_boundaries),
        ("sha256", test_sha256),
        ("probe_import_missing_binary", test_probe_import_missing_binary),
        ("main_no_mqt_exits_cleanly", test_main_no_mqt_exits_cleanly),
        ("harvest_summary_structure", test_harvest_summary_structure),
        ("source_url_constant", test_source_url_constant),
    ]
    failed = []
    for name, fn in tests:
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            try:
                fn(tool, tmp)
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
