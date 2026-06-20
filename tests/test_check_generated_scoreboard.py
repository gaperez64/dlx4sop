#!/usr/bin/env python3
"""Smoke tests for tools/check_generated_scoreboard.py."""

import importlib.util
import json
import pathlib
import sys
import tempfile

TOOLS_DIR = pathlib.Path(__file__).resolve().parent.parent / "tools"


def _load_tool():
    path = TOOLS_DIR / "check_generated_scoreboard.py"
    spec = importlib.util.spec_from_file_location("check_generated_scoreboard", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["check_generated_scoreboard"] = module
    spec.loader.exec_module(module)
    return module


_VALID_JSON = {
    "generated_at": "2026-01-01T00:00:00",
    "tiers": ["0-32"],
    "solver_summary": [
        {
            "tier": "0-32",
            "config": "treewidth",
            "backend": "treewidth",
            "solved": 10,
            "attempted": 10,
        },
    ],
}

_VALID_SVG = '<svg xmlns="http://www.w3.org/2000/svg"><text>ok</text></svg>'


def _make_valid_root(tmp: pathlib.Path) -> pathlib.Path:
    """Create a minimal valid two-mode scoreboard directory tree under tmp."""
    for mode in ("sign", "labelled"):
        assets = tmp / "scoreboard-assets" / mode
        assets.mkdir(parents=True, exist_ok=True)
        (assets / "survival-feynmandd.svg").write_text(_VALID_SVG, encoding="utf-8")
        md = (
            f"# Scoreboard — {mode} QSOPs\n\n"
            f"![Survival curves — FeynmanDD](scoreboard-assets/{mode}/survival-feynmandd.svg)\n"
        )
        (tmp / f"scoreboard-{mode}.md").write_text(md, encoding="utf-8")
        (tmp / f"scoreboard-{mode}.json").write_text(json.dumps(_VALID_JSON), encoding="utf-8")
    (tmp / "scoreboard.md").write_text(
        "# Scoreboard\n\n"
        "- [Sign QSOP scoreboard](scoreboard-sign.md)\n"
        "- [Labelled QSOP scoreboard](scoreboard-labelled.md)\n",
        encoding="utf-8",
    )
    return tmp


def test_valid_tree_passes(tool):
    with tempfile.TemporaryDirectory() as td:
        root = _make_valid_root(pathlib.Path(td))
        rc = tool.main(["--repo-root", str(root)])
        if rc != 0:
            raise AssertionError(f"expected rc=0 for valid tree, got {rc}")


def test_missing_scoreboard_json(tool):
    with tempfile.TemporaryDirectory() as td:
        root = _make_valid_root(pathlib.Path(td))
        (root / "scoreboard-sign.json").unlink()
        errors = tool.check_scoreboard_json(root, "scoreboard-sign.json")
        if not errors:
            raise AssertionError("expected error for missing scoreboard-sign.json")


def test_invalid_json(tool):
    with tempfile.TemporaryDirectory() as td:
        root = _make_valid_root(pathlib.Path(td))
        (root / "scoreboard-sign.json").write_text("not json", encoding="utf-8")
        errors = tool.check_scoreboard_json(root, "scoreboard-sign.json")
        if not errors:
            raise AssertionError("expected error for invalid JSON")


def test_missing_required_key(tool):
    with tempfile.TemporaryDirectory() as td:
        root = _make_valid_root(pathlib.Path(td))
        data = json.loads((root / "scoreboard-sign.json").read_text())
        del data["solver_summary"]
        (root / "scoreboard-sign.json").write_text(json.dumps(data), encoding="utf-8")
        errors = tool.check_scoreboard_json(root, "scoreboard-sign.json")
        if not any("solver_summary" in e for e in errors):
            raise AssertionError("expected error about missing solver_summary")


def test_solved_exceeds_attempted(tool):
    with tempfile.TemporaryDirectory() as td:
        root = _make_valid_root(pathlib.Path(td))
        data = json.loads((root / "scoreboard-sign.json").read_text())
        data["solver_summary"][0]["solved"] = 99
        data["solver_summary"][0]["attempted"] = 10
        (root / "scoreboard-sign.json").write_text(json.dumps(data), encoding="utf-8")
        errors = tool.check_scoreboard_json(root, "scoreboard-sign.json")
        if not any("solved" in e and "attempted" in e for e in errors):
            raise AssertionError("expected error when solved > attempted")


def test_missing_referenced_svg(tool):
    with tempfile.TemporaryDirectory() as td:
        root = _make_valid_root(pathlib.Path(td))
        (root / "scoreboard-sign.md").write_text(
            "![X](scoreboard-assets/sign/nonexistent.svg)\n", encoding="utf-8"
        )
        errors = tool.check_mode_scoreboard_assets(root, "sign")
        if not any("nonexistent.svg" in e for e in errors):
            raise AssertionError("expected error for missing referenced SVG")


def test_non_svg_content(tool):
    with tempfile.TemporaryDirectory() as td:
        root = _make_valid_root(pathlib.Path(td))
        (root / "scoreboard-assets" / "sign" / "bad.svg").write_text(
            "not an svg file", encoding="utf-8"
        )
        errors = tool.check_all_svg_files(root)
        if not any("bad.svg" in e for e in errors):
            raise AssertionError("expected error for SVG without <svg tag")


def test_missing_index_links(tool):
    with tempfile.TemporaryDirectory() as td:
        root = _make_valid_root(pathlib.Path(td))
        (root / "scoreboard.md").write_text("# Scoreboard\n\nNo links here.\n", encoding="utf-8")
        errors = tool.check_index_links(root)
        if not errors:
            raise AssertionError("expected error when index lacks mode links")


def test_real_repo_artifacts(tool):
    """Validator must pass against the actual committed scoreboard artifacts."""
    repo_root = pathlib.Path(__file__).resolve().parent.parent
    rc = tool.main(["--repo-root", str(repo_root)])
    if rc != 0:
        raise AssertionError(
            f"committed scoreboard artifacts failed validation (rc={rc})"
        )


def main() -> int:
    tool = _load_tool()
    tests = [
        ("valid_tree_passes", test_valid_tree_passes),
        ("missing_scoreboard_json", test_missing_scoreboard_json),
        ("invalid_json", test_invalid_json),
        ("missing_required_key", test_missing_required_key),
        ("solved_exceeds_attempted", test_solved_exceeds_attempted),
        ("missing_referenced_svg", test_missing_referenced_svg),
        ("non_svg_content", test_non_svg_content),
        ("missing_index_links", test_missing_index_links),
        ("real_repo_artifacts", test_real_repo_artifacts),
    ]
    failed = []
    for name, fn in tests:
        try:
            fn(tool)
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
