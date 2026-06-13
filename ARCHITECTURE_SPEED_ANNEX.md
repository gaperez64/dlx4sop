# Architecture Speed Annex

This annex keeps performance ideas that are still useful but not yet part of the
stable command-line contract. Historical benchmark snapshots and completed
implementation notes belong in commit history, not here.

## Priority Order

The current performance roadmap is:

1. Rerun importer-fed local and external corpora with the widened rankwidth
   backend, especially cases with large cut signatures.
2. Improve decomposition quality before adding more branch-local heuristics.
3. Compare branch heuristics only when traces show the cache and split machinery
   is being exercised on a representative corpus.
4. Add incremental residual hashing if full-state hashing appears in hot traces.
5. Consider dancing-cells-style linked mutation only if active-edge traversal or
   undo costs dominate.
6. Add specialized residue kernels and CPU dispatch after solver shape is
   stable.
7. Implement labelled rankwidth once the sign-only decomposition pipeline and
   benchmark story are strong enough.

## Reversible Mutation And Dancing Cells

The branch solver already uses reversible residual mutation. The state has
checkpoints, undo, active variables, active edges, active degrees, immutable
incident-edge lists, residual component splitting, and residual memoization.

The missing dancing-cells step is a linked deletion/reinsertion representation
for active incidence. It should not be added just for elegance. It is worth doing
only if traces show that repeatedly walking inactive incident entries or undoing
large local mutations is a real bottleneck.

The desired invariant is unchanged: branch assignment should mutate only local
state, record exactly what changed, and restore the residual by replaying the
trail backwards.

## Hashing And Cache Direction

The residual branch solver has deterministic fingerprints plus exact comparison
inside cache buckets. That makes repeated residuals and cache-hit likelihood
visible.

Still-open hashing work:

- maintain an incremental Zobrist-style residual hash through reversible
  mutation;
- keep exact equality checks after hash lookup;
- separate whole-residual cache keys from future component-local residual cache
  keys;
- report enough cache statistics to compare heuristic changes.

Incremental hashing should be measured against the current full-state
fingerprint before it is made more complicated.

## Width Heuristics

The implemented branch heuristics are ordering policies. They do not replace a
decomposition backend.

Near-term experiments should focus on:

- decomposition builders for sign-only rankwidth DP;
- min-fill and cut-rank split choices;
- trace summaries that report width, table sizes, join counts, cache hits, and
  wall time on the same manifest;
- clear separation between branch variable ordering and decomposition
  construction.

Treewidth-style scoring is useful as a cheap proxy when it predicts fewer
residual components or smaller decomposition tables. Rankwidth-style scoring is
more directly aligned with the decomposition DP, but is more expensive to
evaluate.

## Residue And Count Arithmetic

Residues live modulo `r`; counts are exact assignment cardinalities. The hot path
should continue to use fixed-width integers when safe, and only switch to CRT or
another exact multiword representation when the final histogram cannot fit.

Future arithmetic work:

- specialize common moduli such as `2`, `4`, `8`, `16`, and `24`;
- keep count-table and Fourier modes behind the same result contract;
- consider multi-prime Fourier only if it beats count-table plus CRT on real
  widened benchmarks;
- keep arbitrary-precision libraries out of hot DP tables unless there is no
  fixed-width or CRT alternative.

## Import And Export Boundary

Importers should preserve enough provenance for debugging, but the solver should
see normalized QSOP only. External IDs, framework objects, and corpus-specific
metadata should stay in manifests or sidecar files.

Future import work that affects performance:

- configurable quadratization strategies for gates such as `CCZ`;
- qgraph/qc/ZX translation that records when a diagram remains quadratic;
- importer diagnostics that distinguish unsupported syntax from genuinely
  non-quadratic phase structure.

## Labelled Rankwidth

The labelled rankwidth solver should generalize sign-only cut signatures from
GF(2) parity rows to `Z_r` labelled boundary signatures. The relevant target
width is recorded in `ARCHITECTURE.md`.

Open implementation questions:

- compact interning of labelled signatures;
- join formulas for arbitrary edge labels;
- decomposition heuristics that estimate labelled cut-signature growth;
- benchmarks that actually require labelled interactions rather than only
  sign-edge structure.

## Instrumentation

Benchmark tooling should make solver changes comparable without requiring a
human to inspect verbose traces.

Useful stable metrics:

- imported/skipped/error counts by corpus;
- variables, terms, components, and modulus;
- branch internal nodes, leaves, cache hits, and cache misses;
- rankwidth decomposition width, table entries, signature entries, and join
  counts;
- elapsed time and selected backend configuration.

A structured JSON trace format is useful once the metric set settles. Until
then, concise stats output and manifest-based scripts are enough.

## Tests

Default CI should stay lean. Heavy corpus sweeps and framework comparisons are
valuable but should be opt-in unless they become cheap enough for every push.

Coverage should continue to include:

- parser and canonical writer behavior;
- backend agreement on small examples;
- residual undo and cache invariants;
- importer-fed corpus smoke tests;
- rankwidth large-count paths;
- the configured coverage threshold.
