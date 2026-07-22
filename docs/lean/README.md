# A machine-checked rank-width DP for quantum-circuit simulation

Lean 4 + Mathlib formalization of the core results of *"Quadratic Sums-of-Powers for
Fixed-Parameter Tractable Quantum-Circuit Simulation"* (de Colnet, Geerts, Hai, Laarman,
Lee, Pérez): the correctness and (operation-count) runtime of the rank-decomposition dynamic
program for quadratic sums-of-powers, its Fourier-mode and linear-layout variants, and the
coupling to the real circuit amplitude for `{H, T, CZ}` (= Clifford+T, since `S = T²`).

This directory lives inside the [`dlx4sop`](../..) tool repository, alongside the C
implementation of the algorithms it formalizes, and is linked directly from the paper.

## What is proved

**Abstract SOP layer** (over any quadratic SOP instance `I` and rank-decomposition `D`):

| Paper statement | Lean theorem | File |
|---|---|---|
| `lem:chi-well-defined` (crossing parity well-defined) | `SopInstance.chi_well_defined` | `Formal/Core/Signature.lean` |
| `thm:dp-correct` (DP table = counts, returns `N_j`) | `SopInstance.Dalg_eq_Dspec`, `SopInstance.dp_returns_counts` | `Formal/Core/DPCorrect.lean` |
| ≤ 2^k signatures per cut (row-space bound) | `card_image_vecMul`, `SopInstance.card_sig_image_le` | `Formal/Foundations/CutRank.lean`, `Formal/Core/Width.lean` |
| `thm:sop-rw` (op-count `≤ n·(2+(r·2^k)²)`) | `SopInstance.costFull_le` | `Formal/Core/Cost.lean` |
| `thm:fourier-speedup` per-mode (`≤ n·(2+4^k)`) | `SopInstance.costMode_le'` | `Formal/Core/Cost.lean` |
| DFT inversion (eq:inverse-dft) | `SopInstance.N_inversion`, `SopInstance.sum_chi` | `Formal/Core/Fourier.lean` |
| `cor:single-amplitude` (mode 1 computes `S(f)`) | `SopInstance.single_amplitude`, `SopInstance.Aalg_root` | `Formal/Core/Fourier.lean` |
| `thm:linear-layout-fourier` / `cor:lrw` (base 2 per step) | `SopInstance.costMode_layout_le` | `Formal/Core/LinearLayout.lean` |

**Quantum layer** (the honest capstone, about the real amplitude):

| Paper statement | Lean theorem | File |
|---|---|---|
| path-sum compilation (sec:sop) | `Quantum.compile_invariant` | `Formal/Quantum/Coupling.lean` |
| pinning the boundary (sec:sop) | `Quantum.Sym.pathSum_pinned` | `Formal/Quantum/PinningLemmas.lean` |
| **coupling** `⟨z|C|y⟩ = R_C⁻¹·δ_C·S(f_C)` | `Quantum.amplitude_eq_sop` | `Formal/Quantum/Capstone.lean` |
| rank-width FPT simulation (sec:rw-fpt) | `Quantum.amplitude_by_rank_dp` | `Formal/Quantum/Capstone.lean` |

`δ_C` is the Kronecker delta of Hadamard-free wires (the paper folds it into "the pinned
amplitude is 0 on inconsistent pins").

## Scope, stated honestly

* **Runtime** is formalized as *semantic operation counts* (table states created + join pairs
  scanned) — exactly the quantities the paper's runtime proofs bound. No machine model.
  The `O(r log r)` FFT refinement for the inverse transform is not formalized (the paper
  itself notes direct `O(r²)` inversion suffices for fixed `r`; `N_inversion` is the direct
  inversion).
* The decomposition is **algorithmic input** (as in the paper); finding rank-decompositions
  (Oum–Seymour et al.) is out of scope. Rank-width as a *minimum* over decompositions is not
  needed for these statements and is not defined.
* Out of scope (deferred): gadget universality (`thm:gadget-universality`), the structural
  comparisons (`lem:sop-minor-line`, the `Γ_{h,t}` separation, WMC transfer), and the
  matrix-multiplication barrier (`thm:join-mm-barrier`).

## Axiom profile — how to check

Every theorem above depends on exactly Mathlib's three standard axioms
`[propext, Classical.choice, Quot.sound]` — no `sorry`, no custom axioms. Re-check:

```
lake exe cache get      # fetch Mathlib oleans (once)
lake build              # zero errors, zero `declaration uses 'sorry'`
```

then in any file:

```lean
import Formal
#print axioms Formal.Quantum.amplitude_eq_sop
#print axioms Formal.SopInstance.Dalg_eq_Dspec
-- expected: [propext, Classical.choice, Quot.sound]
```

and `grep -rn "sorry\|admit\|native_decide" Formal/` must return nothing.

## License

This directory (`docs/lean/`) is released under the Apache License, Version 2.0 — see
[LICENSE](LICENSE); each source file's header points here. This differs from the MIT
license covering the rest of the `dlx4sop` repository (top-level [LICENSE](../../LICENSE)).

## Build

Lean `4.32.0`, Mathlib `v4.32.0` (pinned in `lean-toolchain` / `lakefile.toml`).

```
lake exe cache get && lake build
```
