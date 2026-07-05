#!/usr/bin/env python3
"""dlx4sop unified benchmark command.

Subcommands:

  local     Run committed local QSOP corpus through sop-solve backends only.
  ganak     Run committed QSOP corpus through sop2wmc + Ganak.
  native    Run native simulator comparison from QASM manifests.
  render    Render scoreboard from existing artifacts.
  full      Full pipeline: local + WMC + native + render.

Examples:

  # Fast local backend tuning (no external tools needed):
  python3 scripts/bench.py local \\
      --tier tier-17-32 \\
      --backend treewidth \\
      --backend rankwidth:from-treewidth \\
      --backend branch:auto \\
      --timeout 5 \\
      --out artifacts/local/tier-17-32.jsonl

  # Render scoreboard from artifacts:
  python3 scripts/bench.py render \\
      --artifact-dir artifacts/local \\
      --view local \\
      --output /tmp/local-scoreboard.md

  # Full scoreboard refresh (requires Ganak):
  python3 scripts/bench.py full \\
      --artifact-dir artifacts/full \\
      --ganak /tmp/ganak/ganak \\
      --output scoreboard.md
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS_DIR = REPO_ROOT / "scripts"


def _run(cmd: list[str]) -> int:
    result = subprocess.run(cmd)
    return result.returncode


def append_wmc_tuning_args(cmd: list, args: argparse.Namespace, *, block: bool = True) -> None:
    """Append WMC exporter tuning flags shared by benchmark frontends."""
    if getattr(args, "wmc_preprocess", None) is not None:
        cmd += ["--wmc-preprocess", str(args.wmc_preprocess)]
    if getattr(args, "wmc_peel2_fill_budget", None) is not None:
        cmd += ["--wmc-peel2-fill-budget", str(args.wmc_peel2_fill_budget)]
    if block:
        if getattr(args, "wmc_block_min_side", None) is not None:
            cmd += ["--wmc-block-min-side", str(args.wmc_block_min_side)]
        if getattr(args, "wmc_block_min_savings", None) is not None:
            cmd += ["--wmc-block-min-savings", str(args.wmc_block_min_savings)]


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
    corpus_root = REPO_ROOT / "benchmarks" / "corpus" / "sop"
    instances = sorted(corpus_root.glob("**/*.qsop"))
    if not instances:
        print("info: no QSOP instances found in corpus; skipping WMC benchmarks", file=sys.stderr)
        return 0
    cmd = [
        sys.executable,
        str(TOOLS_DIR / "bench_wmc_ganak.py"),
        "--ganak", str(ganak),
        "--sop2wmc", str(args.sop2wmc),
        "--ganak-timeout", str(args.timeout),
        "--format", "jsonl",
    ]
    append_wmc_tuning_args(cmd, args, block=True)
    for enc in (args.encodings or ["amp-block"]):
        cmd += ["--encoding", enc]
    cmd += [str(p) for p in instances]
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with open(args.out, "w") as fout:
            result = subprocess.run(cmd, stdout=fout)
        return result.returncode
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
        "--format", "jsonl",
        "--timeout", str(args.timeout),
        str(args.manifest),
    ]
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with open(args.out, "w") as fout:
            result = subprocess.run(cmd, stdout=fout)
        return result.returncode
    return _run(cmd)


# ---------------------------------------------------------------------------
# Subcommand: render
# ---------------------------------------------------------------------------

def cmd_render(args: argparse.Namespace) -> int:
    """Render scoreboard from existing JSONL artifacts."""
    if args.view == "local":
        return _render_local(args)
    return _render_full(args)


def _render_local(args: argparse.Namespace) -> int:
    """Render local-only scoreboard via scripts/render_scoreboard.py --local-jsonl."""
    artifact_dir = args.artifact_dir or REPO_ROOT / "artifacts"
    output = args.output or REPO_ROOT / "scoreboard-local.md"

    jsonl_files = sorted(artifact_dir.glob("**/*.jsonl")) if artifact_dir.exists() else []
    if not jsonl_files:
        print(f"warning: no .jsonl files found in {artifact_dir}", file=sys.stderr)

    cmd = [sys.executable, str(TOOLS_DIR / "render_scoreboard.py")]
    for path in jsonl_files:
        cmd += ["--local-jsonl", str(path)]
    cmd += ["--output", str(output)]

    output.parent.mkdir(parents=True, exist_ok=True)
    rc = _run(cmd)
    if rc == 0:
        print(f"scoreboard written to {output}", file=sys.stderr)
    return rc


def _render_full(args: argparse.Namespace) -> int:
    """Render the signed scoreboard and plots using refresh_scoreboard.py + plot_scoreboard.py."""
    artifact_dir = args.artifact_dir or REPO_ROOT / "artifacts"
    refresh = str(TOOLS_DIR / "refresh_scoreboard.py")
    plot = str(TOOLS_DIR / "plot_scoreboard.py")
    timeout_note = getattr(args, "timeout_note", "") or ""
    rc = 0

    refresh_cmd = [
        sys.executable, refresh,
        "--artifact-dir", str(artifact_dir),
        "--allow-missing",
        "--qsop-mode", "sign",
        "--assets-subdir", "scoreboard-assets",
        "--output", str(REPO_ROOT / "scoreboard.md"),
        "--json", str(REPO_ROOT / "scoreboard.json"),
    ]
    if timeout_note:
        refresh_cmd += ["--timeout-note", timeout_note]
    rc = rc or _run(refresh_cmd)

    # SVGs include the WMC-vs-solver scaling plot.
    scaling_timeout = "30"
    if timeout_note:
        digits = "".join(ch for ch in timeout_note if ch.isdigit())
        if digits:
            scaling_timeout = digits
    rc = rc or _run([
        sys.executable, plot,
        "--scoreboard-json", str(REPO_ROOT / "scoreboard.json"),
        "--artifact-dir", str(artifact_dir),
        "--qsop-mode", "sign",
        "--output-dir", str(REPO_ROOT / "scoreboard-assets"),
        "--scaling-timeout", scaling_timeout,
    ])

    return rc


# ---------------------------------------------------------------------------
# Subcommand: full
# ---------------------------------------------------------------------------

def cmd_full(args: argparse.Namespace) -> int:
    """Full benchmark pipeline.

    Thin alias for run_corpus_benchmarks.py — the single orchestrator that runs the
    per-tier solver, WMC, native, and MQT jobs (writing canonically-named artifacts) and
    then renders the per-mode scoreboards. Keeping one implementation avoids the earlier
    drift where this command wrote artifact names the renderer could not find.
    """
    artifact_dir = args.artifact_dir or REPO_ROOT / "artifacts" / "full"
    cmd = [
        sys.executable, str(TOOLS_DIR / "run_corpus_benchmarks.py"),
        "--artifact-dir", str(artifact_dir),
        "--sop-solve", str(args.sop_solve),
        "--sop2wmc", str(args.sop2wmc),
        "--qasm2sop", str(args.qasm2sop),
        "--timeout", str(args.timeout),
    ]
    if args.ganak:
        cmd += ["--ganak", str(args.ganak)]
    if args.manifests_dir:
        cmd += ["--manifests", str(args.manifests_dir)]
    if args.skip_solver:
        cmd += ["--skip-solver"]
    if args.skip_wmc:
        cmd += ["--skip-wmc"]
    if args.skip_native:
        cmd += ["--skip-native"]
    if getattr(args, "skip_scoreboard", False):
        cmd += ["--skip-scoreboard"]
    append_wmc_tuning_args(cmd, args, block=True)
    return _run(cmd)


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
    p_ganak.add_argument("--wmc-preprocess", choices=["none", "peel1", "peel2-safe"],
                         default="peel2-safe",
                         help="sop2wmc preprocessing for amplitude-style WMC jobs "
                              "(default: peel2-safe; use none for ablations)")
    p_ganak.add_argument("--wmc-peel2-fill-budget", type=int, default=None,
                         help="sop2wmc peel2-safe fill budget")
    p_ganak.add_argument("--wmc-block-min-side", type=int, default=2,
                         help="amp-block minimum side size for optimized WMC rows")
    p_ganak.add_argument("--wmc-block-min-savings", type=int, default=1,
                         help="amp-block minimum positive savings threshold")
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
    p_render.add_argument("--timeout-note", default="", metavar="TEXT", dest="timeout_note",
                          help="Note the per-instance timeout in the mode scoreboard header (e.g. '30 s')")
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
    p_full.add_argument("--wmc-preprocess", choices=["none", "peel1", "peel2-safe"],
                        default="peel2-safe",
                        help="sop2wmc preprocessing for amplitude-style WMC jobs "
                             "(default: peel2-safe; use none for ablations)")
    p_full.add_argument("--wmc-peel2-fill-budget", type=int, default=None,
                        help="sop2wmc peel2-safe fill budget")
    p_full.add_argument("--wmc-block-min-side", type=int, default=2,
                        help="amp-block minimum side size for optimized WMC rows")
    p_full.add_argument("--wmc-block-min-savings", type=int, default=1,
                        help="amp-block minimum positive savings threshold")
    p_full.add_argument("--skip-solver", action="store_true")
    p_full.add_argument("--skip-wmc", action="store_true")
    p_full.add_argument("--skip-native", action="store_true")
    p_full.add_argument("--skip-scoreboard", action="store_true")
    p_full.set_defaults(func=cmd_full)

    return parser


def main() -> int:
    sys.path.insert(0, str(TOOLS_DIR))
    parser = build_parser()
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
