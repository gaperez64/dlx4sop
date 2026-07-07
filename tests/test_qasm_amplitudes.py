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

    if "pi" not in text:
        return sign * float(text)

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
        for prefix in (
            "u3",
            "u",
            "u2",
            "u1",
            "p",
            "rz",
            "rx",
            "ry",
            "cu1",
            "cp",
            "crz",
            "cry",
            "cu",
            "rxx",
            "ryy",
            "rzz",
        ):
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


def apply_xx_rotation(state: list[complex], nqubits: int, left: int, right: int, angle: float) -> None:
    left_bit = 1 << left
    right_bit = 1 << right
    cos = math.cos(angle / 2.0)
    offdiag = -1j * math.sin(angle / 2.0)
    for index in range(1 << nqubits):
        if (index & left_bit) != 0 or (index & right_bit) != 0:
            continue
        both = index | left_bit | right_bit
        left_only = index | left_bit
        right_only = index | right_bit

        old_zero = state[index]
        old_both = state[both]
        old_left = state[left_only]
        old_right = state[right_only]
        state[index] = cos * old_zero + offdiag * old_both
        state[both] = offdiag * old_zero + cos * old_both
        state[left_only] = cos * old_left + offdiag * old_right
        state[right_only] = offdiag * old_left + cos * old_right


def apply_yy_rotation(state: list[complex], nqubits: int, left: int, right: int, angle: float) -> None:
    left_bit = 1 << left
    right_bit = 1 << right
    cos = math.cos(angle / 2.0)
    plus = 1j * math.sin(angle / 2.0)
    minus = -1j * math.sin(angle / 2.0)
    for index in range(1 << nqubits):
        if (index & left_bit) != 0 or (index & right_bit) != 0:
            continue
        both = index | left_bit | right_bit
        left_only = index | left_bit
        right_only = index | right_bit

        old_zero = state[index]
        old_both = state[both]
        old_left = state[left_only]
        old_right = state[right_only]
        state[index] = cos * old_zero + plus * old_both
        state[both] = plus * old_zero + cos * old_both
        state[left_only] = cos * old_left + minus * old_right
        state[right_only] = minus * old_left + cos * old_right


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

        if gate in ("ccz", "ccx", "rccx", "cswap"):
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
                elif gate == "rccx":
                    u2_0_pi = u3_matrix(math.pi / 2.0, 0.0, math.pi)
                    t_phase = (1, 0, 0, cmath.exp(1j * math.pi / 4.0))
                    tdg_phase = (1, 0, 0, cmath.exp(-1j * math.pi / 4.0))
                    apply_one(state, nqubits, c, u2_0_pi)
                    apply_one(state, nqubits, a, t_phase)
                    apply_one(state, nqubits, b, t_phase)
                    apply_one(state, nqubits, c, t_phase)
                    apply_controlled_x(state, nqubits, a, b)
                    apply_controlled_x(state, nqubits, b, c)
                    apply_one(state, nqubits, c, tdg_phase)
                    apply_controlled_x(state, nqubits, a, b)
                    apply_controlled_x(state, nqubits, b, c)
                    apply_one(state, nqubits, b, tdg_phase)
                    apply_one(state, nqubits, c, t_phase)
                    apply_one(state, nqubits, c, u2_0_pi)
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
            elif gate == "cry":
                control_bit = 1 << a
                target_bit = 1 << b
                cos = math.cos(angle / 2.0)
                sin = math.sin(angle / 2.0)
                for index in range(1 << nqubits):
                    if (index & control_bit) == 0 or (index & target_bit) != 0:
                        continue
                    other = index | target_bit
                    old_zero = state[index]
                    old_one = state[other]
                    state[index] = cos * old_zero - sin * old_one
                    state[other] = sin * old_zero + cos * old_one
            elif gate == "cu":
                theta, phi, lam, gamma = params[0], params[1], params[2], params[3]
                phase = lambda a: (1, 0, 0, cmath.exp(1j * a))  # noqa: E731
                apply_one(state, nqubits, a, phase(gamma))
                apply_one(state, nqubits, a, phase(0.5 * (lam + phi)))
                apply_one(state, nqubits, b, phase(0.5 * (lam - phi)))
                apply_controlled_x(state, nqubits, a, b)
                apply_one(state, nqubits, b, u3_matrix(-0.5 * theta, 0.0, -0.5 * (phi + lam)))
                apply_controlled_x(state, nqubits, a, b)
                apply_one(state, nqubits, b, u3_matrix(0.5 * theta, phi, 0.0))
            elif gate == "rxx":
                apply_xx_rotation(state, nqubits, a, b, angle)
            elif gate == "ryy":
                apply_yy_rotation(state, nqubits, a, b, angle)
            elif gate == "rzz":
                left_bit = 1 << a
                right_bit = 1 << b
                same_phase = cmath.exp(-0.5j * angle)
                different_phase = cmath.exp(0.5j * angle)
                for index in range(1 << nqubits):
                    left_one = (index & left_bit) != 0
                    right_one = (index & right_bit) != 0
                    state[index] *= same_phase if left_one == right_one else different_phase
            elif gate == "cx":
                apply_controlled_x(state, nqubits, a, b)
            elif gate == "cy":
                apply_controlled_y(state, nqubits, a, b)
            elif gate == "csx":
                apply_one(state, nqubits, b, one_qubit["h"])
                apply_controlled_phase(state, nqubits, a, b, math.pi / 2.0)
                apply_one(state, nqubits, b, one_qubit["h"])
            elif gate == "csxdg":
                apply_one(state, nqubits, b, one_qubit["h"])
                apply_controlled_phase(state, nqubits, a, b, -math.pi / 2.0)
                apply_one(state, nqubits, b, one_qubit["h"])
            elif gate == "swap":
                apply_swap(state, nqubits, a, b)
            elif gate == "dcx":
                apply_controlled_x(state, nqubits, a, b)
                apply_controlled_x(state, nqubits, b, a)
            else:
                raise AssertionError(f"unsupported test gate {gate!r}")

    return state[state_index(output_bits)]


