#!/usr/bin/env python3
"""Benchmark sop2wmc + Ganak against sop-solve.

Two encodings are benchmarked:
  residue  -- one DIMACS CNF per residue, plain #SAT (Ganak --mode 0)
  amplitude -- single WPCNF with complex literal weights (Ganak --mode 6)

For each QSOP instance the script cross-checks Ganak's output against the
reference from `sop-solve --format residue-vector`. A non-zero mismatch count
means the export or the counter disagrees with sop-solve.

Emits a markdown table (default) or JSONL.  JSONL output includes
scoreboard-compatible fields (backend, status, solve_elapsed_ns, source, …)
when invoked via --manifest.
"""

import argparse
import cmath
import hashlib
import json
import math
import pathlib
import re
import subprocess
import sys
import tempfile
import time


def materialize_manifest(qasm2sop: pathlib.Path, manifest: pathlib.Path, dest: pathlib.Path):
    """Expand a QASM corpus manifest into per-boundary QSOP files.

    Returns a list of (path, provenance_dict) pairs.
    """
    cases = json.loads(manifest.read_text())
    produced = []
    for case in cases:
        qasm = "\n".join(case["qasm_lines"]) + "\n"
        for inb, outb in case["boundaries"]:
            result = run(
                [str(qasm2sop), "--input", inb, "--output", outb, "-"], input=qasm
            )
            if result.returncode != 0:
                raise RuntimeError(f"qasm2sop failed on {case['name']} {inb}>{outb}:\n{result.stderr}")
            slug = hashlib.sha1(f"{case['name']}_{inb}_{outb}".encode()).hexdigest()[:16]
            path = dest / f"{slug}.qsop"
            path.write_text(result.stdout)
            provenance = {
                "source": case.get("source", ""),
                "source_url": case.get("source_url", ""),
                "source_relative_path": case.get("source_relative_path", ""),
                "case": case.get("name", ""),
                "input": inb,
                "output": outb,
            }
            produced.append((path, provenance))
    return produced


GANAK_INT_PATTERNS = [
    re.compile(r"^c s exact arb int (\d+)"),
    re.compile(r"^c s exact double int (\d+)"),
    re.compile(r"^s mc (\d+)"),
]

GANAK_COMPLEX_PATTERN = re.compile(
    r"^c s exact (?:arb frac|quadruple float)\s+([+-]?[\d.e+-]+)\s*\+\s*([+-]?[\d.e+-]+)i"
)


def run(cmd, **kwargs):
    return subprocess.run(
        cmd, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kwargs
    )


def read_header(qsop: pathlib.Path):
    for line in qsop.read_text().splitlines():
        if line.startswith("p qsop"):
            _, _, r, nvars, nedges = line.split()
            return int(r), int(nvars), int(nedges)
    raise ValueError(f"{qsop}: no 'p qsop' header")


def read_qsop_mode(qsop: pathlib.Path) -> str:
    for line in qsop.read_text().splitlines():
        parts = line.split()
        if parts and parts[0] == "q":
            return "labelled"
    return "sign"


def parse_ganak_int(text: str) -> int:
    for line in text.splitlines():
        stripped = line.strip()
        for pattern in GANAK_INT_PATTERNS:
            match = pattern.match(stripped)
            if match:
                return int(match.group(1))
    raise ValueError(f"could not parse ganak model count:\n{text}")


def parse_ganak_complex(text: str) -> complex:
    for line in text.splitlines():
        stripped = line.strip()
        m = GANAK_COMPLEX_PATTERN.match(stripped)
        if m:
            return complex(float(m.group(1)), float(m.group(2)))
    raise ValueError(f"could not parse ganak complex output:\n{text}")


def parse_amplitude_factor(cnf_text: str) -> complex:
    for line in cnf_text.splitlines():
        if line.startswith("c amplitude_factor "):
            val = line.split(None, 2)[2]
            # parse "a+bi" or "a+-bi" format
            m = re.match(r"([+-]?[\d.e+-]+)\+([+-]?[\d.e+-]+)i", val)
            if m:
                return complex(float(m.group(1)), float(m.group(2)))
    raise ValueError("no c amplitude_factor line in CNF metadata")


def solver_counts(sop_solve: pathlib.Path, qsop: pathlib.Path, extra_args: list | None = None):
    cmd = [str(sop_solve), "--format", "residue-vector"]
    if extra_args:
        cmd.extend(extra_args)
    cmd.append(str(qsop))
    start = time.perf_counter()
    result = run(cmd)
    elapsed = time.perf_counter() - start
    if result.returncode != 0:
        raise RuntimeError(f"sop-solve failed on {qsop}:\n{result.stderr}")
    for line in result.stdout.splitlines():
        if line.startswith("counts"):
            return [int(tok) for tok in line.split()[1:]], elapsed
    raise RuntimeError(f"no counts line from sop-solve on {qsop}")


