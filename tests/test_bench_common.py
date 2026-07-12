#!/usr/bin/env python3
"""Exercise bench_common.import_qsop's exact-then-approx classification directly against
qasm2sop: a dyadic-refusing angle (falls back to --approx) and a non-angle failure (must not be
silently retried as approx)."""

from __future__ import annotations

import importlib.util
import pathlib
import sys

ANGLE_REFUSAL_QASM = 'OPENQASM 2.0;\ninclude "qelib1.inc";\nqreg q[1];\nrz(pi/3) q[0];\n'
NON_ANGLE_FAILURE_QASM = 'OPENQASM 2.0;\ninclude "qelib1.inc";\nqreg q[1];\nfoobar q[0];\n'


def load_module(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} BENCH_COMMON MANIFEST_TOOL QASM2SOP", file=sys.stderr)
        return 2

    bench_common = load_module("bench_common", pathlib.Path(sys.argv[1]))
    manifest_tool = load_module("build_external_qasm_manifest", pathlib.Path(sys.argv[2]))
    qasm2sop = sys.argv[3]

    qsop, mode, error_class, diagnostic = bench_common.import_qsop(
        qasm2sop, ANGLE_REFUSAL_QASM, "0", 30.0, manifest_tool)
    if mode != "approx":
        raise AssertionError(f"expected an angle refusal to fall back to approx, got mode={mode!r}")
    if not qsop.strip():
        raise AssertionError("approx fallback produced no qsop text")
    if not error_class:
        raise AssertionError("angle refusal did not populate an error_class")
    if "angle" not in diagnostic:
        raise AssertionError(f"expected the exact-mode diagnostic to mention 'angle': {diagnostic!r}")

    # exact_only must classify without materializing the (possibly huge) approximate qsop.
    qsop_only, mode_only, error_class_only, diagnostic_only = bench_common.import_qsop(
        qasm2sop, ANGLE_REFUSAL_QASM, "0", 30.0, manifest_tool, exact_only=True)
    if qsop_only != "" or mode_only != "approx" or not error_class_only or not diagnostic_only:
        raise AssertionError(
            f"exact_only classification mismatch: {(qsop_only, mode_only, error_class_only, diagnostic_only)!r}")

    try:
        bench_common.import_qsop(qasm2sop, NON_ANGLE_FAILURE_QASM, "0", 30.0, manifest_tool)
    except RuntimeError as exc:
        if "angle" in str(exc) or "non-sign quadratic" in str(exc):
            raise AssertionError(f"non-angle failure misclassified as approximable: {exc}") from exc
        if "foobar" not in str(exc):
            raise AssertionError(f"expected the original exact-mode error, got: {exc}") from exc
    else:
        raise AssertionError("a non-angle qasm2sop failure must not be silently swallowed")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
