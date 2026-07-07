#!/usr/bin/env python3
"""Ganak/sop2wmc output-parsing helpers shared by the qccq-gauntlet WMC adapter.

Not a standalone CLI: `.gauntlet/adapter_wmc.py` imports this module directly
(`sys.path.insert(0, ".../scripts")`) to parse sop2wmc's CNF metadata comments
and Ganak's model-count output before reconstructing a normalized amplitude.
"""

import re


GANAK_COMPLEX_PATTERN = re.compile(
    r"^c [so] exact (?:arb frac|quadruple float)\s+"
    r"([+-]?[\d.e+-]+)(?:\s*\+\s*([+-]?[\d.e+-]+)i)?"
)


def _parse_kv_tokens(tokens: list[str]) -> dict[str, str]:
    parsed: dict[str, str] = {}
    for token in tokens:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        parsed[key] = value
    return parsed


def _maybe_int(value: str) -> int | str:
    try:
        return int(value)
    except ValueError:
        return value


def parse_wmc_metadata(cnf_text: str) -> dict[str, int | str]:
    """Extract benchmark-friendly structural metadata emitted by sop2wmc."""
    meta: dict[str, int | str] = {}
    block_max_a = 0
    block_max_b = 0
    for line in cnf_text.splitlines():
        if line.startswith("c sop2wmc "):
            fields = _parse_kv_tokens(line.split()[2:])
            if "encoding" in fields:
                meta["wmc_export_encoding"] = fields["encoding"]
            for source, target in (
                ("norm_h", "wmc_norm_h"),
                ("nvars", "wmc_original_nvars"),
                ("nedges", "wmc_original_edges"),
            ):
                if source in fields:
                    meta[target] = _maybe_int(fields[source])
        elif line.startswith("c preprocess "):
            fields = _parse_kv_tokens(line.split()[2:])
            if "nvars_after" in fields:
                meta["wmc_active_vars"] = _maybe_int(fields["nvars_after"])
            if "pairs_after" in fields:
                meta["wmc_residual_edges"] = _maybe_int(fields["pairs_after"])
        elif line.startswith("c block count="):
            fields = _parse_kv_tokens(line.split()[2:])
            for source, target in (
                ("count", "wmc_block_count"),
                ("covered_edges", "wmc_block_edges"),
                ("residual_edges", "wmc_residual_edges"),
                ("nvars_after", "wmc_active_vars"),
            ):
                if source in fields:
                    meta[target] = _maybe_int(fields[source])
        elif line.startswith("c block sign-parity "):
            fields = _parse_kv_tokens(line.split()[3:])
            if "a_size" in fields:
                block_max_a = max(block_max_a, int(fields["a_size"]))
            if "b_size" in fields:
                block_max_b = max(block_max_b, int(fields["b_size"]))
    if block_max_a:
        meta["wmc_block_max_a_size"] = block_max_a
    if block_max_b:
        meta["wmc_block_max_b_size"] = block_max_b
    meta.setdefault("wmc_block_count", 0)
    meta.setdefault("wmc_block_edges", 0)
    return meta


def is_zero_residual_wmc(metadata: dict[str, int | str]) -> bool:
    active_vars = metadata.get("wmc_active_vars", metadata.get("wmc_original_nvars"))
    residual_edges = metadata.get("wmc_residual_edges", metadata.get("wmc_original_edges"))
    return active_vars == 0 and residual_edges == 0


def parse_amplitude_factor(cnf_text: str) -> complex:
    for line in cnf_text.splitlines():
        if line.startswith("c amplitude_factor "):
            val = line.split(None, 2)[2]
            if not val.endswith("i"):
                break
            body = val[:-1]
            for pos in range(len(body) - 1, 0, -1):
                if body[pos] == "+" and body[pos - 1] not in "eE":
                    return complex(float(body[:pos]), float(body[pos + 1:]))
    raise ValueError("no c amplitude_factor line in CNF metadata")


def normalize_amplitude(amplitude: complex, norm_h: int) -> complex:
    return amplitude * (2.0 ** (-norm_h / 2.0))


def row_has_hard_mismatch(row: dict) -> bool:
    return int(row.get("mismatches") or 0) > 0
