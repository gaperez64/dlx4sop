#!/usr/bin/env python3

import argparse
import collections
import json
import pathlib
import subprocess
import sys
from dataclasses import dataclass


LEVELS = {
    "alg": "ALG",
    "indep": "INDEP",
}


DEFAULT_BENCHMARKS = [
    "ghz",
    "qft",
    "qftentangled",
    "bv",
    "qwalk",
    "qpeexact",
    "qpeinexact",
    "wstate",
]


@dataclass(frozen=True)
class MqtModules:
    benchmark_level: object
    get_benchmark: object
    get_available_benchmark_names: object
    qasm2_dumps: object


class MqtScanError(RuntimeError):
    pass


def add_mqt_source(source: pathlib.Path | None) -> None:
    if source is None:
        return
    if not source.exists():
        raise MqtScanError(f"MQT Bench source path does not exist: {source}")

    package_src = source / "src"
    candidate = package_src if package_src.exists() else source
    sys.path.insert(0, str(candidate))


def load_mqt(source: pathlib.Path | None) -> MqtModules:
    add_mqt_source(source)
    try:
        from mqt.bench import BenchmarkLevel, get_benchmark
        from mqt.bench.benchmarks import get_available_benchmark_names
        from qiskit.qasm2 import dumps as qasm2_dumps
    except ModuleNotFoundError as exc:
        raise MqtScanError(
            "scan_mqt_bench.py requires mqt.bench and qiskit; install mqt.bench "
            "or pass --mqt-source /path/to/bench"
        ) from exc
    return MqtModules(BenchmarkLevel, get_benchmark, get_available_benchmark_names, qasm2_dumps)


def parse_csv(text: str) -> list[str]:
    values = []
    for chunk in text.split(","):
        value = chunk.strip()
        if value:
            values.append(value)
    return values


def parse_sizes(text: str) -> list[int]:
    sizes = []
    for value in parse_csv(text):
        try:
            size = int(value)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(f"invalid size: {value}") from exc
        if size <= 0:
            raise argparse.ArgumentTypeError("sizes must be positive")
        sizes.append(size)
    if not sizes:
        raise argparse.ArgumentTypeError("at least one size is required")
    return sizes


def classify(stderr: str) -> str:
    if "dynamic or classical OpenQASM features" in stderr:
        return "dynamic_classical"
    if "unsupported OpenQASM operation 'gate'" in stderr:
        return "unsupported_gate_definition"
    if "unsupported OpenQASM operation" in stderr:
        return "unsupported_gate"
    if "unsupported " in stderr and " angle" in stderr:
        return "unsupported_angle"
    if "statements must end with ';'" in stderr or "operation 'gate'" in stderr:
        return "unsupported_gate_definition"
    return "other_error"


def status_from_exception(prefix: str, exc: Exception) -> dict:
    message = str(exc).strip().splitlines()
    diagnostic = message[0] if message else repr(exc)
    return {
        "status": prefix,
        "diagnostic": f"{type(exc).__name__}: {diagnostic}",
    }


def strip_measurements(qc):
    if hasattr(qc, "remove_final_measurements"):
        return qc.remove_final_measurements(inplace=False)
    return qc


def qasm_filename(benchmark: str, level: str, size: int) -> str:
    safe = "".join(ch if ch.isalnum() or ch in ("-", "_") else "_" for ch in benchmark)
    return f"{safe}_{level}_{size}.qasm"


def maybe_write_qasm(qasm_dir: pathlib.Path | None, benchmark: str, level: str, size: int, qasm: str) -> str | None:
    if qasm_dir is None:
        return None
    qasm_dir.mkdir(parents=True, exist_ok=True)
    path = qasm_dir / qasm_filename(benchmark, level, size)
    path.write_text(qasm, encoding="utf-8")
    return str(path)


