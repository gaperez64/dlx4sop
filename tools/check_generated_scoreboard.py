#!/usr/bin/env python3
"""Validate committed scoreboard artifacts for self-consistency.

Checks:
  1. scoreboard-sign.json and scoreboard-labelled.json exist, parse, and have required fields.
  2. scoreboard-assets/<mode>/ directories contain all SVGs referenced in each mode scoreboard.
  3. Every SVG under scoreboard-assets/**/ is a non-empty, well-formed SVG element.
  4. solver_summary entries have required keys and solved <= attempted.
  5. scoreboard.md exists and links to both scoreboard-sign.md and scoreboard-labelled.md.

Usage:
    python3 tools/check_generated_scoreboard.py [--repo-root PATH]
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys


_REQUIRED_SCOREBOARD_KEYS = ("generated_at", "tiers", "solver_summary")
_REQUIRED_SUMMARY_KEYS = ("tier", "config", "backend", "solved", "attempted")


def _repo_root_default() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parent.parent


def check_scoreboard_json(root: pathlib.Path, name: str = "scoreboard.json") -> list[str]:
    errors: list[str] = []
    path = root / name
    if not path.exists():
        return [f"{name} not found at {path}"]
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return [f"{name} is not valid JSON: {exc}"]
    for key in _REQUIRED_SCOREBOARD_KEYS:
        if key not in data:
            errors.append(f"{name} missing key '{key}'")
    solver_summary = data.get("solver_summary", [])
    if not isinstance(solver_summary, list):
        errors.append(f"{name} solver_summary is not a list")
        return errors
    for i, entry in enumerate(solver_summary):
        for key in _REQUIRED_SUMMARY_KEYS:
            if key not in entry:
                errors.append(
                    f"{name} solver_summary[{i}] missing key '{key}'"
                )
        solved = entry.get("solved", 0)
        attempted = entry.get("attempted", 0)
        if isinstance(solved, int) and isinstance(attempted, int):
            if solved > attempted:
                errors.append(
                    f"{name} solver_summary[{i}]: solved={solved} > "
                    f"attempted={attempted} for config '{entry.get('config', '?')}'"
                )
    return errors


def _svg_refs_from_md(md_path: pathlib.Path) -> list[tuple[str, str]]:
    """Return (subdir, filename) pairs for all SVG refs in a markdown file."""
    pattern = re.compile(r"!\[.*?\]\(([^)]+\.svg)\)")
    results = []
    for match in pattern.finditer(md_path.read_text(encoding="utf-8")):
        ref = match.group(1)
        parts = ref.rsplit("/", 1)
        if len(parts) == 2:
            results.append((parts[0], parts[1]))
        else:
            results.append(("", ref))
    return results


def check_mode_scoreboard_assets(root: pathlib.Path, mode: str) -> list[str]:
    errors: list[str] = []
    md_path = root / f"scoreboard-{mode}.md"
    assets_base = root / "scoreboard-assets" / mode

    if not md_path.exists():
        return [f"scoreboard-{mode}.md not found at {md_path}"]

    for subdir, svg_name in _svg_refs_from_md(md_path):
        svg_path = root / subdir / svg_name
        if not svg_path.exists():
            errors.append(f"scoreboard-{mode}.md references missing asset: {subdir}/{svg_name}")

    return errors


def check_all_svg_files(root: pathlib.Path) -> list[str]:
    errors: list[str] = []
    assets_dir = root / "scoreboard-assets"
    if not assets_dir.is_dir():
        return [f"scoreboard-assets/ directory not found at {assets_dir}"]

    for svg_path in sorted(assets_dir.glob("**/*.svg")):
        content = svg_path.read_text(encoding="utf-8", errors="replace")
        if "<svg" not in content:
            rel = svg_path.relative_to(root)
            errors.append(f"{rel}: does not contain '<svg'")
        if len(content.strip()) == 0:
            rel = svg_path.relative_to(root)
            errors.append(f"{rel}: file is empty")

    return errors


def check_index_links(root: pathlib.Path) -> list[str]:
    errors: list[str] = []
    index_path = root / "scoreboard.md"
    if not index_path.exists():
        return [f"scoreboard.md not found at {index_path}"]
    content = index_path.read_text(encoding="utf-8")
    for name in ("scoreboard-sign.md", "scoreboard-labelled.md"):
        if name not in content:
            errors.append(f"scoreboard.md does not link to {name}")
    return errors


def check_scoreboard_assets(root: pathlib.Path) -> list[str]:
    """Legacy single-mode check: kept for compatibility with old test fixtures."""
    errors: list[str] = []
    assets_dir = root / "scoreboard-assets"
    if not assets_dir.is_dir():
        return [f"scoreboard-assets/ directory not found at {assets_dir}"]

    md_path = root / "scoreboard.md"
    if not md_path.exists():
        errors.append(f"scoreboard.md not found at {md_path}")
        referenced: list[tuple[str, str]] = []
    else:
        referenced = _svg_refs_from_md(md_path)

    for subdir, svg_name in referenced:
        svg_path = root / subdir / svg_name
        if not svg_path.exists():
            errors.append(f"scoreboard.md references missing asset: {subdir}/{svg_name}")

    for svg_path in sorted(assets_dir.glob("*.svg")):
        content = svg_path.read_text(encoding="utf-8", errors="replace")
        if "<svg" not in content:
            errors.append(f"{svg_path.name}: does not contain '<svg'")
        if len(content.strip()) == 0:
            errors.append(f"{svg_path.name}: file is empty")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=pathlib.Path,
        default=_repo_root_default(),
        help="path to the repository root (default: parent of this script)",
    )
    args = parser.parse_args(argv)
    root = args.repo_root.resolve()

    all_errors: list[str] = []
    for mode in ("sign", "labelled"):
        all_errors.extend(check_scoreboard_json(root, f"scoreboard-{mode}.json"))
        all_errors.extend(check_mode_scoreboard_assets(root, mode))
    all_errors.extend(check_all_svg_files(root))
    all_errors.extend(check_index_links(root))

    if all_errors:
        for err in all_errors:
            print(f"ERROR: {err}", file=sys.stderr)
        return 1

    print(f"OK: scoreboard artifacts look consistent under {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
