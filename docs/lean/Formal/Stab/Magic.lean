/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Core.Fourier
import Formal.Stab.QPF

/-!
# Magic vertices and the magic decomposition (`lem:magic-decomp`)

The mode-`a` weight of an assignment `z` is `χ(a · φ(z))`, where

  `φ(z) = ∑_v b_v z_v + η ∑_{{v,w}∈E} z_v z_w`,   `η = r/2`.

Because `η = r/2`, the quadratic part contributes only *signs* (`chi_half`), so it is an
`F₂`-quadratic form (`selCount_cast`) and lands inside a `Formal.Stab.QPF` verbatim.  The
unary part factors vertex by vertex.  Call a vertex `v` **`a`-Clifford** when its unary phase
`ω_r^{a b_v}` is a power of `i`, i.e. `(r/4) ∣ (a · b_v).val` (`SopInstance.ACliff`), and
**`a`-magic** otherwise (`SopInstance.AMagic`).  Powers of `i` only make sense when `4 ∣ r`
(for `{H,T,CZ}` one takes `r = 8`), so `4 ∣ I.r` is an explicit hypothesis everywhere, never
a structure axiom.

## Results

* `SopInstance.isQPF_modeWeight_of_cliff` — **the Ideal case**: if *every* vertex is
  `a`-Clifford then the whole mode-`a` weight is a single QPF.  Through the SOP lens this is
  the Gottesman–Knill theorem: `Rebuild` returns one QPF at every vertex, for *any* graph, at
  any rank-width.  Its two named corollaries are `isQPF_modeWeight_of_even_b` (a Clifford
  instance: every `b_v` even, at `r = 8`) and `isQPF_modeWeight_of_even_mode` (every even
  Fourier mode, at `r = 8`).
* `SopInstance.modeWeight_sum_qpf` — **`lem:magic-decomp`**: in general the mode-`a` weight is
  a sum of QPFs indexed by the powerset of the set of `a`-magic vertices, hence a sum of
  `2^τ` QPFs (`SopInstance.card_powerset_magicSet`), `τ` the number of magic vertices.  The
  source of the `2` is the invertibility of `[[1,1],[1,i]]`, formalized as `one_bit_decomp`.

## Honesty / scope

Only the elementary `2^τ` bound is formalized.  The improvement `sr(τ) = O(2^{0.3963 τ})` of
the stabilizer-rank literature (Bravyi–Smith–Smolin; Qassim–Pashayan–Gosset) is **cited**, not
proved, in the paper; it is correspondingly **not assumed, axiomatized, or claimed anywhere in
this file**.  Nothing here mentions `sr`.  Likewise no runtime claim is made: as elsewhere in
this development, running time is modelled only by the operation counts of `Formal.Core.Cost`.
-/

open scoped BigOperators

namespace Formal

namespace Stab

/-! ## Generic helpers for `ZMod 4` phases and `IsQPF` -/

/-- `ζ₄` turns finite sums into finite products (it is a character of `(ZMod 4, +)`). -/
theorem zeta4_sum {κ : Type} (s : Finset κ) (g : κ → ZMod 4) :
    zeta4 (∑ i ∈ s, g i) = ∏ i ∈ s, zeta4 (g i) := by
  classical
  refine Finset.induction_on s ?_ ?_
  · simp
  · intro x s hx ih
    rw [Finset.sum_insert hx, Finset.prod_insert hx, zeta4_add, ih]

/-- **Invertibility of `[[1,1],[1,i]]`.**  Every function of a single `F₂` bit is a linear
combination of the two QPFs `1` and `i^t`.  This is the elementary source of the factor `2`
per magic vertex in `lem:magic-decomp`. -/
theorem one_bit_decomp (w : ZMod 2 → ℂ) :
    ∃ p q : ℂ, ∀ t : ZMod 2, w t = p + q * zeta4 (lift4 t) := by
  have hI : (Complex.I - 1) ≠ 0 := by
    simp [sub_eq_zero, Complex.ext_iff]
  refine ⟨w 0 - (w 1 - w 0) / (Complex.I - 1), (w 1 - w 0) / (Complex.I - 1), ?_⟩
  intro t
  have ht : t = 0 ∨ t = 1 := by revert t; decide
  have hz : zeta4 (1 : ZMod 4) = Complex.I := by
    rw [zeta4, show (1 : ZMod 4).val = 1 from rfl, pow_one]
  rcases ht with rfl | rfl
  · rw [lift4_zero, zeta4_zero, mul_one]
    ring
  · rw [lift4_one, hz]
    field_simp
    ring

