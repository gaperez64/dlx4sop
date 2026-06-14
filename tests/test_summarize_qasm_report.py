#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


def run_summary(tool: pathlib.Path, *args: str) -> str:
    completed = subprocess.run(
        [str(tool), *args],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"summary failed:\n{completed.stdout}\n{completed.stderr}")
    return completed.stdout


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_summarize_qasm_report.py SUMMARIZER", file=sys.stderr)
        return 2

    tool = pathlib.Path(sys.argv[1])
    report = {
        "source": "Example",
        "source_url": "https://example.invalid/bench",
        "records": [
            {
                "relative_path": "small.qasm",
                "source": "Example",
                "source_url": "https://example.invalid/bench",
                "status": "ok",
                "mode": "sign",
                "max_imported_nvars": 12,
                "max_imported_edges": 8,
            },
            {
                "relative_path": "mid.qasm",
                "source": "Example",
                "source_url": "https://example.invalid/bench",
                "status": "too_many_vars",
                "mode": "labelled",
                "max_imported_nvars": 63,
                "max_imported_edges": 90,
            },
            {
                "relative_path": "huge.qasm",
                "source": "Example",
                "source_url": "https://example.invalid/bench",
                "status": "too_many_vars",
                "mode": "sign",
                "max_imported_nvars": 300,
                "max_imported_edges": 600,
            },
            {
                "relative_path": "bad.qasm",
                "source": "Example",
                "source_url": "https://example.invalid/bench",
                "status": "unsupported_gate",
                "diagnostic": "unsupported OpenQASM operation 'foo'",
            },
        ],
    }

    with tempfile.TemporaryDirectory() as tmp:
        report_path = pathlib.Path(tmp) / "report.json"
        report_path.write_text(json.dumps(report), encoding="utf-8")

        markdown = run_summary(tool, str(report_path), "--top-diagnostics", "1")
        for expected in [
            "| Example | https://example.invalid/bench | 4 | 1 | 2 | 1 |",
            "| 33-64 | 33-64 | 1 | 0 | 1 | 0 | 0 | 1 | 0 |",
            "| 257+ | 257+ | 1 | 0 | 1 | 0 | 1 | 0 | 0 |",
            "| unsupported_gate | 1 | unsupported OpenQASM operation 'foo' |",
        ]:
            if expected not in markdown:
                raise AssertionError(f"missing markdown row {expected!r}:\n{markdown}")

        custom = run_summary(
            tool,
            str(report_path),
            "--tier",
            "tiny:0:16",
            "--tier",
            "wider:17:",
        )
        if "| wider | 17+ | 2 | 0 | 2 | 0 | 1 | 1 | 0 |" not in custom:
            raise AssertionError(f"missing custom tier:\n{custom}")

        summary = json.loads(run_summary(tool, str(report_path), "--format", "json"))
        if summary["records"] != 4 or summary["sources"][0]["too_many_vars"] != 2:
            raise AssertionError(f"unexpected JSON summary:\n{summary}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
