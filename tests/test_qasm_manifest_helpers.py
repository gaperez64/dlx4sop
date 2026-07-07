#!/usr/bin/env python3

import importlib.util
import pathlib
import sys


# A hardware-native r(theta,phi) rotation gate, expressed the way qiskit's own QASM2 exporter
# emits it: as a macro whose body substitutes the call-site angle into compound pi arithmetic.
# With phi=pi/2, "-pi/2 + phi" and "pi/2 - phi" both cancel to exactly 0 -- this is the pattern
# that used to reach qasm2sop as "u(0.564, -pi/2 + pi/2, pi/2 - pi/2)" (internal whitespace from
# unfolded arithmetic), which its tokenizer mis-split. inline_simple_gates now folds it first.
MACRO_ARITHMETIC_QASM_SAMPLE = """OPENQASM 2.0;
include "qelib1.inc";
gate r(param0,param1) q0 {
u(param0, -pi/2 + param1, pi/2 - param1) q0;
}
qreg q[1];
r(0.564,pi/2) q[0];
"""

MACRO_ARITHMETIC_EXACT_QASM_SAMPLE = """OPENQASM 2.0;
include "qelib1.inc";
gate r(param0,param1) q0 {
u(param0, -pi/2 + param1, pi/2 - param1) q0;
}
qreg q[1];
r(pi/4,pi/2) q[0];
"""


def load_manifest_module(builder: pathlib.Path):
    spec = importlib.util.spec_from_file_location("build_external_qasm_manifest", builder)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_angle_folding(manifest_tool) -> None:
    cases = [
        ("0", "0"),
        ("pi", "pi"),
        ("-pi", "-pi"),
        ("pi/2", "pi/2"),
        ("-pi/2", "-pi/2"),
        ("-pi/2+pi/2", "0"),
        ("pi/2-pi/2", "0"),
        ("-pi/2 + pi/2", "0"),
        ("pi/4+pi/8", "3*pi/8"),
        ("3*pi/8", "3*pi/8"),
        ("-(pi/2)", "-pi/2"),
        ("0.564", "0.564"),
    ]
    for expr, expected in cases:
        got = manifest_tool.fold_angle_expression(expr)
        if got != expected:
            raise AssertionError(f"fold_angle_expression({expr!r}) = {got!r}, expected {expected!r}")

    inlined = manifest_tool.inline_simple_gates(MACRO_ARITHMETIC_QASM_SAMPLE)
    if "u(0.564,0,0) q[0];" not in inlined:
        raise AssertionError(f"macro arithmetic was not folded:\n{inlined}")
    manifest_tool.assert_no_internal_param_whitespace(inlined)

    inlined_exact = manifest_tool.inline_simple_gates(MACRO_ARITHMETIC_EXACT_QASM_SAMPLE)
    if "u(pi/4,0,0) q[0];" not in inlined_exact:
        raise AssertionError(f"exact macro arithmetic was not folded:\n{inlined_exact}")
    manifest_tool.assert_no_internal_param_whitespace(inlined_exact)

    unbalanced_class = manifest_tool.classify_error(
        "error: <stdin>:3: unbalanced gate parameter list"
    )
    if unbalanced_class != "parse_error.whitespace_inside_gate_params":
        raise AssertionError(f"unexpected classification for unbalanced parens: {unbalanced_class!r}")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_qasm_manifest_helpers.py MODULE_PATH", file=sys.stderr)
        return 2

    check_angle_folding(load_manifest_module(pathlib.Path(sys.argv[1])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
