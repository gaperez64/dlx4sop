# Audit: signature/table representation in the rank-width DP

*2026-08-11. Read-only audit of `src/solve/rankwidth_*` prompted by the question: do the
DP tables store extrinsic boundary signatures (n-bit) where ρ_u-bit row-space coordinates
would do? Four independent file audits + manual verification of the join kernel.*

## Verdict

**The representation is extrinsic everywhere in the main DP.** Signatures are interned
full `⌈n/64⌉`-word bitsets; every distinct signature additionally pins a full n-bit
representative assignment; every join pair pays O(n/64)–O(n²/64) bitset work. The
intrinsic ρ-bit coordinate machinery **already exists in the codebase**
(`rw_dense_basis_t`) but is quarantined to the twist/WHT and dense-reference kernels
(dim ≤ 22) and is rebuilt and discarded per join.

## Evidence (file:line)

1. **Pool width is n, not ρ.** `rw_signature_pool_t.bits` rows are
   `pool->words = decomposition->words = qsop_bitset_words(nvars)` (rankwidth_decomp.c:315;
   pool inits at rankwidth_dp_counts.c:1714, 2009, 2319; rankwidth_dp_complex.c:1716, 2001).
   Interning fingerprints, compares, and copies all run at full width
   (rankwidth_tables.c:139, 148–149, 170).
2. **Two full-width vectors per distinct signature.** Pool bits (n/8 + 8 B fingerprint)
   plus a per-node representative assignment (n/8 B + 4 B popcount weight,
   rankwidth_tables.c:245, 414–415). Fourier/complex tables replicate the pattern
   (rankwidth_tables.c:605, 668, 741).
3. **Per-join-pair inner loop is full-width** (`rw_compute_join_transition_sign`,
   rankwidth_dp_counts.c:728–755, called per pair from all count/Fourier/complex paths):
   - cross parity: per set bit of the lighter representative, a full-width popcount
     intersection with an adjacency row → O(min(w_L,w_R)·n/64) per pair
     (dp_counts.c:435–467);
   - parent signature: full-width copy+XOR+AND then **pool intern (n-bit hash+compare)
     per pair** (dp_counts.c:739–743);
   - parent representative: full-width OR + hash per pair (dp_counts.c:795–799, 1038–1041).
   The CSR-materialized path runs this sweep **twice** (count pass :786, fill pass :867).
4. **The near-leaf waste is exact.** A leaf table stores *two full n-bit signatures*
   (the raw adjacency row of v, self-bit cleared) and *two full n-bit assignments*
   (dp_counts.c:1194–1214) — for a state space of dimension ρ ≤ 1.
5. **Pool is per-solve and grow-only.** Every distinct signature ever interned across all
   nodes is retained until the pass ends (frees at dp_counts.c:1809, 2065, 2517;
   dp_complex.c:1947, 2237). Ids cap at UINT32_MAX.
6. **CRT path (the large-n regime, nvars ≥ 64, dispatch dp_counts.c:2299–2301) has no
   pair cap**: it materializes join maps for *all* join nodes simultaneously, each entry
   16 B **plus a full-width assignment per (left,right) pair** (dp_counts.c:1126–1128,
   2022–2027, freed only at 2060–2062). `RW_MATERIALIZE_JOIN_MAX_PAIRS` gates only the
   non-CRT CSR path.
7. **Forecasts count entries/pairs, not bytes.** `table_forecast`/`join_pair_forecast`
   (rankwidth_cutrank.c:213–320) carry no term for the n bits per signature, so memory
   forecasting systematically understates the extrinsic footprint.
8. **The coordinate machinery exists but is unused for the main tables.**
   `rw_dense_basis_add/reduce/coord` (rankwidth_tables.c:813–890) computes exactly the
   ρ-bit coordinate of a signature; the twist plan's `parent_basis`/`cross_basis`
   (rankwidth_join_twist.c:32–123) are exactly the P-map basis and crossing form — built
   per join, capped at p+c ≤ 22, and freed (`rw_twist_plan_free`) after each join. Even
   the twist kernel converts back to full-width interned signatures at scatter-back
   (dp_complex.c:1107–1121). Its basis reduce is itself O(dim·n/64) full-width XORs, not
   packed k×k arithmetic.

## Recommended refactor (intrinsic coordinates)

Per node `u`, fix a basis of the *realized* signature span (dimension ρ̂_u ≤ min(ρ_u, k));
a signature is then a ≤ 32-bit coordinate word. Per join, precompute once:

- `P : ρ̂_L → ρ̂_t`, `Q : ρ̂_R → ρ̂_t` — restriction maps as k×k bit-matrices (each column:
  one full-width mask+reduce of a child basis vector; O(k) full-width ops per node);
- `B ∈ F₂^{ρ̂_L×ρ̂_R}` — the crossing form, evaluated on basis pairs by the existing
  cross-parity routine (O(k²) full-width parities **per node**, replacing O(4^k) of them
  **per node**).

Then each join pair costs a handful of word ops: `γ̂ = Pα̂ ⊕ Qβ̂` (k XORs of 32-bit words,
or table-driven), `χ = parity(α̂ᵀ(Bβ̂))` (k ANDs + popcount). Representatives shrink from
one n-bit vector per signature to ρ̂_u basis witnesses per node. The pool, fingerprints,
per-pair interning, and per-pair assignment ORs disappear; parent keys are computed, not
interned. The CRT join-map entry drops from 16+n/8 bytes to ≤ 16 bytes.

**Expected impact** (per join with 2^k states/side): inner-loop word-ops fall from
Θ(4^k · n/64) to Θ(4^k · k/32) — at the corpus maximum N = 163,359 (words ≈ 2553) that is
three orders of magnitude — and per-signature memory falls from ~n/4 B (two full-width
vectors) to ~k bits + one uint32. This directly addresses the observed "tables can be
large even at low width" and removes the O(n) factor that currently multiplies every
pair even on bounded-rank-width families.

Cheap first steps, in order: (1) coordinate-key the *leaf and low-ρ̂* tables (the waste is
maximal there and the basis is trivial); (2) hoist the crossing form `B` out of the pair
loop (pure speed, no representation change: compute B on ρ̂_L×ρ̂_R basis pairs, replace
`cross_parity_bitsets_weighted` by the bilinear evaluation); (3) full coordinate keying +
pool retirement; (4) cap or stream the CRT join maps; (5) add a bytes term to
`table_forecast`.

## Cross-check against the paper

The paper's bounds are unaffected (its `poly(n)` absorbs the extrinsic representation —
`thm:sop-rw`'s proof says "explicit bit-vector representations"), but the refactor
realizes the `O(k²)`-per-pair implementation the theory permits. The Lean formalization
keys tables by abstract functions and is likewise unaffected.
