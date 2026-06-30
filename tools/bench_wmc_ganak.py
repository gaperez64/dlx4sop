#!/usr/bin/env python3
"""Benchmark sop2wmc + Ganak against sop-solve.

Five encodings are supported:
  residue          -- one DIMACS CNF per residue, plain #SAT (Ganak --mode 0)
  amplitude/amp-and -- single WPCNF, Tseitin AND auxiliaries (Ganak --mode 6)
  amp-soft         -- single WPCNF, implication-only auxiliaries (Ganak --mode 6)
  amp-block        -- single WPCNF, bipartite sign-block parity factors (Ganak --mode 6)
  residue-fourier  -- one WPCNF block for Fourier mode 1 (Ganak --mode 6)
  all              -- run all five encodings

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

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from bench_common import cgroup_limited_command, command_memout, memory_limited_preexec  # noqa: E402


def iter_manifest_instances(qasm2sop: pathlib.Path, manifest: pathlib.Path, dest: pathlib.Path,
                            memory_limit_mib: int | None = None,
                            cgroup_memory_limit_mib: int | None = None,
                            timeout: float | None = None):
    """Expand a QASM corpus manifest into per-boundary QSOP files.

    Yields (path, provenance_dict) pairs as soon as each boundary is
    materialized so long WMC runs can emit rows incrementally.
    """
    cases = json.loads(manifest.read_text())
    for case in cases:
        qasm = "\n".join(case["qasm_lines"]) + "\n"
        for inb, outb in case["boundaries"]:
            try:
                result = run(
                    [str(qasm2sop), "--input", inb, "--output", outb, "-"], input=qasm,
                    timeout=timeout,
                    memory_limit_mib=memory_limit_mib,
                    cgroup_memory_limit_mib=cgroup_memory_limit_mib,
                )
            except subprocess.TimeoutExpired:
                print(
                    f"warning: qasm2sop timed out for {case.get('name', '<unknown>')} "
                    f"{inb}->{outb}",
                    file=sys.stderr,
                )
                continue
            if result.returncode != 0:
                continue
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
            yield path, provenance


def materialize_manifest(qasm2sop: pathlib.Path, manifest: pathlib.Path, dest: pathlib.Path,
                         memory_limit_mib: int | None = None,
                         cgroup_memory_limit_mib: int | None = None,
                         timeout: float | None = None):
    """Expand a QASM corpus manifest into per-boundary QSOP files."""
    return list(iter_manifest_instances(
        qasm2sop,
        manifest,
        dest,
        memory_limit_mib=memory_limit_mib,
        cgroup_memory_limit_mib=cgroup_memory_limit_mib,
        timeout=timeout,
    ))


GANAK_INT_PATTERNS = [
    re.compile(r"^c [so] exact arb int (\d+)"),
    re.compile(r"^c [so] exact double int (\d+)"),
    re.compile(r"^s mc (\d+)"),
]

GANAK_COMPLEX_PATTERN = re.compile(
    r"^c [so] exact (?:arb frac|quadruple float)\s+"
    r"([+-]?[\d.e+-]+)(?:\s*\+\s*([+-]?[\d.e+-]+)i)?"
)


def run(
    cmd,
    *,
    memory_limit_mib: int | None = None,
    cgroup_memory_limit_mib: int | None = None,
    **kwargs,
):
    completed = subprocess.run(
        cgroup_limited_command([str(arg) for arg in cmd], cgroup_memory_limit_mib),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        preexec_fn=memory_limited_preexec(memory_limit_mib),
        **kwargs,
    )
    completed.memout = command_memout(
        completed.returncode,
        completed.stderr,
        cgroup_limited=cgroup_memory_limit_mib is not None,
    )
    return completed


def read_header(qsop: pathlib.Path):
    for line in qsop.read_text().splitlines():
        if line.startswith("p qsop-sign"):
            _, _, r, nvars, nedges = line.split()
            return int(r), int(nvars), int(nedges)
    raise ValueError(f"{qsop}: no 'p qsop-sign' header")


def read_qsop_mode(qsop: pathlib.Path) -> str:
    read_header(qsop)
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
            return complex(float(m.group(1)), float(m.group(2) or 0.0))
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


def _parse_kv_tokens(tokens: list[str]) -> dict[str, str]:
    parsed: dict[str, str] = {}
    for token in tokens:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        parsed[key] = value
    return parsed


def _maybe_int(value: str) -> int | str:
    try:
        return int(value)
    except ValueError:
        return value


def parse_wmc_metadata(cnf_text: str) -> dict[str, int | str]:
    """Extract benchmark-friendly structural metadata emitted by sop2wmc."""
    meta: dict[str, int | str] = {}
    block_max_a = 0
    block_max_b = 0
    for line in cnf_text.splitlines():
        if line.startswith("c sop2wmc "):
            fields = _parse_kv_tokens(line.split()[2:])
            if "encoding" in fields:
                meta["wmc_export_encoding"] = fields["encoding"]
            for source, target in (
                ("norm_h", "wmc_norm_h"),
                ("nvars", "wmc_original_nvars"),
                ("nedges", "wmc_original_edges"),
            ):
                if source in fields:
                    meta[target] = _maybe_int(fields[source])
        elif line.startswith("c preprocess "):
            fields = _parse_kv_tokens(line.split()[2:])
            if "nvars_after" in fields:
                meta["wmc_active_vars"] = _maybe_int(fields["nvars_after"])
            if "pairs_after" in fields:
                meta["wmc_residual_edges"] = _maybe_int(fields["pairs_after"])
        elif line.startswith("c block count="):
            fields = _parse_kv_tokens(line.split()[2:])
            for source, target in (
                ("count", "wmc_block_count"),
                ("covered_edges", "wmc_block_edges"),
                ("residual_edges", "wmc_residual_edges"),
                ("nvars_after", "wmc_active_vars"),
            ):
                if source in fields:
                    meta[target] = _maybe_int(fields[source])
        elif line.startswith("c block sign-parity "):
            fields = _parse_kv_tokens(line.split()[3:])
            if "a_size" in fields:
                block_max_a = max(block_max_a, int(fields["a_size"]))
            if "b_size" in fields:
                block_max_b = max(block_max_b, int(fields["b_size"]))
    if block_max_a:
        meta["wmc_block_max_a_size"] = block_max_a
    if block_max_b:
        meta["wmc_block_max_b_size"] = block_max_b
    meta.setdefault("wmc_block_count", 0)
    meta.setdefault("wmc_block_edges", 0)
    return meta


def is_zero_residual_wmc(metadata: dict[str, int | str]) -> bool:
    return metadata.get("wmc_active_vars") == 0 and metadata.get("wmc_residual_edges", 0) == 0


def solver_counts(sop_solve: pathlib.Path, qsop: pathlib.Path,
                  extra_args: list | None = None, timeout: float | None = None,
                  memory_limit_mib: int | None = None,
                  cgroup_memory_limit_mib: int | None = None):
    cmd = [str(sop_solve), "--format", "residue-vector"]
    if extra_args:
        cmd.extend(extra_args)
    cmd.append(str(qsop))
    start = time.perf_counter()
    try:
        result = run(
            [str(a) for a in cmd],
            timeout=timeout,
            memory_limit_mib=memory_limit_mib,
            cgroup_memory_limit_mib=cgroup_memory_limit_mib,
        )
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"sop-solve timed out on {qsop}")
    elapsed = time.perf_counter() - start
    if getattr(result, "memout", False):
        raise RuntimeError(f"sop-solve memout on {qsop}")
    if result.returncode != 0:
        raise RuntimeError(f"sop-solve failed on {qsop}:\n{result.stderr}")
    for line in result.stdout.splitlines():
        if line.startswith("counts"):
            return [int(tok) for tok in line.split()[1:]], elapsed
    raise RuntimeError(f"no counts line from sop-solve on {qsop}")


def ganak_residue(sop2wmc: pathlib.Path, ganak: pathlib.Path, qsop: pathlib.Path, r: int,
                  timeout: float | None, memory_limit_mib: int | None = None,
                  ganak_memory_limit_mib: int | None = None,
                  cgroup_memory_limit_mib: int | None = None):
    """Run residue encoding: r separate CNF files, plain #SAT each."""
    counts, export_s, count_s = [], 0.0, 0.0
    with tempfile.TemporaryDirectory() as tmp:
        for k in range(r):
            cnf = pathlib.Path(tmp) / f"r{k}.cnf"
            start = time.perf_counter()
            exported = run([str(sop2wmc), "--encoding", "residue",
                            "--residue", str(k), "-o", str(cnf), str(qsop)],
                           memory_limit_mib=memory_limit_mib,
                           cgroup_memory_limit_mib=cgroup_memory_limit_mib)
            export_s += time.perf_counter() - start
            if exported.returncode != 0:
                if getattr(exported, "memout", False):
                    return None, export_s, count_s, "memout"
                raise RuntimeError(f"sop2wmc --residue {k} failed:\n{exported.stderr}")
            start = time.perf_counter()
            try:
                counted = run(
                    [str(ganak), "--verb", "0", str(cnf)],
                    timeout=timeout,
                    memory_limit_mib=ganak_memory_limit_mib,
                    cgroup_memory_limit_mib=cgroup_memory_limit_mib,
                )
            except subprocess.TimeoutExpired:
                count_s += (timeout or 0.0)
                return None, export_s, count_s, "timeout"
            count_s += time.perf_counter() - start
            if getattr(counted, "memout", False):
                return None, export_s, count_s, "memout"
            if counted.returncode != 0:
                return None, export_s, count_s, "error"
            try:
                counts.append(parse_ganak_int(counted.stdout + counted.stderr))
            except ValueError:
                return None, export_s, count_s, "error"
    return counts, export_s, count_s, "ok"