/-- QPFs are closed under finite products (`lem:qpf-closure`(i), iterated). -/
theorem isQPF_finsetProd {ι : Type} [Fintype ι] {κ : Type} (s : Finset κ)
    (f : κ → (ι → ZMod 2) → ℂ) (h : ∀ i ∈ s, IsQPF (f i)) :
    IsQPF (fun z => ∏ i ∈ s, f i z) := by
  classical
  induction s using Finset.induction_on with
  | empty => simp only [Finset.prod_empty]; exact isQPF_one
  | insert x s hx ih =>
      have hxq : IsQPF (f x) := h x (Finset.mem_insert_self x s)
      have hsq : IsQPF (fun z => ∏ i ∈ s, f i z) :=
        ih (fun i hi => h i (Finset.mem_insert_of_mem hi))
      exact (hxq.mul hsq).congr (fun z => by rw [Finset.prod_insert hx])

/-- A scaled single-bit phase `κ · i^{z_v}` is a QPF (linear part `= e_v`, no quadratic part,
no constraints).  These are the factors produced at a magic vertex by `one_bit_decomp`. -/
theorem isQPF_bitPhase {ι : Type} [Fintype ι] (κ : ℂ) (v : ι) :
    IsQPF (fun z : ι → ZMod 2 => κ * zeta4 (lift4 (z v))) := by
  classical
  refine (isQPF_phase κ (fun j => if j = v then (1 : ZMod 4) else 0) 0).congr ?_
  intro x
  have h1 : (∑ j, (if j = v then (1 : ZMod 4) else 0) * lift4 (x j)) = lift4 (x v) := by
    rw [Finset.sum_eq_single v]
    · rw [if_pos rfl, one_mul]
    · intro j _ hj; rw [if_neg hj, zero_mul]
    · intro h; exact absurd (Finset.mem_univ v) h
  have h2 : (∑ i, ∑ j, (0 : Matrix ι ι (ZMod 2)) i j * x i * x j) = 0 := by simp
  rw [h1, h2, sgn_zero, mul_one]

end Stab

namespace SopInstance

open Stab

variable (I : SopInstance)

/-! ## Step 1: the quarter power of `ω_r`, and the `η = r/2` sign rule

The half power `omega_pow_half` is a fact about `ω_r` alone, so it lives with `omega_pow_r`
and `omega_pow_mod` in `Formal/Core/Fourier.lean`. -/

/-- `ω_r^{r/4} = i` when `4 ∣ r`.  This is what makes "the unary phase is a power of `i`"
(`ACliff`) an honest statement about `ω_r`, and it needs `4 ∣ r` — for `{H,T,CZ}`, `r = 8`. -/
theorem omega_pow_quarter (h4 : 4 ∣ I.r) : I.omega ^ (I.r / 4) = Complex.I := by
  obtain ⟨k, hk⟩ := h4
  have hr0 := I.hr0
  have hr4 : I.r / 4 = k := by omega
  have hk0 : (k : ℂ) ≠ 0 := Nat.cast_ne_zero.mpr (by omega)
  have hrC : (I.r : ℂ) = 4 * (k : ℂ) := by rw [hk]; push_cast; ring
  have hmul : ((I.r / 4 : ℕ) : ℂ) * (2 * (Real.pi : ℂ) * Complex.I / (I.r : ℂ))
      = (Real.pi : ℂ) / 2 * Complex.I := by
    rw [hr4, hrC]; field_simp; ring
  rw [SopInstance.omega, ← Complex.exp_nat_mul, hmul, Complex.exp_pi_div_two_mul_I]

/-- **The `η = r/2` reduction, character form.**  Scaling by `η = r/2` turns an integer
multiplier into a bare sign: `χ((r/2)·m) = (−1)^m`.  This is the precise sense in which "the
quadratic terms contribute only signs". -/
theorem chi_half (m : ℕ) :
    I.chi (((I.r / 2 : ℕ) : ZMod I.r) * (m : ZMod I.r)) = sgn (m : ZMod 2) := by
  have hmod : ((m : ℕ) : ZMod 2) = ((m % 2 : ℕ) : ZMod 2) := (ZMod.natCast_mod m 2).symm
  rw [Formal.half_mul_reduce I.hr m, hmod]
  rcases Nat.mod_two_eq_zero_or_one m with h | h <;> rw [h]
  · rw [Nat.cast_zero, mul_zero, I.chi_zero, Nat.cast_zero, sgn_zero]
  · rw [Nat.cast_one, mul_one, I.chi_natCast, I.omega_pow_half, Nat.cast_one, sgn,
      show (1 : ZMod 2).val = 1 from rfl, pow_one]

