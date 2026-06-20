#!/usr/bin/env python3
"""Tests for record_qsop_mode, write_mode_scoreboard, and write_index."""

import importlib.util
import io
import pathlib
import sys

TOOLS_DIR = pathlib.Path(__file__).resolve().parent.parent / "tools"


def _load_render():
    sys.path.insert(0, str(TOOLS_DIR))
    path = TOOLS_DIR / "render_scoreboard.py"
    spec = importlib.util.spec_from_file_location("render_scoreboard", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["render_scoreboard"] = mod
    spec.loader.exec_module(mod)
    return mod


def _load_refresh(render_mod):
    sys.path.insert(0, str(TOOLS_DIR))
    path = TOOLS_DIR / "refresh_scoreboard.py"
    spec = importlib.util.spec_from_file_location("refresh_scoreboard", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["refresh_scoreboard"] = mod
    spec.loader.exec_module(mod)
    return mod


# --- record_qsop_mode ---

def test_explicit_qsop_mode_field(render_mod):
    m = render_mod.record_qsop_mode
    assert m({"qsop_mode": "sign"}) == "sign"
    assert m({"qsop_mode": "labelled"}) == "labelled"


def test_derive_from_input_output(render_mod):
    m = render_mod.record_qsop_mode
    assert m({"input": "01", "output": "10"}) == "labelled"
    assert m({"input": "00", "output": "00"}) == "sign"


def test_legacy_mode_field(render_mod):
    m = render_mod.record_qsop_mode
    assert m({"mode": "sign"}) == "sign"


def test_unknown_when_no_fields(render_mod):
    m = render_mod.record_qsop_mode
    assert m({}) == "unknown"


# --- write_mode_scoreboard smoke ---

_SIGN_RECORD = {
    "backend": "treewidth",
    "treewidth_order": "min-fill-max-degree",
    "source": "FeynmanDD",
    "source_url": "https://example.invalid/feynman",
    "source_relative_path": "bell.qasm",
    "case": "bell",
    "input": "0",
    "output": "0",
    "qsop_mode": "sign",
    "solve_elapsed_ns": 1_000,
    "status": "ok",
    "stats": {},
}

_LABELLED_RECORD = {
    **_SIGN_RECORD,
    "input": "0",
    "output": "1",
    "qsop_mode": "labelled",
}


def test_write_mode_scoreboard_sign(refresh_mod):
    buf = io.StringIO()
    refresh_mod.write_mode_scoreboard(
        [("0-32", [_SIGN_RECORD])],
        [],
        mode="sign",
        assets_subdir="scoreboard-assets/sign",
        file=buf,
    )
    out = buf.getvalue()
    expected_headings = [
        "# Scoreboard — sign QSOPs",
        "## Benchmarks",
        "## Survival Curves",
        "## Solver Time by Tier",
        "## Speedup vs Treewidth Baseline",
        "## Branch Dispatch",
        "## WMC Solve Time Breakdown",
        "## Internal Solver Configurations",
        "## Competitor Comparisons",
        "## Current Takeaway",
    ]
    for heading in expected_headings:
        if heading not in out:
            raise AssertionError(f"missing heading {heading!r} in write_mode_scoreboard output")
    if "scoreboard-assets/sign/survival-feynmandd.svg" not in out:
        raise AssertionError("expected sign assets subdir in SVG paths")
    if "labelled" in out.lower() and "scoreboard-assets/labelled" in out:
        raise AssertionError("labelled assets path leaked into sign scoreboard")


def test_write_mode_scoreboard_labelled(refresh_mod):
    buf = io.StringIO()
    refresh_mod.write_mode_scoreboard(
        [("0-32", [_LABELLED_RECORD])],
        [],
        mode="labelled",
        assets_subdir="scoreboard-assets/labelled",
        file=buf,
    )
    out = buf.getvalue()
    if "# Scoreboard — labelled QSOPs" not in out:
        raise AssertionError("expected labelled scoreboard title")
    if "scoreboard-assets/labelled/survival-feynmandd.svg" not in out:
        raise AssertionError("expected labelled assets subdir in SVG paths")


def test_write_mode_scoreboard_no_details_link(refresh_mod):
    buf = io.StringIO()
    refresh_mod.write_mode_scoreboard(
        [("0-32", [_SIGN_RECORD])],
        [],
        mode="sign",
        assets_subdir="scoreboard-assets/sign",
        file=buf,
    )
    out = buf.getvalue()
    if "scoreboard-details.md" in out:
        raise AssertionError("scoreboard-details.md link must not appear in mode scoreboard")


def test_write_index(refresh_mod):
    buf = io.StringIO()
    refresh_mod.write_index(
        [("0-32", [_SIGN_RECORD, _LABELLED_RECORD])],
        buf,
    )
    out = buf.getvalue()
    if "# Scoreboard" not in out:
        raise AssertionError("expected '# Scoreboard' in index")
    if "scoreboard-sign.md" not in out:
        raise AssertionError("expected link to scoreboard-sign.md")
    if "scoreboard-labelled.md" not in out:
        raise AssertionError("expected link to scoreboard-labelled.md")
    if "| Source | Total solved | Sign | Labelled |" not in out:
        raise AssertionError("expected Source × {Sign, Labelled} table in index")
    if "scoreboard-assets" in out:
        raise AssertionError("index must not contain SVG paths")


def main() -> int:
    render_mod = _load_render()
    refresh_mod = _load_refresh(render_mod)

    tests = [
        ("explicit_qsop_mode_field", lambda: test_explicit_qsop_mode_field(render_mod)),
        ("derive_from_input_output", lambda: test_derive_from_input_output(render_mod)),
        ("legacy_mode_field", lambda: test_legacy_mode_field(render_mod)),
        ("unknown_when_no_fields", lambda: test_unknown_when_no_fields(render_mod)),
        ("write_mode_scoreboard_sign", lambda: test_write_mode_scoreboard_sign(refresh_mod)),
        ("write_mode_scoreboard_labelled", lambda: test_write_mode_scoreboard_labelled(refresh_mod)),
        ("write_mode_scoreboard_no_details_link", lambda: test_write_mode_scoreboard_no_details_link(refresh_mod)),
        ("write_index", lambda: test_write_index(refresh_mod)),
    ]
    failed = []
    for name, fn in tests:
        try:
            fn()
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