def ganak_amplitude(sop2wmc: pathlib.Path, ganak: pathlib.Path, qsop: pathlib.Path,
                    timeout: float | None, enc: str = "amplitude",
                    sop2wmc_extra: list[str] | None = None,
                    memory_limit_mib: int | None = None,
                    ganak_memory_limit_mib: int | None = None,
                    cgroup_memory_limit_mib: int | None = None):
    """Run a single-WPCNF amplitude encoding (amp-and, amp-soft, amp-block): Ganak --mode 6."""
    with tempfile.TemporaryDirectory() as tmp:
        cnf_path = pathlib.Path(tmp) / "amp.cnf"
        start = time.perf_counter()
        cmd = [str(sop2wmc), "--encoding", enc]
        if sop2wmc_extra:
            cmd.extend(sop2wmc_extra)
        cmd.extend(["-o", str(cnf_path), str(qsop)])
        exported = run(
            cmd,
            memory_limit_mib=memory_limit_mib,
            cgroup_memory_limit_mib=cgroup_memory_limit_mib,
        )
        export_s = time.perf_counter() - start
        if exported.returncode != 0:
            if getattr(exported, "memout", False):
                return None, export_s, 0.0, "memout", {"memout": True}
            raise RuntimeError(f"sop2wmc --encoding {enc} failed:\n{exported.stderr}")

        cnf_text = cnf_path.read_text()
        metadata = parse_wmc_metadata(cnf_text)
        factor = parse_amplitude_factor(cnf_text)
        if factor == 0j:
            return 0j, export_s, 0.0, "ok", metadata
        if is_zero_residual_wmc(metadata):
            return factor, export_s, 0.0, "ok", metadata

        start = time.perf_counter()
        try:
            counted = run(
                [str(ganak), "--mode", "6", "--verb", "0", str(cnf_path)],
                timeout=timeout,
                memory_limit_mib=ganak_memory_limit_mib,
                cgroup_memory_limit_mib=cgroup_memory_limit_mib,
            )
        except subprocess.TimeoutExpired:
            count_s = timeout or 0.0
            return None, export_s, count_s, "timeout", metadata
        count_s = time.perf_counter() - start

        if getattr(counted, "memout", False):
            return None, export_s, count_s, "memout", metadata
        if counted.returncode != 0:
            return None, export_s, count_s, "error", metadata
        try:
            raw = parse_ganak_complex(counted.stdout + counted.stderr)
        except ValueError:
            return None, export_s, count_s, "error", metadata
        amplitude = raw * factor
        return amplitude, export_s, count_s, "ok", metadata


