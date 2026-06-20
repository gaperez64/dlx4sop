#!/usr/bin/env python3
"""dlx4sop unified benchmark command.

Subcommands:

  local     Run committed local QSOP corpus through sop-solve backends only.
  ganak     Run committed QSOP corpus through sop2wmc + Ganak.
  native    Run native simulator comparison from QASM manifests.
  render    Render scoreboard from existing artifacts.
  full      Full pipeline: local + WMC + native + render.
  tune-mqt      Run MQT materialized corpus through sop-solve backends.
  harvest-mqt   Harvest MQT QASM circuits and generate manifests.
  materialize-mqt  Convert MQT manifests to local QSOP corpus.
  profile-mqt   Profile MQT QSOP corpus structure (width, expansion, memory risk).

Examples:

  # Fast local backend tuning (no external tools needed):
  python3 tools/bench.py local \\
      --tier tier-17-32 \\
      --backend treewidth \\
      --backend rankwidth:from-treewidth \\
      --backend branch:auto \\
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
    for enc in (args.encodings or ["amp-soft"]):
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

    summary_path = artifact_dir / "mqt-tuning-summary.md"
    _run([
        sys.executable, str(TOOLS_DIR / "render_scoreboard.py"),
        "--mqt-tuning-jsonl", str(out_jsonl),
        "--output", str(summary_path),
    ])
    return 0


# ---------------------------------------------------------------------------
# Subcommand: render
# ---------------------------------------------------------------------------

def cmd_render(args: argparse.Namespace) -> int:
    """Render scoreboard from existing JSONL artifacts."""
    if args.view == "local":
        return _render_local(args)
    return _render_full(args)


def _render_local(args: argparse.Namespace) -> int:
    """Render local-only scoreboard via tools/render_scoreboard.py --local-jsonl."""
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
    """Render full per-mode scoreboards and index using refresh_scoreboard.py + plot_scoreboard.py."""
    artifact_dir = args.artifact_dir or REPO_ROOT / "artifacts"
    refresh = str(TOOLS_DIR / "refresh_scoreboard.py")
    plot = str(TOOLS_DIR / "plot_scoreboard.py")
    timeout_note = getattr(args, "timeout_note", "") or ""
    rc = 0

    # 1. sign scoreboard + JSON
    sign_cmd = [
        sys.executable, refresh,
        "--artifact-dir", str(artifact_dir),
        "--allow-missing",
        "--qsop-mode", "sign",
        "--assets-subdir", "scoreboard-assets/sign",
        "--output", str(REPO_ROOT / "scoreboard-sign.md"),
        "--json", str(REPO_ROOT / "scoreboard-sign.json"),
    ]
    if timeout_note:
        sign_cmd += ["--timeout-note", timeout_note]
    rc = rc or _run(sign_cmd)

    # 2. labelled scoreboard + JSON
    labelled_cmd = [
        sys.executable, refresh,
        "--artifact-dir", str(artifact_dir),
        "--allow-missing",
        "--qsop-mode", "labelled",
        "--assets-subdir", "scoreboard-assets/labelled",
        "--output", str(REPO_ROOT / "scoreboard-labelled.md"),
        "--json", str(REPO_ROOT / "scoreboard-labelled.json"),
    ]
    if timeout_note:
        labelled_cmd += ["--timeout-note", timeout_note]
    rc = rc or _run(labelled_cmd)

    # 3. sign SVGs
    sign_assets = REPO_ROOT / "scoreboard-assets" / "sign"
    rc = rc or _run([
        sys.executable, plot,
        "--scoreboard-json", str(REPO_ROOT / "scoreboard-sign.json"),
        "--artifact-dir", str(artifact_dir),
        "--qsop-mode", "sign",
        "--output-dir", str(sign_assets),
    ])

    # 4. labelled SVGs
    labelled_assets = REPO_ROOT / "scoreboard-assets" / "labelled"
    rc = rc or _run([
        sys.executable, plot,
        "--scoreboard-json", str(REPO_ROOT / "scoreboard-labelled.json"),
        "--artifact-dir", str(artifact_dir),
        "--qsop-mode", "labelled",
        "--output-dir", str(labelled_assets),
    ])

    # 5. index
    rc = rc or _run([
        sys.executable, refresh,
        "--artifact-dir", str(artifact_dir),
        "--allow-missing",
        "--index",
        "--output", str(REPO_ROOT / "scoreboard.md"),
    ])

    return rc


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
    timeout_note = f"{int(args.timeout)} s" if getattr(args, "timeout", None) else ""
    render_args = argparse.Namespace(
        artifact_dir=artifact_dir,
        view="full",
        output=output,
        json_out=json_out,
        timeout_note=timeout_note,
    )
    rc = rc or cmd_render(render_args)

    return rc


# ---------------------------------------------------------------------------
# Subcommand: harvest-mqt
# ---------------------------------------------------------------------------

def cmd_harvest_mqt(args: argparse.Namespace) -> int:
    """Run harvest_mqt_bench.py to generate MQT QASM manifests."""
    cmd = [
        sys.executable,
        str(TOOLS_DIR / "harvest_mqt_bench.py"),
        "--output-dir", str(args.manifest_dir),
        "--qasm2sop", str(args.qasm2sop),
    ]
    if args.sop_stats:
        cmd += ["--sop-stats", str(args.sop_stats)]
    if args.mqt_source:
        cmd += ["--mqt-source", str(args.mqt_source)]
    for tier in (args.target_tiers or []):
        cmd += ["--target-tier", tier]
    for family in (args.families or []):
        cmd += ["--family", family]
    for size in (args.sizes or []):
        cmd += ["--size", str(size)]
    for opt in (args.opt_levels or []):
        cmd += ["--opt-level", str(opt)]
    if args.verbose:
        cmd += ["--verbose"]
    return _run(cmd)


# ---------------------------------------------------------------------------
# Subcommand: materialize-mqt
# ---------------------------------------------------------------------------

def cmd_materialize_mqt(args: argparse.Namespace) -> int:
    """Run materialize_mqt_qsop_corpus.py to convert manifests to QSOP files."""
    cmd = [
        sys.executable,
        str(TOOLS_DIR / "materialize_mqt_qsop_corpus.py"),
        "--manifest-dir", str(args.manifest_dir),
        "--output-dir", str(args.corpus_dir),
        "--qasm2sop", str(args.qasm2sop),
        "--sop-solve", str(args.sop_solve),
        "--timeout", str(args.timeout),
    ]
    if args.max_rows:
        cmd += ["--max-rows", str(args.max_rows)]
    return _run(cmd)


# ---------------------------------------------------------------------------
# Subcommand: profile-mqt
# ---------------------------------------------------------------------------

def cmd_profile_mqt(args: argparse.Namespace) -> int:
    """Run profile_mqt_qsop.py to profile MQT corpus structure."""
    cmd = [
        sys.executable,
        str(TOOLS_DIR / "profile_mqt_qsop.py"),
        "--corpus-dir", str(args.corpus_dir),
        "--artifact-dir", str(args.artifact_dir),
        "--sop-solve", str(args.sop_solve),
        "--timeout", str(args.timeout),
    ]
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

    # ---- harvest-mqt ----
    _mqt_manifests = REPO_ROOT / "benchmarks" / "manifests" / "mqt"
    _sop_stats = REPO_ROOT / "build" / "sop-stats"
    p_harvest = subs.add_parser("harvest-mqt", help="Harvest MQT QASM circuits and generate manifests")
    p_harvest.add_argument("--manifest-dir", type=pathlib.Path, default=_mqt_manifests)
    p_harvest.add_argument("--qasm2sop", type=pathlib.Path, default=_qasm2sop)
    p_harvest.add_argument("--sop-stats", type=pathlib.Path, default=None)
    p_harvest.add_argument("--mqt-source", type=pathlib.Path, default=None)
    p_harvest.add_argument("--target-tier", action="append", dest="target_tiers", metavar="TIER")
    p_harvest.add_argument("--family", action="append", dest="families", metavar="FAMILY")
    p_harvest.add_argument("--size", action="append", dest="sizes", type=int, metavar="N")
    p_harvest.add_argument("--opt-level", action="append", dest="opt_levels", type=int, metavar="N")
    p_harvest.add_argument("--verbose", action="store_true")
    p_harvest.set_defaults(func=cmd_harvest_mqt)

    # ---- materialize-mqt ----
    _mqt_corpus = REPO_ROOT / "benchmarks" / "corpus" / "sop" / "materialized-external" / "mqt-bench"
    p_mat = subs.add_parser("materialize-mqt", help="Convert MQT manifests to local QSOP corpus")
    p_mat.add_argument("--manifest-dir", type=pathlib.Path, default=_mqt_manifests)
    p_mat.add_argument("--corpus-dir", type=pathlib.Path, default=_mqt_corpus)
    p_mat.add_argument("--qasm2sop", type=pathlib.Path, default=_qasm2sop)
    p_mat.add_argument("--sop-solve", type=pathlib.Path, default=_sop_solve)
    p_mat.add_argument("--timeout", type=float, default=60.0)
    p_mat.add_argument("--max-rows", type=int, default=None)
    p_mat.set_defaults(func=cmd_materialize_mqt)

    # ---- profile-mqt ----
    p_prof = subs.add_parser("profile-mqt", help="Profile MQT QSOP corpus structure")
    p_prof.add_argument("--corpus-dir", type=pathlib.Path, default=_mqt_corpus)
    p_prof.add_argument("--artifact-dir", type=pathlib.Path,
                        default=REPO_ROOT / "artifacts" / "mqt")
    p_prof.add_argument("--sop-solve", type=pathlib.Path, default=_sop_solve)
    p_prof.add_argument("--timeout", type=float, default=30.0)
    p_prof.set_defaults(func=cmd_profile_mqt)

    return parser


def main() -> int:
    sys.path.insert(0, str(TOOLS_DIR))
    parser = build_parser()
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