/-! ## Step 2: `selCount` mod `2` is an `F₂`-quadratic form -/

/-- The selection product on a concrete edge: `edgeProd z s(v,w) = z_v z_w`. -/
theorem edgeProd_mk (z : I.V → ZMod 2) (v w : I.V) : I.edgeProd z s(v, w) = z v * z w := rfl

/-- **Strictly triangular adjacency.**  `QPF.quadVal` sums over *all* ordered pairs `(v,w)`,
so the symmetric matrix `I.adj` would cancel mod `2`.  Cutting it down along an ordering of
`I.V` — supplied by an equivalence `e : I.V ≃ Fin (card I.V)` — leaves exactly the paper's
`∑_{v<w} A_{vw} x_v x_w`. -/
def triAdj (e : I.V ≃ Fin (Fintype.card I.V)) : Matrix I.V I.V (ZMod 2) :=
  Matrix.of fun v w => if e v < e w then I.adj v w else 0

/-- Entries of the triangular adjacency matrix. -/
theorem triAdj_apply (e : I.V ≃ Fin (Fintype.card I.V)) (v w : I.V) :
    I.triAdj e v w = if e v < e w then I.adj v w else 0 := rfl

/-- The pointwise identity behind `selCount_cast`: an ordered pair `(v,w)` contributes `1` to
the triangular quadratic form exactly when it is an ordered adjacent pair of selected
vertices. -/
theorem triAdj_mul_eq_ite (e : I.V ≃ Fin (Fintype.card I.V)) (z : I.V → ZMod 2) (v w : I.V) :
    ((if (e v < e w ∧ I.G.Adj v w ∧ z v = 1 ∧ z w = 1) then (1 : ℕ) else 0 : ℕ) : ZMod 2)
      = I.triAdj e v w * z v * z w := by
  have hzz : ∀ s t : ZMod 2, (if (s = 1 ∧ t = 1) then (1 : ZMod 2) else 0) = s * t := by decide
  by_cases h1 : e v < e w
  · by_cases h2 : I.G.Adj v w
    · have ht : I.triAdj e v w = 1 := by
        rw [triAdj_apply, if_pos h1, SopInstance.adj, SimpleGraph.adjMatrix_apply, if_pos h2]
      rw [ht, one_mul, ← hzz (z v) (z w)]
      by_cases h3 : z v = 1 ∧ z w = 1
      · rw [if_pos ⟨h1, h2, h3.1, h3.2⟩, if_pos h3, Nat.cast_one]
      · rw [if_neg (fun hc => h3 ⟨hc.2.2.1, hc.2.2.2⟩), if_neg h3, Nat.cast_zero]
    · have ht : I.triAdj e v w = 0 := by
        rw [triAdj_apply, if_pos h1, SopInstance.adj, SimpleGraph.adjMatrix_apply, if_neg h2]
      rw [ht, zero_mul, zero_mul, if_neg (fun hc => h2 hc.2.1), Nat.cast_zero]
  · have ht : I.triAdj e v w = 0 := by rw [triAdj_apply, if_neg h1]
    rw [ht, zero_mul, zero_mul, if_neg (fun hc => h1 hc.1), Nat.cast_zero]