def _split_fourier_blocks(text: str) -> list[tuple[int, str]]:
    """Split multi-document residue-fourier output into (t, cnf_text) pairs."""
    blocks: list[tuple[int, str]] = []
    current_t: int | None = None
    current_lines: list[str] = []
    for line in text.splitlines(keepends=True):
        m = re.match(r"c --- fourier t=(\d+) r=\d+ ---", line)
        if m:
            if current_t is not None:
                blocks.append((current_t, "".join(current_lines)))
            current_t = int(m.group(1))
            current_lines = []
        elif current_t is not None:
            current_lines.append(line)
    if current_t is not None:
        blocks.append((current_t, "".join(current_lines)))
    return blocks


def _parse_z0_log2(cnf_text: str) -> int | None:
    for line in cnf_text.splitlines():
        m = re.search(r"z0_log2=(\d+)", line)
        if m:
            return int(m.group(1))
    return None


def ganak_residue_fourier(sop2wmc: pathlib.Path, ganak: pathlib.Path, qsop: pathlib.Path,
                          r: int, timeout: float | None,
                          sop2wmc_extra: list[str] | None = None,
                          memory_limit_mib: int | None = None,
                          ganak_memory_limit_mib: int | None = None,
                          cgroup_memory_limit_mib: int | None = None):
    """Run residue-fourier amplitude encoding for Fourier mode 1 only."""
    export_s, count_s = 0.0, 0.0
    with tempfile.TemporaryDirectory() as tmp:
        start = time.perf_counter()
        cmd = [str(sop2wmc), "--encoding", "residue-fourier", "--wmc-fourier-mode", "1"]
        if sop2wmc_extra:
            cmd.extend(sop2wmc_extra)
        cmd.append(str(qsop))
        exported = run(
            cmd,
            memory_limit_mib=memory_limit_mib,
            cgroup_memory_limit_mib=cgroup_memory_limit_mib,
        )
        export_s = time.perf_counter() - start
        if exported.returncode != 0:
            if getattr(exported, "memout", False):
                return None, export_s, count_s, "memout", {"memout": True}
            raise RuntimeError(f"sop2wmc --encoding residue-fourier failed:\n{exported.stderr}")

        blocks = _split_fourier_blocks(exported.stdout)
        if len(blocks) != 1:
            raise RuntimeError(f"residue-fourier: expected 1 mode-1 block, got {len(blocks)}")

        for t, cnf_text in blocks:
            if t != 1:
                raise RuntimeError(f"residue-fourier: expected t=1 block, got t={t}")
            metadata = parse_wmc_metadata(cnf_text)
            metadata["wmc_fourier_mode"] = t
            factor = parse_amplitude_factor(cnf_text)
            if factor == 0j:
                return 0j, export_s, count_s, "ok", metadata
            if is_zero_residual_wmc(metadata):
                return factor, export_s, count_s, "ok", metadata
            cnf_path = pathlib.Path(tmp) / f"t{t}.cnf"
            cnf_path.write_text(cnf_text)
            start = time.perf_counter()
            try:
                counted = run(
                    [str(ganak), "--mode", "6", "--verb", "0", str(cnf_path)],
                    timeout=timeout,
                    memory_limit_mib=ganak_memory_limit_mib,
                    cgroup_memory_limit_mib=cgroup_memory_limit_mib,
                )
            except subprocess.TimeoutExpired:
                count_s += (timeout or 0.0)
                return None, export_s, count_s, "timeout", metadata
            count_s += time.perf_counter() - start
            if getattr(counted, "memout", False):
                return None, export_s, count_s, "memout", metadata
            if counted.returncode != 0:
                return None, export_s, count_s, "error", metadata
            try:
                raw = parse_ganak_complex(counted.stdout + counted.stderr)
            except ValueError:
                return None, export_s, count_s, "error", metadata
            return raw * factor, export_s, count_s, "ok", metadata

    return None, export_s, count_s, "error", {}


