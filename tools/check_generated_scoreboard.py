#!/usr/bin/env python3
"""Validate committed scoreboard artifacts for self-consistency.

Checks:
  1. scoreboard.json exists, parses, and has required fields.
  2. scoreboard-assets/ contains all SVGs referenced in scoreboard.md.
  3. Every SVG in scoreboard-assets/ is a non-empty, well-formed SVG element.
  4. solver_summary entries have required keys and solved <= attempted.

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


def check_scoreboard_json(root: pathlib.Path) -> list[str]:
    errors: list[str] = []
    path = root / "scoreboard.json"
    if not path.exists():
        return [f"scoreboard.json not found at {path}"]
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return [f"scoreboard.json is not valid JSON: {exc}"]
    for key in _REQUIRED_SCOREBOARD_KEYS:
        if key not in data:
            errors.append(f"scoreboard.json missing key '{key}'")
    solver_summary = data.get("solver_summary", [])
    if not isinstance(solver_summary, list) or len(solver_summary) == 0:
        errors.append("scoreboard.json solver_summary is empty or not a list")
        return errors
    for i, entry in enumerate(solver_summary):
        for key in _REQUIRED_SUMMARY_KEYS:
            if key not in entry:
                errors.append(
                    f"scoreboard.json solver_summary[{i}] missing key '{key}'"
                )
        solved = entry.get("solved", 0)
        attempted = entry.get("attempted", 0)
        if isinstance(solved, int) and isinstance(attempted, int):
            if solved > attempted:
                errors.append(
                    f"scoreboard.json solver_summary[{i}]: solved={solved} > "
                    f"attempted={attempted} for config '{entry.get('config', '?')}'"
                )
    return errors


def _svg_refs_from_md(md_path: pathlib.Path) -> list[str]:
    """Return all SVG filenames referenced via Markdown image syntax."""
    pattern = re.compile(r"!\[.*?\]\(scoreboard-assets/([^)]+\.svg)\)")
    return pattern.findall(md_path.read_text(encoding="utf-8"))


def check_scoreboard_assets(root: pathlib.Path) -> list[str]:
    errors: list[str] = []
    assets_dir = root / "scoreboard-assets"
    if not assets_dir.is_dir():
        return [f"scoreboard-assets/ directory not found at {assets_dir}"]

    md_path = root / "scoreboard.md"
    if not md_path.exists():
        errors.append(f"scoreboard.md not found at {md_path}")
        referenced: list[str] = []
    else:
        referenced = _svg_refs_from_md(md_path)

    for svg_name in referenced:
        svg_path = assets_dir / svg_name
        if not svg_path.exists():
            errors.append(f"scoreboard.md references missing asset: {svg_name}")

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
    all_errors.extend(check_scoreboard_json(root))
    all_errors.extend(check_scoreboard_assets(root))

    if all_errors:
        for err in all_errors:
            print(f"ERROR: {err}", file=sys.stderr)
        return 1

    print(f"OK: scoreboard artifacts look consistent under {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