/-- The `Sym2` edge count `selCount` equals the count of *ordered* adjacent selected pairs
`(v,w)` with `e v < e w`: each selected edge has exactly one such representative. -/
theorem card_orderedSel_eq_selCount (e : I.V ≃ Fin (Fintype.card I.V)) (z : I.V → ZMod 2) :
    (Finset.univ.filter (fun p : I.V × I.V =>
        e p.1 < e p.2 ∧ I.G.Adj p.1 p.2 ∧ z p.1 = 1 ∧ z p.2 = 1)).card = I.selCount z := by
  have hzz : ∀ s t : ZMod 2, s * t = 1 → s = 1 ∧ t = 1 := by decide
  rw [SopInstance.selCount]
  refine Finset.card_bij (fun p _ => s(p.1, p.2)) ?_ ?_ ?_
  · rintro ⟨v, w⟩ hp
    rw [Finset.mem_filter] at hp
    obtain ⟨-, -, hadj, hzv, hzw⟩ := hp
    refine Finset.mem_filter.mpr ⟨by simpa using hadj, ?_⟩
    rw [I.edgeProd_mk z v w, hzv, hzw, one_mul]
  · rintro ⟨v, w⟩ hp ⟨v', w'⟩ hp' heq
    rw [Finset.mem_filter] at hp hp'
    obtain ⟨-, hlt, -, -, -⟩ := hp
    obtain ⟨-, hlt', -, -, -⟩ := hp'
    rcases Sym2.eq_iff.mp heq with ⟨rfl, rfl⟩ | ⟨rfl, rfl⟩
    · rfl
    · exact absurd hlt (lt_asymm hlt')
  · refine Sym2.ind ?_
    intro v w hx
    rw [Finset.mem_filter] at hx
    obtain ⟨hmem, hprod⟩ := hx
    have hadj : I.G.Adj v w := by simpa using hmem
    obtain ⟨hzv, hzw⟩ := hzz (z v) (z w) (by rw [← I.edgeProd_mk z v w]; exact hprod)
    have hne : e v ≠ e w := fun h => hadj.ne (e.injective h)
    rcases lt_or_gt_of_ne hne with hlt | hlt
    · exact ⟨(v, w), Finset.mem_filter.mpr ⟨Finset.mem_univ _, hlt, hadj, hzv, hzw⟩, rfl⟩
    · exact ⟨(w, v), Finset.mem_filter.mpr ⟨Finset.mem_univ _, hlt, hadj.symm, hzw, hzv⟩,
        Sym2.eq_swap⟩

/-- **`selCount` mod `2` is an `F₂`-quadratic form.**  With `η = r/2` the quadratic part of
`φ` only enters through the parity of the number of selected edges, and that parity is exactly
the strictly triangular quadratic form `∑_{v,w} (triAdj)_{vw} z_v z_w`.  This is the shape a
`Formal.Stab.QPF` accepts verbatim. -/
theorem selCount_cast (e : I.V ≃ Fin (Fintype.card I.V)) (z : I.V → ZMod 2) :
    ((I.selCount z : ℕ) : ZMod 2) = ∑ v, ∑ w, (I.triAdj e v w) * z v * z w := by
  rw [← I.card_orderedSel_eq_selCount e z, Finset.card_filter, Nat.cast_sum,
    Fintype.sum_prod_type]
  exact Finset.sum_congr rfl fun v _ =>
    Finset.sum_congr rfl fun w _ => I.triAdj_mul_eq_ite e z v w

/-! ## Step 3: Clifford and magic vertices, and the single-bit phase -/

/-- **`a`-Clifford vertex.**  The paper's "the unary phase `ω_r^{a b_v}` *is* a power of `i`":
by `omega_pow_quarter` (which needs `4 ∣ r`) that says `(r/4) ∣ (a·b_v)`.  For `r = 8` it reads
"`a b_v` is even". -/
def ACliff (a : ZMod I.r) (v : I.V) : Prop := (I.r / 4) ∣ (a * I.b v).val

/-- **`a`-magic vertex.**  The paper's "the unary phase `ω_r^{a b_v}` is *not* a power of
`i`"; for `r = 8`, "`a b_v` is odd". -/
def AMagic (a : ZMod I.r) (v : I.V) : Prop := ¬ I.ACliff a v

/-- `a`-Cliffordness is decidable: it is a divisibility of naturals. -/
instance decidableACliff (a : ZMod I.r) (v : I.V) : Decidable (I.ACliff a v) := by
  unfold ACliff; infer_instance

/-- `a`-magicness is decidable (it is the negation of `ACliff`); this is what makes
`magicSet` a `Finset`. -/
instance decidableAMagic (a : ZMod I.r) (v : I.V) : Decidable (I.AMagic a v) := by
  unfold AMagic; infer_instance

/-- Divisibility of the *integer* product suffices for `a`-Cliffordness: reduction mod `r`
cannot destroy it, because `(r/4) ∣ r`. -/
theorem ACliff_of_dvd_mul (h4 : 4 ∣ I.r) {a : ZMod I.r} {v : I.V}
    (h : (I.r / 4) ∣ a.val * (I.b v).val) : I.ACliff a v := by
  have hrdvd : (I.r / 4) ∣ I.r := ⟨4, by omega⟩
  unfold ACliff
  rw [ZMod.val_mul]
  exact (Nat.dvd_mod_iff hrdvd).mpr h

/-- If the *unary coefficient itself* is a multiple of `r/4` (its phase `ω_r^{b_v}` is a power
of `i`) then `v` is `a`-Clifford for **every** mode `a`. -/
theorem ACliff_of_dvd_b (h4 : 4 ∣ I.r) {v : I.V} (hb : (I.r / 4) ∣ (I.b v).val)
    (a : ZMod I.r) : I.ACliff a v :=
  I.ACliff_of_dvd_mul h4 (Dvd.dvd.mul_left hb _)

/-- If the *mode* `a` is a multiple of `r/4` then every vertex is `a`-Clifford. -/
theorem ACliff_of_dvd_mode (h4 : 4 ∣ I.r) {a : ZMod I.r} (ha : (I.r / 4) ∣ a.val)
    (v : I.V) : I.ACliff a v :=
  I.ACliff_of_dvd_mul h4 (Dvd.dvd.mul_right ha _)

/-- **The single-bit Clifford phase.**  At an `a`-Clifford vertex the unary factor
`χ(a b_v · z_v)` is literally a `ZMod 4` phase `i^{c z_v}`, i.e. a one-variable QPF.  This is
`ω_r^{r/4} = i` (`omega_pow_quarter`) applied to the witness of `(r/4) ∣ (a·b_v)`. -/
theorem exists_zeta4_of_cliff (h4 : 4 ∣ I.r) {a : ZMod I.r} {v : I.V} (hv : I.ACliff a v) :
    ∃ c : ZMod 4, ∀ t : ZMod 2,
      I.chi ((a * I.b v) * (((t.val : ℕ)) : ZMod I.r)) = zeta4 (c * lift4 t) := by
  unfold ACliff at hv
  obtain ⟨m, hm⟩ := hv
  refine ⟨(m : ZMod 4), ?_⟩
  intro t
  have ht : t = 0 ∨ t = 1 := by revert t; decide
  rcases ht with rfl | rfl
  · rw [show ((0 : ZMod 2).val : ℕ) = 0 from rfl, Nat.cast_zero, mul_zero, I.chi_zero,
      lift4_zero, mul_zero, zeta4_zero]
  · rw [show ((1 : ZMod 2).val : ℕ) = 1 from rfl, Nat.cast_one, mul_one, lift4_one, mul_one,
      SopInstance.chi, hm, pow_mul, I.omega_pow_quarter h4, zeta4, ZMod.val_natCast, I_pow_mod]

/-! ## Step 4: the Ideal case — Gottesman–Knill in the SOP setting -/

/-- `χ` turns finite sums into finite products (it is a character of `(ZMod r, +)`). -/
theorem chi_sum {κ : Type} (s : Finset κ) (g : κ → ZMod I.r) :
    I.chi (∑ i ∈ s, g i) = ∏ i ∈ s, I.chi (g i) := by
  classical
  refine Finset.induction_on s ?_ ?_
  · rw [Finset.sum_empty, Finset.prod_empty, I.chi_zero]
  · intro x s hx ih
    rw [Finset.sum_insert hx, Finset.prod_insert hx, I.chi_add, ih]

/-- **The mode-`a` weight factors.**  Since `η = r/2`, the quadratic part of `φ` contributes
only the sign of an `F₂`-quadratic form (`chi_half` + `selCount_cast`), and what is left is one
unary factor per vertex. -/
theorem chi_mode_phi (e : I.V ≃ Fin (Fintype.card I.V)) (a : ZMod I.r) (z : I.V → ZMod 2) :
    I.chi (a * I.phi z)
      = (∏ v, I.chi ((a * I.b v) * (((z v).val : ℕ) : ZMod I.r)))
        * sgn (∑ v, ∑ w, ((a.val : ZMod 2) * I.triAdj e v w) * z v * z w) := by
  have hcast : ((a.val * I.selCount z : ℕ) : ZMod I.r)
      = a * ((I.selCount z : ℕ) : ZMod I.r) := by
    rw [Nat.cast_mul, ZMod.natCast_zmod_val]
  have hsplit : a * I.phi z
      = (∑ v, (a * I.b v) * (((z v).val : ℕ) : ZMod I.r))
        + ((I.r / 2 : ℕ) : ZMod I.r) * ((a.val * I.selCount z : ℕ) : ZMod I.r) := by
    rw [SopInstance.phi, mul_add, Finset.mul_sum, hcast]
    congr 1
    · exact Finset.sum_congr rfl fun v _ => by ring
    · ring
  have hquad : ((a.val * I.selCount z : ℕ) : ZMod 2)
      = ∑ v, ∑ w, ((a.val : ZMod 2) * I.triAdj e v w) * z v * z w := by
    rw [Nat.cast_mul, I.selCount_cast e z, Finset.mul_sum]
    refine Finset.sum_congr rfl fun v _ => ?_
    rw [Finset.mul_sum]
    exact Finset.sum_congr rfl fun w _ => by ring
  rw [hsplit, I.chi_add, I.chi_sum, I.chi_half, hquad]

/-- **`lem:magic-decomp`, Clifford case — Gottesman–Knill in the SOP setting.**  If every
vertex is `a`-Clifford then the whole mode-`a` weight `z ↦ χ(a·φ(z))` is a *single* QPF: the
unary phases are `ZMod 4`-linear and the `η = r/2` quadratic part is an `F₂`-quadratic form.
`Rebuild` therefore returns one QPF at every vertex of the decomposition, for *any* graph, at
any rank-width. -/
theorem isQPF_modeWeight_of_cliff (h4 : 4 ∣ I.r) (a : ZMod I.r) (hc : ∀ v, I.ACliff a v) :
    IsQPF (fun z : I.V → ZMod 2 => I.chi (a * I.phi z)) := by
  classical
  let e : I.V ≃ Fin (Fintype.card I.V) := Fintype.equivFin I.V
  choose c hcv using fun v => I.exists_zeta4_of_cliff h4 (hc v)
  refine (isQPF_phase (1 : ℂ) c (fun v w => (a.val : ZMod 2) * I.triAdj e v w)).congr ?_
  intro z
  rw [one_mul, I.chi_mode_phi e a z]
  congr 1
  rw [zeta4_sum]
  exact Finset.prod_congr rfl fun v _ => (hcv v (z v)).symm

/-- **The paper's "Ideal case": a Clifford instance.**  At `r = 8` (the `{H,T,CZ}` modulus),
"every `b_v` is even" makes every vertex `a`-Clifford for every mode `a`, so the whole dynamic
program collapses to a single QPF — one `O(n³)` Gauss sum — on every graph. -/
theorem isQPF_modeWeight_of_even_b (hr8 : I.r = 8) (hb : ∀ v, Even ((I.b v).val))
    (a : ZMod I.r) : IsQPF (fun z : I.V → ZMod 2 => I.chi (a * I.phi z)) := by
  have h4 : 4 ∣ I.r := by rw [hr8]; norm_num
  have hq : I.r / 4 = 2 := by rw [hr8]
  exact I.isQPF_modeWeight_of_cliff h4 a
    (fun v => I.ACliff_of_dvd_b h4 (by rw [hq]; exact (hb v).two_dvd) a)

/-- **The paper's "every even Fourier mode".**  At `r = 8`, an even mode `a` makes every vertex
`a`-Clifford, so `χ(a·φ(·))` is a single QPF — every even Fourier mode is a single Gauss sum,
on every graph, regardless of width.  (Stated at `r = 8`; the general form is
`ACliff_of_dvd_mode`, which asks `(r/4) ∣ a.val`.) -/
theorem isQPF_modeWeight_of_even_mode (hr8 : I.r = 8) (a : ZMod I.r) (ha : Even (a.val)) :
    IsQPF (fun z : I.V → ZMod 2 => I.chi (a * I.phi z)) := by
  have h4 : 4 ∣ I.r := by rw [hr8]; norm_num
  have hq : I.r / 4 = 2 := by rw [hr8]
  exact I.isQPF_modeWeight_of_cliff h4 a
    (I.ACliff_of_dvd_mode h4 (by rw [hq]; exact ha.two_dvd))

/-! ## Step 5: the general `2^τ` magic decomposition -/

/-- The set of `a`-magic vertices; the paper's `τ_u` is its cardinality (here taken over the
whole vertex set rather than a subtree). -/
def magicSet (a : ZMod I.r) : Finset I.V := Finset.univ.filter (fun v => I.AMagic a v)

