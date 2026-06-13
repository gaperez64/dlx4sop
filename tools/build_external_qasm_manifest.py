#!/usr/bin/env python3

import argparse
import collections
import json
import pathlib
import re
import subprocess
import sys


QREG_RE = re.compile(r"^qreg\s+[A-Za-z_][A-Za-z0-9_]*\[(\d+)\]\s*;$")


def sanitize_name(text: str) -> str:
    name = re.sub(r"[^A-Za-z0-9_.-]+", "_", text)
    return name.strip("_") or "case"


def strip_line_comment(line: str) -> str:
    return line.split("//", 1)[0].strip()


def qasm_qubits(qasm: str) -> int:
    total = 0
    for raw in qasm.splitlines():
        match = QREG_RE.match(strip_line_comment(raw))
        if match is not None:
            total += int(match.group(1))
    if total <= 0:
        raise ValueError("QASM input has no qreg declarations")
    return total


def source_files(roots: list[pathlib.Path], include_qc: bool, include_invalid: bool) -> list[tuple[pathlib.Path, pathlib.Path]]:
    files: list[tuple[pathlib.Path, pathlib.Path]] = []
    suffixes = {".qasm"}
    if include_qc:
        suffixes.add(".qc")

    for root in roots:
        if root.is_file():
            candidates = [root]
            base = root.parent
        else:
            candidates = sorted(path for path in root.rglob("*") if path.is_file())
            base = root
        for path in candidates:
            if path.suffix.lower() not in suffixes:
                continue
            if not include_invalid and "invalid" in path.parts:
                continue
            files.append((base, path))
    return files


def translate_qc(qc2qasm: pathlib.Path, path: pathlib.Path) -> str:
    completed = subprocess.run(
        [str(qc2qasm), str(path)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError(completed.stderr.strip().splitlines()[-1] if completed.stderr.strip() else "qc2qasm failed")
    return completed.stdout


def load_source(path: pathlib.Path, qc2qasm: pathlib.Path | None) -> str:
    if path.suffix.lower() == ".qc":
        if qc2qasm is None:
            raise RuntimeError(".qc input requires --qc2qasm")
        return translate_qc(qc2qasm, path)
    return path.read_text(encoding="utf-8")


def boundary_pairs(nqubits: int, mode: str) -> list[list[str]]:
    zero = "0" * nqubits
    one = "1" * nqubits
    if mode == "zero":
        return [[zero, zero]]
    if mode == "zero-and-one":
        return [[zero, zero], [one, one]]
    if mode == "zero-to-one":
        return [[zero, one]]
    raise AssertionError(f"unhandled boundary mode {mode}")


def qsop_header(qsop: str) -> tuple[int, int]:
    for line in qsop.splitlines():
        parts = line.split()
        if len(parts) == 5 and parts[:2] == ["p", "qsop"]:
            return int(parts[3]), int(parts[4])
    raise RuntimeError("missing QSOP header")


def import_boundary(qasm2sop: pathlib.Path, qasm: str, input_bits: str, output_bits: str) -> tuple[int, int]:
    completed = subprocess.run(
        [str(qasm2sop), "--input", input_bits, "--output", output_bits, "-"],
        check=False,
        input=qasm,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip().splitlines()[-1] if completed.stderr.strip() else "qasm2sop failed"
        raise RuntimeError(diagnostic)
    return qsop_header(completed.stdout)


def build_case(
    qasm2sop: pathlib.Path,
    root: pathlib.Path,
    path: pathlib.Path,
    qasm: str,
    boundaries: list[list[str]],
    max_vars: int,
    source_prefix: str,
) -> tuple[dict | None, str]:
    max_nvars = 0
    max_edges = 0
    for input_bits, output_bits in boundaries:
        nvars, nedges = import_boundary(qasm2sop, qasm, input_bits, output_bits)
        max_nvars = max(max_nvars, nvars)
        max_edges = max(max_edges, nedges)

    if max_nvars > max_vars:
        return None, "too_many_vars"

    relpath = path.relative_to(root)
    name = sanitize_name(f"{source_prefix}_{relpath.with_suffix('').as_posix()}")
    return (
        {
            "name": name,
            "source_path": str(path),
            "qasm_lines": qasm.rstrip("\n").splitlines(),
            "boundaries": boundaries,
            "max_imported_nvars": max_nvars,
            "max_imported_edges": max_edges,
        },
        "ok",
    )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a qasm_solver_corpus-compatible manifest from external QASM/QC files."
    )
    parser.add_argument("qasm2sop", type=pathlib.Path)
    parser.add_argument("roots", nargs="+", type=pathlib.Path)
    parser.add_argument("--qc2qasm", type=pathlib.Path, help="translator used when --include-qc is set")
    parser.add_argument("--include-qc", action="store_true")
    parser.add_argument("--include-invalid", action="store_true")
    parser.add_argument("--source-prefix", default="external")
    parser.add_argument("--limit", type=int, help="limit source files before import filtering")
    parser.add_argument("--max-cases", type=int, help="limit emitted manifest cases")
    parser.add_argument("--max-vars", type=int, default=24)
    parser.add_argument("--output", type=pathlib.Path, help="write manifest JSON to this path instead of stdout")
    parser.add_argument(
        "--boundaries",
        choices=("zero", "zero-and-one", "zero-to-one"),
        default="zero",
        help="fixed-boundary amplitudes to include per source file",
    )
    args = parser.parse_args(argv)
    if args.limit is not None and args.limit < 0:
        parser.error("--limit must be non-negative")
    if args.max_cases is not None and args.max_cases < 0:
        parser.error("--max-cases must be non-negative")
    if args.max_vars < 0:
        parser.error("--max-vars must be non-negative")
    if args.include_qc and args.qc2qasm is None:
        parser.error("--include-qc requires --qc2qasm")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    inputs = source_files(args.roots, args.include_qc, args.include_invalid)
    if args.limit is not None:
        inputs = inputs[: args.limit]

    counts: collections.Counter[str] = collections.Counter()
    cases = []
    for root, path in inputs:
        if args.max_cases is not None and len(cases) >= args.max_cases:
            break
        try:
            qasm = load_source(path, args.qc2qasm)
            nqubits = qasm_qubits(qasm)
            boundaries = boundary_pairs(nqubits, args.boundaries)
            case, status = build_case(
                args.qasm2sop,
                root,
                path,
                qasm,
                boundaries,
                args.max_vars,
                args.source_prefix,
            )
        except Exception:
            counts["skipped"] += 1
            continue

        counts[status] += 1
        if case is not None:
            cases.append(case)

    manifest = json.dumps(cases, indent=2, sort_keys=True) + "\n"
    if args.output is None:
        print(manifest, end="")
    else:
        args.output.write_text(manifest, encoding="utf-8")
    print(
        "build_external_qasm_manifest: "
        f"inputs={len(inputs)} emitted={len(cases)} "
        + " ".join(f"{key}={counts[key]}" for key in sorted(counts)),
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