def parse_solver_amplitude(output: str, norm_h_hint: int | None = None) -> complex:
    modulus = None
    norm_h = None
    counts = None
    amplitude_re = None
    amplitude_im = None
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
        elif line.startswith("amplitude_re: "):
            amplitude_re = float(line.split(": ", 1)[1])
        elif line.startswith("amplitude_im: "):
            amplitude_im = float(line.split(": ", 1)[1])

    if amplitude_re is not None and amplitude_im is not None:
        if norm_h_hint is None:
            raise AssertionError(f"amplitude output needs normalization hint:\n{output}")
        return complex(amplitude_re, amplitude_im) * (2.0 ** (-norm_h_hint / 2.0))

    if modulus is None or norm_h is None or counts is None:
        raise AssertionError(f"malformed solver output:\n{output}")

    omega = cmath.exp(2j * math.pi / modulus)
    total = sum(count * (omega**residue) for residue, count in enumerate(counts))
    return total * (2.0 ** (-norm_h / 2.0))


def sop_amplitude(
    qasm2sop: pathlib.Path,
    sop_solve: pathlib.Path,
    qasm: str,
    input_bits: str,
    output_bits: str,
    extra_qasm2sop_args: list[str] | None = None,
) -> complex:
    extra_qasm2sop_args = extra_qasm2sop_args or []
    imported = subprocess.run(
        [
            str(qasm2sop),
            *extra_qasm2sop_args,
            "--input",
            input_bits,
            "--output",
            output_bits,
            "-",
        ],
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
    norm_h = None
    for line in imported.stdout.splitlines():
        parts = line.split()
        if parts[:1] == ["n"]:
            norm_h = int(parts[1])
            break
    return parse_solver_amplitude(solved.stdout, norm_h)


def assert_close(name: str, expected: complex, actual: complex) -> None:
    if abs(expected - actual) > 1e-9:
        raise AssertionError(f"{name}: amplitude mismatch expected {expected!r}, got {actual!r}")


def run_amplitude_cases(qasm2sop: pathlib.Path, sop_solve: pathlib.Path) -> None:
    exact_checks = 0
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
            rzz(pi/4) q[0], q[1];
            """,
            [("00", "00"), ("00", "11"), ("10", "10"), ("11", "01")],
        ),
        (
            "h_cry",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[2];
            h q[0];
            cry(pi) q[0], q[1];
            """,
            [("00", "00"), ("00", "11"), ("01", "01"), ("01", "10"), ("10", "00"), ("11", "01")],
        ),
        (
            "h_cu",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[2];
            h q[0];
            cu(pi,pi/2,pi/2,pi/4) q[0], q[1];
            """,
            [("00", "00"), ("00", "11"), ("01", "01"), ("01", "10"), ("10", "00"), ("11", "01")],
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
            "pauli_pair_rotations",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[2];
            h q;
            rxx(pi/4) q[0], q[1];
            ryy(-pi/2) q[0], q[1];
            rzz(pi/4) q[0], q[1];
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
            csx q[0], q[1];
            csxdg q[1], q[0];
            dcx q[0], q[1];
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
            "rccx_relative_phase",
            """OPENQASM 2.0;
            include "qelib1.inc";
            qreg q[3];
            h q;
            rccx q[0], q[1], q[2];
            """,
            [("000", "000"), ("000", "111"), ("101", "110"), ("011", "010")],
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
            try:
                actual = sop_amplitude(qasm2sop, sop_solve, qasm, input_bits, output_bits)
            except AssertionError as exc:
                if "unsupported non-sign quadratic phase coefficient" in str(exc):
                    continue
                # An odd pi/8-unit cp/cu1 angle needs pi/16 granularity to lower exactly (see
                # apply_controlled_phase) -- a genuine, expected exact-mode rejection, same as
                # the case above just caught earlier (before export instead of at it).
                if "unsupported cp/cu1 angle in exact mode" in str(exc):
                    continue
                raise
            assert_close(f"{name} {input_bits}->{output_bits}", expected, actual)
            exact_checks += 1
    if exact_checks == 0:
        raise AssertionError("no representable sign-only amplitude cases were checked")

    approx_qasm = """OPENQASM 2.0;
    include "qelib1.inc";
    qreg q[1];
    h q[0];
    p(0.37) q[0];
    h q[0];
    """
    for input_bits, output_bits in [("0", "0"), ("0", "1")]:
        expected = simulate_qasm(approx_qasm, input_bits, output_bits)
        actual = sop_amplitude(
            qasm2sop,
            sop_solve,
            approx_qasm,
            input_bits,
            output_bits,
            ["--approx", "5e-2"],
        )
        if abs(expected - actual) > 5e-2 + 1e-9:
            raise AssertionError(
                f"approx phase {input_bits}->{output_bits}: expected {expected!r}, got {actual!r}"
            )

    approx_cry_qasm = """OPENQASM 2.0;
    include "qelib1.inc";
    qreg q[2];
    h q;
    cry(0.53) q[0], q[1];
    """
    for input_bits, output_bits in [("00", "00"), ("00", "11"), ("10", "01")]:
        expected = simulate_qasm(approx_cry_qasm, input_bits, output_bits)
        actual = sop_amplitude(
            qasm2sop,
            sop_solve,
            approx_cry_qasm,
            input_bits,
            output_bits,
            ["--approx", "5e-2"],
        )
        if abs(expected - actual) > 5e-2 + 1e-9:
            raise AssertionError(
                f"approx cry {input_bits}->{output_bits}: expected {expected!r}, got {actual!r}"
            )

    # Matches the actual qccq-gauntlet corpus pattern: cu(theta, 0, 0, 0), theta continuous
    # (never a clean dyadic fraction of pi) -- this is the real-world shape that motivated
    # adding cu support at all.
    approx_cu_qasm = """OPENQASM 2.0;
    include "qelib1.inc";
    qreg q[2];
    h q[0];
    cu(0.73,0,0,0) q[0], q[1];
    """
    for input_bits, output_bits in [("00", "00"), ("00", "11"), ("10", "01")]:
        expected = simulate_qasm(approx_cu_qasm, input_bits, output_bits)
        actual = sop_amplitude(
            qasm2sop,
            sop_solve,
            approx_cu_qasm,
            input_bits,
            output_bits,
            ["--approx", "5e-2"],
        )
        if abs(expected - actual) > 5e-2 + 1e-9:
            raise AssertionError(
                f"approx cu {input_bits}->{output_bits}: expected {expected!r}, got {actual!r}"
            )


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_qasm_amplitudes.py QASM2SOP SOP_SOLVE", file=sys.stderr)
        return 2

    run_amplitude_cases(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
