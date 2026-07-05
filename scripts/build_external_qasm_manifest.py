#!/usr/bin/env python3

import argparse
import collections
import json
import math
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from fractions import Fraction


QREG_RE = re.compile(r"^qreg\s+([A-Za-z_][A-Za-z0-9_]*)\[(\d+)\]\s*;$")


def sanitize_name(text: str) -> str:
    name = re.sub(r"[^A-Za-z0-9_.-]+", "_", text)
    return name.strip("_") or "case"


def strip_line_comment(line: str) -> str:
    return line.split("//", 1)[0].strip()


def qasm_qubits(qasm: str) -> int:
    total = 0
    for raw in qasm.splitlines():
        match = QREG_RE.match(strip_line_comment(raw))
        if match is not None:
            total += int(match.group(2))
    if total <= 0:
        raise ValueError("QASM input has no qreg declarations")
    return total


def qasm_qreg_names(qasm: str) -> list[str]:
    names: list[str] = []
    for raw in qasm.splitlines():
        match = QREG_RE.match(strip_line_comment(raw))
        if match is not None:
            names.append(match.group(1))
    return names


def repair_single_register_alias(qasm: str, alias: str) -> str:
    names = sorted(set(qasm_qreg_names(qasm)))
    if len(names) != 1 or alias in names:
        return qasm
    return re.sub(rf"\b{re.escape(alias)}\s*\[", f"{names[0]}[", qasm)


def source_files(roots: list[pathlib.Path], include_invalid: bool) -> list[tuple[pathlib.Path, pathlib.Path]]:
    files: list[tuple[pathlib.Path, pathlib.Path]] = []
    suffixes = {".qasm"}

    for root in roots:
        if root.is_file():
            candidates = [root]
            base = root.parent
        else:
            candidates = sorted(path for path in root.rglob("*") if path.is_file())
            base = root
        for path in candidates:
            if path.suffix.lower() not in suffixes:
                continue
            if not include_invalid and "invalid" in path.parts:
                continue
            files.append((base, path))
    return files


def load_source(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def starts_with_keyword(text: str, keyword: str) -> bool:
    return text == keyword or text.startswith(f"{keyword} ") or text.startswith(f"{keyword}\t")


def strip_terminal_measurements(qasm: str) -> str:
    stripped_lines: list[str] = []
    saw_measurement = False
    for raw in qasm.splitlines():
        statement = strip_line_comment(raw)
        if not statement:
            stripped_lines.append(raw)
            continue
        if starts_with_keyword(statement, "if") or starts_with_keyword(statement, "reset"):
            raise RuntimeError("cannot strip dynamic OpenQASM with if/reset")
        if starts_with_keyword(statement, "creg") or starts_with_keyword(statement, "measure"):
            if starts_with_keyword(statement, "measure"):
                saw_measurement = True
            continue
        if saw_measurement and not starts_with_keyword(statement, "barrier"):
            raise RuntimeError("cannot strip non-terminal measurement")
        stripped_lines.append(raw)
    return "\n".join(stripped_lines).rstrip("\n") + "\n"


def parse_csv_operands(text: str) -> list[str]:
    return [chunk.strip() for chunk in text.split(",") if chunk.strip()]


def split_gate_invocation(text: str) -> tuple[str, list[str], str]:
    match = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*(?:\(([^)]*)\))?(?:\s+(.*))?$", text.strip())
    if match is None:
        raise RuntimeError("invalid gate invocation")
    name = match.group(1)
    params = parse_csv_operands(match.group(2) or "")
    operands = (match.group(3) or "").strip()
    return name, params, operands


def split_name_params(text: str) -> tuple[str, list[str]]:
    name, params, operands = split_gate_invocation(text)
    if operands:
        raise RuntimeError("invalid parameterized gate name")
    return name, params


def parse_gate_definition(header: str) -> tuple[str, list[str], list[str]]:
    declaration = header.split("{", 1)[0].strip()
    if not starts_with_keyword(declaration, "gate"):
        raise RuntimeError("invalid simple gate definition")
    name, params, formal_text = split_gate_invocation(declaration[len("gate"):])
    formals = parse_csv_operands(formal_text)
    return name, params, formals