/-- The index set of `lem:magic-decomp` has exactly `2^τ` elements, `τ = #magic vertices`.
This — and *only* this — is the term count the decomposition below is claimed to achieve. -/
theorem card_powerset_magicSet (a : ZMod I.r) :
    (I.magicSet a).powerset.card = 2 ^ (I.magicSet a).card :=
  Finset.card_powerset _

/-- **`lem:magic-decomp`.**  The mode-`a` weight `z ↦ χ(a·φ(z))` is a sum of QPFs indexed by
the subsets of the set of `a`-magic vertices, hence a sum of at most `2^τ` QPFs, where
`τ = (I.magicSet a).card` (see `card_powerset_magicSet`).  Each magic factor is a function of
one bit, and every function of one bit is a linear combination of the two QPFs `1` and `i^z`
because `[[1,1],[1,i]]` is invertible (`Formal.Stab.one_bit_decomp`); expanding the product
over the magic vertices with `Finset.prod_add` gives the `2^τ` terms.

The sharper stabilizer-rank bound `sr(τ) = O(2^{0.3963 τ})` used in the paper is **cited
literature** (Bravyi–Smith–Smolin; Qassim–Pashayan–Gosset) and is deliberately **not**
formalized, assumed, or claimed here — only the elementary `2^τ` count is proved. -/
theorem modeWeight_sum_qpf (h4 : 4 ∣ I.r) (a : ZMod I.r) :
    ∃ F : Finset I.V → ((I.V → ZMod 2) → ℂ),
      (∀ T, IsQPF (F T)) ∧
      ∀ z, I.chi (a * I.phi z) = ∑ T ∈ (I.magicSet a).powerset, F T z := by
  classical
  let e : I.V ≃ Fin (Fintype.card I.V) := Fintype.equivFin I.V
  -- Clifford vertices: a genuine `ZMod 4` phase; magic vertices: coefficient set to `0`.
  have hex : ∀ v : I.V, ∃ cv : ZMod 4,
      (v ∈ I.magicSet a → cv = 0) ∧
      (v ∉ I.magicSet a → ∀ t : ZMod 2,
        I.chi ((a * I.b v) * (((t.val : ℕ)) : ZMod I.r)) = zeta4 (cv * lift4 t)) := by
    intro v
    by_cases hv : v ∈ I.magicSet a
    · exact ⟨0, fun _ => rfl, fun h => absurd hv h⟩
    · have hcl : I.ACliff a v :=
        Classical.byContradiction fun h => hv (Finset.mem_filter.mpr ⟨Finset.mem_univ v, h⟩)
      obtain ⟨cv, hcv⟩ := I.exists_zeta4_of_cliff h4 hcl
      exact ⟨cv, fun h => absurd h hv, fun _ => hcv⟩
  choose c hc0 hc using hex
  -- Every vertex (magic or not) has a single-bit decomposition in the basis `1, i^z`.
  have hex2 : ∀ v : I.V, ∃ pq : ℂ × ℂ, ∀ t : ZMod 2,
      I.chi ((a * I.b v) * (((t.val : ℕ)) : ZMod I.r)) = pq.1 + pq.2 * zeta4 (lift4 t) := by
    intro v
    obtain ⟨p, q, hpq⟩ :=
      one_bit_decomp (fun t => I.chi ((a * I.b v) * (((t.val : ℕ)) : ZMod I.r)))
    exact ⟨(p, q), hpq⟩
  choose pq hpq using hex2
  refine ⟨fun T z =>
    ((∏ v ∈ T, (pq v).1) * ∏ v ∈ (I.magicSet a) \ T, ((pq v).2 * zeta4 (lift4 (z v))))
      * ((1 : ℂ) * zeta4 (∑ j, c j * lift4 (z j))
          * sgn (∑ i, ∑ j, ((a.val : ZMod 2) * I.triAdj e i j) * z i * z j)), ?_, ?_⟩
  · intro T
    exact (IsQPF.smul _ (isQPF_finsetProd _ _ fun v _ => isQPF_bitPhase _ v)).mul
      (isQPF_phase 1 c _)
  · intro z
    have hcsum : (∑ v ∈ (I.magicSet a)ᶜ, c v * lift4 (z v)) = ∑ j, c j * lift4 (z j) :=
      Finset.sum_subset (Finset.subset_univ _) fun x _ hx => by
        rw [hc0 x (by simpa using hx), zero_mul]
    have hprod : (∏ v : I.V, I.chi ((a * I.b v) * (((z v).val : ℕ) : ZMod I.r)))
        = (∑ T ∈ (I.magicSet a).powerset,
            (∏ v ∈ T, (pq v).1) * ∏ v ∈ (I.magicSet a) \ T, ((pq v).2 * zeta4 (lift4 (z v))))
          * zeta4 (∑ j, c j * lift4 (z j)) := by
      rw [← Finset.prod_mul_prod_compl (I.magicSet a)
        (fun v => I.chi ((a * I.b v) * (((z v).val : ℕ) : ZMod I.r)))]
      congr 1
      · rw [Finset.prod_congr rfl fun v _ => hpq v (z v)]
        exact Finset.prod_add _ _ _
      · rw [Finset.prod_congr rfl fun v hv => hc v (Finset.mem_compl.mp hv) (z v),
          ← zeta4_sum, hcsum]
    rw [I.chi_mode_phi e a z, hprod, mul_assoc, Finset.sum_mul]
    exact Finset.sum_congr rfl fun T _ => by ring

