#!/usr/bin/env python3
"""Refresh the local scoreboard by running bench_sop.py and optional external tools.

DEPRECATED: use tools/bench.py instead.
    python3 tools/bench.py local --out artifacts/local.jsonl
    python3 tools/bench.py render --artifact-dir artifacts --view local --output scoreboard.md
    python3 tools/bench.py full --ganak /tmp/ganak/ganak --output scoreboard.md

This script remains for backwards compatibility but tools/bench.py is authoritative.

For local-only refresh (no external tools required):
    python3 scripts/refresh_scoreboard.py \\
        --out results/local_backends.jsonl

For external refresh (Ganak, native simulators, etc.):
    python3 scripts/refresh_scoreboard.py \\
        --refresh-ganak \\
        --refresh-native \\
        --out results/scoreboard_refresh.jsonl

After running, render the scoreboard:
    python3 scripts/render_scoreboard.py \\
        --local results/local_backends.jsonl \\
        --out scoreboard.md
"""

import argparse
import json
import pathlib
import subprocess
import sys
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_SOP_SOLVE = REPO_ROOT / "build" / "sop-solve"
DEFAULT_SOP2WMC = REPO_ROOT / "build" / "sop2wmc"
DEFAULT_CORPUS = REPO_ROOT / "benchmarks" / "corpus" / "sop"


def run_bench_sop(sop_solve: pathlib.Path, corpus_dir: pathlib.Path,
                  tiers: list[str] | None, backends: list[str] | None,
                  timeout: float, out_path: pathlib.Path) -> int:
    cmd = [
        sys.executable,
        str(REPO_ROOT / "scripts" / "bench_sop.py"),
        "--sop-solve", str(sop_solve),
        "--corpus-dir", str(corpus_dir),
        "--timeout", str(timeout),
        "--output", str(out_path),
    ]
    if tiers:
        for t in tiers:
            cmd += ["--tier", t]
    if backends:
        for b in backends:
            cmd += ["--backend", b]
    print(f"[local] running bench_sop.py → {out_path}", file=sys.stderr)
    result = subprocess.run(cmd, capture_output=False)
    return result.returncode


def _ganak_result_hash(stdout: str) -> str:
    """Extract a short hash of the complex WMC result from ganak output."""
    import hashlib
    for line in stdout.splitlines():
        if line.startswith("c s exact quadruple float"):
            return hashlib.sha1(line.encode()).hexdigest()[:12]
    return ""


def run_ganak_refresh(sop2wmc: pathlib.Path, corpus_dir: pathlib.Path,
                      timeout: float, out_path: pathlib.Path,
                      encodings: list[str]) -> None:
    """Run sop2wmc + ganak for each encoding and append results to out_path."""
    import subprocess
    ganak_bin = pathlib.Path("ganak")  # must be in PATH

    written = 0
    with open(out_path, "a", encoding="utf-8") as fout:
        for tier_dir in sorted(corpus_dir.iterdir()):
            if not tier_dir.is_dir():
                continue
            for qsop in sorted(tier_dir.glob("*.qsop")):
                meta_path = qsop.with_suffix(".meta.json")
                meta = {}
                if meta_path.exists():
                    with open(meta_path, encoding="utf-8") as f:
                        meta = json.load(f)
                for enc in encodings:
                    t0 = time.monotonic()
                    try:
                        wmc_proc = subprocess.run(
                            [str(sop2wmc), "--encoding", enc, str(qsop)],
                            capture_output=True, text=True, timeout=timeout)
                        if wmc_proc.returncode != 0:
                            status = "error"
                            wall_ms = (time.monotonic() - t0) * 1000
                            result_hash = ""
                        else:
                            ganak_proc = subprocess.run(
                                [str(ganak_bin), "--mode", "6", "--verb", "0",
                                 "/dev/stdin"],
                                input=wmc_proc.stdout,
                                capture_output=True, text=True, timeout=timeout)
                            wall_ms = (time.monotonic() - t0) * 1000
                            status = "ok" if ganak_proc.returncode == 0 else "error"
                            result_hash = _ganak_result_hash(ganak_proc.stdout)
                    except subprocess.TimeoutExpired:
                        wall_ms = timeout * 1000
                        status = "timeout"
                        result_hash = ""
                    record = {
                        "schema": "sop_external_bench_result_v1",
                        "instance_id": qsop.stem,
                        "tier": tier_dir.name,
                        "external_source": "local-corpus-ganak",
                        "runner_kind": "ganak",
                        "runner_name": f"ganak-{enc}",
                        "status": status,
                        "wall_ms": round(wall_ms, 3),
                        "max_rss_kb": 0,
                        "result_hash": result_hash,
                        "nvars": meta.get("nvars", 0),
                        "r": meta.get("r", 0),
                        "encoding": enc,
                    }
                    fout.write(json.dumps(record) + "\n")
                    written += 1
                    print(f"  [ganak/{enc}] {qsop.stem}: {status} ({wall_ms:.1f} ms)",
                          file=sys.stderr)
    print(f"[ganak] wrote {written} records → {out_path}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sop-solve", type=pathlib.Path, default=DEFAULT_SOP_SOLVE)
    parser.add_argument("--sop2wmc", type=pathlib.Path, default=DEFAULT_SOP2WMC)
    parser.add_argument("--corpus-dir", type=pathlib.Path, default=DEFAULT_CORPUS)
    parser.add_argument("--tier", action="append", dest="tiers",
                        help="restrict to these tiers")
    parser.add_argument("--backend", action="append", dest="backends",
                        help="local backends to benchmark")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("results/local_backends.jsonl"))
    parser.add_argument("--refresh-ganak", action="store_true",
                        help="also run Ganak (requires ganak in PATH and sop2wmc)")
    parser.add_argument("--ganak-encoding", action="append", dest="ganak_encodings",
                        help="WMC encoding for Ganak runs (default: amp-soft)")
    parser.add_argument("--refresh-native", action="store_true",
                        help="placeholder: run native simulators (not yet implemented)")
    args = parser.parse_args()

    if not args.sop_solve.exists():
        print(f"error: sop-solve not found at {args.sop_solve}", file=sys.stderr)
        print("  run: meson compile -C build", file=sys.stderr)
        return 1

    args.out.parent.mkdir(parents=True, exist_ok=True)

    # Local backend benchmark.
    rc = run_bench_sop(args.sop_solve, args.corpus_dir, args.tiers, args.backends,
                       args.timeout, args.out)
    if rc != 0:
        print(f"error: bench_sop.py exited with code {rc}", file=sys.stderr)
        return rc

    # Optional Ganak refresh.
    if args.refresh_ganak:
        if not args.sop2wmc.exists():
            print(f"error: sop2wmc not found at {args.sop2wmc}", file=sys.stderr)
            return 1
        encodings = args.ganak_encodings or ["amp-soft"]
        ganak_out = args.out.parent / "scoreboard_refresh.jsonl"
        try:
            run_ganak_refresh(args.sop2wmc, args.corpus_dir, args.timeout,
                              ganak_out, encodings)
        except FileNotFoundError:
            print("error: ganak not found in PATH; skipping Ganak refresh", file=sys.stderr)

    if args.refresh_native:
        print(
            "[native] The local QSOP corpus has no QASM manifest; "
            "native-simulator benchmarks require QASM manifests.\n"
            "  For a full refresh including native simulators, use:\n"
            "    python3 tools/bench.py full --ganak <path> --manifests benchmarks/manifests\n"
            "  To run native only from an existing manifest:\n"
            "    python3 tools/bench.py native --manifest benchmarks/manifests/<manifest>.json",
            file=sys.stderr,
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
