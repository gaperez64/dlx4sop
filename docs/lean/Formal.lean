/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Foundations.CutRank
import Formal.Core.DPCorrect
import Formal.Core.Width
import Formal.Core.Cost
import Formal.Core.Fourier
import Formal.Core.LinearLayout
import Formal.Quantum.Capstone
import Formal.Paper
import Formal.Stab.Paper

/-!
# Rank-width FPT quantum-circuit simulation — library root

`import Formal` exposes the whole development. Headline results:

* `Formal.SopInstance.Dalg_eq_Dspec`, `dp_returns_counts` — DP correctness (thm:dp-correct)
* `Formal.SopInstance.card_sig_image_le` — ≤ 2^k signatures per cut
* `Formal.SopInstance.costFull_le`, `costMode_le'` — op-count runtime (thm:sop-rw / fourier)
* `Formal.SopInstance.single_amplitude`, `N_inversion` — Fourier mode-1 / inversion
* `Formal.SopInstance.costMode_layout_le` — linear-layout base-2 bound (cor:lrw)
* `Formal.Quantum.amplitude_eq_sop`, `amplitude_by_rank_dp` — the honest capstone
* `Formal.SopInstance.rank_dp_spec`, `fourier_mode_spec`, `linear_fourier_mode_spec` and
  `Formal.Quantum.amplitude_eq_sop_normalized`, `amplitude_by_rank_dp_normalized`. These are
  the exact paper-facing compositions used by the Lean badges.

The stabilizer-rank join (`sec:stabjoin`) adds:

* `Formal.Stab.IsQPF`, `Formal.Stab.QPF` — quadratic phase functions (def:qpf)
* `Formal.Stab.qpf_closure_spec` — closure under products, single-variable summation and
  pushforward (lem:qpf-closure)
* `Formal.Stab.stab_join_spec` — the join stays inside the class (lem:stab-join)
* `Formal.Stab.magic_decomp_spec` — the `2^τ` magic decomposition (lem:magic-decomp)
* `Formal.Stab.clifford_collapse_spec` — the Ideal case: a single QPF and `6·|V|` operations,
  independent of the width (Gottesman–Knill through the SOP lens)
* `Formal.Stab.hybrid_never_loses_spec` — the Bad case: the same `|V|·(2+4^k)` bound as the
  plain per-mode join (thm:hybrid-dp)
-/
