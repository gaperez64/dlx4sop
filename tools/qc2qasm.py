#!/usr/bin/env python3

import argparse
import pathlib
import sys


class QcError(RuntimeError):
    pass


def load_input(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    return pathlib.Path(path).read_text()


def q(index: int) -> str:
    return f"q[{index}]"


def strip_comment(line: str) -> str:
    return line.split("#", 1)[0].strip()


def collect_labels(preamble: str) -> dict[str, int]:
    labels: dict[str, int] = {}
    for raw in preamble.splitlines():
        line = strip_comment(raw)
        if not line:
            continue
        if not line.startswith("."):
            raise QcError(f"unknown preamble expression: {line}")
        for label in line[2:].replace(",", " ").split():
            if label not in labels:
                labels[label] = len(labels)
    return labels


def target_indices(labels: dict[str, int], text: str) -> list[int]:
    targets = []
    for label in text.replace(",", " ").split():
        if label not in labels:
            raise QcError(f"unknown .qc wire label: {label}")
        targets.append(labels[label])
    if not targets:
        raise QcError("gate has no operands")
    return targets


def emit_tofolli(lines: list[str], first: int, second: int, target: int) -> None:
    lines.append(f"ccx {q(first)}, {q(second)}, {q(target)};")


def ensure_ancillas(labels: dict[str, int], count: int) -> list[int]:
    ancillas = []
    while len(ancillas) < count:
        name = f"__ancilla_{len(ancillas)}"
        if name not in labels:
            labels[name] = len(labels)
        ancillas.append(labels[name])
    return ancillas


def emit_multi_tofolli(lines: list[str], labels: dict[str, int], controls: list[int], target: int) -> None:
    if len(controls) == 0:
        lines.append(f"x {q(target)};")
        return
    if len(controls) == 1:
        lines.append(f"cx {q(controls[0])}, {q(target)};")
        return
    if len(controls) == 2:
        emit_tofolli(lines, controls[0], controls[1], target)
        return
    if len(controls) > 6:
        raise QcError("multi-control tof supports at most six controls")

    ancillas = ensure_ancillas(labels, len(controls) - 2)
    emit_tofolli(lines, controls[0], controls[1], ancillas[0])
    if len(controls) == 3:
        emit_tofolli(lines, controls[2], ancillas[0], target)
    else:
        emit_tofolli(lines, controls[2], controls[3], ancillas[1])
        if len(controls) == 4:
            emit_tofolli(lines, ancillas[0], ancillas[1], target)
        elif len(controls) == 5:
            emit_tofolli(lines, ancillas[0], ancillas[1], ancillas[2])
            emit_tofolli(lines, controls[4], ancillas[2], target)
            emit_tofolli(lines, ancillas[0], ancillas[1], ancillas[2])
        else:
            emit_tofolli(lines, controls[4], controls[5], ancillas[2])
            emit_tofolli(lines, ancillas[0], ancillas[1], ancillas[3])
            emit_tofolli(lines, ancillas[2], ancillas[3], target)
            emit_tofolli(lines, ancillas[0], ancillas[1], ancillas[3])
            emit_tofolli(lines, controls[4], controls[5], ancillas[2])
        emit_tofolli(lines, controls[2], controls[3], ancillas[1])
    emit_tofolli(lines, controls[0], controls[1], ancillas[0])


def emit_multi_cz(lines: list[str], labels: dict[str, int], targets: list[int]) -> None:
    if len(targets) == 1:
        lines.append(f"z {q(targets[0])};")
    elif len(targets) == 2:
        lines.append(f"cz {q(targets[0])}, {q(targets[1])};")
    elif len(targets) == 3:
        lines.append(f"ccz {q(targets[0])}, {q(targets[1])}, {q(targets[2])};")
    else:
        *controls, target = targets
        lines.append(f"h {q(target)};")
        emit_multi_tofolli(lines, labels, controls, target)
        lines.append(f"h {q(target)};")


def emit_gate(lines: list[str], labels: dict[str, int], name: str, targets: list[int]) -> None:
    gate = name.lower()
    if gate in ("h", "x", "not", "t1"):
        qasm_gate = "x" if gate in ("x", "not", "t1") else "h"
        for target in targets:
            lines.append(f"{qasm_gate} {q(target)};")
        return
    if gate in ("s", "p", "s*", "p*", "t", "t*"):
        qasm_gate = {"s": "s", "p": "s", "s*": "sdg", "p*": "sdg", "t": "t", "t*": "tdg"}[gate]
        for target in targets:
            lines.append(f"{qasm_gate} {q(target)};")
        return
    if gate in ("z", "zd", "cz", "ccz"):
        emit_multi_cz(lines, labels, targets)
        return
    if gate in ("cnot", "t2"):
        if len(targets) != 2:
            raise QcError(f"{name} expects exactly two operands")
        lines.append(f"cx {q(targets[0])}, {q(targets[1])};")
        return
    if gate == "swap":
        if len(targets) != 2:
            raise QcError("swap expects exactly two operands")
        lines.append(f"swap {q(targets[0])}, {q(targets[1])};")
        return
    if gate in ("tof", "t3", "t4", "t5", "t6", "t7"):
        *controls, target = targets
        emit_multi_tofolli(lines, labels, controls, target)
        return
    raise QcError(f"unknown .qc gate: {name}")


def translate(text: str) -> str:
    begin = text.find("BEGIN")
    end = text.rfind("END")
    if begin < 0 or end < begin:
        raise QcError(".qc input must contain BEGIN and END")

    labels = collect_labels(text[:begin])
    body = text[begin + len("BEGIN") : end]
    lines: list[str] = []
    for raw in body.splitlines():
        line = strip_comment(raw)
        if not line:
            continue
        try:
            name, rest = line.split(None, 1)
        except ValueError as exc:
            raise QcError(f"could not parse gate line: {line}") from exc
        emit_gate(lines, labels, name, target_indices(labels, rest))

    header = ["OPENQASM 2.0;", 'include "qelib1.inc";', f"qreg q[{len(labels)}];"]
    return "\n".join(header + lines) + "\n"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Translate a PyZX/T-Par .qc circuit to OpenQASM.")
    parser.add_argument("path", nargs="?", default="-", help=".qc path, or '-' for stdin")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        print(translate(load_input(args.path)), end="")
    except (OSError, QcError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