def counts_to_amplitude(counts: list, r: int) -> complex:
    # Half-period symmetry: if c[k] == c[k + r/2] for all k then the DFT
    # evaluates to exactly 0 because omega^(r/2) = e^(i*pi) = -1, so each
    # pair c[k]*(omega^k + omega^(k+r/2)) = c[k]*omega^k*(1 + (-1)) = 0.
    # This avoids catastrophic cancellation for large integer counts (~2^60).
    if r % 2 == 0:
        half = r // 2
        if all(counts[k] == counts[k + half] for k in range(half)):
            return complex(0, 0)
    # Use exact integer arithmetic for r=2,4,8 to avoid catastrophic cancellation
    # when large integer counts nearly cancel (e.g. 2^51 counts summing to 0).
    if r == 2:
        return complex(counts[0] - counts[1], 0)
    if r == 4:
        return complex(counts[0] - counts[2], counts[1] - counts[3])
    if r == 8:
        inv_sqrt2 = math.sqrt(2) / 2
        a_re = counts[0] - counts[4]
        b_re = counts[1] + counts[7] - counts[3] - counts[5]
        a_im = counts[2] - counts[6]
        b_im = counts[1] - counts[7] + counts[3] - counts[5]
        return complex(a_re + b_re * inv_sqrt2, a_im + b_im * inv_sqrt2)
    omega = cmath.exp(2 * math.pi * 1j / r)
    return sum(c * omega ** k for k, c in enumerate(counts))


