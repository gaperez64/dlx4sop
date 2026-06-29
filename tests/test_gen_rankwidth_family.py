#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        **kwargs,
    )


def test_single_instance(tool: pathlib.Path) -> None:
    completed = run([sys.executable, str(tool), "--height", "1", "--blowup", "3"])
    if completed.returncode != 0:
        raise AssertionError(f"generator failed:\n{completed.stdout}\n{completed.stderr}")
    lines = completed.stdout.splitlines()
    if not lines or lines[0] != "p qsop-sign 8 9 27":
        raise AssertionError(f"unexpected header:\n{completed.stdout}")
    if lines.count("n 0") != 1:
        raise AssertionError(f"missing normalization line:\n{completed.stdout}")
    if sum(1 for line in lines if line.startswith("u ")) != 9:
        raise AssertionError(f"unexpected unary count:\n{completed.stdout}")
    if sum(1 for line in lines if line.startswith("e ")) != 27:
        raise AssertionError(f"unexpected edge count:\n{completed.stdout}")


def test_materialize_manifest(tool: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        out_dir = pathlib.Path(tmp) / "rankwidth"
        completed = run(
            [
                sys.executable,
                str(tool),
                "--heights",
                "1,2",
                "--blowups",
                "2",
                "--materialize-dir",
                str(out_dir),
            ]
        )
        if completed.returncode != 0:
            raise AssertionError(f"materialize failed:\n{completed.stdout}\n{completed.stderr}")
        manifest = out_dir / "manifest.jsonl"
        rows = [json.loads(line) for line in manifest.read_text(encoding="utf-8").splitlines()]
        if [row["name"] for row in rows] != [
            "btclique-h01-t02-r8-all-t",
            "btclique-h02-t02-r8-all-t",
        ]:
            raise AssertionError(f"unexpected manifest rows: {rows}")
        for row in rows:
            qsop = out_dir / row["path"]
            meta = out_dir / row["meta_path"]
            if not qsop.exists() or not meta.exists():
                raise AssertionError(f"missing materialized files for {row}")
            payload = json.loads(meta.read_text(encoding="utf-8"))
            if payload["theoretical_rankwidth"] != "O(1)" or payload["qsop_mode"] != "sign":
                raise AssertionError(f"unexpected metadata: {payload}")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_gen_rankwidth_family.py GEN_TOOL", file=sys.stderr)
        return 2
    tool = pathlib.Path(sys.argv[1])
    test_single_instance(tool)
    test_materialize_manifest(tool)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