def gate_body_statements(text: str) -> list[str]:
    return [chunk.strip() for chunk in text.split(";") if chunk.strip()]


class AngleParseError(Exception):
    pass


@dataclass(frozen=True)
class AngleExpr:
    """A parsed angle: pi_coeff * pi + numeric_offset, in radians.

    pi_coeff stays an exact Fraction so rational multiples of pi (e.g. 3*pi/8) fold without
    floating-point drift. numeric_offset carries any non-pi decimal term.
    """

    pi_coeff: Fraction
    numeric_offset: Decimal
    approximate: bool

    @staticmethod
    def pi() -> "AngleExpr":
        return AngleExpr(Fraction(1), Decimal(0), False)

    @staticmethod
    def number(text: str) -> "AngleExpr":
        try:
            value = Decimal(text)
        except InvalidOperation as exc:
            raise AngleParseError(f"not a number: {text!r}") from exc
        return AngleExpr(Fraction(0), value, value != 0)

    def _as_scalar(self) -> Decimal | None:
        return self.numeric_offset if self.pi_coeff == 0 else None

    def __neg__(self) -> "AngleExpr":
        return AngleExpr(-self.pi_coeff, -self.numeric_offset, self.approximate)

    def __add__(self, other: "AngleExpr") -> "AngleExpr":
        return AngleExpr(
            self.pi_coeff + other.pi_coeff,
            self.numeric_offset + other.numeric_offset,
            self.approximate or other.approximate,
        )

    def __sub__(self, other: "AngleExpr") -> "AngleExpr":
        return self + (-other)

    def __mul__(self, other: "AngleExpr") -> "AngleExpr":
        scalar = other._as_scalar()
        if scalar is not None:
            return AngleExpr(
                self.pi_coeff * Fraction(scalar), self.numeric_offset * scalar,
                self.approximate or other.approximate,
            )
        scalar = self._as_scalar()
        if scalar is not None:
            return other * self
        raise AngleParseError("cannot multiply two pi-valued terms")

    def __truediv__(self, other: "AngleExpr") -> "AngleExpr":
        scalar = other._as_scalar()
        if scalar is None or scalar == 0:
            raise AngleParseError("cannot divide by a pi-valued or zero term")
        return AngleExpr(
            self.pi_coeff / Fraction(scalar), self.numeric_offset / scalar,
            self.approximate or other.approximate,
        )


class _AngleParser:
    """Recursive-descent parser for `expr := term (('+'|'-') term)*`,
    `term := factor (('*'|'/') factor)*`, `factor := number | 'pi' | '(' expr ')' | ('+'|'-') factor`.
    """

    _NUMBER_RE = re.compile(r"\d+(?:\.\d+)?(?:[eE][+-]?\d+)?")
    _IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

    def __init__(self, text: str) -> None:
        self.text = text
        self.pos = 0

    def parse(self) -> AngleExpr:
        value = self._expr()
        self._skip_space()
        if self.pos != len(self.text):
            raise AngleParseError(f"unexpected trailing text: {self.text[self.pos:]!r}")
        return value

    def _skip_space(self) -> None:
        while self.pos < len(self.text) and self.text[self.pos].isspace():
            self.pos += 1

    def _peek(self) -> str:
        self._skip_space()
        return self.text[self.pos] if self.pos < len(self.text) else ""

    def _expr(self) -> AngleExpr:
        value = self._term()
        while True:
            op = self._peek()
            if op == "+":
                self.pos += 1
                value = value + self._term()
            elif op == "-":
                self.pos += 1
                value = value - self._term()
            else:
                return value

    def _term(self) -> AngleExpr:
        value = self._factor()
        while True:
            op = self._peek()
            if op == "*":
                self.pos += 1
                value = value * self._factor()
            elif op == "/":
                self.pos += 1
                value = value / self._factor()
            else:
                return value

    def _factor(self) -> AngleExpr:
        ch = self._peek()
        if ch == "+":
            self.pos += 1
            return self._factor()
        if ch == "-":
            self.pos += 1
            return -self._factor()
        if ch == "(":
            self.pos += 1
            value = self._expr()
            if self._peek() != ")":
                raise AngleParseError("unbalanced parentheses")
            self.pos += 1
            return value
        self._skip_space()
        match = re.match(r"pi\b", self.text[self.pos :])
        if match:
            self.pos += match.end()
            return AngleExpr.pi()
        match = self._NUMBER_RE.match(self.text[self.pos :])
        if match:
            self.pos += match.end()
            return AngleExpr.number(match.group())
        match = self._IDENT_RE.match(self.text[self.pos :])
        if match:
            raise AngleParseError(f"unresolved identifier: {match.group()!r}")
        raise AngleParseError(f"unexpected character at {self.pos}: {self.text[self.pos:]!r}")