/-! ## Even Fourier modes factorize, at every even modulus

The QPF route above shows that at `r = 8` every even mode is a single quadratic phase function.
That is not the sharpest statement available, and it is not the real reason the paper's even
modes are easy.  Because `η = r/2`, an even mode annihilates *every* cross term — `a·(r/2) = 0`
in `ZMod r` — so the mode sum factorizes completely into one factor per vertex, with no
reference to the graph at all.  This needs no `4 ∣ r`, no QPFs and no decomposition. -/

/-- An even mode kills the quadratic coefficient: `a·(r/2) = 0` in `ZMod r`. -/
theorem mul_half_eq_zero_of_even {a : ZMod I.r} (ha : Even a.val) :
    a * ((I.r / 2 : ℕ) : ZMod I.r) = 0 := by
  obtain ⟨m, hm⟩ := ha
  obtain ⟨s, hs⟩ := I.hr
  have hhalf : I.r / 2 = s := by omega
  have : a.val * (I.r / 2) = m * I.r := by rw [hhalf, hm, hs]; ring
  calc a * ((I.r / 2 : ℕ) : ZMod I.r)
      = ((a.val : ℕ) : ZMod I.r) * ((I.r / 2 : ℕ) : ZMod I.r) := by
        rw [ZMod.natCast_zmod_val]
    _ = ((a.val * (I.r / 2) : ℕ) : ZMod I.r) := by push_cast; ring
    _ = ((m * I.r : ℕ) : ZMod I.r) := by rw [this]
    _ = 0 := by push_cast [ZMod.natCast_self]; ring

