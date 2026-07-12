#!/usr/bin/env python3

import pathlib
import subprocess
import sys


def run_case(exe: pathlib.Path, source_root: pathlib.Path, name: str) -> None:
    qasm = source_root / "tests" / "golden" / f"{name}.qasm"
    expected = source_root / "tests" / "golden" / f"{name}.expected"
    completed = subprocess.run(
        [str(exe), str(qasm)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"{name}: qasm2sop failed\n{completed.stderr}")
    expected_text = expected.read_text()
    if completed.stdout != expected_text:
        raise AssertionError(
            f"{name}: imported QSOP mismatch\n"
            f"expected:\n{expected_text}\n"
            f"actual:\n{completed.stdout}\n"
        )


def run_boundary_case(
    exe: pathlib.Path, source_root: pathlib.Path, name: str, options: list[str]
) -> None:
    qasm = source_root / "tests" / "golden" / f"{name}.qasm"
    expected = source_root / "tests" / "golden" / f"{name}.expected"
    completed = subprocess.run(
        [str(exe), *options, str(qasm)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0 or completed.stdout != expected.read_text():
        raise AssertionError(
            f"{name}: unexpected boundary import\n{completed.stdout}\n{completed.stderr}"
        )


def run_cli_paths(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    qasm = source_root / "tests" / "golden" / "qasm_hth.qasm"
    expected = source_root / "tests" / "golden" / "qasm_hth.expected"

    help_result = subprocess.run(
        [str(exe), "--help"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if help_result.returncode != 0 or "usage: qasm2sop" not in help_result.stdout:
        raise AssertionError(f"unexpected --help result:\n{help_result.stdout}\n{help_result.stderr}")

    version_result = subprocess.run(
        [str(exe), "--version"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if version_result.returncode != 0 or version_result.stdout != "qasm2sop 0.4\n":
        raise AssertionError(
            f"unexpected --version result:\n{version_result.stdout}\n{version_result.stderr}"
        )

    stdin_result = subprocess.run(
        [str(exe), "-"],
        input=qasm.read_text(),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if stdin_result.returncode != 0 or stdin_result.stdout != expected.read_text():
        raise AssertionError(f"unexpected stdin result:\n{stdin_result.stdout}\n{stdin_result.stderr}")

    opaque_result = subprocess.run(
        [str(exe), "-"],
        input=qasm.read_text().replace('include "qelib1.inc";\n', 'include "qelib1.inc";\nopaque vendor_gate a;\n'),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if opaque_result.returncode != 0 or opaque_result.stdout != expected.read_text():
        raise AssertionError(
            f"unexpected opaque declaration result:\n{opaque_result.stdout}\n{opaque_result.stderr}"
        )

    # An inert classical register (declared, never written by a measure or read by an if)
    # has no effect on the amplitude, so qasm2sop ignores it: the import must match the
    # creg-free golden byte-for-byte.
    creg_result = subprocess.run(
        [str(exe), "-"],
        input=qasm.read_text().replace('include "qelib1.inc";\n', 'include "qelib1.inc";\ncreg c[2];\n'),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if creg_result.returncode != 0 or creg_result.stdout != expected.read_text():
        raise AssertionError(
            f"unexpected inert creg result:\n{creg_result.stdout}\n{creg_result.stderr}"
        )

    # Classical feed-forward (`if`) is still genuinely dynamic and must be rejected.
    rejected_if = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\ncreg c[1];\nif (c==1) x q[0];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if rejected_if.returncode == 0 or "dynamic or classical" not in rejected_if.stderr:
        raise AssertionError(
            f"expected rejection of if feed-forward:\n{rejected_if.stdout}\n{rejected_if.stderr}"
        )

    # A no-feed-forward measurement is deferred and dropped (coherent identity), and a reset
    # restarts the wire at |0>; both are now accepted, in single and whole-register forms.
    for accepted in (
        "measure q[0] -> c[0];",
        "measure q -> c;",
        "reset q[0];",
        "reset q;",
    ):
        ok = subprocess.run(
            [str(exe), "-"],
            input=f"OPENQASM 2.0;\nqreg q[1];\ncreg c[1];\nh q[0];\n{accepted}\n",
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if ok.returncode != 0 or "p qsop-sign" not in ok.stdout:
            raise AssertionError(f"expected {accepted!r} to be accepted:\n{ok.stdout}\n{ok.stderr}")

    # Malformed measure/reset are still rejected with a specific diagnostic.
    for source, needle in (
        ("measure q[0];", "measure must look like"),
        ("measure q[0] -> ;", "missing its classical target"),
        ("measure r[0] -> c[0];", "unknown qreg"),
        ("reset r[0];", "unknown qreg"),
    ):
        bad = subprocess.run(
            [str(exe), "-"],
            input=f"OPENQASM 2.0;\nqreg q[1];\ncreg c[1];\n{source}\n",
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if bad.returncode == 0 or needle not in bad.stderr:
            raise AssertionError(
                f"expected {source!r} to be rejected with {needle!r}:\n{bad.stdout}\n{bad.stderr}"
            )

    bad_phase = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nu1(pi/3) q[0];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if bad_phase.returncode == 0 or "unsupported u1 phase angle" not in bad_phase.stderr:
        raise AssertionError(f"unexpected bad phase result:\n{bad_phase.stderr}")

    # A theta=0 u-gate lowers to the single phase P(phi+lambda), so odd pi/8-unit phi/lambda are
    # fine (u(0,5pi/8,-3*pi/8)=T imports exactly). theta=+/-pi/2 also has a direct P-H-P
    # lowering. Any other theta takes the general rz-ry-rz path (apply_u3), which -- since phi
    # and lambda no longer need their own halving there -- only rejects when theta itself needs
    # a finer tick than the modulus provides (rz(pi/8)-style; see u_theta_needs_retry below).
    u_theta0_ok = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nu(0,5*pi/8,-3*pi/8) q[0];\n",
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if u_theta0_ok.returncode != 0 or "p qsop-sign" not in u_theta0_ok.stdout:
        raise AssertionError(f"expected theta=0 u-gate to import exactly:\n{u_theta0_ok.stderr}")
    u_half_pi_ok = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nu(pi/2,pi/8,0) q[0];\n",
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if u_half_pi_ok.returncode != 0 or "p qsop-sign 16" not in u_half_pi_ok.stdout:
        raise AssertionError(f"expected theta=pi/2 u-gate to import exactly:\n{u_half_pi_ok.stderr}")
    u_theta_even_ok = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nu(pi/4,pi/8,0) q[0];\n",
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if u_theta_even_ok.returncode != 0 or "p qsop-sign 16" not in u_theta_even_ok.stdout:
        raise AssertionError(
            f"expected even-eighths-theta u-gate to import exactly:\n{u_theta_even_ok.stderr}"
        )
    # theta=pi/8 (an odd eighths-tick) needs the modulus-16 -> cap retry to halve exactly.
    u_theta_needs_retry = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nu(pi/8,pi/8,0) q[0];\n",
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if u_theta_needs_retry.returncode != 0 or "p qsop-sign" not in u_theta_needs_retry.stdout:
        raise AssertionError(
            f"expected pi/8-theta u-gate to import via the modulus retry:\n"
            f"{u_theta_needs_retry.stderr}"
        )
    u_theta_odd = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nu(pi/3,pi/8,0) q[0];\n",
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if u_theta_odd.returncode == 0 or "unsupported u angle" not in u_theta_odd.stderr:
        raise AssertionError(f"expected non-dyadic-theta u-gate rejection:\n{u_theta_odd.stderr}")

    approx_phase = subprocess.run(
        [str(exe), "--approx", "5e-2", "--input", "0", "--output", "0", "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nh q[0];\np(0.37) q[0];\nh q[0];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        approx_phase.returncode != 0
        or "qasm2sop_approx additive_amplitude_error_bound" not in approx_phase.stdout
        or "p qsop-sign" not in approx_phase.stdout
    ):
        raise AssertionError(
            f"unexpected approximate phase result:\n{approx_phase.stdout}\n{approx_phase.stderr}"
        )

    approx_gphase = subprocess.run(
        [str(exe), "--approx=5e-2", "--input", "0", "--output", "0", "-"],
        input="OPENQASM 2.0;\nqreg q[1];\ngphase(0.37);\nid q[0];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if approx_gphase.returncode != 0 or "qasm2sop_approx" not in approx_gphase.stdout:
        raise AssertionError(
            f"unexpected approximate gphase result:\n{approx_gphase.stdout}\n{approx_gphase.stderr}"
        )

    spaced_phase = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nh q[0];\nu1 (3*pi/4) q[0];\nh q[0];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    u1_expected = source_root / "tests" / "golden" / "qasm_u1.expected"
    if spaced_phase.returncode != 0 or spaced_phase.stdout != u1_expected.read_text():
        raise AssertionError(f"unexpected spaced u1 result:\n{spaced_phase.stdout}\n{spaced_phase.stderr}")

    mismatched_qregs = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg a[1];\nqreg b[2];\ncx a, b;\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if mismatched_qregs.returncode == 0 or "matching sizes" not in mismatched_qregs.stderr:
        raise AssertionError(f"unexpected mismatched qreg result:\n{mismatched_qregs.stderr}")

    mismatched_three_qregs = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg a[1];\nqreg b[1];\nqreg c[2];\nccz a, b, c;\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        mismatched_three_qregs.returncode == 0
        or "three-qubit qreg operands must have matching sizes"
        not in mismatched_three_qregs.stderr
    ):
        raise AssertionError(
            f"unexpected mismatched three-qreg result:\n{mismatched_three_qregs.stderr}"
        )

    bad_controlled_phase = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[2];\ncu1(pi/3) q[0], q[1];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        bad_controlled_phase.returncode == 0
        or "unsupported cu1 phase angle" not in bad_controlled_phase.stderr
    ):
        raise AssertionError(
            f"unexpected bad controlled phase result:\n{bad_controlled_phase.stderr}"
        )

    # --no-optimize keeps the sign edge visible: this asserts the *lowering* stays sign-only
    # (the simplification pass would otherwise collapse the pinned edge into the constant).
    approx_controlled_phase = subprocess.run(
        [str(exe), "--no-optimize", "--approx", "5e-2", "--input", "11", "--output", "11", "-"],
        input="OPENQASM 2.0;\nqreg q[2];\ncp(pi/3) q[0], q[1];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        approx_controlled_phase.returncode != 0
        or "\ne " not in approx_controlled_phase.stdout
        or "unsupported non-sign quadratic" in approx_controlled_phase.stderr
    ):
        raise AssertionError(
            "unexpected approximate controlled phase result:\n"
            f"{approx_controlled_phase.stdout}\n{approx_controlled_phase.stderr}"
        )

    approx_gate_sweep = subprocess.run(
        [str(exe), "--approx", "1e0", "--input", "000", "--output", "000", "-"],
        input="""OPENQASM 2.0;
include "qelib1.inc";
qreg q[3];
gphase(0.11);
phase(0.37) q;
u1(0.19) q[0];
p(pi/3) q[1];
rz(0.23) q[0];
rx(0.31) q[1];
ry(0.41) q[2];
u2(0.13,0.17) q[0];
u3(0.11,0.13,0.17) q[1];
u(0.19,0.23,0.29) q[2];
cp(0.21) q[0], q[1];
cu1(0.27) q[1], q[2];
cphase(0.33) q[0], q[2];
crz(0.35) q[1], q[0];
rzz(0.39) q[0], q[1];
rxx(0.43) q[1], q[2];
ryy(0.47) q[0], q[2];
cs q[0], q[1];
ctdg q[1], q[2];
csx q[0], q[2];
csxdg q[2], q[0];
""",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        approx_gate_sweep.returncode != 0
        or "qasm2sop_approx rounded_phase_ops" not in approx_gate_sweep.stdout
        or "p qsop-sign" not in approx_gate_sweep.stdout
    ):
        raise AssertionError(
            f"unexpected approximate gate sweep result:\n"
            f"{approx_gate_sweep.stdout}\n{approx_gate_sweep.stderr}"
        )

    approx_qreg_sweep = subprocess.run(
        [str(exe), "--approx", "1e0", "--input", "0000", "--output", "0000", "-"],
        input="""OPENQASM 2.0;
qreg a[2];
qreg b[2];
rx(0.17) a;
cp(0.23) a, b;
rzz(0.31) a, b;
""",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if approx_qreg_sweep.returncode != 0 or "qasm2sop_approx" not in approx_qreg_sweep.stdout:
        raise AssertionError(
            f"unexpected approximate qreg sweep result:\n"
            f"{approx_qreg_sweep.stdout}\n{approx_qreg_sweep.stderr}"
        )

    bad_approx_gphase_operand = subprocess.run(
        [str(exe), "--approx", "1e0", "-"],
        input="OPENQASM 2.0;\nqreg q[1];\ngphase(0.37) q[0];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        bad_approx_gphase_operand.returncode == 0
        or "gphase gate does not take qubit operands" not in bad_approx_gphase_operand.stderr
    ):
        raise AssertionError(
            f"unexpected bad approximate gphase operand result:\n"
            f"{bad_approx_gphase_operand.stderr}"
        )

    approx_file_input = subprocess.run(
        [str(exe), "--approx", "1e0", "--input", "0", "--output", "0", str(qasm)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if approx_file_input.returncode != 0 or "qasm2sop_approx" not in approx_file_input.stdout:
        raise AssertionError(
            f"unexpected approximate file input result:\n"
            f"{approx_file_input.stdout}\n{approx_file_input.stderr}"
        )

    approx_param_qregs = subprocess.run(
        [str(exe), "--approx", "1e0", "--input", "00", "--output", "00", "-"],
        input="""OPENQASM 2.0;
qreg q[2];
u2(0.13,0.17) q;
u3(0.11,0.13,0.17) q;
u(0.19,0.23,0.29) q;
""",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if approx_param_qregs.returncode != 0 or "qasm2sop_approx" not in approx_param_qregs.stdout:
        raise AssertionError(
            f"unexpected approximate parameter qreg result:\n"
            f"{approx_param_qregs.stdout}\n{approx_param_qregs.stderr}"
        )

    # cphase(pi/4) now lowers through cx + single-qubit phases (same trick as cu/cp), so this
    # imports exactly instead of hitting the old non-sign-quadratic export rejection.
    exact_aliases = subprocess.run(
        [str(exe), "--no-optimize", "--input", "11", "--output", "11", "-"],
        input="OPENQASM 2.0;\nqreg q[2];\ngphase(pi/4);\nphase(pi/4) q[0];\ncphase(pi/4) q[0], q[1];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if exact_aliases.returncode != 0 or "\ne " not in exact_aliases.stdout:
        raise AssertionError(
            f"unexpected exact alias result:\n{exact_aliases.stdout}\n{exact_aliases.stderr}"
        )

    approx_angle_errors = [
        ("p()", "unsupported p phase angle"),
        ("p(pi/0)", "unsupported p phase angle"),
        ("u2(0.1,0.2,0.3)", "unsupported u2 angle list"),
        ("u3(0.1,0.2)", "unsupported u3 angle list"),
    ]
    for gate_text, expected_error in approx_angle_errors:
        failed = subprocess.run(
            [str(exe), "--approx", "1e0", "-"],
            input=f"OPENQASM 2.0;\nqreg q[1];\n{gate_text} q[0];\n",
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if failed.returncode == 0 or expected_error not in failed.stderr:
            raise AssertionError(
                f"unexpected approximate angle error for {gate_text}:\n{failed.stderr}"
            )

    # Same pi/4 lowering improvement as exact_aliases above; this case specifically exercises a
    # space between the gate name and its parenthesized angle.
    spaced_controlled_phase = subprocess.run(
        [str(exe), "--no-optimize", "--input", "11", "--output", "11", "-"],
        input="OPENQASM 2.0;\nqreg q[2];\ncu1 (pi/4) q[0], q[1];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if spaced_controlled_phase.returncode != 0 or "\ne " not in spaced_controlled_phase.stdout:
        raise AssertionError(
            "unexpected spaced controlled phase result:\n"
            f"{spaced_controlled_phase.stdout}\n{spaced_controlled_phase.stderr}"
        )

    # Whitespace inside a gate's parenthesized argument list used to truncate the tokenizer's
    # gate token mid-expression (see apply_controlled_phase's callers around qasm2sop.c's
    # parse_statement). This is what macro-expanded angle arithmetic like
    # scripts/build_external_qasm_manifest.py's inline_simple_gates produces before folding.
    # All four spellings below are the same expression and must import to the same QSOP.
    whitespace_variants = [
        "u(0.564, -pi/2 + pi/2, pi/2 - pi/2) q[0];",
        "u(0.564,-pi/2+pi/2,pi/2-pi/2) q[0];",
        "u(0.1,(-pi/2+pi/2),(pi/2-pi/2)) q[0];",
    ]
    outputs = []
    for statement in whitespace_variants[:2]:
        result = subprocess.run(
            [str(exe), "--approx", "0.001", "--input", "0", "--output", "0", "-"],
            input=f"OPENQASM 2.0;\nqreg q[1];\n{statement}\n",
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode != 0:
            raise AssertionError(f"unexpected whitespace tokenizer result for {statement!r}:\n{result.stderr}")
        outputs.append(result.stdout)
    if outputs[0] != outputs[1]:
        raise AssertionError(
            f"whitespace and no-whitespace variants produced different QSOP:\n{outputs[0]}\n{outputs[1]}"
        )
    nested_parens = subprocess.run(
        [str(exe), "--approx", "0.001", "--input", "0", "--output", "0", "-"],
        input=f"OPENQASM 2.0;\nqreg q[1];\n{whitespace_variants[2]}\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if nested_parens.returncode != 0:
        raise AssertionError(f"unexpected nested-parens tokenizer result:\n{nested_parens.stderr}")

    unbalanced_parens = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nu(pi(2,0,0) q[0];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if unbalanced_parens.returncode == 0 or "unbalanced gate parameter list" not in unbalanced_parens.stderr:
        raise AssertionError(f"unexpected unbalanced-parens result:\n{unbalanced_parens.stderr}")

    # Compound angle arithmetic in parse_angle_radians: equivalent expressions must fold to the
    # same coefficient, both when they cancel to zero and when they combine to a nonzero value.
    angle_equivalences = [
        ("-pi/2+pi/2", "0"),
        ("pi/2-pi/2", "0"),
        ("pi/4+pi/8", "3*pi/8"),
        ("-(pi/2)", "-pi/2"),
    ]
    for lhs, rhs in angle_equivalences:
        results = []
        for expr in (lhs, rhs):
            completed = subprocess.run(
                [str(exe), "--approx", "1e-6", "--input", "0", "--output", "0", "-"],
                input=f"OPENQASM 2.0;\nqreg q[1];\nrz({expr}) q[0];\n",
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            if completed.returncode != 0:
                raise AssertionError(f"unexpected angle arithmetic result for rz({expr}):\n{completed.stderr}")
            results.append(completed.stdout)
        if results[0] != results[1]:
            raise AssertionError(
                f"rz({lhs}) and rz({rhs}) produced different QSOP:\n{results[0]}\n{results[1]}"
            )

    bad_rz = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nrz(pi/3) q[0];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if bad_rz.returncode == 0 or "unsupported rz phase angle" not in bad_rz.stderr:
        raise AssertionError(f"unexpected bad rz result:\n{bad_rz.stderr}")

    # rz(pi/8) is an odd eighths-tick: apply_rz's global -theta/2 term doesn't halve exactly at
    # modulus 16, but does after the retry at QASM_EXACT_MODULUS_MAX.
    rz_eighth_ok = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nrz(pi/8) q[0];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if rz_eighth_ok.returncode != 0 or "p qsop-sign" not in rz_eighth_ok.stdout:
        raise AssertionError(f"expected pi/8-turn rz to import via the modulus retry:\n{rz_eighth_ok.stderr}")

    bad_rx = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nrx(pi/3) q[0];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if bad_rx.returncode == 0 or "unsupported rx phase angle" not in bad_rx.stderr:
        raise AssertionError(f"unexpected bad rx result:\n{bad_rx.stderr}")

    bad_u2 = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nu2(pi/4) q[0];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if bad_u2.returncode == 0 or "unsupported u2 angle list" not in bad_u2.stderr:
        raise AssertionError(f"unexpected bad u2 result:\n{bad_u2.stderr}")

    bad_u3 = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nu3(pi/3,0,0) q[0];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if bad_u3.returncode == 0 or "unsupported u3 angle" not in bad_u3.stderr:
        raise AssertionError(f"unexpected bad u3 result:\n{bad_u3.stderr}")

    error_cases = [
        ([str(exe), "--bad"], "unknown option"),
        ([str(exe), "--input"], "missing value"),
        ([str(exe), "--approx"], "missing value"),
        ([str(exe), "--approx=0", str(qasm)], "positive finite"),
        ([str(exe), str(qasm), str(qasm)], "at most one input"),
        ([str(exe), str(source_root / "tests" / "golden" / "missing.qasm")], "No such file"),
    ]
    for cmd, expected_error in error_cases:
        completed = subprocess.run(
            cmd,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode == 0 or expected_error not in completed.stderr:
            raise AssertionError(f"unexpected error result for {cmd}:\n{completed.stderr}")


def run_boundary_options(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    qasm = source_root / "tests" / "golden" / "qasm_h_boundary.qasm"
    expected = source_root / "tests" / "golden" / "qasm_h_boundary.expected"

    completed = subprocess.run(
        [str(exe), "--input", "1", "--output=1", str(qasm)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0 or completed.stdout != expected.read_text():
        raise AssertionError(f"unexpected boundary import:\n{completed.stdout}\n{completed.stderr}")

    zero_qasm = "OPENQASM 2.0;\nqreg q[1];\nid q[0];\n"
    zero_expected = "p qsop-sign 8 1 0\nn 0\ncst 0\n\nu 0 4\n"
    zero_result = subprocess.run(
        [str(exe), "--input=0", "--output", "1", "-"],
        input=zero_qasm,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if zero_result.returncode != 0 or zero_result.stdout != zero_expected:
        raise AssertionError(f"unexpected zero-boundary import:\n{zero_result.stdout}\n{zero_result.stderr}")

    boundary_errors = [
        ([str(exe), "--input", "00", str(qasm)], "length 2 does not match 1"),
        ([str(exe), "--output", "2", str(qasm)], "must contain only 0 or 1"),
        ([str(exe), "--input", "0", "--input=0", str(qasm)], "duplicate --input"),
    ]
    for cmd, expected_error in boundary_errors:
        failed = subprocess.run(
            cmd,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if failed.returncode == 0 or expected_error not in failed.stderr:
            raise AssertionError(f"unexpected boundary error for {cmd}:\n{failed.stderr}")


def run_decomposed_gates(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    cases = [
        ("qasm_x", ["--input", "0", "--output", "1"]),
        ("qasm_cx", ["--input", "10", "--output", "11"]),
        ("qasm_y", ["--input", "0", "--output", "1"]),
        ("qasm_cy", ["--input", "10", "--output", "11"]),
        ("qasm_rccx", ["--input", "110", "--output", "111"]),
    ]
    for name, options in cases:
        run_boundary_case(exe, source_root, name, options)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_qasm2sop.py QASM2SOP SOURCE_ROOT", file=sys.stderr)
        return 2

    exe = pathlib.Path(sys.argv[1])
    source_root = pathlib.Path(sys.argv[2])
    run_case(exe, source_root, "qasm_hth")
    run_case(exe, source_root, "qasm_cz")
    run_case(exe, source_root, "qasm_swap_id")
    run_case(exe, source_root, "qasm_u1")
    run_case(exe, source_root, "qasm_u1_negative")
    run_case(exe, source_root, "qasm_u2")
    run_boundary_case(exe, source_root, "qasm_u2_eighths",
                      ["--input", "0", "--output", "1"])
    run_case(exe, source_root, "qasm_u3")
    run_boundary_case(exe, source_root, "qasm_u_half_pi",
                      ["--input", "00", "--output", "11"])
    run_boundary_case(exe, source_root, "qasm_u_pi_eighths",
                      ["--input", "0", "--output", "1"])
    run_case(exe, source_root, "qasm_p")
    run_case(exe, source_root, "qasm_sx")
    run_case(exe, source_root, "qasm_sxdg")
    # Uncompute identities collapsed by the Hadamard-simplification pass (default-on).
    run_case(exe, source_root, "qasm_cx_cx")
    run_case(exe, source_root, "qasm_x_x")
    run_case(exe, source_root, "qasm_sx_sxdg")
    run_case(exe, source_root, "qasm_h_h")
    run_case(exe, source_root, "qasm_feynmandd_quadratic")
    run_case(exe, source_root, "qasm_rz")
    run_case(exe, source_root, "qasm_rz_quarter")
    run_boundary_case(exe, source_root, "qasm_rx_quarter", ["--input", "0", "--output", "1"])
    run_boundary_case(exe, source_root, "qasm_ry_quarter", ["--input", "0", "--output", "1"])
    run_case(exe, source_root, "qasm_register_unary")
    # `reset` free-sums the pre-reset variable and restarts the wire at |0>; with optimization off
    # the abandoned Hadamard variable survives as a free variable (a plain no-op would not add it).
    run_boundary_case(exe, source_root, "qasm_reset", ["--no-optimize"])
    run_boundary_case(
        exe, source_root, "qasm_register_cx", ["--input", "1100", "--output", "1111"]
    )
    run_boundary_case(exe, source_root, "qasm_ch", ["--input", "10", "--output", "10"])
    run_boundary_case(exe, source_root, "qasm_crz", ["--input", "10", "--output", "10"])
    run_boundary_case(exe, source_root, "qasm_cry", ["--input", "10", "--output", "10"])
    run_boundary_case(exe, source_root, "qasm_cu", ["--input", "10", "--output", "10"])
    # These all now lower through cx + single-qubit phases (same trick as cu/cp) instead of a
    # direct weighted edge, so they import exactly instead of hitting the non-sign-quadratic
    # export rejection they used to.
    run_boundary_case(exe, source_root, "qasm_cu1", ["--input", "11", "--output", "11"])
    run_boundary_case(exe, source_root, "qasm_crz_quarter", ["--input", "10", "--output", "10"])
    run_boundary_case(exe, source_root, "qasm_cry_quarter", ["--input", "10", "--output", "10"])
    run_boundary_case(exe, source_root, "qasm_rxx_ryy", ["--input", "00", "--output", "11"])
    run_boundary_case(exe, source_root, "qasm_rzz", ["--input", "10", "--output", "10"])
    run_boundary_case(exe, source_root, "qasm_named_cphase", ["--input", "11", "--output", "11"])
    run_boundary_case(
        exe, source_root, "qasm_register_cp", ["--input", "1010", "--output", "1010"]
    )
    run_boundary_case(exe, source_root, "qasm_csx_dcx", ["--input", "10", "--output", "01"])

    # cp(-7*pi/8): an odd multiple of the finest representable tick (pi/8) needs pi/16
    # granularity to lower exactly (see apply_controlled_phase's coeff-parity check) -- no longer
    # a rejection now that exact mode retries at a wider modulus on an angle refusal.
    run_boundary_case(exe, source_root, "qasm_phase_eighth", ["--input", "11", "--output", "11"])
    # A 4-qubit QFT's cp(pi/2^k) chain (k up to 3 here) is the dynamic-modulus retry's marquee
    # case: each controlled-phase needs one more halving than modulus 16 provides.
    run_boundary_case(exe, source_root, "qasm_qft4", ["--input", "1011", "--output", "1010"])
    run_boundary_case(exe, source_root, "qasm_rz_sixteenth", ["--output", "1"])
    # A decimal literal close to pi/1024 (as Qiskit's float dumps produce), snapped by
    # parse_numeric_pi_units's hybrid tolerance and then requiring the modulus retry to halve.
    run_boundary_case(
        exe, source_root, "qasm_cp_decimal_dyadic", ["--input", "11", "--output", "11"]
    )
    run_cli_paths(exe, source_root)
    run_boundary_options(exe, source_root)
    run_decomposed_gates(exe, source_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