def _format_pi_rational(coeff: Fraction) -> str:
    if coeff == 0:
        return "0"
    sign = "-" if coeff < 0 else ""
    coeff = abs(coeff)
    if coeff.denominator == 1:
        return f"{sign}pi" if coeff.numerator == 1 else f"{sign}{coeff.numerator}*pi"
    if coeff.numerator == 1:
        return f"{sign}pi/{coeff.denominator}"
    return f"{sign}{coeff.numerator}*pi/{coeff.denominator}"


def format_angle_expr(expr: AngleExpr) -> str:
    if expr.numeric_offset == 0:
        return _format_pi_rational(expr.pi_coeff)
    total = float(expr.pi_coeff) * math.pi + float(expr.numeric_offset)
    return repr(total)


def fold_angle_expression(text: str) -> str:
    """Parse and simplify a substituted gate-parameter expression, e.g. '-pi/2 + pi/2' -> '0'.

    Falls back to returning `text` unchanged (still substituted, just unfolded) whenever it
    isn't a recognized angle expression -- callers rely on this being a safe no-op rather than
    a hard failure, since gate parameters aren't always angles.
    """
    try:
        expr = _AngleParser(text).parse()
    except AngleParseError:
        return text
    return format_angle_expr(expr)


def assert_no_internal_param_whitespace(qasm: str) -> None:
    """Test-only helper: fail if any non-`gate`-definition statement has whitespace inside a
    parenthesized argument list, which is what qasm2sop.c's tokenizer used to choke on.
    """
    for lineno, raw in enumerate(qasm.splitlines(), start=1):
        statement = strip_line_comment(raw)
        if not statement or starts_with_keyword(statement, "gate"):
            continue
        for match in re.finditer(r"\(([^()]*)\)", statement):
            if any(ch.isspace() for ch in match.group(1)):
                raise AssertionError(
                    f"line {lineno}: whitespace inside gate parameter list: {raw!r}"
                )