def ganak_residue(sop2wmc: pathlib.Path, ganak: pathlib.Path, qsop: pathlib.Path, r: int, timeout: float | None):
    """Run residue encoding: r separate CNF files, plain #SAT each."""
    counts, export_s, count_s = [], 0.0, 0.0
    with tempfile.TemporaryDirectory() as tmp:
        for k in range(r):
            cnf = pathlib.Path(tmp) / f"r{k}.cnf"
            start = time.perf_counter()
            exported = run([str(sop2wmc), "--encoding", "residue",
                            "--residue", str(k), "-o", str(cnf), str(qsop)])
            export_s += time.perf_counter() - start
            if exported.returncode != 0:
                raise RuntimeError(f"sop2wmc --residue {k} failed:\n{exported.stderr}")
            start = time.perf_counter()
            try:
                counted = subprocess.run(
                    [str(ganak), "--verb", "0", str(cnf)],
                    check=False,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=timeout,
                )
            except subprocess.TimeoutExpired:
                count_s += (timeout or 0.0)
                return None, export_s, count_s, "timeout"
            count_s += time.perf_counter() - start
            counts.append(parse_ganak_int(counted.stdout))
    return counts, export_s, count_s, "ok"


def ganak_amplitude(sop2wmc: pathlib.Path, ganak: pathlib.Path, qsop: pathlib.Path, timeout: float | None):
    """Run amplitude encoding: single WPCNF, complex WMC (Ganak --mode 6)."""
    with tempfile.TemporaryDirectory() as tmp:
        cnf_path = pathlib.Path(tmp) / "amp.cnf"
        start = time.perf_counter()
        exported = run([str(sop2wmc), "--encoding", "amplitude",
                        "-o", str(cnf_path), str(qsop)])
        export_s = time.perf_counter() - start
        if exported.returncode != 0:
            raise RuntimeError(f"sop2wmc --encoding amplitude failed:\n{exported.stderr}")

        cnf_text = cnf_path.read_text()
        factor = parse_amplitude_factor(cnf_text)

        start = time.perf_counter()
        try:
            counted = subprocess.run(
                [str(ganak), "--mode", "6", "--verb", "0", str(cnf_path)],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            count_s = timeout or 0.0
            return None, export_s, count_s, "timeout"
        count_s = time.perf_counter() - start

        raw = parse_ganak_complex(counted.stdout)
        amplitude = raw * factor
        return amplitude, export_s, count_s, "ok"


def counts_to_amplitude(counts: list, r: int) -> complex:
    omega = cmath.exp(2 * math.pi * 1j / r)
    return sum(c * omega ** k for k, c in enumerate(counts))


def check_amplitude(got: complex, ref: complex, tol: float = 1e-5) -> bool:
    return abs(got - ref) <= tol * max(1.0, abs(ref))


def bench(sop2wmc, sop_solve, ganak, qsop, encoding, timeout, provenance, sop_solve_extra=None):
    r, nvars, nedges = read_header(qsop)
    qsop_mode = read_qsop_mode(qsop)
    try:
        reference, solve_s = solver_counts(sop_solve, qsop, sop_solve_extra)
        ref_amplitude = counts_to_amplitude(reference, r)
    except RuntimeError:
        reference = None
        ref_amplitude = None
        solve_s = 0.0

    base = {
        "backend": "wmc",
        "wmc_encoding": encoding,
        "nvars": nvars,
        "nedges": nedges,
        "r": r,
        "qsop_mode": qsop_mode,
        "sop_solve_ms": round(solve_s * 1e3, 3),
        **provenance,
    }

    if encoding == "residue":
        result, export_s, count_s, status = ganak_residue(sop2wmc, ganak, qsop, r, timeout)
        export_ns = int(export_s * 1e9)
        ganak_ns = int(count_s * 1e9)
        solve_ns = export_ns + ganak_ns
        if status == "timeout" or result is None:
            return {
                **base,
                "status": "timeout",
                "solve_elapsed_ns": solve_ns,
                "wmc_export_elapsed_ns": export_ns,
                "wmc_ganak_elapsed_ns": ganak_ns,
                "export_ms": round(export_s * 1e3, 3),
                "ganak_ms": round(count_s * 1e3, 3),
                "ganak_total_ms": round((export_s + count_s) * 1e3, 3),
                "mismatches": 0,
            }
        if reference is not None:
            mismatches = sum(1 for a, b in zip(result, reference) if a != b)
        else:
            mismatches = -1
        amp = counts_to_amplitude(result, r)
        row = {
            **base,
            "status": "ok",
            "solve_elapsed_ns": solve_ns,
            "wmc_export_elapsed_ns": export_ns,
            "wmc_ganak_elapsed_ns": ganak_ns,
            "export_ms": round(export_s * 1e3, 3),
            "ganak_ms": round(count_s * 1e3, 3),
            "ganak_total_ms": round((export_s + count_s) * 1e3, 3),
            "mismatches": mismatches,
            "amplitude_real": amp.real,
            "amplitude_imag": amp.imag,
        }
        return row
    else:
        amplitude, export_s, count_s, status = ganak_amplitude(sop2wmc, ganak, qsop, timeout)
        export_ns = int(export_s * 1e9)
        ganak_ns = int(count_s * 1e9)
        solve_ns = export_ns + ganak_ns
        if status == "timeout" or amplitude is None:
            return {
                **base,
                "status": "timeout",
                "solve_elapsed_ns": solve_ns,
                "wmc_export_elapsed_ns": export_ns,
                "wmc_ganak_elapsed_ns": ganak_ns,
                "export_ms": round(export_s * 1e3, 3),
                "ganak_ms": round(count_s * 1e3, 3),
                "ganak_total_ms": round((export_s + count_s) * 1e3, 3),
                "mismatches": 0,
            }
        if ref_amplitude is not None:
            ok = check_amplitude(amplitude, ref_amplitude)
            mismatches = 0 if ok else 1
        else:
            mismatches = -1
        row = {
            **base,
            "status": "ok",
            "solve_elapsed_ns": solve_ns,
            "wmc_export_elapsed_ns": export_ns,
            "wmc_ganak_elapsed_ns": ganak_ns,
            "export_ms": round(export_s * 1e3, 3),
            "ganak_ms": round(count_s * 1e3, 3),
            "ganak_total_ms": round((export_s + count_s) * 1e3, 3),
            "mismatches": mismatches,
            "amplitude_real": amplitude.real,
            "amplitude_imag": amplitude.imag,
        }
        return row


def render_markdown(rows):
    header = (
        "| instance | encoding | nvars | edges | r | sop-solve (ms) | export (ms) | "
        "ganak (ms) | mismatches |"
    )
    sep = "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
    lines = [header, sep]
    for row in rows:
        lines.append(
            f"| {row['instance']} | {row['wmc_encoding']} | {row['nvars']} | {row['nedges']} | "
            f"{row['r']} | {row['sop_solve_ms']} | {row['export_ms']} | {row['ganak_ms']} | "
            f"{row['mismatches']} |"
        )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sop2wmc", default="build/sop2wmc")
    parser.add_argument("--sop-solve", default="build/sop-solve")
    parser.add_argument("--qasm2sop", default="build/qasm2sop")
    parser.add_argument("--ganak", default="ganak")
    parser.add_argument("--encoding", choices=["residue", "amplitude", "both"],
                        default="residue", help="WMC encoding to benchmark")
    parser.add_argument("--manifest", type=pathlib.Path, help="QASM corpus manifest to expand into QSOP instances")
    parser.add_argument("--format", choices=["markdown", "jsonl"], default="markdown")
    parser.add_argument("--ganak-timeout", type=float, default=30.0,
                        help="per-Ganak-call timeout in seconds (default: 30)")
    parser.add_argument("--sop-solve-backend", default=None,
                        help="backend passed to sop-solve for reference cross-check (e.g. treewidth)")
    parser.add_argument("--sop-solve-max-vars", type=int, default=None,
                        help="--max-vars passed to sop-solve for reference cross-check")
    parser.add_argument("instances", nargs="*", help="QSOP instance files")
    args = parser.parse_args()

    sop2wmc = pathlib.Path(args.sop2wmc)
    sop_solve = pathlib.Path(args.sop_solve)
    ganak = pathlib.Path(args.ganak)
    timeout = args.ganak_timeout if args.ganak_timeout > 0 else None

    encodings = ["residue", "amplitude"] if args.encoding == "both" else [args.encoding]

    sop_solve_extra = []
    if args.sop_solve_backend:
        sop_solve_extra += ["--backend", args.sop_solve_backend]
    if args.sop_solve_max_vars is not None:
        sop_solve_extra += ["--max-vars", str(args.sop_solve_max_vars)]

    with tempfile.TemporaryDirectory() as tmp:
        # instances from command line (no provenance)
        plain_instances = [(pathlib.Path(p), {}) for p in args.instances]
        manifest_instances = []
        if args.manifest:
            manifest_instances = materialize_manifest(
                pathlib.Path(args.qasm2sop), args.manifest, pathlib.Path(tmp)
            )
        all_instances = plain_instances + manifest_instances

        if not all_instances:
            parser.error("provide QSOP instances or --manifest")

        rows = []
        for enc in encodings:
            for instance, provenance in all_instances:
                row = bench(sop2wmc, sop_solve, ganak, instance, enc, timeout, provenance,
                            sop_solve_extra or None)
                # Add "instance" key for backward-compat markdown mode
                row["instance"] = instance.name
                rows.append(row)

    if args.format == "jsonl":
        for row in rows:
            print(json.dumps(row))
    else:
        print(render_markdown(rows))
    return 1 if any(row.get("mismatches") for row in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main())