/-- For an even mode the phase polynomial loses its quadratic part entirely. -/
theorem mul_f_of_even {a : ZMod I.r} (ha : Even a.val) (x : I.V → ZMod 2) :
    a * I.f x = a * I.c + ∑ v, (a * I.b v) * (((x v).val : ℕ) : ZMod I.r) := by
  have hz : a * (((I.r / 2 : ℕ) : ZMod I.r) * ((I.selCount x : ℕ) : ZMod I.r)) = 0 := by
    rw [← mul_assoc, I.mul_half_eq_zero_of_even ha, zero_mul]
  rw [SopInstance.f, mul_add, mul_add, hz, add_zero, Finset.mul_sum]
  congr 1
  exact Finset.sum_congr rfl fun v _ => by ring

/-- **Every even Fourier mode factorizes, on every graph and at every even modulus.**
Since `η = r/2`, an even mode `a` has `a·η = 0`, so no cross term survives and

  `N̂(a) = χ(a·c) · ∏_v (1 + χ(a·b_v))`,

a product of `|V|` terms.  Every even mode of the residue histogram is therefore computable in
`O(n)` character operations on **any** graph, regardless of rank-width — and, unlike
`isQPF_modeWeight_of_even_mode`, this needs neither `4 ∣ r` nor `r = 8`. -/
theorem Nhat_even {a : ZMod I.r} (ha : Even a.val) :
    I.Nhat a = I.chi (a * I.c) * ∏ v, (1 + I.chi (a * I.b v)) := by
  classical
  have hterm : ∀ x : I.V → ZMod 2, I.chi (a * I.f x)
      = I.chi (a * I.c) * ∏ v, I.chi ((a * I.b v) * (((x v).val : ℕ) : ZMod I.r)) := by
    intro x
    rw [I.mul_f_of_even ha, I.chi_add, I.chi_sum]
  rw [SopInstance.Nhat, Finset.sum_congr rfl fun x _ => hterm x, ← Finset.mul_sum]
  congr 1
  -- `∑_x ∏_v g v (x v) = ∏_v ∑_t g v t`, then evaluate the two-element inner sum.
  -- `∑_x ∏_v g v (x v) = ∏_v ∑_t g v t`, instantiated explicitly to avoid higher-order
  -- unification in `rw`.
  have hswap := Finset.prod_univ_sum (fun _ : I.V => (Finset.univ : Finset (ZMod 2)))
    (fun (v : I.V) (t : ZMod 2) => I.chi ((a * I.b v) * ((t.val : ℕ) : ZMod I.r)))
  rw [Fintype.piFinset_univ] at hswap
  rw [← hswap]
  refine Finset.prod_congr rfl fun v _ => ?_
  rw [show (Finset.univ : Finset (ZMod 2)) = {0, 1} by decide,
    Finset.sum_insert (by decide), Finset.sum_singleton]
  have h0 : ((0 : ZMod 2).val : ℕ) = 0 := rfl
  have h1 : ((1 : ZMod 2).val : ℕ) = 1 := rfl
  rw [h0, h1, Nat.cast_zero, Nat.cast_one, mul_zero, mul_one, I.chi_zero]

end SopInstance
end Formal
