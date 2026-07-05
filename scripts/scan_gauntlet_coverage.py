#!/usr/bin/env python3
"""Measure qasm2sop coverage of the qccq-gauntlet benchmark datasets.

For each requested gauntlet dataset this loads every QPY circuit, strips
final measurements, converts to OpenQASM 2 (qiskit.qasm2.dumps), and attempts
the real qasm2sop import for the all-zero boundary. Reuses the classify_error
taxonomy from build_external_qasm_manifest.py so failure categories match the
existing external-corpus tooling exactly.

qasm2sop supports two import modes:
  - exact (default): phases outside the finite grid are rejected.
  - approximate (--approx EPS): continuous/deep-dyadic phases and non-sign
    2-qubit couplings are rounded to a modulus chosen so the total additive
    amplitude error stays within EPS, certified via `c qasm2sop_approx ...`
    comment lines in the output (chosen modulus, rounded phase count,
    additive amplitude error bound).

Every case that fails the exact import gets a second attempt at two
epsilons: a loose 1e-3 budget (decides the "approximate" bucket cheaply) and
a tight 1e-8 budget matching the qccq-gauntlet's own zero-amplitude
correctness tolerance (characterizes the modulus/cost a real registration
would actually need). Only cases where a case fails at 1e-3 skip the 1e-8
attempt, since epsilon does not change which gates/features are recognized
-- a failure at the loose budget almost always fails for the same
non-epsilon reason at the tight one too.

Produces a per-dataset / per-benchmark-family coverage report (JSON + MD).
"""

from __future__ import annotations

import argparse
import collections
import importlib.util
import json
import math
import pathlib
import subprocess
import sys
import warnings

FORBIDDEN_OPS = {"measure", "reset", "initialize", "set_density_matrix", "set_statevector"}

DEFAULT_DATASETS = ["smoke", "inferq603", "mqt-easy", "mqt-fixed", "mqt2040", "supermarq"]

# Loose budget decides the "approximate" bucket cheaply (small modulus, fast).
# Tight budget matches the gauntlet's own zero-amplitude tolerance (1e-8
# absolute+relative, see qccq-gauntlet/src/gauntlet/runner.py) so its modulus
# and error-bound numbers are directly relevant to a real registration.
APPROX_EPSILONS = [1e-3, 1e-8]


