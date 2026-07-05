#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


def test_existing_render(tool: pathlib.Path) -> None:
    report = {
        "source": "Synthetic",
        "source_url": "https://example.invalid/synthetic",
        "records": [
            {"source": "Synthetic", "source_url": "https://example.invalid/synthetic", "status": "ok", "mode": "sign", "max_imported_nvars": 4},
            {"source": "Synthetic", "source_url": "https://example.invalid/synthetic", "status": "too_many_vars", "mode": "sign", "max_imported_nvars": 80},
        ],
    }

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        report_path = tmp_path / "report.json"
        report_path.write_text(json.dumps(report), encoding="utf-8")
        completed = subprocess.run(
            [
                str(tool),
                "--import-report",
                str(report_path),
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"scoreboard render failed:\n{completed.stdout}\n{completed.stderr}")
        for expected in (
            "## Import Coverage",
            "| Synthetic | https://example.invalid/synthetic | 2 | 1 | 0 | 1 | 0 |",
        ):
            if expected not in completed.stdout:
                raise AssertionError(f"missing {expected!r} in:\n{completed.stdout}")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_render_scoreboard.py RENDER_SCOREBOARD", file=sys.stderr)
        return 2
    tool = pathlib.Path(sys.argv[1])
    errors = []
    for fn, name in [
        (test_existing_render, "existing_render"),
    ]:
        try:
            fn(tool)
            print(f"  PASS {name}")
        except AssertionError as exc:
            print(f"  FAIL {name}: {exc}")
            errors.append(name)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
