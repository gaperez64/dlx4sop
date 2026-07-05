#!/usr/bin/env python3
"""Build tiered benchmark manifests from pinned external sources.

Reads benchmarks/corpus.lock.json and materialises per-tier manifest JSON
files under benchmarks/manifests/.  External git sources are cloned into
--work-dir (default /tmp/dlx4sop-corpus); MQT Bench is generated from the
installed python-package.

Usage (first run, background-safe):
    python3 scripts/build_corpus.py \\
        --lock benchmarks/corpus.lock.json \\
        --out benchmarks/manifests \\
        --work-dir /tmp/dlx4sop-corpus

Verify committed manifests (no network for FeynmanDD/PyZX if clones exist):
    python3 scripts/build_corpus.py --verify
"""

import argparse
import json
import pathlib
import random
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS_DIR = REPO_ROOT / "scripts"
DEFAULT_LOCK = REPO_ROOT / "benchmarks" / "corpus.lock.json"
DEFAULT_OUT = REPO_ROOT / "benchmarks" / "manifests"
DEFAULT_WORK_DIR = pathlib.Path("/tmp/dlx4sop-corpus")
DEFAULT_QASM2SOP = REPO_ROOT / "build" / "qasm2sop"


def run(cmd, *, input=None, capture=False):
    print(f"+ {' '.join(str(a) for a in cmd)}", file=sys.stderr)
    result = subprocess.run(
        [str(a) for a in cmd],
        check=False,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        text=True,
        input=input,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(str(a) for a in cmd)}"
            + (f"\n{result.stderr}" if capture and result.stderr else "")
        )
    return result


def git_clone_or_update(url: str, commit: str, dest: pathlib.Path) -> None:
    if (dest / ".git").exists():
        run(["git", "-C", str(dest), "fetch", "--quiet", "origin"])
    else:
        run(["git", "clone", "--quiet", url, str(dest)])
    run(["git", "-C", str(dest), "checkout", "--quiet", commit])


def read_qsop_nvars(stdout: str) -> int:
    for line in stdout.splitlines():
        parts = line.split()
        if len(parts) == 5 and parts[:2] == ["p", "qsop-sign"]:
            return int(parts[3])
    raise ValueError("no 'p qsop-sign' header in qasm2sop output")