def scan_case(
    modules: MqtModules,
    qasm2sop: pathlib.Path,
    benchmark: str,
    level_name: str,
    size: int,
    opt_level: int,
    random_parameters: bool,
    strip_final_measurements: bool,
    qasm_dir: pathlib.Path | None,
) -> dict:
    level = getattr(modules.benchmark_level, LEVELS[level_name])
    record = {"benchmark": benchmark, "level": level_name, "size": size}

    try:
        qc = modules.get_benchmark(
            benchmark=benchmark,
            level=level,
            circuit_size=size,
            opt_level=opt_level,
            random_parameters=random_parameters,
        )
        if strip_final_measurements:
            qc = strip_measurements(qc)
    except Exception as exc:
        record.update(status_from_exception("generation_error", exc))
        return record

    record["qubits"] = getattr(qc, "num_qubits", None)
    record["clbits"] = getattr(qc, "num_clbits", None)
    try:
        record["operations"] = len(qc.data)
    except Exception:
        pass

    try:
        qasm = modules.qasm2_dumps(qc)
    except Exception as exc:
        record.update(status_from_exception("qasm_dump_error", exc))
        return record

    qasm_path = maybe_write_qasm(qasm_dir, benchmark, level_name, size, qasm)
    if qasm_path is not None:
        record["qasm_path"] = qasm_path

    completed = subprocess.run(
        [str(qasm2sop), "-"],
        check=False,
        input=qasm,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode == 0:
        record["status"] = "ok"
        record["qsop_lines"] = len(completed.stdout.splitlines())
        return record

    record["status"] = classify(completed.stderr)
    record["diagnostic"] = completed.stderr.strip().splitlines()[-1] if completed.stderr.strip() else ""
    return record


def selected_benchmarks(modules: MqtModules, requested: str) -> list[str]:
    if requested == "default":
        return DEFAULT_BENCHMARKS.copy()
    if requested == "all":
        return modules.get_available_benchmark_names()
    return parse_csv(requested)


def scan(args: argparse.Namespace) -> dict:
    modules = load_mqt(args.mqt_source)
    benchmarks = selected_benchmarks(modules, args.benchmarks)
    levels = parse_csv(args.levels)
    unknown_levels = [level for level in levels if level not in LEVELS]
    if unknown_levels:
        raise MqtScanError(f"unsupported level(s): {', '.join(unknown_levels)}")

    records = []
    for benchmark in benchmarks:
        for level in levels:
            for size in args.sizes:
                if args.limit is not None and len(records) >= args.limit:
                    break
                records.append(
                    scan_case(
                        modules,
                        args.qasm2sop,
                        benchmark,
                        level,
                        size,
                        args.opt_level,
                        args.random_parameters,
                        args.strip_final_measurements,
                        args.qasm_dir,
                    )
                )
            if args.limit is not None and len(records) >= args.limit:
                break
        if args.limit is not None and len(records) >= args.limit:
            break

    counts = collections.Counter(record["status"] for record in records)
    return {
        "source": str(args.mqt_source) if args.mqt_source else "python-environment",
        "benchmarks": benchmarks,
        "levels": levels,
        "sizes": args.sizes,
        "cases": len(records),
        "counts": dict(sorted(counts.items())),
        "failures": [record for record in records if record["status"] != "ok"],
        "records": records,
    }


def write_text(report: dict, max_failures: int) -> None:
    print(f"source: {report['source']}")
    print(f"cases: {report['cases']}")
    for key, value in report["counts"].items():
        print(f"{key}: {value}")

    failures = report["failures"][:max_failures]
    if failures:
        print("failures:")
        for failure in failures:
            diagnostic = failure.get("diagnostic", "")
            label = f"{failure['benchmark']}/{failure['level']}/{failure['size']}"
            print(f"- {failure['status']}: {label}: {diagnostic}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate MQT Bench QASM2 cases and scan them through qasm2sop."
    )
    parser.add_argument("qasm2sop", type=pathlib.Path)
    parser.add_argument(
        "--mqt-source",
        type=pathlib.Path,
        help="local Munich Quantum Toolkit Bench checkout; its src/ directory is added to sys.path",
    )
    parser.add_argument(
        "--benchmarks",
        default="default",
        help="comma-separated benchmark names, 'default', or 'all'",
    )
    parser.add_argument("--levels", default="indep", help="comma-separated levels: alg,indep")
    parser.add_argument("--sizes", type=parse_sizes, default=[3], help="comma-separated positive sizes")
    parser.add_argument("--opt-level", type=int, default=0, choices=range(4))
    parser.add_argument("--random-parameters", action="store_true")
    parser.add_argument(
        "--keep-final-measurements",
        dest="strip_final_measurements",
        action="store_false",
        help="keep terminal measurements instead of stripping them for strong-simulation import",
    )
    parser.set_defaults(strip_final_measurements=True)
    parser.add_argument("--qasm-dir", type=pathlib.Path, help="optional directory for generated QASM2 files")
    parser.add_argument("--limit", type=int)
    parser.add_argument("--format", choices=("text", "json"), default="text")
    parser.add_argument("--max-failures", type=int, default=20)
    args = parser.parse_args(argv)
    if args.limit is not None and args.limit < 0:
        parser.error("--limit must be non-negative")
    if args.max_failures < 0:
        parser.error("--max-failures must be non-negative")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        report = scan(args)
    except MqtScanError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if args.format == "json":
        print(json.dumps(report, sort_keys=True, indent=2))
    else:
        write_text(report, args.max_failures)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
