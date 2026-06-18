#!/usr/bin/env python3
"""dlx4sop unified benchmark command.

Subcommands:

  local     Run committed local QSOP corpus through sop-solve backends only.
  ganak     Run committed QSOP corpus through sop2wmc + Ganak.
  native    Run native simulator comparison from QASM manifests.
  render    Render scoreboard from existing artifacts.
  full      Full pipeline: local + WMC + native + render.
  tune-mqt  Run MQT materialized corpus through sop-solve backends.

Examples:

  # Fast local backend tuning (no external tools needed):
  python3 tools/bench.py local \\
      --tier tier-17-32 \\
      --backend treewidth \\
      --backend rankwidth \\
      --backend branch \\
      --timeout 5 \\
      --out artifacts/local/tier-17-32.jsonl

  # Render scoreboard from artifacts:
  python3 tools/bench.py render \\
      --artifact-dir artifacts/local \\
      --view local \\
      --output /tmp/local-scoreboard.md

  # Full scoreboard refresh (requires Ganak):
  python3 tools/bench.py full \\
      --artifact-dir artifacts/full \\
      --ganak /tmp/ganak/ganak \\
      --output scoreboard.md

  # MQT tuning (no external tools needed after materialization):
  python3 tools/bench.py tune-mqt \\
      --corpus benchmarks/corpus/sop/materialized-external/mqt-bench \\
      --artifact-dir artifacts/mqt-tune \\
      --timeout 30
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS_DIR = REPO_ROOT / "tools"


def _run(cmd: list[str]) -> int:
    result = subprocess.run(cmd)
    return result.returncode


# ---------------------------------------------------------------------------
# Subcommand: local
# ---------------------------------------------------------------------------

def cmd_local(args: argparse.Namespace) -> int:
    """Run the local QSOP corpus through sop-solve backends."""
    cmd = [
        sys.executable,
        str(TOOLS_DIR / "bench_sop_local.py"),
        "--sop-solve", str(args.sop_solve),
        "--timeout", str(args.timeout),
    ]
    for corpus_dir in (args.corpus_dirs or []):
        cmd += ["--corpus-dir", str(corpus_dir)]
    for tier in (args.tiers or []):
        cmd += ["--tier", tier]
    for backend in (args.backends or []):
        cmd += ["--backend", backend]
    if args.max_vars is not None:
        cmd += ["--max-vars", str(args.max_vars)]
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        cmd += ["--out", str(args.out)]
    if args.quiet:
        cmd.append("--quiet")
    return _run(cmd)


# ---------------------------------------------------------------------------
# Subcommand: ganak
# ---------------------------------------------------------------------------

def cmd_ganak(args: argparse.Namespace) -> int:
    """Run sop2wmc + Ganak WMC benchmarks."""
    ganak = args.ganak or "ganak"
    cmd = [
        sys.executable,
        str(TOOLS_DIR / "bench_wmc_ganak.py"),
        "--ganak", str(ganak),
        "--sop2wmc", str(args.sop2wmc),
        "--timeout", str(args.timeout),
    ]
    for enc in (args.encodings or ["amp-soft"]):
        cmd += ["--encoding", enc]
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        cmd += ["--output", str(args.out)]
    return _run(cmd)


# ---------------------------------------------------------------------------
# Subcommand: native
# ---------------------------------------------------------------------------

def cmd_native(args: argparse.Namespace) -> int:
    """Run native simulator comparison from QASM manifests."""
    if not args.manifest:
        print("error: --manifest is required for native subcommand", file=sys.stderr)
        return 1
    cmd = [
        sys.executable,
        str(TOOLS_DIR / "bench_qasm_native_simulator.py"),
        "--manifest", str(args.manifest),
        "--qasm2sop", str(args.qasm2sop),
        "--sop-solve", str(args.sop_solve),
        "--timeout", str(args.timeout),
    ]
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        cmd += ["--output", str(args.out)]
    return _run(cmd)


# ---------------------------------------------------------------------------
# Subcommand: tune-mqt
# ---------------------------------------------------------------------------

MQT_DEFAULT_BACKENDS = [
    "treewidth",
    "branch:auto",
    "branch:no-rankwidth",
    "branch:from-treewidth",
    "rankwidth:from-treewidth",
    "rankwidth:best",
]


def cmd_tune_mqt(args: argparse.Namespace) -> int:
    """Run MQT materialized QSOP corpus through sop-solve backends."""
    corpus = args.corpus or (REPO_ROOT / "benchmarks" / "corpus" / "sop" /
                              "materialized-external" / "mqt-bench")
    if not corpus.exists():
        print(f"error: MQT corpus directory not found: {corpus}", file=sys.stderr)
        print("  Run tools/materialize_mqt_qsop_corpus.py first.", file=sys.stderr)
        return 1

    artifact_dir = args.artifact_dir
    artifact_dir.mkdir(parents=True, exist_ok=True)

    out_jsonl = artifact_dir / "mqt-tuning.jsonl"
    backends = args.backends or MQT_DEFAULT_BACKENDS

    cmd = [
        sys.executable,
        str(TOOLS_DIR / "bench_sop_local.py"),
        "--sop-solve", str(args.sop_solve),
        "--corpus-dir", str(corpus),
        "--timeout", str(args.timeout),
        "--out", str(out_jsonl),
    ]
    for b in backends:
        cmd += ["--backend", b]

    rc = _run(cmd)
    if rc != 0:
        return rc

    # Render a compact MQT summary
    summary_path = artifact_dir / "mqt-tuning-summary.md"
    _render_mqt_summary(out_jsonl, summary_path)
    return 0


def _render_mqt_summary(jsonl_path: pathlib.Path, output: pathlib.Path) -> None:
    """Render a simple Markdown summary from MQT tuning results."""
    import json
    import collections

    if not jsonl_path.exists():
        return

    records: list[dict] = []
    with open(jsonl_path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    records.append(json.loads(line))
                except json.JSONDecodeError:
                    pass

    if not records:
        output.write_text("No MQT tuning records found.\n", encoding="utf-8")
        return

    # Group by backend
    by_backend: dict[str, list[dict]] = collections.defaultdict(list)
    for r in records:
        by_backend[r.get("backend", "unknown")].append(r)

    lines = ["# MQT Tuning Summary\n"]
    lines.append(f"Total records: {len(records)}\n")
    lines.append("## Backend performance\n")
    lines.append("| Backend | Solved | Total | Total time |")
    lines.append("|---|---:|---:|---:|")

    for backend, recs in sorted(by_backend.items()):
        solved = sum(1 for r in recs if r.get("status") == "ok")
        total_ns = sum(r.get("elapsed_ns", 0) for r in recs if r.get("status") == "ok")
        ms = total_ns / 1_000_000
        lines.append(f"| {backend} | {solved} | {len(recs)} | {ms:.1f} ms |")

    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


# ---------------------------------------------------------------------------
# Subcommand: render
# ---------------------------------------------------------------------------

def cmd_render(args: argparse.Namespace) -> int:
    """Render scoreboard from existing JSONL artifacts."""
    if args.view == "local":
        return _render_local(args)
    return _render_full(args)


def _render_local(args: argparse.Namespace) -> int:
    """Render local-only scoreboard using scripts/render_scoreboard.py."""
    artifact_dir = args.artifact_dir or REPO_ROOT / "artifacts"
    output = args.output or REPO_ROOT / "scoreboard-local.md"

    # Collect all JSONL files in the artifact dir.
    jsonl_files = sorted(artifact_dir.glob("**/*.jsonl")) if artifact_dir.exists() else []
    if not jsonl_files:
        print(f"warning: no .jsonl files found in {artifact_dir}", file=sys.stderr)

    # Use scripts/render_scoreboard.py for local-format artifacts.
    import json
    import tempfile
    import os

    # Merge all JSONL from artifact_dir into one file for scripts/ renderer.
    records = []
    for jf in jsonl_files:
        try:
            with open(jf, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line:
                        records.append(json.loads(line))
        except Exception as e:
            print(f"warning: could not read {jf}: {e}", file=sys.stderr)

    if not records:
        print("no records to render", file=sys.stderr)
        return 1

    # Normalize to scripts/ render format (name, tier, backend, solve_elapsed_ns, status).
    from bench_common import normalize_record

    normalized = []
    for r in records:
        n = normalize_record(r)
        # scripts/render_scoreboard.py expects "name" key
        row = {
            "name": n.get("instance_id", n.get("name", "")),
            "tier": n.get("tier", ""),
            "backend": n.get("backend", ""),
            "solve_elapsed_ns": n.get("elapsed_ns", n.get("solve_elapsed_ns", 0)),
            "status": n.get("status", ""),
            "nvars": n.get("nvars", 0),
            "nedges": n.get("nedges", 0),
            "r": n.get("r", 0),
            "counts_hash": n.get("counts_hash", ""),
        }
        normalized.append(row)

    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".jsonl", delete=False, encoding="utf-8"
    ) as tmp:
        for row in normalized:
            tmp.write(json.dumps(row) + "\n")
        tmp_path = tmp.name

    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        rc = _run([
            sys.executable,
            str(REPO_ROOT / "scripts" / "render_scoreboard.py"),
            "--local", tmp_path,
            "--out", str(output),
        ])
    finally:
        os.unlink(tmp_path)

    if rc == 0:
        print(f"scoreboard written to {output}", file=sys.stderr)
    return rc


def _render_full(args: argparse.Namespace) -> int:
    """Render full scoreboard using tools/refresh_scoreboard.py."""
    artifact_dir = args.artifact_dir or REPO_ROOT / "artifacts"
    output = args.output or REPO_ROOT / "scoreboard.md"
    json_out = getattr(args, "json_out", None)
    cmd = [
        sys.executable,
        str(TOOLS_DIR / "refresh_scoreboard.py"),
        "--artifact-dir", str(artifact_dir),
        "--allow-missing",
        "--output", str(output),
    ]
    if json_out:
        cmd += ["--json", str(json_out)]
    return _run(cmd)


# ---------------------------------------------------------------------------
# Subcommand: full
# ---------------------------------------------------------------------------

def cmd_full(args: argparse.Namespace) -> int:
    """Full benchmark pipeline: local + WMC + native + render."""
    artifact_dir = args.artifact_dir or REPO_ROOT / "artifacts" / "full"
    artifact_dir.mkdir(parents=True, exist_ok=True)

    rc = 0

    # Local backends
    if not args.skip_solver:
        local_out = artifact_dir / "local.jsonl"
        local_args = argparse.Namespace(
            sop_solve=args.sop_solve,
            corpus_dirs=None,
            tiers=None,
            backends=None,
            max_vars=None,
            timeout=args.timeout,
            out=local_out,
            quiet=False,
        )
        rc = rc or cmd_local(local_args)

    # WMC / Ganak
    if not args.skip_wmc and args.ganak:
        ganak_out = artifact_dir / "ganak.jsonl"
        ganak_args = argparse.Namespace(
            ganak=args.ganak,
            sop2wmc=args.sop2wmc,
            encodings=["amp-and", "amp-soft", "amp-block", "residue-fourier"],
            timeout=args.timeout,
            out=ganak_out,
        )
        rc = rc or cmd_ganak(ganak_args)
    elif not args.skip_wmc and not args.ganak:
        print("info: --ganak not provided; skipping WMC benchmarks", file=sys.stderr)

    # Native simulators
    if not args.skip_native:
        manifests_dir = args.manifests_dir or (REPO_ROOT / "benchmarks" / "manifests")
        if manifests_dir.exists():
            for manifest in sorted(manifests_dir.glob("*.json")):
                native_out = artifact_dir / f"native-{manifest.stem}.jsonl"
                native_args = argparse.Namespace(
                    manifest=manifest,
                    qasm2sop=args.qasm2sop,
                    sop_solve=args.sop_solve,
                    timeout=args.timeout,
                    out=native_out,
                )
                rc = rc or cmd_native(native_args)
        else:
            print("info: no manifests dir; skipping native benchmarks", file=sys.stderr)

    # Render
    output = args.output or REPO_ROOT / "scoreboard.md"
    json_out = getattr(args, "json_out", None)
    render_args = argparse.Namespace(
        artifact_dir=artifact_dir,
        view="full",
        output=output,
        json_out=json_out,
    )
    rc = rc or cmd_render(render_args)

    return rc


# ---------------------------------------------------------------------------
# Argument parser
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="bench.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    subs = parser.add_subparsers(dest="subcommand", metavar="SUBCOMMAND")
    subs.required = True

    # Common paths
    _sop_solve = REPO_ROOT / "build" / "sop-solve"
    _sop2wmc = REPO_ROOT / "build" / "sop2wmc"
    _qasm2sop = REPO_ROOT / "build" / "qasm2sop"

    # ---- local ----
    p_local = subs.add_parser("local", help="Run local QSOP corpus through sop-solve")
    p_local.add_argument("--sop-solve", type=pathlib.Path, default=_sop_solve)
    p_local.add_argument("--corpus-dir", type=pathlib.Path, action="append",
                         dest="corpus_dirs", metavar="DIR")
    p_local.add_argument("--tier", action="append", dest="tiers", metavar="TIER")
    p_local.add_argument("--backend", action="append", dest="backends", metavar="BACKEND")
    p_local.add_argument("--max-vars", type=int, default=None)
    p_local.add_argument("--timeout", type=float, default=10.0)
    p_local.add_argument("--out", type=pathlib.Path, default=None)
    p_local.add_argument("--quiet", action="store_true")
    p_local.set_defaults(func=cmd_local)

    # ---- ganak ----
    p_ganak = subs.add_parser("ganak", help="Run sop2wmc + Ganak WMC benchmarks")
    p_ganak.add_argument("--ganak", type=pathlib.Path, default=None)
    p_ganak.add_argument("--sop2wmc", type=pathlib.Path, default=_sop2wmc)
    p_ganak.add_argument("--encoding", action="append", dest="encodings", metavar="ENC")
    p_ganak.add_argument("--timeout", type=float, default=30.0)
    p_ganak.add_argument("--out", type=pathlib.Path, default=None)
    p_ganak.set_defaults(func=cmd_ganak)

    # ---- native ----
    p_native = subs.add_parser("native", help="Run native simulator comparison")
    p_native.add_argument("--manifest", type=pathlib.Path, required=False)
    p_native.add_argument("--qasm2sop", type=pathlib.Path, default=_qasm2sop)
    p_native.add_argument("--sop-solve", type=pathlib.Path, default=_sop_solve)
    p_native.add_argument("--timeout", type=float, default=30.0)
    p_native.add_argument("--out", type=pathlib.Path, default=None)
    p_native.set_defaults(func=cmd_native)

    # ---- render ----
    p_render = subs.add_parser("render", help="Render scoreboard from artifacts")
    p_render.add_argument("--artifact-dir", type=pathlib.Path, default=None)
    p_render.add_argument(
        "--view",
        choices=["local", "full"],
        default="full",
        help="'local' renders local-only bar chart; 'full' renders full scoreboard",
    )
    p_render.add_argument("--output", type=pathlib.Path, default=None)
    p_render.add_argument("--json", type=pathlib.Path, default=None, dest="json_out",
                          help="Write normalized scoreboard intermediate JSON")
    p_render.set_defaults(func=cmd_render)

    # ---- full ----
    p_full = subs.add_parser("full", help="Full pipeline: local + WMC + native + render")
    p_full.add_argument("--sop-solve", type=pathlib.Path, default=_sop_solve)
    p_full.add_argument("--sop2wmc", type=pathlib.Path, default=_sop2wmc)
    p_full.add_argument("--qasm2sop", type=pathlib.Path, default=_qasm2sop)
    p_full.add_argument("--ganak", type=pathlib.Path, default=None)
    p_full.add_argument("--manifests-dir", type=pathlib.Path, default=None,
                        dest="manifests_dir")
    p_full.add_argument("--artifact-dir", type=pathlib.Path, default=None)
    p_full.add_argument("--output", type=pathlib.Path, default=None)
    p_full.add_argument("--json", type=pathlib.Path, default=None, dest="json_out",
                        help="Write normalized scoreboard intermediate JSON")
    p_full.add_argument("--timeout", type=float, default=30.0)
    p_full.add_argument("--skip-solver", action="store_true")
    p_full.add_argument("--skip-wmc", action="store_true")
    p_full.add_argument("--skip-native", action="store_true")
    p_full.set_defaults(func=cmd_full)

    # ---- tune-mqt ----
    p_mqt = subs.add_parser("tune-mqt", help="Run MQT materialized corpus through sop-solve")
    p_mqt.add_argument("--corpus", type=pathlib.Path, default=None,
                       help="MQT materialized QSOP corpus directory")
    p_mqt.add_argument("--artifact-dir", type=pathlib.Path,
                       default=REPO_ROOT / "artifacts" / "mqt-tune")
    p_mqt.add_argument("--sop-solve", type=pathlib.Path, default=_sop_solve)
    p_mqt.add_argument("--backend", action="append", dest="backends", metavar="BACKEND")
    p_mqt.add_argument("--timeout", type=float, default=30.0)
    p_mqt.set_defaults(func=cmd_tune_mqt)

    return parser


def main() -> int:
    sys.path.insert(0, str(TOOLS_DIR))
    parser = build_parser()
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
