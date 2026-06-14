#!/usr/bin/env python3

import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_qasm_native_compat.py SOURCE_ROOT", file=sys.stderr)
        return 2

    source_root = pathlib.Path(sys.argv[1])
    sys.path.insert(0, str(source_root / "tools"))
    from qasm_native_compat import missing_native_compat_gates, qasm_with_native_compat_definitions

    qasm = """OPENQASM 2.0;
include "qelib1.inc";
qreg q[3];
iswap q[0],q[1];
ccz q[0],q[1],q[2];
"""
    patched = qasm_with_native_compat_definitions(qasm)
    if "gate iswap a,b" not in patched or "gate ccz a,b,c" not in patched:
        raise AssertionError(f"missing compatibility definitions:\n{patched}")
    if patched.index("gate iswap") > patched.index("qreg q[3];"):
        raise AssertionError(f"compatibility definitions were inserted after qreg:\n{patched}")
    if missing_native_compat_gates(patched):
        raise AssertionError(f"definitions should not be reported missing after patch:\n{patched}")
    if qasm_with_native_compat_definitions(patched) != patched:
        raise AssertionError("compatibility patching should be idempotent")

    defined = """OPENQASM 2.0;
gate iswap a,b { cx a,b; }
qreg q[2];
iswap q[0],q[1];
"""
    if "gate ccz" in qasm_with_native_compat_definitions(defined):
        raise AssertionError("unused gates should not be inserted")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
