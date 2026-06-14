#!/usr/bin/env python3

import re
from collections.abc import Iterable


NATIVE_COMPAT_GATE_DEFINITIONS = {
    "ccz": "gate ccz a,b,c { h c; ccx a,b,c; h c; }",
    "iswap": "gate iswap a,b { s a; s b; h a; cx a,b; cx b,a; h b; }",
}


def defined_gate_names(qasm: str) -> set[str]:
    return set(re.findall(r"(?m)^\s*gate\s+([A-Za-z_][A-Za-z0-9_]*)\b", qasm))


def uses_gate(qasm: str, name: str) -> bool:
    pattern = rf"(?m)^\s*(?:if\s*\([^)]*\)\s*)?{re.escape(name)}\s*(?:\(|[A-Za-z_][A-Za-z0-9_]*|\[)"
    return re.search(pattern, qasm) is not None


def missing_native_compat_gates(qasm: str, gates: Iterable[str] | None = None) -> list[str]:
    gate_names = list(gates or NATIVE_COMPAT_GATE_DEFINITIONS)
    defined = defined_gate_names(qasm)
    return [name for name in gate_names if name not in defined and uses_gate(qasm, name)]


def qasm_with_native_compat_definitions(qasm: str, gates: Iterable[str] | None = None) -> str:
    missing = missing_native_compat_gates(qasm, gates)
    if not missing:
        return qasm

    lines = qasm.splitlines()
    insert_at = 0
    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("OPENQASM") or stripped.startswith("include "):
            insert_at = index + 1
            continue
        if not stripped or stripped.startswith("//"):
            continue
        break

    definitions = [NATIVE_COMPAT_GATE_DEFINITIONS[name] for name in missing]
    patched = lines[:insert_at] + definitions + lines[insert_at:]
    suffix = "\n" if qasm.endswith("\n") else ""
    return "\n".join(patched) + suffix


def pyzx_state_index(bits: str, qubits: int) -> int:
    if len(bits) > qubits:
        raise ValueError(f"state bit string has {len(bits)} bits for {qubits} qubits")
    return int(bits.ljust(qubits, "0"), 2) if bits else 0
