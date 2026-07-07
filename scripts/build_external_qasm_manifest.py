#!/usr/bin/env python3
"""OpenQASM text-munging helpers shared by the qccq-gauntlet adapters.

Not a standalone CLI: `.gauntlet/adapter.py` and `.gauntlet/adapter_wmc.py`
import this module directly (`sys.path.insert(0, ".../scripts")`) to inline
simple gate definitions, count qubits, and classify qasm2sop import errors
before invoking the solver.
"""

import re
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from fractions import Fraction
import math


QREG_RE = re.compile(r"^qreg\s+([A-Za-z_][A-Za-z0-9_]*)\[(\d+)\]\s*;$")


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


def starts_with_keyword(text: str, keyword: str) -> bool:
    return text == keyword or text.startswith(f"{keyword} ") or text.startswith(f"{keyword}\t")


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
