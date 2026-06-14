#!/usr/bin/env python3

import cmath
import math
import pathlib
import re
import subprocess
import sys


QREG_RE = re.compile(r"^qreg\s+([A-Za-z_][A-Za-z0-9_]*)\[(\d+)\]$")


def parse_angle(text: str) -> float:
    sign = 1.0
    if text.startswith("-"):
        sign = -1.0
        text = text[1:]
    elif text.startswith("+"):
        text = text[1:]

    if text == "0":
        return 0.0

    multiplier = 1.0
    if "*" in text:
        multiplier_text, text = text.split("*", 1)
        multiplier = float(int(multiplier_text))

    if text == "pi":
        denominator = 1.0
    elif text.startswith("pi/"):
        denominator = float(int(text[3:]))
    else:
        raise AssertionError(f"unsupported test angle {text!r}")

    return sign * multiplier * math.pi / denominator


def split_statements(qasm: str) -> list[str]:
    statements: list[str] = []
    for line in qasm.splitlines():
        line = line.split("//", 1)[0].strip()
        if not line:
            continue
        statements.extend(part.strip() for part in line.split(";") if part.strip())
    return statements


def parse_qasm(qasm: str) -> tuple[list[tuple[str, list[str], list[float]]], dict[str, tuple[int, int]], int]:
    regs: dict[str, tuple[int, int]] = {}
    nqubits = 0
    gates: list[tuple[str, list[str], list[float]]] = []

    for statement in split_statements(qasm):
        if statement == "OPENQASM 2.0" or statement.startswith("include "):
            continue
        match = QREG_RE.match(statement)
        if match is not None:
            name = match.group(1)
            size = int(match.group(2))
            regs[name] = (nqubits, size)
            nqubits += size
            continue
        if statement.startswith("barrier"):
            continue

        gate, rest = statement.split(None, 1)
        params: list[float] = []
        for prefix in ("u3", "u", "u2", "u1", "p", "rz", "rx", "ry", "cu1", "cp", "crz"):
            if gate.startswith(f"{prefix}(") and gate.endswith(")"):
                params = [
                    parse_angle(part.strip()) for part in gate[len(prefix) + 1 : -1].split(",")
                ]
                gate = prefix
                break
        operands = [operand.strip() for operand in rest.split(",")]
        gates.append((gate, operands, params))

    return gates, regs, nqubits


def operand_qubits(operand: str, regs: dict[str, tuple[int, int]]) -> list[int]:
    if "[" in operand:
        name, index_text = operand[:-1].split("[", 1)
        offset, size = regs[name]
        index = int(index_text)
        if index >= size:
            raise AssertionError(f"qreg index out of range in test operand {operand!r}")
        return [offset + index]

    offset, size = regs[operand]
    return list(range(offset, offset + size))


def state_index(bits: str) -> int:
    return sum((1 << i) for i, bit in enumerate(bits) if bit == "1")


def apply_one(state: list[complex], nqubits: int, qubit: int, matrix: tuple[complex, complex, complex, complex]) -> None:
    bit = 1 << qubit
    a, b, c, d = matrix
    for base in range(1 << nqubits):
        if base & bit:
            continue
        other = base | bit
        old_zero = state[base]
        old_one = state[other]
        state[base] = a * old_zero + b * old_one
        state[other] = c * old_zero + d * old_one


def apply_controlled_x(state: list[complex], nqubits: int, control: int, target: int) -> None:
    control_bit = 1 << control
    target_bit = 1 << target
    for base in range(1 << nqubits):
        if (base & control_bit) == 0 or (base & target_bit) != 0:
            continue
        other = base | target_bit
        state[base], state[other] = state[other], state[base]


def apply_controlled_y(state: list[complex], nqubits: int, control: int, target: int) -> None:
    control_bit = 1 << control
    target_bit = 1 << target
    for base in range(1 << nqubits):
        if (base & control_bit) == 0 or (base & target_bit) != 0:
            continue
        other = base | target_bit
        old_zero = state[base]
        old_one = state[other]
        state[base] = -1j * old_one
        state[other] = 1j * old_zero


