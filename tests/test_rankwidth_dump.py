#!/usr/bin/env python3
"""Tests for --rankwidth-dump round-trip serialisation."""

import pathlib
import subprocess
import sys
import tempfile


def make_path_qsop(nvars: int, r: int = 8) -> str:
    nedges = nvars - 1
    lines = [f"p qsop {r} {nvars} {nedges}", "n 0", "cst 0"]
    for i in range(nedges):
        lines.append(f"q {i} {i + 1} 1")
    return "\n".join(lines) + "\n"


def run_rankwidth(exe: pathlib.Path, qsop_text: str,
                  decomp_path: str | None = None,
                  generator: str | None = None,
                  extra: list[str] | None = None) -> subprocess.CompletedProcess:
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(qsop_text)
        qsop_path = f.name
    args = [str(exe), "--backend", "rankwidth", "--max-vars", "64"]
    if decomp_path is not None:
        args += ["--rankwidth-decomposition", decomp_path]
    if generator is not None:
        args += ["--rankwidth-generate", generator]
    if extra:
        args += extra
    args.append(qsop_path)
    return subprocess.run(args, check=False, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True)


def test_dump_round_trip(exe: pathlib.Path) -> None:
    """Dump a decomposition, reload it, and check that results match."""
    generators = ["left-deep", "balanced", "min-fill", "min-fill-cut"]
    for nvars in range(3, 10):
        for gen in generators:
            qsop_text = make_path_qsop(nvars)
            with tempfile.NamedTemporaryFile(suffix=".rwdec", delete=False) as df:
                dump_path = df.name

            # generate + dump
            r1 = run_rankwidth(exe, qsop_text, generator=gen,
                               extra=["--rankwidth-dump", dump_path])
            assert r1.returncode == 0, (
                f"dump failed ({gen}, nvars={nvars}): {r1.stderr}"
            )

            # reload + solve
            r2 = run_rankwidth(exe, qsop_text, decomp_path=dump_path)
            assert r2.returncode == 0, (
                f"reload failed ({gen}, nvars={nvars}): {r2.stderr}"
            )

            assert r1.stdout == r2.stdout, (
                f"round-trip mismatch ({gen}, nvars={nvars}):\n"
                f"  original:  {r1.stdout!r}\n  reloaded:  {r2.stdout!r}"
            )


def test_dump_requires_rankwidth_backend(exe: pathlib.Path) -> None:
    """--rankwidth-dump without --backend rankwidth must be rejected."""
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(make_path_qsop(4))
        qsop_path = f.name
    with tempfile.NamedTemporaryFile(suffix=".rwdec", delete=False) as df:
        dump_path = df.name
    result = subprocess.run(
        [str(exe), "--backend", "branch", "--rankwidth-dump", dump_path, qsop_path],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    assert result.returncode != 0, "--rankwidth-dump with --backend branch should fail"
    assert "rankwidth" in result.stderr, (
        f"Expected error mentioning rankwidth, got: {result.stderr!r}"
    )


def test_dump_file_content(exe: pathlib.Path) -> None:
    """Dump file must contain a valid p rwdec header and l/j node records."""
    qsop_text = make_path_qsop(5)
    with tempfile.NamedTemporaryFile(suffix=".rwdec", delete=False) as df:
        dump_path = df.name

    r = run_rankwidth(exe, qsop_text, extra=["--rankwidth-dump", dump_path])
    assert r.returncode == 0, f"dump failed: {r.stderr}"

    content = pathlib.Path(dump_path).read_text()
    lines = [ln for ln in content.splitlines() if ln.strip()]
    assert lines[0].startswith("p rwdec "), f"missing header: {lines[0]!r}"
    parts = lines[0].split()
    assert parts[1] == "rwdec"
    nvars, nnodes, root = int(parts[2]), int(parts[3]), int(parts[4])
    assert nvars == 5
    assert nnodes > 0
    assert root < nnodes

    node_ids = set()
    for ln in lines[1:]:
        fields = ln.split()
        assert fields[0] in ("l", "j"), f"unexpected record: {ln!r}"
        node_id = int(fields[1])
        assert node_id not in node_ids, f"duplicate node id {node_id}"
        node_ids.add(node_id)
        if fields[0] == "l":
            assert len(fields) == 3
        else:
            assert len(fields) == 4

    assert len(node_ids) == nnodes, "node count mismatch"


def main(argv: list[str]) -> None:
    if len(argv) < 3:
        print(f"usage: {argv[0]} <sop-solve> <source-root>")
        sys.exit(2)
    exe = pathlib.Path(argv[1])

    test_dump_round_trip(exe)
    test_dump_requires_rankwidth_backend(exe)
    test_dump_file_content(exe)

    print("all rankwidth dump tests passed")


if __name__ == "__main__":
    main(sys.argv)