def inline_simple_gates(qasm: str) -> str:
    macros: dict[str, tuple[list[str], list[str], list[str]]] = {}
    output: list[str] = []
    lines = qasm.splitlines()
    i = 0

    def rewrite_gate_name(name: str, params: list[str], param_mapping: dict[str, str]) -> str:
        if not params:
            return name
        rewritten = []
        for param in params:
            expr = param
            for formal, actual in param_mapping.items():
                expr = re.sub(rf"\b{re.escape(formal)}\b", actual, expr)
            rewritten.append(fold_angle_expression(expr))
        return f"{name}({','.join(rewritten)})"

    def expand_statement(statement: str, depth: int = 0) -> list[str]:
        if depth > 16:
            raise RuntimeError("simple gate expansion is too deep")
        if not statement.endswith(";"):
            statement = statement + ";"
        bare = statement[:-1].strip()
        if not bare:
            return []
        name, params, operand_text = split_gate_invocation(bare)
        if name not in macros:
            return [statement]
        param_formals, formals, body = macros[name]
        if len(params) != len(param_formals):
            raise RuntimeError(f"gate {name} expects {len(param_formals)} parameters, got {len(params)}")
        param_mapping = dict(zip(param_formals, params, strict=True))
        actuals = parse_csv_operands(operand_text)
        if len(actuals) != len(formals):
            raise RuntimeError(f"gate {name} expects {len(formals)} operands, got {len(actuals)}")
        operand_mapping = dict(zip(formals, actuals, strict=True))
        expanded: list[str] = []
        for body_statement in body:
            body_name, body_params, body_operand_text = split_gate_invocation(body_statement)
            operands = parse_csv_operands(body_operand_text)
            rewritten_operands = [operand_mapping.get(operand, operand) for operand in operands]
            rewritten = rewrite_gate_name(body_name, body_params, param_mapping)
            if rewritten_operands:
                rewritten += " " + ",".join(rewritten_operands)
            rewritten += ";"
            expanded.extend(expand_statement(rewritten, depth + 1))
        return expanded

    while i < len(lines):
        raw = lines[i]
        statement = strip_line_comment(raw)
        if not starts_with_keyword(statement, "gate"):
            if statement:
                output.extend(expand_statement(statement))
            else:
                output.append(raw)
            i += 1
            continue

        collected = [statement]
        while "}" not in collected[-1]:
            i += 1
            if i >= len(lines):
                raise RuntimeError("unterminated simple gate definition")
            collected.append(strip_line_comment(lines[i]))
        definition = "\n".join(collected)
        before, after_open = definition.split("{", 1)
        body_text, _, after_close = after_open.partition("}")
        if after_close.strip():
            raise RuntimeError("simple gate definition has trailing text after '}'")
        name, params, formals = parse_gate_definition(before + "{")
        macros[name] = (params, formals, gate_body_statements(body_text))
        i += 1

    return "\n".join(output).rstrip("\n") + "\n"


def has_gate_definition(qasm: str) -> bool:
    return any(starts_with_keyword(strip_line_comment(line), "gate") for line in qasm.splitlines())


def boundary_pairs(nqubits: int, mode: str) -> list[list[str]]:
    zero = "0" * nqubits
    one = "1" * nqubits
    if mode == "zero":
        return [[zero, zero]]
    if mode == "zero-and-one":
        return [[zero, zero], [one, one]]
    if mode == "zero-to-one":
        return [[zero, one]]
    raise AssertionError(f"unhandled boundary mode {mode}")


def qsop_metadata(qsop: str) -> dict[str, int | str]:
    metadata: dict[str, int | str] | None = None
    for line in qsop.splitlines():
        parts = line.split()
        if len(parts) == 5 and parts[:2] == ["p", "qsop-sign"]:
            metadata = {
                "modulus": int(parts[2]),
                "nvars": int(parts[3]),
                "nedges": int(parts[4]),
            }
            continue
    if metadata is None:
        raise RuntimeError("missing QSOP header")
    metadata["format"] = "qsop-sign"
    return metadata


def classify_error(message: str) -> str:
    if "too_many_vars" in message:
        return "too_many_vars"
    if "dynamic OpenQASM" in message or "dynamic or classical OpenQASM features" in message:
        return "dynamic_classical"
    if "unsupported OpenQASM operation 'gate'" in message:
        return "unsupported_gate_definition"
    if "unsupported OpenQASM operation" in message:
        return "unsupported_gate"
    if "unsupported non-sign quadratic phase coefficient" in message:
        return "unsupported_sign_quadratic"
    # qasm2sop.c's tokenizer surfaces this only when a gate's parenthesized argument list is
    # genuinely malformed (mismatched parens), not for whitespace inside a well-formed one -- that
    # case is handled transparently by the paren-depth-aware tokenizer and never reaches here.
    # Distinct from unsupported_angle/unsupported_gate: this is a parse-shape failure, not an SOP
    # expressivity gap.
    if "unbalanced gate parameter list" in message:
        return "parse_error.whitespace_inside_gate_params"
    if "unsupported " in message and (" angle" in message or " angle list" in message):
        return "unsupported_angle"
    if "parameterized gate definitions" in message or "simple gate definition" in message:
        return "unsupported_gate_definition"
    if (
        "missing OPENQASM" in message
        or "statements must end with ';'" in message
        or "invalid " in message
        or "unterminated" in message
    ):
        return "parse_error"
    return "other_error"