def apply_controlled_phase(state: list[complex], nqubits: int, control: int, target: int, angle: float) -> None:
    control_bit = 1 << control
    target_bit = 1 << target
    phase = cmath.exp(1j * angle)
    for index in range(1 << nqubits):
        if (index & control_bit) != 0 and (index & target_bit) != 0:
            state[index] *= phase


def apply_swap(state: list[complex], nqubits: int, left: int, right: int) -> None:
    left_bit = 1 << left
    right_bit = 1 << right
    for index in range(1 << nqubits):
        if (index & left_bit) != 0 or (index & right_bit) == 0:
            continue
        other = (index | left_bit) & ~right_bit
        state[index], state[other] = state[other], state[index]


def apply_ccz(state: list[complex], nqubits: int, first: int, second: int, third: int) -> None:
    first_bit = 1 << first
    second_bit = 1 << second
    third_bit = 1 << third
    for index in range(1 << nqubits):
        if (index & first_bit) != 0 and (index & second_bit) != 0 and (index & third_bit) != 0:
            state[index] *= -1


def apply_ccx(state: list[complex], nqubits: int, first: int, second: int, target: int) -> None:
    first_bit = 1 << first
    second_bit = 1 << second
    target_bit = 1 << target
    for base in range(1 << nqubits):
        if (base & first_bit) == 0 or (base & second_bit) == 0 or (base & target_bit) != 0:
            continue
        other = base | target_bit
        state[base], state[other] = state[other], state[base]


def apply_cswap(state: list[complex], nqubits: int, control: int, left: int, right: int) -> None:
    control_bit = 1 << control
    left_bit = 1 << left
    right_bit = 1 << right
    for index in range(1 << nqubits):
        if (index & control_bit) == 0 or (index & left_bit) != 0 or (index & right_bit) == 0:
            continue
        other = (index | left_bit) & ~right_bit
        state[index], state[other] = state[other], state[index]


def u3_matrix(theta: float, phi: float, lam: float) -> tuple[complex, complex, complex, complex]:
    return (
        math.cos(theta / 2.0),
        -cmath.exp(1j * lam) * math.sin(theta / 2.0),
        cmath.exp(1j * phi) * math.sin(theta / 2.0),
        cmath.exp(1j * (phi + lam)) * math.cos(theta / 2.0),
    )


