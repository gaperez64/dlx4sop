#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys


def git_output(source_root: pathlib.Path, *args: str) -> str | None:
    try:
        completed = subprocess.run(
            ["git", "-C", str(source_root), *args],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except OSError:
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout.strip()


def live_git_version(source_root: pathlib.Path, dirty: bool) -> str | None:
    if git_output(source_root, "rev-parse", "--git-dir") is None:
        return None

    version = git_output(source_root, "describe", "--tags", "--exact-match", "HEAD")
    if not version:
        version = git_output(source_root, "rev-parse", "--short=7", "HEAD")
    if not version:
        return None
    version = version.removeprefix("v")

    if dirty:
        status = git_output(source_root, "status", "--porcelain")
        if status:
            version += "-dirty"
    return version


def archive_version(source_root: pathlib.Path) -> str | None:
    archive_file = source_root / "scripts" / "version-archive.txt"
    try:
        fields = dict(
            line.split("=", 1)
            for line in archive_file.read_text(encoding="utf-8").splitlines()
            if "=" in line
        )
    except OSError:
        return None

    describe = fields.get("describe", "")
    commit = fields.get("commit", "")
    placeholder = "$" + "Format:"
    if placeholder in describe or placeholder in commit:
        return None

    if describe and not re.search(r"-\d+-g[0-9a-f]+$", describe):
        return describe.removeprefix("v")
    return commit or None


def parse_args(args: list[str]) -> tuple[pathlib.Path, bool]:
    source_root = pathlib.Path(sys.argv[0]).resolve().parent.parent
    dirty = False
    index = 0
    while index < len(args):
        if args[index] == "--dirty":
            dirty = True
        elif args[index] == "--source-root" and index + 1 < len(args):
            index += 1
            source_root = pathlib.Path(args[index]).resolve()
        index += 1
    return source_root, dirty


def main() -> None:
    try:
        source_root, dirty = parse_args(sys.argv[1:])
        version = live_git_version(source_root, dirty) or archive_version(source_root) or "unknown"
    except Exception:
        version = "unknown"
    print(version)


if __name__ == "__main__":
    main()
