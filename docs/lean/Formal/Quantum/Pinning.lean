/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Quantum.SymLemmas
import Formal.Core.SopInstance

/-!
# Pinning the boundary: from a compiled symbolic state to a `SopInstance`

The compile invariant expresses `⟨z|C|y⟩` through `pathSum (compile y C) y z`, whose output
indicator still pins the *live* variables (each wire's most recent Hadamard variable) to `z`.
This file performs the paper's "pinning the boundary" substitution (sec:sop): the live
variables are substituted by their forced values, producing a quadratic SOP over the *free*
variables — an honest `SopInstance` whose graph is the odd-parity edge set among free
variables. Wires never hit by a Hadamard contribute the Kronecker delta `⟦y a = z a⟧`.

Beyond `WFS`, the substitution needs three more compiler invariants, all preserved by `step`:
`CurInj` (distinct wires carry distinct live variables), `EparSym` (edge parities are stored
symmetrically), `EparIrrefl` (no self-edges). Their preservation proofs live in
`Formal.Quantum.PinningLemmas` (agent-proved); this file holds definitions and statements.
-/

open scoped BigOperators

namespace Formal
namespace Quantum
namespace Sym

variable {n : ℕ}

/-- Distinct wires carry distinct live variables. -/
def CurInj (S : Sym n) : Prop :=
  ∀ a b i, S.cur a = some i → S.cur b = some i → a = b

/-- Edge parities are stored symmetrically. -/
def EparSym (S : Sym n) : Prop := ∀ i j, S.epar i j = S.epar j i

/-- No self-edges are ever recorded. -/
def EparIrrefl (S : Sym n) : Prop := ∀ i, S.epar i i = 0

/-- The set of live variable names: the current symbol of some wire. -/
def liveSet (S : Sym n) : Finset ℕ :=
  (Finset.univ : Finset (Fin n)).biUnion (fun a => (S.cur a).toFinset)

/-- The set of free (non-live) variable names below `next`. -/
def freeSet (S : Sym n) : Finset ℕ := (Finset.range S.next) \ S.liveSet

open Classical in
/-- The value forced on a live variable by the output pins `z` (0 on non-live names). -/
noncomputable def pinVal (S : Sym n) (z : Fin n → ZMod 2) (i : ℕ) : ZMod 2 :=
  if h : ∃ a, S.cur a = some i then z h.choose else 0

/-- The Kronecker delta for Hadamard-free wires: `1` iff every constant wire agrees. -/
noncomputable def constDelta (S : Sym n) (y z : Fin n → ZMod 2) : ℂ :=
  if ∀ a, S.cur a = none → y a = z a then 1 else 0

/-- **The pinned quadratic SOP** (the paper's `f_C` after boundary substitution):
variables are the free names; the graph keeps odd-parity edges between free variables;
edges into live variables are folded into the unary coefficients scaled by the pinned
values; fully live terms fold into the constant. -/
noncomputable def pinnedInstance (S : Sym n) (hsym : S.EparSym) (z : Fin n → ZMod 2) :
    SopInstance where
  r := 8
  hr := ⟨4, by norm_num⟩
  hr0 := by norm_num
  V := {i : ℕ // i ∈ S.freeSet}
  fV := by infer_instance
  dV := by infer_instance
  G :=
    { Adj := fun u v => u ≠ v ∧ S.epar u.1 v.1 = 1
      symm := ⟨fun u v h => ⟨h.1.symm, by rw [hsym v.1 u.1]; exact h.2⟩⟩
      loopless := ⟨fun u h => h.1 rfl⟩ }
  dG := by
    intro u v
    exact instDecidableAnd
  eG := by infer_instance
  b := fun u =>
    S.b u.1 + 4 * ∑ j ∈ S.liveSet, lift28 (S.epar u.1 j) * lift28 (S.pinVal z j)
  c :=
    S.c + (∑ i ∈ S.liveSet, S.b i * lift28 (S.pinVal z i))
        + 4 * (∑ i ∈ S.liveSet, ∑ j ∈ S.liveSet,
            if i < j then lift28 (S.epar i j) * lift28 (S.pinVal z i) * lift28 (S.pinVal z j)
            else 0)

/-- **Statement of the pinning theorem** (proved in `Formal.Quantum.PinningLemmas`):
for a well-formed symbolic state, the output-pinned path sum is the constant-wire delta
times the unnormalized SOP value of the pinned instance. -/
def PinningHolds (n : ℕ) : Prop :=
  ∀ (S : Sym n) (_ : S.WFS) (_ : S.CurInj) (hsym : S.EparSym) (_ : S.EparIrrefl)
    (y z : Fin n → ZMod 2),
    S.pathSum y z = S.constDelta y z * (S.pinnedInstance hsym z).S

end Sym
end Quantum
end Formal