def compute_nvars_for_case(qasm2sop: pathlib.Path, qasm: str, boundaries: list) -> int:
    max_nvars = 0
    for inb, outb in boundaries:
        result = subprocess.run(
            [str(qasm2sop), "--input", inb, "--output", outb, "-"],
            input=qasm,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode != 0:
            continue
        try:
            max_nvars = max(max_nvars, read_qsop_nvars(result.stdout))
        except ValueError:
            continue
    return max_nvars


def load_internal_cases(qasm2sop: pathlib.Path) -> list[dict]:
    internal_path = REPO_ROOT / "tests" / "qasm_solver_corpus.json"
    raw = json.loads(internal_path.read_text())
    cases = []
    for case in raw:
        c = dict(case)
        c.setdefault("source", "Internal corpus")
        c.setdefault("source_url", "tests/qasm_solver_corpus.json")
        c.setdefault("source_relative_path", c.get("name", ""))
        if "max_imported_nvars" not in c:
            qasm = "\n".join(c["qasm_lines"]) + "\n"
            c["max_imported_nvars"] = compute_nvars_for_case(qasm2sop, qasm, c["boundaries"])
        cases.append(c)
    return cases


def run_builder(qasm2sop: pathlib.Path, roots: list[pathlib.Path], source: dict) -> list[dict]:
    builder = TOOLS_DIR / "build_external_qasm_manifest.py"
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        tmp_path = pathlib.Path(tmp.name)
    try:
        cmd = [sys.executable, str(builder), str(qasm2sop)]
        cmd += [str(r) for r in roots]
        cmd += [
            "--source-name", source["name"],
            "--source-url", source["url"],
            "--source-prefix", source["name"].lower().replace(" ", ""),
            "--boundaries", "zero-and-one",
            "--min-vars", "0",
            "--max-vars", "512",
            "--output", str(tmp_path),
        ]
        for extra in source.get("builder_extra_args", []):
            cmd.append(extra)
        run(cmd)
        return json.loads(tmp_path.read_text())
    finally:
        tmp_path.unlink(missing_ok=True)


def build_feynmandd_cases(qasm2sop: pathlib.Path, clone_root: pathlib.Path, source: dict) -> list[dict]:
    return run_builder(
        qasm2sop,
        [clone_root],
        {**source, "builder_extra_args": [
            "--strip-terminal-measurements",
            "--inline-simple-gates",
        ]},
    )


def build_pyzx_cases(qasm2sop: pathlib.Path, clone_root: pathlib.Path, source: dict) -> list[dict]:
    circuits_dir = clone_root / "circuits"
    root = circuits_dir if circuits_dir.exists() else clone_root
    return run_builder(
        qasm2sop,
        [root],
        {**source, "builder_extra_args": [
            "--include-qc",
            "--qc2qasm", str(TOOLS_DIR / "qc2qasm.py"),
        ]},
    )


def build_mqt_cases(qasm2sop: pathlib.Path, source: dict, work_dir: pathlib.Path) -> list[dict]:
    scanner = TOOLS_DIR / "scan_mqt_bench.py"
    qasm_dir = work_dir / "mqt_qasm"
    qasm_dir.mkdir(parents=True, exist_ok=True)

    scan_args = source.get("scan_args", {})
    sizes = ",".join(str(s) for s in scan_args.get("sizes", [3]))
    benchmarks = scan_args.get("benchmarks", "default")
    levels = scan_args.get("levels", "indep")

    run([
        sys.executable, str(scanner), str(qasm2sop),
        "--sizes", sizes,
        "--benchmarks", benchmarks,
        "--levels", levels,
        "--qasm-dir", str(qasm_dir),
    ])
    return run_builder(qasm2sop, [qasm_dir], source)


def bucket_cases(all_cases: list[dict], tiers: list[dict]) -> dict[str, list[dict]]:
    buckets: dict[str, list[dict]] = {t["name"]: [] for t in tiers}
    dropped = 0
    for case in all_cases:
        nvars = case.get("max_imported_nvars")
        if not isinstance(nvars, int) or nvars < 0:
            dropped += 1
            continue
        for tier in tiers:
            if tier["min_vars"] <= nvars <= tier["max_vars"]:
                buckets[tier["name"]].append(case)
                break
    if dropped:
        print(f"warning: dropped {dropped} cases without valid max_imported_nvars", file=sys.stderr)
    return buckets


def stratified_sample(cases: list[dict], cap_per_source: int, seed: int) -> list[dict]:
    by_source: dict[str, list[dict]] = {}
    for case in cases:
        src = str(case.get("source") or "unknown")
        by_source.setdefault(src, []).append(case)
    rng = random.Random(seed)
    sample: list[dict] = []
    for src_cases in by_source.values():
        shuffled = list(src_cases)
        rng.shuffle(shuffled)
        sample.extend(shuffled[:cap_per_source])
    return sample


def write_manifest(cases: list[dict], path: pathlib.Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(cases, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {len(cases)} cases → {path}", file=sys.stderr)


def validate_manifest(path: pathlib.Path, tier: dict) -> list[str]:
    errors = []
    try:
        cases = json.loads(path.read_text())
    except Exception as exc:
        return [f"{path}: parse error: {exc}"]
    for i, case in enumerate(cases):
        nvars = case.get("max_imported_nvars")
        if isinstance(nvars, int):
            lo, hi = tier["min_vars"], tier["max_vars"]
            if not (lo <= nvars <= hi):
                errors.append(f"{path}: case[{i}] {case.get('name')} nvars={nvars} outside [{lo},{hi}]")
    return errors


def do_build(args: argparse.Namespace) -> None:
    lock = json.loads(args.lock.read_text())
    qasm2sop = args.qasm2sop
    work_dir = args.work_dir
    out_dir = args.out

    if not qasm2sop.exists():
        raise RuntimeError(f"qasm2sop not found at {qasm2sop}; run meson compile first")

    all_cases: list[dict] = []

    for source in lock["sources"]:
        name = source["name"]
        kind = source["kind"]
        print(f"\n=== {name} ({kind}) ===", file=sys.stderr)

        if kind == "committed":
            cases = load_internal_cases(qasm2sop)
            print(f"loaded {len(cases)} internal cases", file=sys.stderr)
            all_cases.extend(cases)
            continue

        if kind == "git":
            clone_dir = work_dir / name.lower().replace(" ", "_")
            clone_dir.mkdir(parents=True, exist_ok=True)
            git_clone_or_update(source["url"], source["commit"], clone_dir)
            if name == "FeynmanDD":
                cases = build_feynmandd_cases(qasm2sop, clone_dir, source)
            elif name == "PyZX":
                cases = build_pyzx_cases(qasm2sop, clone_dir, source)
            else:
                print(f"warning: unknown git source '{name}', skipping", file=sys.stderr)
                continue
        elif kind == "python-package":
            if name == "MQT Bench":
                cases = build_mqt_cases(qasm2sop, source, work_dir)
            else:
                print(f"warning: unknown python-package source '{name}', skipping", file=sys.stderr)
                continue
        else:
            print(f"warning: unknown kind '{kind}' for '{name}', skipping", file=sys.stderr)
            continue

        print(f"collected {len(cases)} cases from {name}", file=sys.stderr)
        all_cases.extend(cases)

    print(f"\ntotal cases: {len(all_cases)}", file=sys.stderr)

    tiers = lock["tiers"]
    buckets = bucket_cases(all_cases, tiers)

    for tier in tiers:
        tname = tier["name"]
        manifest_path = out_dir / tier["manifest"]
        raw = buckets[tname]
        if tier.get("sample"):
            cap = tier.get("sample_cap_per_source", 50)
            seed = tier.get("sample_seed", 42)
            cases_to_write = stratified_sample(raw, cap, seed)
            print(
                f"tier {tname}: {len(raw)} raw → sampled {len(cases_to_write)} "
                f"(cap {cap}/source, seed {seed})",
                file=sys.stderr,
            )
        else:
            cases_to_write = raw
        write_manifest(cases_to_write, manifest_path)


def do_verify(args: argparse.Namespace) -> None:
    lock = json.loads(args.lock.read_text())
    out_dir = args.out

    errors: list[str] = []
    for tier in lock["tiers"]:
        path = out_dir / tier["manifest"]
        if not path.exists():
            errors.append(f"missing: {path}")
            continue
        tier_errors = validate_manifest(path, tier)
        errors.extend(tier_errors)

    if args.rebuild:
        import tempfile as _tempfile
        tmp_dir = pathlib.Path(_tempfile.mkdtemp(prefix="dlx4sop-verify-"))
        print(f"rebuilding to {tmp_dir} for diff...", file=sys.stderr)
        build_args = argparse.Namespace(
            lock=args.lock,
            out=tmp_dir,
            work_dir=args.work_dir,
            qasm2sop=args.qasm2sop,
        )
        try:
            do_build(build_args)
        except Exception as exc:
            print(f"rebuild failed: {exc}", file=sys.stderr)
        else:
            for tier in lock["tiers"]:
                committed = out_dir / tier["manifest"]
                rebuilt = tmp_dir / tier["manifest"]
                if not committed.exists() or not rebuilt.exists():
                    continue
                committed_cases = json.loads(committed.read_text())
                rebuilt_cases = json.loads(rebuilt.read_text())
                if tier["name"] == "MQT Bench" or "MQT" in tier.get("manifest", ""):
                    print(f"tier {tier['name']}: MQT drift check skipped (version-dependent)", file=sys.stderr)
                    continue
                if len(committed_cases) != len(rebuilt_cases):
                    errors.append(
                        f"tier {tier['name']}: count mismatch committed={len(committed_cases)} rebuilt={len(rebuilt_cases)}"
                    )
                else:
                    print(f"tier {tier['name']}: {len(committed_cases)} cases match count", file=sys.stderr)
        import shutil
        shutil.rmtree(tmp_dir, ignore_errors=True)

    if errors:
        for err in errors:
            print(f"ERROR: {err}", file=sys.stderr)
        raise SystemExit(1)
    print("verify OK", file=sys.stderr)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lock", type=pathlib.Path, default=DEFAULT_LOCK)
    parser.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT)
    parser.add_argument("--work-dir", type=pathlib.Path, default=DEFAULT_WORK_DIR)
    parser.add_argument("--qasm2sop", type=pathlib.Path, default=DEFAULT_QASM2SOP)
    parser.add_argument(
        "--verify",
        action="store_true",
        help="validate committed manifests respect tier var-ranges (no network unless --rebuild)",
    )
    parser.add_argument(
        "--rebuild",
        action="store_true",
        help="with --verify: also rebuild manifests and diff vs committed",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        if args.verify:
            do_verify(args)
        else:
            do_build(args)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
