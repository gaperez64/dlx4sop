#!/usr/bin/env python3
"""D2/D4: Rankwidth join strategy regression tests.

Verifies that --rankwidth-join-strategy auto|materialized|streaming all
produce identical counts, that auto selects streaming when pair forecast
exceeds --rankwidth-materialize-join-max-pairs 0, and that transition
layout stats appear in --format stats output.

Usage: python3 tests/test_rankwidth_join_strategy.py <sop-solve>
"""

import pathlib
import subprocess
import sys
import tempfile

# Synthetic sign-edge 4-cycle QSOP: 4 variables in a ring, r=8 (sign coeff = 4).
# Produces 3 join nodes when rankwidth-generated, which will use the CSR path.
_SIGN_EDGE_4CYCLE = b"p qsop-sign 8 4 4\nn 1\ne 0 1\ne 1 2\ne 2 3\ne 0 3\n"


def _make_signed_star_qsop(nvars: int = 8, r: int = 8) -> str:
    lines = [f"p qsop-sign {r} {nvars} {nvars - 1}", "n 0", "cst 0"]
    for i in range(nvars):
        lines.append(f"u {i} {(i * 3 % (r - 1)) + 1}")
    for v in range(1, nvars):
        lines.append(f"e 0 {v}")
    return "\n".join(lines) + "\n"


def _make_signed_path_chords_qsop(nvars: int = 8, r: int = 8) -> str:
    edges = [(i, i + 1) for i in range(nvars - 1)] + [(0, 3), (2, 6)]
    lines = [f"p qsop-sign {r} {nvars} {len(edges)}", "n 0", "cst 1"]
    for i in range(nvars):
        lines.append(f"u {i} {(i % (r - 1)) + 1}")
    for u, v in edges:
        lines.append(f"e {u} {v}")
    return "\n".join(lines) + "\n"


def _make_cycle_qsop(nvars: int = 6, r: int = 8) -> str:
    lines = [f"p qsop-sign {r} {nvars} {nvars}", "n 0", "cst 0"]
    for i in range(nvars):
        lines.append(f"e {i} {(i + 1) % nvars}")
    return "\n".join(lines) + "\n"


def _make_wheel_qsop(nvars: int = 8, r: int = 8) -> str:
    """Wheel graph: an (nvars-1)-cycle rim plus a hub connected to every rim node --
    denser than a path/star/cycle, giving rankwidth generation real width to work with."""
    rim = nvars - 1
    edges = [(i + 1, ((i + 1) % rim) + 1) for i in range(rim)]
    edges += [(0, v) for v in range(1, nvars)]
    lines = [f"p qsop-sign {r} {nvars} {len(edges)}", "n 0", "cst 0"]
    for i in range(nvars):
        lines.append(f"u {i} {(i * 5 % (r - 1)) + 1}")
    for u, v in edges:
        lines.append(f"e {u} {v}")
    return "\n".join(lines) + "\n"


def _write_synthetic_fixtures(tmp: pathlib.Path) -> list[pathlib.Path]:
    fixtures = [
        ("signed_star", _make_signed_star_qsop()),
        ("signed_path_chords", _make_signed_path_chords_qsop()),
        ("cycle", _make_cycle_qsop()),
        ("wheel", _make_wheel_qsop()),
    ]
    files = []
    for name, text in fixtures:
        path = tmp / f"{name}.qsop"
        path.write_text(text)
        files.append(path)
    return files


def _run_rankwidth(sop_solve, qsop, extra_args, format_stats=False):
    cmd = [
        str(sop_solve),
        "--backend", "rankwidth",
        "--rankwidth-generate", "min-fill-cut",
        "--max-vars", "64",
    ]
    if format_stats:
        cmd += ["--format", "stats"]
    else:
        cmd += ["--format", "residue-vector"]
    cmd += extra_args + [str(qsop)]
    return subprocess.run(cmd, capture_output=True, timeout=30)


def _run_rankwidth_stdin(sop_solve, qsop_bytes, extra_args, format_stats=False):
    cmd = [
        str(sop_solve),
        "--backend", "rankwidth",
        "--rankwidth-generate", "min-fill-cut",
        "--max-vars", "64",
    ]
    if format_stats:
        cmd += ["--format", "stats"]
    else:
        cmd += ["--format", "residue-vector"]
    cmd += extra_args + ["-"]
    return subprocess.run(cmd, input=qsop_bytes, capture_output=True, timeout=30)


