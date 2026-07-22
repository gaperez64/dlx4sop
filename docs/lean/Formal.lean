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

/-!
# Rank-width FPT quantum-circuit simulation — library root

`import Formal` exposes the whole development. Headline results:

* `Formal.SopInstance.Dalg_eq_Dspec`, `dp_returns_counts` — DP correctness (thm:dp-correct)
* `Formal.SopInstance.card_sig_image_le` — ≤ 2^k signatures per cut
* `Formal.SopInstance.costFull_le`, `costMode_le'` — op-count runtime (thm:sop-rw / fourier)
* `Formal.SopInstance.single_amplitude`, `N_inversion` — Fourier mode-1 / inversion
* `Formal.SopInstance.costMode_layout_le` — linear-layout base-2 bound (cor:lrw)
* `Formal.Quantum.amplitude_eq_sop`, `amplitude_by_rank_dp` — the honest capstone
-/