def simulate_qasm(qasm: str, input_bits: str, output_bits: str) -> complex:
    gates, regs, nqubits = parse_qasm(qasm)
    state = [0j] * (1 << nqubits)
    state[state_index(input_bits)] = 1.0 + 0j

    inv_sqrt2 = 1.0 / math.sqrt(2.0)
    one_qubit = {
        "id": (1, 0, 0, 1),
        "h": (inv_sqrt2, inv_sqrt2, inv_sqrt2, -inv_sqrt2),
        "x": (0, 1, 1, 0),
        "y": (0, -1j, 1j, 0),
        "sx": (0.5 + 0.5j, 0.5 - 0.5j, 0.5 - 0.5j, 0.5 + 0.5j),
        "sxdg": (0.5 - 0.5j, 0.5 + 0.5j, 0.5 + 0.5j, 0.5 - 0.5j),
        "z": (1, 0, 0, -1),
        "s": (1, 0, 0, 1j),
        "sdg": (1, 0, 0, -1j),
        "t": (1, 0, 0, cmath.exp(1j * math.pi / 4.0)),
        "tdg": (1, 0, 0, cmath.exp(-1j * math.pi / 4.0)),
    }
    controlled_phase_angles = {
        "cs": math.pi / 2.0,
        "ct": math.pi / 4.0,
        "csdg": -math.pi / 2.0,
        "ctdg": -math.pi / 4.0,
    }

    for gate, operands, params in gates:
        angle = params[0] if params else 0.0
        if gate in ("u3", "u"):
            matrix = u3_matrix(params[0], params[1], params[2])
            for qubit in operand_qubits(operands[0], regs):
                apply_one(state, nqubits, qubit, matrix)
            continue
        if gate == "u2":
            matrix = u3_matrix(math.pi / 2.0, params[0], params[1])
            for qubit in operand_qubits(operands[0], regs):
                apply_one(state, nqubits, qubit, matrix)
            continue
        if gate in ("u1", "p"):
            matrix = (1, 0, 0, cmath.exp(1j * angle))
            for qubit in operand_qubits(operands[0], regs):
                apply_one(state, nqubits, qubit, matrix)
            continue
        if gate == "rz":
            matrix = (cmath.exp(-0.5j * angle), 0, 0, cmath.exp(0.5j * angle))
            for qubit in operand_qubits(operands[0], regs):
                apply_one(state, nqubits, qubit, matrix)
            continue
        if gate == "rx":
            matrix = (
                math.cos(angle / 2.0),
                -1j * math.sin(angle / 2.0),
                -1j * math.sin(angle / 2.0),
                math.cos(angle / 2.0),
            )
            for qubit in operand_qubits(operands[0], regs):
                apply_one(state, nqubits, qubit, matrix)
            continue
        if gate == "ry":
            matrix = (
                math.cos(angle / 2.0),
                -math.sin(angle / 2.0),
                math.sin(angle / 2.0),
                math.cos(angle / 2.0),
            )
            for qubit in operand_qubits(operands[0], regs):
                apply_one(state, nqubits, qubit, matrix)
            continue
        if gate in one_qubit:
            for qubit in operand_qubits(operands[0], regs):
                apply_one(state, nqubits, qubit, one_qubit[gate])
            continue

        if gate in ("ccz", "ccx", "cswap"):
            first = operand_qubits(operands[0], regs)
            second = operand_qubits(operands[1], regs)
            third = operand_qubits(operands[2], regs)
            if len(first) != len(second) or len(first) != len(third):
                raise AssertionError("test qreg operands must have matching sizes")
            for a, b, c in zip(first, second, third):
                if gate == "ccz":
                    apply_ccz(state, nqubits, a, b, c)
                elif gate == "ccx":
                    apply_ccx(state, nqubits, a, b, c)
                else:
                    apply_cswap(state, nqubits, a, b, c)
            continue

        left = operand_qubits(operands[0], regs)
        right = operand_qubits(operands[1], regs)
        if len(left) != len(right):
            raise AssertionError("test qreg operands must have matching sizes")

        for a, b in zip(left, right):
            if gate == "cz":
                apply_controlled_phase(state, nqubits, a, b, math.pi)
            elif gate in controlled_phase_angles:
                apply_controlled_phase(state, nqubits, a, b, controlled_phase_angles[gate])
            elif gate in ("cu1", "cp"):
                apply_controlled_phase(state, nqubits, a, b, angle)
            elif gate == "crz":
                control_bit = 1 << a
                target_bit = 1 << b
                for index in range(1 << nqubits):
                    if (index & control_bit) != 0:
                        if (index & target_bit) == 0:
                            state[index] *= cmath.exp(-0.5j * angle)
                        else:
                            state[index] *= cmath.exp(0.5j * angle)
            elif gate == "cx":
                apply_controlled_x(state, nqubits, a, b)
            elif gate == "cy":
                apply_controlled_y(state, nqubits, a, b)
            elif gate == "swap":
                apply_swap(state, nqubits, a, b)
            else:
                raise AssertionError(f"unsupported test gate {gate!r}")

    return state[state_index(output_bits)]


def parse_solver_amplitude(output: str) -> complex:
    modulus = None
    norm_h = None
    counts = None
    for line in output.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[:2] == ["p", "qsop-result"]:
            modulus = int(parts[2])
        elif parts[0] == "n":
            norm_h = int(parts[1])
        elif parts[0] == "counts":
            counts = [int(part) for part in parts[1:]]

    if modulus is None or norm_h is None or counts is None:
        raise AssertionError(f"malformed solver output:\n{output}")

    omega = cmath.exp(2j * math.pi / modulus)
    total = sum(count * (omega**residue) for residue, count in enumerate(counts))
    return total * (2.0 ** (-norm_h / 2.0))


