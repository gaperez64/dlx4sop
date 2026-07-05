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


def test_mqt_scaling_table(tool: pathlib.Path) -> None:
    """C4.3: verify --mqt-scaling-table renders a correct Markdown table from fixture data."""
    scaling = [
        {
            "family": "qft",
            "rows": 12,
            "qubits_p50": 8.0, "qubits_p90": 14.0, "qubits_max": 20,
            "nvars_p50": 64.0, "nvars_p90": 112.0, "nvars_max": 160,
            "treewidth_p50": 4.0, "treewidth_p90": 7.0, "treewidth_max": 10,
            "cut_rank_p50": 3.0, "cut_rank_max": 6,
            "qsop_mode": "sign", "sign_count": 12,
        },
        {
            "family": "grover",
            "rows": 6,
            "qubits_p50": 5.0, "qubits_p90": 9.0, "qubits_max": 12,
            "nvars_p50": 40.0, "nvars_p90": 72.0, "nvars_max": 96,
            "treewidth_p50": 3.0, "treewidth_p90": 5.0, "treewidth_max": 8,
            "cut_rank_p50": 2.0, "cut_rank_max": 4,
            "qsop_mode": "sign", "sign_count": 6,
        },
    ]
    with tempfile.TemporaryDirectory() as tmp:
        table_path = pathlib.Path(tmp) / "mqt-scaling-table.json"
        table_path.write_text(json.dumps(scaling), encoding="utf-8")
        completed = subprocess.run(
            [str(tool), "--mqt-scaling-table", str(table_path)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(
                f"render failed with exit {completed.returncode}:\n{completed.stderr}"
            )
        out = completed.stdout
        expected_snippets = [
            "## MQT Bench Scaling by Family",
            "| Family | Mode |",
            "Cut-rank p50",
            "| qft | sign | 12 |",
            "| grover | sign | 6 |",
        ]
        for s in expected_snippets:
            if s not in out:
                raise AssertionError(f"missing {s!r} in output:\n{out}")


def test_mqt_empty_manifest_notice(tool: pathlib.Path) -> None:
    """C4.1: verify --mqt-manifest-dir emits notice when directory has no tier-*.json."""
    with tempfile.TemporaryDirectory() as tmp:
        empty_dir = pathlib.Path(tmp) / "empty-mqt"
        empty_dir.mkdir()
        completed = subprocess.run(
            [str(tool), "--mqt-manifest-dir", str(empty_dir)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(
                f"render failed with exit {completed.returncode}:\n{completed.stderr}"
            )
        out = completed.stdout
        expected = [
            "## MQT Bench Data",
            "MQT Bench manifests not found",
            "python3 scripts/bench.py harvest-mqt",
            "python3 scripts/bench.py materialize-mqt",
            "python3 scripts/bench.py profile-mqt",
        ]
        for s in expected:
            if s not in out:
                raise AssertionError(f"missing {s!r} in output:\n{out}")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_render_scoreboard.py RENDER_SCOREBOARD", file=sys.stderr)
        return 2
    tool = pathlib.Path(sys.argv[1])
    errors = []
    for fn, name in [
        (test_existing_render, "existing_render"),
        (test_mqt_scaling_table, "mqt_scaling_table"),
        (test_mqt_empty_manifest_notice, "mqt_empty_manifest_notice"),
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