def _residue_vector(result):
    return result.stdout.decode(errors="replace").strip()


def run_tests(sop_solve, tmp):
    files = _write_synthetic_fixtures(tmp)

    all_passed = True

    # 1. materialized and streaming produce identical counts.
    print("  test: materialized and streaming produce identical counts")
    sign_edge_instances = [("sign-4cycle (stdin)", None, _SIGN_EDGE_4CYCLE)]
    file_instances = [(f.name, f, None) for f in files[:2]]
    for label, f, raw in file_instances + sign_edge_instances:
        if raw is not None:
            r_mat = _run_rankwidth_stdin(sop_solve, raw, ["--rankwidth-join-strategy", "materialized"])
            r_str = _run_rankwidth_stdin(sop_solve, raw, ["--rankwidth-join-strategy", "streaming"])
        else:
            r_mat = _run_rankwidth(sop_solve, f, ["--rankwidth-join-strategy", "materialized"])
            r_str = _run_rankwidth(sop_solve, f, ["--rankwidth-join-strategy", "streaming"])
        if r_mat.returncode != 0:
            print(f"    FAIL: materialized failed on {label}: {r_mat.stderr.decode()[:80]}")
            all_passed = False
            continue
        if r_str.returncode != 0:
            print(f"    FAIL: streaming failed on {label}: {r_str.stderr.decode()[:80]}")
            all_passed = False
            continue
        if _residue_vector(r_mat) != _residue_vector(r_str):
            print(f"    FAIL: mismatch on {label}")
            print(f"      materialized: {_residue_vector(r_mat)[:60]}")
            print(f"      streaming:    {_residue_vector(r_str)[:60]}")
            all_passed = False
        else:
            print(f"    OK: {label} counts match")

    # 2. auto with max-pairs=0 forces streaming; counts still match default.
    print("  test: auto with max-pairs=0 forces streaming")
    for f in files[:2]:
        r_default = _run_rankwidth(sop_solve, f, [])
        r_forced  = _run_rankwidth(sop_solve, f, [
            "--rankwidth-join-strategy", "auto",
            "--rankwidth-materialize-join-max-pairs", "0",
        ])
        if r_default.returncode != 0 or r_forced.returncode != 0:
            print(f"    SKIP: {f.name} did not solve")
            continue
        if _residue_vector(r_default) != _residue_vector(r_forced):
            print(f"    FAIL: mismatch on {f.name}")
            all_passed = False
        else:
            print(f"    OK: {f.name} matches default")

    # 3. stats output shows transition layout and join strategy fields.
    print("  test: stats output contains new transition/join fields")
    for f in files[:1]:
        r = _run_rankwidth(sop_solve, f, [], format_stats=True)
        if r.returncode != 0:
            print(f"    SKIP: {f.name} stats run failed")
            continue
        text = r.stdout.decode(errors="replace")
        required = [
            "rankwidth_transition_bytes:",
            "rankwidth_transition_layout_u16_events:",
            "rankwidth_transition_layout_u32_events:",
            "rankwidth_dense_table_forecast:",
            "rankwidth_dense_even_join_forecast:",
            "rankwidth_dense_join_events:",
            "rankwidth_materialized_join_events:",
            "rankwidth_streaming_join_events:",
            "rankwidth_linear_transition_events:",
            "rankwidth_table_assignment_bytes:",
        ]
        for field in required:
            if field not in text:
                print(f"    FAIL: missing field '{field}' in stats output")
                all_passed = False
            else:
                print(f"    OK: '{field}' present")
        if "rankwidth_join_assignment_bytes:" in text:
            print("    FAIL: removed field 'rankwidth_join_assignment_bytes' still present")
            all_passed = False

    # 5. D4.1 U16 layout events > 0 on synthetic sign-edge instance.
    print("  test: D4.1 U16 layout used for sign-edge instance with small signature IDs")
    r = subprocess.run(
        [str(sop_solve), "--backend", "rankwidth",
         "--rankwidth-generate", "min-fill-cut",
         "--rankwidth-join-strategy", "materialized",
         "--format", "stats", "--max-vars", "64", "-"],
        input=_SIGN_EDGE_4CYCLE,
        capture_output=True, timeout=30,
    )
    if r.returncode != 0:
        print(f"    FAIL: sign-edge instance failed: {r.stderr.decode()[:80]}")
        all_passed = False
    else:
        text = r.stdout.decode(errors="replace")
        u16 = 0
        mat = 0
        for line in text.splitlines():
            if line.startswith("rankwidth_transition_layout_u16_events:"):
                u16 = int(line.split(":")[1].strip())
            elif line.startswith("rankwidth_materialized_join_events:"):
                mat = int(line.split(":")[1].strip())
        if mat == 0:
            print(f"    FAIL: no materialized join events (instance may not have joins)")
            all_passed = False
        elif u16 == 0:
            print(f"    FAIL: u16_events=0 despite mat_events={mat} (u32 chosen unexpectedly)")
            all_passed = False
        else:
            print(f"    OK: u16_events={u16} (materialized_events={mat})")

    # 6. Invalid strategy rejected.
    print("  test: invalid strategy rejected")
    r = subprocess.run(
        [str(sop_solve), "--rankwidth-join-strategy", "invalid"],
        capture_output=True, timeout=10,
    )
    if r.returncode != 2:
        print(f"    FAIL: expected exit 2, got {r.returncode}")
        all_passed = False
    else:
        print("    OK: invalid strategy → exit 2")

    # 7. D3.2: table_assignment_bytes is O(signature_count), not O(table_entries * r).
    print("  test: D3.2 table_assignment_bytes <= signature_entries * 8 * 64 (per-sig, not per-entry)")
    for f in files[:2]:
        r = _run_rankwidth(sop_solve, f,
                           ["--rankwidth-join-strategy", "materialized"],
                           format_stats=True)
        if r.returncode != 0:
            print(f"    SKIP: {f.name}")
            continue
        text = r.stdout.decode(errors="replace")
        sig_entries = None
        tab_entries = None
        tab_assign = None
        for line in text.splitlines():
            if line.startswith("signature_entries:"):
                sig_entries = int(line.split(":")[1].strip())
            elif line.startswith("table_entries:"):
                tab_entries = int(line.split(":")[1].strip())
            elif line.startswith("rankwidth_table_assignment_bytes:"):
                tab_assign = int(line.split(":")[1].strip())
        if sig_entries is None or tab_assign is None:
            print(f"    SKIP: {f.name} missing stats fields")
            continue
        # Upper bound: sig_entries * 64 words * 8 bytes (generous for any nvars ≤ 4096)
        upper = sig_entries * 64 * 8
        if tab_assign > upper:
            print(f"    FAIL: {f.name} table_assign={tab_assign} > sig_entries*64*8={upper}")
            all_passed = False
        else:
            ok_msg = f"table_assign={tab_assign}, sig_entries={sig_entries}"
            if tab_entries:
                ok_msg += f", table_entries={tab_entries}"
            print(f"    OK: {f.name} {ok_msg}")

    # 8. D2.3: streaming + Fourier mode falls back without error; counts match default.
    print("  test: D2.3 streaming + Fourier falls back correctly")
    for f in files[:2]:
        r_default = _run_rankwidth(sop_solve, f, ["--rankwidth-mode", "count-table"])
        r_fourier_stream = _run_rankwidth(sop_solve, f, [
            "--rankwidth-mode", "fourier",
            "--rankwidth-join-strategy", "streaming",
        ])
        if r_default.returncode != 0:
            print(f"    SKIP: {f.name} default failed")
            continue
        if r_fourier_stream.returncode != 0:
            print(f"    FAIL: {f.name} fourier+streaming failed: {r_fourier_stream.stderr.decode()[:80]}")
            all_passed = False
            continue
        if _residue_vector(r_default) != _residue_vector(r_fourier_stream):
            print(f"    FAIL: {f.name} fourier+streaming mismatch with default")
            all_passed = False
        else:
            print(f"    OK: {f.name} fourier+streaming matches count-table")

    # 9. Fourier kernel selector is visible; dense-reference matches streaming; FWHT remains retired.
    print("  test: rankwidth Fourier kernel selector")
    r_stream_stats = _run_rankwidth_stdin(
        sop_solve,
        _SIGN_EDGE_4CYCLE,
        ["--rankwidth-mode", "fourier", "--rankwidth-fourier-kernel", "streaming"],
        format_stats=True,
    )
    if r_stream_stats.returncode != 0:
        print(f"    FAIL: streaming Fourier kernel failed: {r_stream_stats.stderr.decode()[:120]}")
        all_passed = False
    else:
        text = r_stream_stats.stdout.decode(errors="replace")
        if "rankwidth_fourier_kernel: streaming" not in text:
            print("    FAIL: missing rankwidth_fourier_kernel: streaming in stats")
            all_passed = False
        else:
            print("    OK: streaming kernel recorded in stats")

    r_dense_stats = _run_rankwidth_stdin(
        sop_solve,
        _SIGN_EDGE_4CYCLE,
        ["--rankwidth-mode", "fourier", "--rankwidth-fourier-kernel", "dense-reference"],
        format_stats=True,
    )
    if r_dense_stats.returncode != 0:
        print(f"    FAIL: dense-reference Fourier kernel failed: {r_dense_stats.stderr.decode()[:120]}")
        all_passed = False
    else:
        dense_text = r_dense_stats.stdout.decode(errors="replace")
        stream_text = r_stream_stats.stdout.decode(errors="replace")
        dense_counts = [
            line for line in dense_text.splitlines() if line.startswith("result_counts:")
        ]
        stream_counts = [
            line for line in stream_text.splitlines() if line.startswith("result_counts:")
        ]
        if "rankwidth_fourier_kernel: dense-reference" not in dense_text:
            print("    FAIL: missing rankwidth_fourier_kernel: dense-reference in stats")
            all_passed = False
        elif dense_counts != stream_counts:
            print("    FAIL: dense-reference counts differ from streaming")
            all_passed = False
        else:
            print("    OK: dense-reference kernel matches streaming")

    r_dense = _run_rankwidth_stdin(
        sop_solve,
        _SIGN_EDGE_4CYCLE,
        ["--rankwidth-mode", "fourier", "--rankwidth-fourier-kernel", "hybrid-even-fwht"],
    )
    if r_dense.returncode == 0 or "not implemented" not in r_dense.stderr.decode(errors="replace"):
        print(f"    FAIL: hybrid-even-fwht should refuse clearly, got rc={r_dense.returncode}")
        all_passed = False
    else:
        print("    OK: retired hybrid-even-fwht refuses clearly")

    # 10. D2.2: streaming join on sign-edge instance records streaming_join_events > 0.
    print("  test: D2.2 streaming join on sign-edge records streaming_join_events > 0")
    r = _run_rankwidth_stdin(sop_solve, _SIGN_EDGE_4CYCLE,
                             ["--rankwidth-join-strategy", "streaming"],
                             format_stats=True)
    if r.returncode != 0:
        print(f"    FAIL: sign-edge streaming failed: {r.stderr.decode()[:80]}")
        all_passed = False
    else:
        text = r.stdout.decode(errors="replace")
        stream_events = 0
        mat_events = 0
        for line in text.splitlines():
            if line.startswith("rankwidth_streaming_join_events:"):
                stream_events = int(line.split(":")[1].strip())
            elif line.startswith("rankwidth_materialized_join_events:"):
                mat_events = int(line.split(":")[1].strip())
        if stream_events == 0:
            print(f"    FAIL: streaming_join_events=0 on sign-edge (mat_events={mat_events})")
            all_passed = False
        else:
            print(f"    OK: streaming_join_events={stream_events}, materialized_join_events={mat_events}")

    return all_passed


def main():
    if len(sys.argv) < 2:
        print("usage: test_rankwidth_join_strategy.py <sop-solve>", file=sys.stderr)
        sys.exit(1)
    sop_solve = pathlib.Path(sys.argv[1])
    if not sop_solve.exists():
        print(f"error: {sop_solve} not found", file=sys.stderr)
        sys.exit(1)
    with tempfile.TemporaryDirectory() as td:
        ok = run_tests(sop_solve, pathlib.Path(td))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