def sop_amplitude(qasm2sop: pathlib.Path, sop_solve: pathlib.Path, qasm: str, input_bits: str, output_bits: str) -> complex:
    imported = subprocess.run(
        [str(qasm2sop), "--input", input_bits, "--output", output_bits, "-"],
        input=qasm,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if imported.returncode != 0:
        raise AssertionError(f"qasm2sop failed\n{imported.stderr}")

    solved = subprocess.run(
        [str(sop_solve), "-"],
        input=imported.stdout,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if solved.returncode != 0:
        raise AssertionError(f"sop-solve failed\n{solved.stderr}\nQSOP:\n{imported.stdout}")
    return parse_solver_amplitude(solved.stdout)


def assert_close(name: str, expected: complex, actual: complex) -> None:
    if abs(expected - actual) > 1e-9:
        raise AssertionError(f"{name}: amplitude mismatch expected {expected!r}, got {actual!r}")


def run_amplitude_cases(qasm2sop: pathlib.Path, sop_solve: pathlib.Path) -> None:
    cases = [
        (
            "bell_cx",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[2];
            h q[0];
            cx q[0], q[1];
            """,
            [("00", "00"), ("00", "11"), ("00", "01"), ("10", "01")],
        ),
        (
            "phase_mix",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[2];
            h q;
            sx q[1];
            p(pi/2) q[0];
            p(pi/8) q[1];
            cp(pi/4) q[0], q[1];
            cp(-7*pi/8) q[1], q[0];
            sxdg q[1];
            cy q[1], q[0];
            """,
            [("00", "00"), ("00", "10"), ("11", "01"), ("10", "11")],
        ),
        (
            "register_pair",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg a[2];
            qreg b[2];
            h a;
            cx a, b;
            cp(3*pi/4) a, b;
            swap a[0], b[1];
            """,
            [("0000", "0000"), ("0000", "1111"), ("1010", "1110"), ("0101", "0110")],
        ),
        (
            "controlled_phase_zero",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[2];
            cu1(pi/4) q[0], q[1];
            """,
            [("11", "11"), ("11", "10"), ("01", "01"), ("00", "00")],
        ),
        (
            "rz_crz",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[2];
            h q;
            rz(pi/4) q[0];
            crz(pi/4) q[0], q[1];
            """,
            [("00", "00"), ("00", "11"), ("10", "10"), ("11", "01")],
        ),
        (
            "axis_rotations",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[2];
            h q;
            rx(pi/4) q[0];
            ry(-pi/2) q[1];
            cx q[0], q[1];
            """,
            [("00", "00"), ("00", "11"), ("10", "01"), ("11", "10")],
        ),
        (
            "u_gates",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[2];
            u2(pi/4,-pi/2) q[0];
            u(pi/4,pi/4,pi/2) q[1];
            cx q[0], q[1];
            """,
            [("00", "00"), ("00", "11"), ("10", "01"), ("11", "10")],
        ),
        (
            "named_controlled_phase",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[2];
            h q;
            cs q[0], q[1];
            ctdg q[0], q[1];
            """,
            [("00", "00"), ("00", "11"), ("11", "11"), ("10", "01")],
        ),
        (
            "ccz_phase",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[3];
            ccz q[0], q[1], q[2];
            """,
            [("111", "111"), ("110", "110"), ("111", "110")],
        ),
        (
            "ccx_flip",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[3];
            ccx q[0], q[1], q[2];
            """,
            [("110", "111"), ("111", "110"), ("010", "010")],
        ),
        (
            "cswap",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[3];
            cswap q[0], q[1], q[2];
            """,
            [("101", "110"), ("110", "101"), ("001", "001"), ("100", "100")],
        ),
    ]

    for name, qasm, boundaries in cases:
        for input_bits, output_bits in boundaries:
            expected = simulate_qasm(qasm, input_bits, output_bits)
            actual = sop_amplitude(qasm2sop, sop_solve, qasm, input_bits, output_bits)
            assert_close(f"{name} {input_bits}->{output_bits}", expected, actual)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_qasm_amplitudes.py QASM2SOP SOP_SOLVE", file=sys.stderr)
        return 2

    run_amplitude_cases(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