def diagnostic_from_exception(exc: Exception) -> str:
    message = str(exc).strip().splitlines()
    return message[-1] if message else repr(exc)


def relative_path(root: pathlib.Path, path: pathlib.Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.name


def import_boundary_metadata(
    qasm2sop: pathlib.Path, qasm: str, input_bits: str, output_bits: str
) -> dict[str, int | str]:
    completed = subprocess.run(
        [str(qasm2sop), "--input", input_bits, "--output", output_bits, "-"],
        check=False,
        input=qasm,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip().splitlines()[-1] if completed.stderr.strip() else "qasm2sop failed"
        raise RuntimeError(diagnostic)
    return qsop_metadata(completed.stdout)


def build_case(
    qasm2sop: pathlib.Path,
    root: pathlib.Path,
    path: pathlib.Path,
    qasm: str,
    boundaries: list[list[str]],
    min_vars: int,
    max_vars: int,
    source_prefix: str,
    source_name: str,
    source_url: str | None,
) -> tuple[dict | None, str, list[dict[str, int | str]], int, int, str]:
    max_nvars = 0
    max_edges = 0
    boundary_records: list[dict[str, int | str]] = []
    for input_bits, output_bits in boundaries:
        metadata = import_boundary_metadata(qasm2sop, qasm, input_bits, output_bits)
        nvars = int(metadata["nvars"])
        nedges = int(metadata["nedges"])
        max_nvars = max(max_nvars, nvars)
        max_edges = max(max_edges, nedges)
        boundary_records.append(
            {
                "input": input_bits,
                "output": output_bits,
                **metadata,
            }
        )

    mode = "sign"

    if max_nvars > max_vars:
        return None, "too_many_vars", boundary_records, max_nvars, max_edges, mode
    if max_nvars < min_vars:
        return None, "below_min_vars", boundary_records, max_nvars, max_edges, mode

    relpath = path.relative_to(root)
    name = sanitize_name(f"{source_prefix}_{relpath.with_suffix('').as_posix()}")
    case = {
        "name": name,
        "source": source_name,
        "source_path": str(path),
        "source_relative_path": relpath.as_posix(),
        "qasm_lines": qasm.rstrip("\n").splitlines(),
        "boundaries": boundaries,
        "max_imported_nvars": max_nvars,
        "max_imported_edges": max_edges,
    }
    if source_url is not None:
        case["source_url"] = source_url
    return (case, "ok", boundary_records, max_nvars, max_edges, mode)


def report_record(
    root: pathlib.Path,
    path: pathlib.Path,
    status: str,
    source_name: str,
    source_url: str | None,
    diagnostic: str | None = None,
    boundaries: list[dict[str, int | str]] | None = None,
    max_nvars: int | None = None,
    max_edges: int | None = None,
    mode: str | None = None,
) -> dict:
    record = {
        "path": str(path),
        "relative_path": relative_path(root, path),
        "source": source_name,
        "source_type": path.suffix.lower().lstrip(".") or "unknown",
        "status": status,
    }
    if source_url is not None:
        record["source_url"] = source_url
    if diagnostic:
        record["diagnostic"] = diagnostic
    if boundaries is not None:
        record["boundaries"] = boundaries
    if max_nvars is not None:
        record["max_imported_nvars"] = max_nvars
    if max_edges is not None:
        record["max_imported_edges"] = max_edges
    if mode is not None:
        record["mode"] = mode
    return record


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a qasm_solver_corpus-compatible manifest from external QASM files."
    )
    parser.add_argument("qasm2sop", type=pathlib.Path)
    parser.add_argument("roots", nargs="+", type=pathlib.Path)
    parser.add_argument("--include-invalid", action="store_true")
    parser.add_argument(
        "--strip-terminal-measurements",
        action="store_true",
        help="drop creg declarations and terminal measure statements for strong-simulation imports",
    )
    parser.add_argument(
        "--inline-simple-gates",
        action="store_true",
        help="inline non-parameterized OpenQASM gate definitions for benchmark ingestion",
    )
    parser.add_argument("--source-prefix", default="external")
    parser.add_argument("--source-name", help="human-readable source label stored in emitted cases and reports")
    parser.add_argument("--source-url", help="upstream source repository or benchmark URL stored in reports")
    parser.add_argument(
        "--repair-single-register-alias",
        action="append",
        default=[],
        metavar="NAME",
        help="rewrite NAME[index] to the sole declared qreg name when a source has a known alias typo",
    )
    parser.add_argument("--limit", type=int, help="limit source files before import filtering")
    parser.add_argument("--max-cases", type=int, help="limit emitted manifest cases")
    parser.add_argument("--min-vars", type=int, default=0, help="only emit cases with at least this many imported SOP variables")
    parser.add_argument("--max-vars", type=int, default=24)
    parser.add_argument("--output", type=pathlib.Path, help="write manifest JSON to this path instead of stdout")
    parser.add_argument("--report", type=pathlib.Path, help="write per-source import classification JSON")
    parser.add_argument(
        "--boundaries",
        choices=("zero", "zero-and-one", "zero-to-one"),
        default="zero",
        help="fixed-boundary amplitudes to include per source file",
    )
    args = parser.parse_args(argv)
    if args.limit is not None and args.limit < 0:
        parser.error("--limit must be non-negative")
    if args.max_cases is not None and args.max_cases < 0:
        parser.error("--max-cases must be non-negative")
    if args.max_vars < 0:
        parser.error("--max-vars must be non-negative")
    if args.min_vars < 0:
        parser.error("--min-vars must be non-negative")
    if args.min_vars > args.max_vars:
        parser.error("--min-vars must be less than or equal to --max-vars")
    args.source_name = args.source_name or args.source_prefix
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    inputs = source_files(args.roots, args.include_invalid)
    if args.limit is not None:
        inputs = inputs[: args.limit]

    counts: collections.Counter[str] = collections.Counter()
    cases = []
    report_records = []
    for root, path in inputs:
        if args.max_cases is not None and len(cases) >= args.max_cases:
            break
        qasm = None
        try:
            qasm = load_source(path)
            if args.inline_simple_gates:
                qasm = inline_simple_gates(qasm)
            if args.strip_terminal_measurements:
                qasm = strip_terminal_measurements(qasm)
            for alias in args.repair_single_register_alias:
                qasm = repair_single_register_alias(qasm, alias)
            nqubits = qasm_qubits(qasm)
            boundaries = boundary_pairs(nqubits, args.boundaries)
            case, status, boundary_records, max_nvars, max_edges, mode = build_case(
                args.qasm2sop,
                root,
                path,
                qasm,
                boundaries,
                args.min_vars,
                args.max_vars,
                args.source_prefix,
                args.source_name,
                args.source_url,
            )
        except Exception as exc:
            diagnostic = diagnostic_from_exception(exc)
            status = classify_error(diagnostic)
            if status == "parse_error" and qasm is not None and has_gate_definition(qasm):
                status = "unsupported_gate_definition"
            counts[status] += 1
            report_records.append(
                report_record(
                    root,
                    path,
                    status,
                    args.source_name,
                    args.source_url,
                    diagnostic=diagnostic,
                )
            )
            continue

        counts[status] += 1
        report_records.append(
            report_record(
                root,
                path,
                status,
                args.source_name,
                args.source_url,
                boundaries=boundary_records,
                max_nvars=max_nvars,
                max_edges=max_edges,
                mode=mode,
            )
        )
        if case is not None:
            cases.append(case)

    manifest = json.dumps(cases, indent=2, sort_keys=True) + "\n"
    if args.output is None:
        print(manifest, end="")
    else:
        args.output.write_text(manifest, encoding="utf-8")
    if args.report is not None:
        report = {
            "roots": [str(root) for root in args.roots],
            "source": args.source_name,
            "source_url": args.source_url,
            "inputs": len(inputs),
            "emitted": len(cases),
            "counts": dict(sorted(counts.items())),
            "records": report_records,
        }
        args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        "build_external_qasm_manifest: "
        f"inputs={len(inputs)} emitted={len(cases)} "
        + " ".join(f"{key}={counts[key]}" for key in sorted(counts)),
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