def check_amplitude(got: complex, ref: complex, tol: float = 2e-5) -> bool:
    return abs(got - ref) <= tol * max(1.0, abs(ref))


def bench(sop2wmc, sop_solve, ganak, qsop, encoding, timeout, provenance,
          sop_solve_extra=None, sop_solve_timeout=None, sop2wmc_extra=None,
          memory_limit_mib: int | None = None,
          ganak_memory_limit_mib: int | None = None,
          cgroup_memory_limit_mib: int | None = None):
    r, nvars, nedges = read_header(qsop)
    qsop_mode = read_qsop_mode(qsop)
    try:
        reference, solve_s = solver_counts(sop_solve, qsop, sop_solve_extra,
                                           timeout=sop_solve_timeout,
                                           memory_limit_mib=memory_limit_mib,
                                           cgroup_memory_limit_mib=cgroup_memory_limit_mib)
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
    if cgroup_memory_limit_mib is not None:
        base["cgroup_memory_limit_mib"] = cgroup_memory_limit_mib

    if encoding == "residue-fourier":
        amplitude, export_s, count_s, status, metadata = ganak_residue_fourier(
            sop2wmc, ganak, qsop, r, timeout, sop2wmc_extra=sop2wmc_extra,
            memory_limit_mib=memory_limit_mib,
            ganak_memory_limit_mib=ganak_memory_limit_mib,
            cgroup_memory_limit_mib=cgroup_memory_limit_mib,
        )
        export_ns = int(export_s * 1e9)
        ganak_ns = int(count_s * 1e9)
        solve_ns = export_ns + ganak_ns
        if status != "ok" or amplitude is None:
            return {**base, **metadata, "status": status, "solve_elapsed_ns": solve_ns,
                    **({"memout": True} if status == "memout" else {}),
                    "wmc_export_elapsed_ns": export_ns, "wmc_ganak_elapsed_ns": ganak_ns,
                    "export_ms": round(export_s * 1e3, 3), "ganak_ms": round(count_s * 1e3, 3),
                    "ganak_total_ms": round((export_s + count_s) * 1e3, 3), "mismatches": 0}
        mismatches = (0 if check_amplitude(amplitude, ref_amplitude) else 1) if ref_amplitude is not None else -1
        return {**base, **metadata, "status": "ok", "solve_elapsed_ns": solve_ns,
                "wmc_export_elapsed_ns": export_ns, "wmc_ganak_elapsed_ns": ganak_ns,
                "export_ms": round(export_s * 1e3, 3), "ganak_ms": round(count_s * 1e3, 3),
                "ganak_total_ms": round((export_s + count_s) * 1e3, 3),
                "mismatches": mismatches, "amplitude_real": amplitude.real, "amplitude_imag": amplitude.imag}
    elif encoding == "residue":
        result, export_s, count_s, status = ganak_residue(
            sop2wmc, ganak, qsop, r, timeout, memory_limit_mib=memory_limit_mib,
            ganak_memory_limit_mib=ganak_memory_limit_mib,
            cgroup_memory_limit_mib=cgroup_memory_limit_mib,
        )
        export_ns = int(export_s * 1e9)
        ganak_ns = int(count_s * 1e9)
        solve_ns = export_ns + ganak_ns
        if status != "ok" or result is None:
            return {
                **base,
                "status": status,
                **({"memout": True} if status == "memout" else {}),
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
        amplitude, export_s, count_s, status, metadata = ganak_amplitude(
            sop2wmc, ganak, qsop, timeout, enc=encoding, sop2wmc_extra=sop2wmc_extra,
            memory_limit_mib=memory_limit_mib,
            ganak_memory_limit_mib=ganak_memory_limit_mib,
            cgroup_memory_limit_mib=cgroup_memory_limit_mib,
        )
        export_ns = int(export_s * 1e9)
        ganak_ns = int(count_s * 1e9)
        solve_ns = export_ns + ganak_ns
        if status != "ok" or amplitude is None:
            return {
                **base,
                **metadata,
                "status": status,
                **({"memout": True} if status == "memout" else {}),
                "solve_elapsed_ns": solve_ns,
                "wmc_export_elapsed_ns": export_ns,
                "wmc_ganak_elapsed_ns": ganak_ns,
                "export_ms": round(export_s * 1e3, 3),
                "ganak_ms": round(count_s * 1e3, 3),
                "ganak_total_ms": round((export_s + count_s) * 1e3, 3),
                "mismatches": 0,
            }
        if ref_amplitude is not None and reference is not None:
            count_scale = max(abs(c) for c in reference)
            r_terms = len(reference)
            # Estimated double-precision error in the DFT reference.
            # When this dominates the reference value, the cross-check is unreliable.
            est_dp_err = r_terms * count_scale * 2.2e-16
            if ref_amplitude == 0j:
                # Reference is exactly 0 from integer/symmetry arithmetic.
                # Ganak's MPFR residual scales with circuit size; accept if well below count scale.
                ok = abs(amplitude) <= max(2e-5, count_scale * 1e-13)
            elif est_dp_err > abs(ref_amplitude):
                # Reference dominated by DP rounding; skip check (mismatches=-1).
                ok = None
            else:
                # Standard check with tolerance scaled to DP reference precision.
                eff_tol = max(2e-5, est_dp_err / abs(ref_amplitude))
                ok = check_amplitude(amplitude, ref_amplitude, tol=eff_tol)
            mismatches = (0 if ok else 1) if ok is not None else -1
        else:
            mismatches = -1
        row = {
            **base,
            **metadata,
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
    parser.add_argument("--encoding",
                        choices=["residue", "amplitude", "amp-and", "amp-soft",
                                 "amp-block", "residue-fourier", "both", "all"],
                        default="residue", help="WMC encoding to benchmark")
    parser.add_argument("--manifest", type=pathlib.Path, help="QASM corpus manifest to expand into QSOP instances")
    parser.add_argument("--format", choices=["markdown", "jsonl"], default="markdown")
    parser.add_argument("--ganak-timeout", type=float, default=30.0,
                        help="per-Ganak-call timeout in seconds (default: 30)")
    parser.add_argument("--sop-solve-backend", default=None,
                        help="backend passed to sop-solve for reference cross-check (e.g. treewidth)")
    parser.add_argument("--sop-solve-max-vars", type=int, default=None,
                        help="--max-vars passed to sop-solve for reference cross-check")
    parser.add_argument("--sop-solve-timeout", type=float, default=None,
                        help="timeout in seconds for sop-solve reference computation (default: no timeout)")
    parser.add_argument("--qasm2sop-timeout", type=float, default=30.0,
                        help="per-boundary qasm2sop timeout for --manifest expansion "
                             "(default: 30; <=0 disables)")
    parser.add_argument("--memory-limit-mib", type=int, default=None,
                        help="per-child address-space cap in MiB for qasm2sop, sop2wmc, and sop-solve")
    parser.add_argument("--ganak-memory-limit-mib", type=int, default=None,
                        help="optional Ganak address-space cap in MiB; disabled by default because Ganak may reserve large virtual memory")
    parser.add_argument("--cgroup-memory-limit-mib", type=int, default=None,
                        help="per-child cgroup physical-memory cap in MiB for qasm2sop, sop2wmc, sop-solve, and Ganak")
    parser.add_argument("--wmc-preprocess", choices=["none", "peel1", "peel2-safe"],
                        default=None, help="sop2wmc preprocessing mode")
    parser.add_argument("--wmc-peel2-fill-budget", type=int, default=None,
                        help="sop2wmc peel2-safe fill budget")
    parser.add_argument("--wmc-block-min-side", type=int, default=None,
                        help="sop2wmc amp-block minimum side size")
    parser.add_argument("--wmc-block-min-savings", type=int, default=None,
                        help="sop2wmc amp-block minimum savings threshold")
    parser.add_argument("instances", nargs="*", help="QSOP instance files")
    args = parser.parse_args()

    sop2wmc = pathlib.Path(args.sop2wmc)
    sop_solve = pathlib.Path(args.sop_solve)
    ganak = pathlib.Path(args.ganak)
    timeout = args.ganak_timeout if args.ganak_timeout > 0 else None
    sop_solve_timeout = args.sop_solve_timeout if args.sop_solve_timeout and args.sop_solve_timeout > 0 else None
    qasm2sop_timeout = args.qasm2sop_timeout if args.qasm2sop_timeout and args.qasm2sop_timeout > 0 else None
    if args.memory_limit_mib is not None and args.memory_limit_mib <= 0:
        parser.error("--memory-limit-mib must be positive")
    if args.ganak_memory_limit_mib is not None and args.ganak_memory_limit_mib <= 0:
        parser.error("--ganak-memory-limit-mib must be positive")
    if args.cgroup_memory_limit_mib is not None and args.cgroup_memory_limit_mib <= 0:
        parser.error("--cgroup-memory-limit-mib must be positive")

    _all_encodings = ["residue", "amplitude", "amp-soft", "amp-block", "residue-fourier"]
    if args.encoding in ("both", "all"):
        encodings = _all_encodings
    elif args.encoding == "amp-and":
        encodings = ["amplitude"]
    else:
        encodings = [args.encoding]

    sop_solve_extra = []
    if args.sop_solve_backend:
        sop_solve_extra += ["--backend", args.sop_solve_backend]
    if args.sop_solve_max_vars is not None:
        sop_solve_extra += ["--max-vars", str(args.sop_solve_max_vars)]

    sop2wmc_extra = []
    if args.wmc_preprocess is not None:
        sop2wmc_extra += ["--wmc-preprocess", args.wmc_preprocess]
    if args.wmc_peel2_fill_budget is not None:
        sop2wmc_extra += ["--wmc-peel2-fill-budget", str(args.wmc_peel2_fill_budget)]
    if args.wmc_block_min_side is not None:
        sop2wmc_extra += ["--wmc-block-min-side", str(args.wmc_block_min_side)]
    if args.wmc_block_min_savings is not None:
        sop2wmc_extra += ["--wmc-block-min-savings", str(args.wmc_block_min_savings)]

    rows = []
    row_count = 0
    had_mismatch = False

    def emit_row(row: dict) -> None:
        nonlocal row_count, had_mismatch
        row_count += 1
        had_mismatch = had_mismatch or bool(row.get("mismatches"))
        if args.format == "jsonl":
            print(json.dumps(row), flush=True)
        else:
            rows.append(row)

    def run_instance(instance: pathlib.Path, provenance: dict) -> None:
        for enc in encodings:
            row = bench(sop2wmc, sop_solve, ganak, instance, enc, timeout, provenance,
                        sop_solve_extra or None, sop_solve_timeout=sop_solve_timeout,
                        sop2wmc_extra=sop2wmc_extra or None,
                        memory_limit_mib=args.memory_limit_mib,
                        ganak_memory_limit_mib=args.ganak_memory_limit_mib,
                        cgroup_memory_limit_mib=args.cgroup_memory_limit_mib)
            # Add "instance" key for backward-compat markdown mode
            row["instance"] = instance.name
            emit_row(row)

    with tempfile.TemporaryDirectory() as tmp:
        for plain in args.instances:
            run_instance(pathlib.Path(plain), {})

        if args.manifest:
            for instance, provenance in iter_manifest_instances(
                pathlib.Path(args.qasm2sop),
                args.manifest,
                pathlib.Path(tmp),
                memory_limit_mib=args.memory_limit_mib,
                cgroup_memory_limit_mib=args.cgroup_memory_limit_mib,
                timeout=qasm2sop_timeout,
            ):
                run_instance(instance, provenance)

    if row_count == 0:
        parser.error("provide QSOP instances or --manifest")

    if args.format == "jsonl":
        return 1 if had_mismatch else 0
    else:
        print(render_markdown(rows))
    return 1 if had_mismatch else 0


if __name__ == "__main__":
    raise SystemExit(main())
