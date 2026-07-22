/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Quantum.Circuit

/-!
# Symbolic path-sum compilation of a circuit (sec:sop)

Processing the circuit gate-by-gate, we maintain a symbolic description of the Feynman path
sum accumulated so far (for a fixed input `y`):

* each wire currently carries either the pinned constant `y a` (no Hadamard seen: `cur a =
  none`) or the path variable introduced by its most recent Hadamard (`cur a = some i`);
* `c` collects constant phase, `b i` unary phases, `epar i j` the parity of the multiplicity
  of the sign edge `{i,j}` (two parallel `4 x_i x_j` terms cancel mod 8);
* `mH` counts Hadamards (normalization `R = (√2)^mH`).

Variables are named by naturals `0, 1, 2, …` (`next` = first unused name); this avoids
dependent reindexing when a Hadamard introduces a fresh variable. The compile invariant
(`Formal.Quantum.Coupling`) states that the circuit-matrix entry equals the symbolic sum.
-/

open scoped BigOperators

namespace Formal
namespace Quantum

/-- Symbolic path-sum state for an `n`-wire circuit at a fixed input. -/
structure Sym (n : ℕ) where
  /-- first unused variable name; variables in play are `0, …, next-1` -/
  next : ℕ
  /-- current symbol on each wire: `none` = the pinned input constant, `some i` = variable `i` -/
  cur : Fin n → Option ℕ
  /-- accumulated constant phase -/
  c : ZMod 8
  /-- accumulated unary phases -/
  b : ℕ → ZMod 8
  /-- parity of the multiplicity of the quadratic sign edge `{i,j}` (symmetric use) -/
  epar : ℕ → ℕ → ZMod 2
  /-- number of Hadamards processed -/
  mH : ℕ

namespace Sym

variable {n : ℕ}

/-- The initial state: all wires constant, empty phase data. -/
def init (n : ℕ) : Sym n :=
  { next := 0, cur := fun _ => none, c := 0, b := fun _ => 0, epar := fun _ _ => 0, mH := 0 }

/-- Flip the edge parity of the (unordered) pair `{i, j}`; we store symmetrically. -/
def flipEdge (S : Sym n) (i j : ℕ) : Sym n :=
  { S with epar := fun p q =>
      if (p = i ∧ q = j) ∨ (p = j ∧ q = i) then S.epar p q + 1 else S.epar p q }

/-- Add `t` to the unary coefficient of variable `i`. -/
def addB (S : Sym n) (i : ℕ) (t : ZMod 8) : Sym n :=
  { S with b := fun p => if p = i then S.b p + t else S.b p }

/-- Add `t` to the constant phase. -/
def addC (S : Sym n) (t : ZMod 8) : Sym n :=
  { S with c := S.c + t }

/-- `ZMod 2 → ZMod 8` on bit values (0 ↦ 0, 1 ↦ 1). -/
def lift28 (t : ZMod 2) : ZMod 8 := (t.val : ZMod 8)

/-- Process one gate at input `y` (the paper's local contributions, eqn:deltas). -/
def step (y : Fin n → ZMod 2) (S : Sym n) : Gate n → Sym n
  | Gate.T a =>
      match S.cur a with
      | none => S.addC (lift28 (y a))
      | some i => S.addB i 1
  | Gate.CZ a b =>
      match S.cur a, S.cur b with
      | none, none => S.addC (4 * lift28 (y a) * lift28 (y b))
      | none, some j => S.addB j (4 * lift28 (y a))
      | some i, none => S.addB i (4 * lift28 (y b))
      | some i, some j => if i = j then S.addB i 4 else S.flipEdge i j
  | Gate.H a =>
      let ν := S.next
      let S' :=
        match S.cur a with
        | none => S.addB ν (4 * lift28 (y a))  -- edge to a pinned constant is a unary phase
        | some i => S.flipEdge i ν
      { S' with
        next := S.next + 1
        cur := fun w => if w = a then some ν else S.cur w
        mH := S.mH + 1 }

/-- Compile a whole circuit at input `y`. -/
def compile (y : Fin n → ZMod 2) (C : Circuit n) : Sym n :=
  C.foldl (step y) (init n)

/-! ## Semantics of a symbolic state -/

/-- Zero-extend an assignment of the live variables `Fin S.next` to all names. -/
def ext (S : Sym n) (x : Fin S.next → ZMod 2) : ℕ → ZMod 2 :=
  fun m => if h : m < S.next then x ⟨m, h⟩ else 0

/-- The value currently carried by wire `a`, under input `y` and assignment `x`. -/
def wireVal (S : Sym n) (y : Fin n → ZMod 2) (x : Fin S.next → ZMod 2) (a : Fin n) : ZMod 2 :=
  match S.cur a with
  | none => y a
  | some i => S.ext x i

/-- The accumulated quadratic phase polynomial (mod 8): `c + ∑ b_i x_i + 4 ∑_{i<j} epar x_i x_j`. -/
def phase (S : Sym n) (x : Fin S.next → ZMod 2) : ZMod 8 :=
  S.c + (∑ i ∈ Finset.range S.next, S.b i * lift28 (S.ext x i))
      + 4 * (∑ i ∈ Finset.range S.next, ∑ j ∈ Finset.range S.next,
          if i < j then lift28 (S.epar i j) * lift28 (S.ext x i) * lift28 (S.ext x j) else 0)

/-- The symbolic path sum pinned at output `w`: sum over assignments compatible with `w`. -/
noncomputable def pathSum (S : Sym n) (y w : Fin n → ZMod 2) : ℂ :=
  ∑ x : Fin S.next → ZMod 2,
    (if ∀ a, S.wireVal y x a = w a then (1 : ℂ) else 0) * omega8 ^ (S.phase x).val

end Sym

/-- **The compile invariant** (statement; proof in `Formal.Quantum.Coupling`): the
circuit-matrix entry is the normalized symbolic path sum. -/
def CompileInvariant (n : ℕ) : Prop :=
  ∀ (C : Circuit n) (y w : Fin n → ZMod 2),
    circuitMatrix C w y
      = (((Real.sqrt 2 : ℝ) : ℂ) ^ (Sym.compile y C).mH)⁻¹ * (Sym.compile y C).pathSum y w

end Quantum
end Formal
