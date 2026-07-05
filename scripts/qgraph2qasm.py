#!/usr/bin/env python3

import argparse
import json
import pathlib
import sys


def load_input(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    return pathlib.Path(path).read_text()


def load_pyzx():
    try:
        import pyzx as zx
    except ModuleNotFoundError as exc:
        raise RuntimeError(
            "qgraph2qasm requires PyZX; install pyzx to translate .qgraph diagrams"
        ) from exc
    return zx


def graph_from_json(zx, text: str):
    if hasattr(zx, "Graph") and hasattr(zx.Graph, "from_json"):
        return zx.Graph.from_json(text)

    try:
        from pyzx.io import json_to_graph
    except ImportError as exc:
        raise RuntimeError("installed PyZX does not expose qgraph JSON loading") from exc
    return json_to_graph(text)


def extract_circuit(zx, graph):
    candidates = []
    if hasattr(zx, "extract_circuit"):
        candidates.append(zx.extract_circuit)
    try:
        from pyzx.extract import extract_circuit as extract_circuit_fn

        candidates.append(extract_circuit_fn)
    except ImportError:
        pass

    last_error: Exception | None = None
    for candidate in candidates:
        try:
            work = graph.copy() if hasattr(graph, "copy") else graph
            return candidate(work)
        except Exception as exc:  # PyZX raises several graph-shape specific errors.
            last_error = exc

    if last_error is not None:
        raise RuntimeError(f"PyZX could not extract a circuit from this qgraph: {last_error}")
    raise RuntimeError("installed PyZX does not expose circuit extraction")


def circuit_to_qasm(circuit) -> str:
    if hasattr(circuit, "to_qasm"):
        return circuit.to_qasm()
    if hasattr(circuit, "to_basic_gates") and hasattr(circuit.to_basic_gates(), "to_qasm"):
        return circuit.to_basic_gates().to_qasm()
    raise RuntimeError("PyZX returned a circuit object without to_qasm()")


def translate(text: str) -> str:
    try:
        json.loads(text)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"invalid qgraph JSON: {exc}") from exc

    zx = load_pyzx()
    graph = graph_from_json(zx, text)
    circuit = extract_circuit(zx, graph)
    qasm = circuit_to_qasm(circuit)
    if not qasm.endswith("\n"):
        qasm += "\n"
    return qasm


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Translate a PyZX/Quantomatic .qgraph JSON diagram to OpenQASM."
    )
    parser.add_argument("path", nargs="?", default="-", help=".qgraph path, or '-' for stdin")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        print(translate(load_input(args.path)), end="")
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