def load_manifest_tool(tool_path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("build_external_qasm_manifest", tool_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def classify_angle(theta: float, max_k: int = 16, atol: float = 1e-9) -> int | None:
    """Smallest k with theta = m*pi/2^k (mod 2*pi) for some integer m, else None.

    atol is a fixed absolute tolerance on theta itself (radians), not scaled by
    2**k: the grid spacing at max_k=16 is pi/65536 ~= 4.8e-5, five orders of
    magnitude above atol, so a genuinely continuous angle cannot spuriously
    match a deep grid point. Scaling atol up with denom (as a naive rescaled
    check would) makes the test *looser* at high k and misclassifies
    continuous angles as dyadic -- this was caught empirically on the mqt-easy
    "ae" family, whose amplitude-estimation rotations are arctan-derived.
    """
    twopi = 2.0 * math.pi
    t = theta % twopi
    for k in range(0, max_k + 1):
        denom = 2**k
        m = round(t * denom / math.pi)
        candidate = (m * math.pi / denom) % twopi
        if abs(t - candidate) < atol:
            return k
    return None


_QASM_NON_GATE_KEYWORDS = ("OPENQASM", "include", "qreg", "creg", "barrier", "measure", "if", "reset")


def qasm_text_dyadic_class(qasm_text: str, manifest_tool) -> tuple[str, int | None]:
    """Diagnostic-only classification used for unsupported_angle sub-triage.

    Classifies angles from the dumped QASM text rather than the qiskit
    circuit object: composite/black-box gate wrappers (amplitude-estimation
    operators, QFT-dagger boxes, etc.) expose an empty `.params` at the top
    level of `circuit.data` even when they carry a continuous angle deep
    inside their `.definition` sub-circuit. `qasm2.dumps` already recursively
    flattens all such composites into elementary qelib1.inc gates (verified:
    zero emitted `gate` blocks across the sampled corpus), so the dumped text
    is both a faithful and a fully-flattened view of every numeric parameter
    qasm2sop itself will see.
    """
    max_k = 0
    any_param = False
    for raw in qasm_text.splitlines():
        statement = raw.split("//", 1)[0].strip()
        if not statement or not statement.endswith(";"):
            continue
        bare = statement[:-1].strip()
        if not bare or any(manifest_tool.starts_with_keyword(bare, kw) for kw in _QASM_NON_GATE_KEYWORDS):
            continue
        try:
            _name, params, _operands = manifest_tool.split_gate_invocation(bare)
        except Exception:
            continue
        for param_expr in params:
            try:
                theta = float(eval(param_expr, {"__builtins__": {}, "pi": math.pi}))
            except Exception:
                return "continuous", None
            any_param = True
            k = classify_angle(theta)
            if k is None:
                return "continuous", None
            max_k = max(max_k, k)
    if not any_param:
        return "no_params", 0
    return "dyadic", max_k


def forbidden_ops_present(circuit) -> set[str]:
    return {instr.operation.name for instr in circuit.data if instr.operation.name in FORBIDDEN_OPS}


def strip_unused_clbits(circuit):
    """Drop classical registers no instruction actually touches.

    Several MQT-derived QPY payloads carry a dangling `creg` declaration left
    over from textual measurement-stripping upstream (the source QASM's
    `measure`/`if` lines were removed but the `creg` line was not). qasm2sop's
    parser hard-rejects any `creg`/`measure` line as a dynamic/classical
    feature, even when the register is provably inert. A real adapter would
    need this same preprocessing, so it belongs here rather than being
    reported as a false "non-unitary" rejection.
    """
    if circuit.num_clbits == 0:
        return circuit
    live = any(
        len(instr.clbits) > 0 or getattr(instr.operation, "condition", None) is not None
        for instr in circuit.data
    )
    if live:
        return circuit
    from qiskit import QuantumCircuit

    stripped = QuantumCircuit(*circuit.qregs)
    for instr in circuit.data:
        stripped.append(instr.operation, instr.qubits, [])
    return stripped


def nvars_tier(nvars: int) -> str:
    if nvars <= 32:
        return "0-32"
    if nvars <= 64:
        return "33-64"
    if nvars <= 128:
        return "65-128"
    if nvars <= 256:
        return "129-256"
    if nvars <= 512:
        return "257-512"
    return "512+"


def parse_qsop_with_approx(text: str) -> dict:
    """Parse the `p qsop-sign ...` header plus any `c qasm2sop_approx ...` lines."""
    result: dict = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 5 and parts[:2] == ["p", "qsop-sign"]:
            result["modulus"] = int(parts[2])
            result["nvars"] = int(parts[3])
            result["nedges"] = int(parts[4])
        elif len(parts) >= 4 and parts[:2] == ["c", "qasm2sop_approx"]:
            key = parts[2]
            value = parts[3]
            if key in ("epsilon", "additive_amplitude_error_bound"):
                result[f"approx_{key}"] = float(value)
            else:
                result[f"approx_{key}"] = int(value)
    if "nvars" not in result:
        raise RuntimeError("missing QSOP header")
    return result


def import_boundary_metadata_approx(
    qasm2sop: pathlib.Path, qasm: str, input_bits: str, output_bits: str, epsilon: float
) -> dict:
    completed = subprocess.run(
        [str(qasm2sop), "--approx", repr(epsilon), "--input", input_bits, "--output", output_bits, "-"],
        check=False,
        input=qasm,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip().splitlines()[-1] if completed.stderr.strip() else "qasm2sop failed"
        raise RuntimeError(diagnostic)
    return parse_qsop_with_approx(completed.stdout)


def scan_dataset(
    gauntlet_root: pathlib.Path,
    dataset: str,
    qasm2sop: pathlib.Path,
    manifest_tool,
    limit: int | None,
) -> list[dict]:
    import yaml
    from qiskit import qpy

    manifest_path = gauntlet_root / "datasets" / dataset / "v1" / "manifest.yaml"
    with manifest_path.open() as f:
        manifest = yaml.safe_load(f)

    records = []
    cases = manifest["cases"]
    if limit is not None:
        cases = cases[:limit]

    for case in cases:
        payload_path = gauntlet_root / "datasets" / dataset / "v1" / case["payload"]
        meta = case.get("metadata", {})
        record = {
            "dataset": dataset,
            "case_id": case["id"],
            "benchmark": meta.get("benchmark", "(none)"),
            "qubits": meta.get("qubits"),
            "operations": meta.get("operations"),
        }

        if payload_path.suffix.lower() != ".qpy":
            record["status"] = "skipped_non_qpy"
            records.append(record)
            continue

        try:
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                with payload_path.open("rb") as f:
                    circuits = qpy.load(f)
            circuit = circuits[0]
        except Exception as exc:
            record["status"] = "qpy_load_error"
            record["diagnostic"] = str(exc).splitlines()[-1] if str(exc) else repr(exc)
            records.append(record)
            continue

        circuit = circuit.copy()
        circuit.remove_final_measurements(inplace=True)
        circuit = strip_unused_clbits(circuit)
        forbidden = forbidden_ops_present(circuit)
        if forbidden or circuit.num_clbits:
            record["status"] = "non_unitary_forbidden_op"
            record["diagnostic"] = f"forbidden ops: {sorted(forbidden)}, clbits={circuit.num_clbits}"
            records.append(record)
            continue

        import qiskit.qasm2 as qasm2mod

        try:
            qasm_text = qasm2mod.dumps(circuit)
        except Exception as exc:
            record["status"] = "qasm2_dump_error"
            record["diagnostic"] = str(exc).splitlines()[-1] if str(exc) else repr(exc)
            records.append(record)
            continue

        try:
            qasm_text = manifest_tool.inline_simple_gates(qasm_text)
        except Exception:
            pass  # defensive only; dumps() has not emitted gate defs in practice

        nqubits = manifest_tool.qasm_qubits(qasm_text)
        zero = "0" * nqubits

        try:
            metadata = manifest_tool.import_boundary_metadata(qasm2sop, qasm_text, zero, zero)
        except Exception as exc:
            diagnostic = manifest_tool.diagnostic_from_exception(exc)
            status = manifest_tool.classify_error(diagnostic)
            record["status_exact"] = status
            record["diagnostic_exact"] = diagnostic
            if status == "unsupported_angle":
                angle_class, max_k = qasm_text_dyadic_class(qasm_text, manifest_tool)
                record["angle_subclass"] = angle_class
                record["angle_max_k"] = max_k

            for epsilon in APPROX_EPSILONS:
                try:
                    approx_meta = import_boundary_metadata_approx(qasm2sop, qasm_text, zero, zero, epsilon)
                except Exception as approx_exc:
                    approx_diag = manifest_tool.diagnostic_from_exception(approx_exc)
                    record[f"status_approx_{epsilon:g}"] = manifest_tool.classify_error(approx_diag)
                    record[f"diagnostic_approx_{epsilon:g}"] = approx_diag
                    # A tighter epsilon only ever needs an equal-or-larger modulus, so a
                    # failure at a looser budget cannot be rescued by a tighter one --
                    # whatever gate/feature/overflow caused it is not epsilon-dependent.
                    break
                else:
                    record[f"status_approx_{epsilon:g}"] = "ok"
                    record[f"modulus_approx_{epsilon:g}"] = approx_meta["modulus"]
                    record[f"nvars_approx_{epsilon:g}"] = approx_meta["nvars"]
                    record[f"error_bound_approx_{epsilon:g}"] = approx_meta.get(
                        "approx_additive_amplitude_error_bound"
                    )

            record["status"] = "ok" if record.get(f"status_approx_{APPROX_EPSILONS[0]:g}") == "ok" else status
            records.append(record)
            continue

        record["status_exact"] = "ok"
        record["status"] = "ok"
        record["nvars"] = int(metadata["nvars"])
        record["nedges"] = int(metadata["nedges"])
        record["nvars_tier"] = nvars_tier(int(metadata["nvars"]))
        records.append(record)

    return records


def approx_status_of(record: dict) -> str | None:
    for epsilon in APPROX_EPSILONS:
        if record.get(f"status_approx_{epsilon:g}") == "ok":
            return f"{epsilon:g}"
    return None


BUCKETS = {
    "exact": lambda r: r.get("status_exact") == "ok",
    "approximate": lambda r: r.get("status_exact") != "ok" and approx_status_of(r) is not None,
    "unsupported": lambda r: (
        r.get("status_exact") != "ok"
        and approx_status_of(r) is None
        and (
            r.get("status_exact") in ("dynamic_classical", "non_unitary_forbidden_op")
            or (r.get("status_exact") == "unsupported_angle" and r.get("angle_subclass") == "continuous")
        )
    ),
}


def bucket_of(record: dict) -> str:
    for name, pred in BUCKETS.items():
        if pred(record):
            return name
    return "needs_triage"


def aggregate(records: list[dict]) -> dict:
    by_dataset: dict[str, dict] = {}
    by_family: dict[tuple[str, str], dict] = {}

    def bump(target: dict, record: dict) -> None:
        target["total"] = target.get("total", 0) + 1
        bucket = bucket_of(record)
        target.setdefault("buckets", collections.Counter())[bucket] += 1
        target.setdefault("statuses_exact", collections.Counter())[record.get("status_exact", "?")] += 1
        if bucket == "approximate":
            eps = approx_status_of(record)
            target.setdefault("approx_epsilon_used", collections.Counter())[eps] += 1

    for r in records:
        bump(by_dataset.setdefault(r["dataset"], {}), r)
        bump(by_family.setdefault((r["dataset"], r["benchmark"]), {}), r)

    def finalize(d: dict) -> dict:
        out = dict(d)
        out["buckets"] = dict(sorted(d.get("buckets", {}).items()))
        out["statuses_exact"] = dict(sorted(d.get("statuses_exact", {}).items()))
        out["approx_epsilon_used"] = dict(sorted(d.get("approx_epsilon_used", {}).items()))
        return out

    return {
        "by_dataset": {k: finalize(v) for k, v in sorted(by_dataset.items())},
        "by_family": {f"{k[0]}/{k[1]}": finalize(v) for k, v in sorted(by_family.items())},
    }


def write_markdown(records: list[dict], summary: dict, file) -> None:
    total = len(records)
    overall = collections.Counter(bucket_of(r) for r in records)
    print("# qccq-gauntlet coverage report\n", file=file)
    print(
        f"Scanned {total} fixed-boundary cases across "
        f"{len(summary['by_dataset'])} datasets. Every case that fails the exact import is "
        f"retried with `--approx` at {', '.join(f'{e:g}' for e in APPROX_EPSILONS)} "
        f"(additive amplitude error budget); the tighter budget matches the gauntlet's own "
        f"zero-amplitude correctness tolerance (1e-8).\n",
        file=file,
    )
    print("## Overall\n", file=file)
    print("| Bucket | Rows | Share |", file=file)
    print("| --- | ---: | ---: |", file=file)
    for bucket in ("exact", "approximate", "unsupported", "needs_triage"):
        n = overall.get(bucket, 0)
        pct = 100.0 * n / total if total else 0.0
        print(f"| {bucket} | {n} | {pct:.1f}% |", file=file)
    print(file=file)

    print("## By dataset\n", file=file)
    print("| Dataset | Total | Exact | Approximate | Unsupported | Needs triage |", file=file)
    print("| --- | ---: | ---: | ---: | ---: | ---: |", file=file)
    for name, entry in summary["by_dataset"].items():
        b = entry["buckets"]
        print(
            f"| {name} | {entry['total']} | {b.get('exact', 0)} | "
            f"{b.get('approximate', 0)} | {b.get('unsupported', 0)} | "
            f"{b.get('needs_triage', 0)} |",
            file=file,
        )
    print(file=file)

    print("## By benchmark family\n", file=file)
    print("| Dataset/Family | Total | Exact | Approximate | Unsupported | Needs triage |", file=file)
    print("| --- | ---: | ---: | ---: | ---: | ---: |", file=file)
    for name, entry in summary["by_family"].items():
        b = entry["buckets"]
        print(
            f"| {name} | {entry['total']} | {b.get('exact', 0)} | "
            f"{b.get('approximate', 0)} | {b.get('unsupported', 0)} | "
            f"{b.get('needs_triage', 0)} |",
            file=file,
        )
    print(file=file)

    print("## Approximate-import cost\n", file=file)
    print(
        "For rows that only import approximately, this reports the modulus/variable-count "
        "qasm2sop needs at each tested error budget, plus the *achieved* error (often far "
        "tighter than requested -- see below) and how often the tight 1e-8 budget (the "
        "gauntlet's own correctness tolerance) overflows qasm2sop's `uint32_t` modulus ceiling "
        "outright rather than genuinely being infeasible.\n",
        file=file,
    )
    for epsilon in APPROX_EPSILONS:
        key = f"{epsilon:g}"
        print(f"### Budget {key}\n", file=file)
        print(
            "| Dataset/Family | Rows fit | Modulus overflow | Median modulus | Max modulus | "
            "Median achieved error |",
            file=file,
        )
        print("| --- | ---: | ---: | ---: | ---: | ---: |", file=file)
        for name, entry_records in _group_by_family(records).items():
            attempted = [r for r in entry_records if f"status_approx_{key}" in r]
            fit = [r for r in attempted if r[f"status_approx_{key}"] == "ok"]
            overflow = [
                r for r in attempted
                if r[f"status_approx_{key}"] != "ok"
                and "modulus larger than" in (r.get(f"diagnostic_approx_{key}") or "")
            ]
            if not attempted:
                continue
            moduli = sorted(r[f"modulus_approx_{key}"] for r in fit)
            errors = sorted(r[f"error_bound_approx_{key}"] for r in fit if r.get(f"error_bound_approx_{key}") is not None)
            median_mod = moduli[len(moduli) // 2] if moduli else "-"
            max_mod = max(moduli) if moduli else "-"
            median_err = f"{errors[len(errors) // 2]:.2e}" if errors else "-"
            print(
                f"| {name} | {len(fit)}/{len(attempted)} | {len(overflow)} | {median_mod} | "
                f"{max_mod} | {median_err} |",
                file=file,
            )
        print(file=file)

    print("## Status detail (exact-import classification)\n", file=file)
    status_counts = collections.Counter(r.get("status_exact", "?") for r in records)
    print("| Status | Rows |", file=file)
    print("| --- | ---: |", file=file)
    for status, n in sorted(status_counts.items(), key=lambda kv: -kv[1]):
        print(f"| {status} | {n} |", file=file)
    print(file=file)

    needs_triage = [r for r in records if bucket_of(r) == "needs_triage"]
    if needs_triage:
        print("## Needs-triage samples\n", file=file)
        print("| Dataset/Family | Case | Exact status | Approx (1e-3) status | Diagnostic |", file=file)
        print("| --- | --- | --- | --- | --- |", file=file)
        seen_status: set[tuple[str, str]] = set()
        for r in needs_triage:
            key = (r.get("status_exact", "?"), r.get(f"status_approx_{APPROX_EPSILONS[0]:g}", "?"))
            if key in seen_status:
                continue
            seen_status.add(key)
            # Show whichever diagnostic actually explains this bucket: the approx one
            # when an approx attempt ran and failed, else the exact one.
            diag_key = f"diagnostic_approx_{APPROX_EPSILONS[0]:g}" if f"status_approx_{APPROX_EPSILONS[0]:g}" in r else "diagnostic_exact"
            diag = (r.get(diag_key) or r.get("diagnostic_exact") or "").replace("|", "\\|")[:120]
            print(
                f"| {r['dataset']}/{r['benchmark']} | {r['case_id']} | {key[0]} | {key[1]} | {diag} |",
                file=file,
            )
        print(file=file)


def _group_by_family(records: list[dict]) -> dict[str, list[dict]]:
    groups: dict[str, list[dict]] = {}
    for r in records:
        groups.setdefault(f"{r['dataset']}/{r['benchmark']}", []).append(r)
    return dict(sorted(groups.items()))


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qasm2sop", type=pathlib.Path)
    parser.add_argument("--gauntlet-root", type=pathlib.Path, required=True)
    parser.add_argument("--datasets", default=",".join(DEFAULT_DATASETS))
    parser.add_argument("--limit-per-dataset", type=int, default=None)
    parser.add_argument("--out-json", type=pathlib.Path, required=True)
    parser.add_argument("--out-md", type=pathlib.Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    tool_path = pathlib.Path(__file__).parent / "build_external_qasm_manifest.py"
    manifest_tool = load_manifest_tool(tool_path)

    all_records: list[dict] = []
    for dataset in args.datasets.split(","):
        dataset = dataset.strip()
        if not dataset:
            continue
        print(f"scanning {dataset} ...", file=sys.stderr)
        records = scan_dataset(
            args.gauntlet_root, dataset, args.qasm2sop, manifest_tool, args.limit_per_dataset
        )
        all_records.extend(records)
        print(f"  {len(records)} cases", file=sys.stderr)

    summary = aggregate(all_records)
    args.out_json.write_text(
        json.dumps({"records": all_records, "summary": summary}, indent=2, sort_keys=True) + "\n"
    )
    with args.out_md.open("w") as f:
        write_markdown(all_records, summary, f)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
