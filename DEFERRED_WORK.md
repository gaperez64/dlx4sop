# Deferred Work — dlx4sop sprint handoff

Four items from the post-sprint audit. Each section gives the exact diagnosis
and the minimum change needed. All changes should be followed by
`meson test -C build` (43 tests, ≥75% coverage gate) before committing.

Items 1–3 are **resolved** (see commit history). Item 4 needs a decision.

---

## 1. ✅ `scripts/refresh_scoreboard.py` — wire up `--refresh-native`

**File:** `scripts/refresh_scoreboard.py:143,173`

The `--refresh-native` flag exists but prints "not yet implemented" and exits:

```python
if args.refresh_native:
    print("[native] native-solver refresh not yet implemented", file=sys.stderr)
```

The real native benchmark tool is `tools/bench_qasm_native_simulator.py`, which
reads a manifest (`--manifest`), converts each QASM entry via `qasm2sop`, and
runs native simulators. However, the local corpus (`benchmarks/corpus/sop/`) is
pure QSOP with no QASM; it has no manifest equivalent.

**What to implement:** either (a) generate a lightweight QASM manifest for the
local synthetic corpus so `bench_qasm_native_simulator.py` can consume it, or
(b) mark the flag as "local-corpus only: no QASM, skip native" with a clear
message and add a `--manifests` variant that calls the tool on an external
manifest. Option (b) is the path of least resistance.

**Acceptance:** `python3 scripts/refresh_scoreboard.py --refresh-native` must
not print "not yet implemented" — either it runs something or it prints a clear
"local corpus has no QASM; use tools/run_corpus_benchmarks.py for native runs".

---

## 2. ✅ Branch backend errors — missing `--max-vars` passthrough

**Files:** `scripts/bench_sop.py:33,42,100`

Six (instance, backend) pairs record `status=error`:

| Instance | r | nvars | backends |
|---|---|---|---|
| `tier-17-32-cycle-n26-r16-01` | 16 | 26 | `branch`, `branch:from-treewidth` |
| `tier-17-32-path-n27-r8-00` | 8 | 27 | `branch`, `branch:from-treewidth` |
| `tier-33-64-cycle-n35-r8-01` | 8 | 35 | `branch`, `branch:from-treewidth` |
| `tier-33-64-path-n63-r8-00` | 8 | 63 | `branch`, `branch:from-treewidth` |

All fail with:

```
error: …: residual branch solver refuses 26 variables;
       pass a larger --max-vars or use a future backend
```

`bench_sop.py` sets `BACKEND_MAX_VARS["branch"] = 64` to gate which instances
are fed to the branch backend, but it never passes `--max-vars` to the `sop-solve`
CLI. The branch solver has its own internal cap that defaults below 26.

**Fix:** add `"branch"` to `BACKEND_EXTRA_ARGS` with `["--max-vars", "64"]`:

```python
BACKEND_EXTRA_ARGS = {
    "rankwidth": ["--max-vars", "256"],
    "branch":    ["--max-vars", "64"],   # ← add this
}
```

Also apply the same extra args when `branch:from-treewidth` is invoked
(lines ~100–110 of `bench_instance()`):

```python
result = run_backend(sop_solve, qsop, "branch",
                     ["--branch-rw-source", "from-treewidth", "--max-vars", "64"],
                     timeout)
```

**Acceptance:** re-run `python3 scripts/bench_sop.py` and confirm zero `error`
records for the six pairs above. The cycle-n26/r=16 instance is expected to be
slow at branch-width 26 with r=16; add it to `BACKEND_MAX_VARS["branch"]` cap
considerations if it times out.

---

## 3. ✅ `tier-33-64-sparse-n58-r8-02` — hard instance, all backends fail

**File:** `scripts/bench_sop.py` (corpus annotation) and
`benchmarks/corpus/sop/tier-33-64/tier-33-64-sparse-n58-r8-02.meta.json`

This instance (58 vars, 249 edges, r=8) is genuinely hard:

| Backend | Result | Root cause |
|---|---|---|
| `treewidth` | error | Induced treewidth ≈ 25; backend refuses bags > default cap |
| `rankwidth` | timeout | Too many edges for current generator in 30 s |
| `branch` | error | Same internal max-vars issue as item 2 |

The treewidth error is:

```
error: …: treewidth backend refuses a 25-variable bag;
       pass a larger --max-vars or use another backend
```

At treewidth 25 with r=8, the DP table would be 8^25 ≈ 10^22 entries —
infeasible regardless of `--max-vars`. This instance is structurally outside
every current backend's tractable range.

**Two-part fix:**

**3a. Annotate the instance** — add `"known_hard": true` and
`"skip_backends": ["treewidth", "rankwidth", "branch"]` to
`tier-33-64-sparse-n58-r8-02.meta.json` so `bench_sop.py` can skip it
cleanly instead of recording errors/timeouts.

**3b. Honour the annotation** — in `bench_instance()` in `bench_sop.py`,
read `skip_backends` from meta and skip those entries:

```python
skip = set(meta.get("skip_backends", []))
for backend in backends:
    if backend in skip:
        continue
    ...
```

**Acceptance:** `bench_sop.py` produces no error/timeout records for
`sparse-n58-r8-02`; the meta file explains why it is skipped.

---

## 4. Scoreboard scripts duplication — `scripts/` stack superseded by `tools/`

**Files:** `scripts/refresh_scoreboard.py` (180 lines),
`scripts/render_scoreboard.py` (232 lines) vs `tools/refresh_scoreboard.py`
(799 lines), `tools/render_scoreboard.py` (868 lines)

There are two entirely separate scoreboard pipelines:

- **`scripts/`**: operates on the 12-instance synthetic local corpus
  (`benchmarks/corpus/sop/`), reads `.qsop` files directly, produces a simple
  JSONL format, renders a basic bar-chart scoreboard.
- **`tools/`**: the real full pipeline used by `run_corpus_benchmarks.py`;
  reads artifact JSONL from all tiers, all backends, all WMC encodings, native
  simulator; renders the complete multi-section scoreboard with skip-reason
  analysis, dispatch traces, and cache stats.

The `scripts/` stack was the original local-only prototype. Now that
`tools/run_corpus_benchmarks.py` exists and covers the full corpus, `scripts/`
is largely redundant. Nobody reconciled them.

**What to do:** decide between two options:

- **(a) Delete `scripts/refresh_scoreboard.py` and `scripts/render_scoreboard.py`**
  and update `scripts/bench_sop.py` to write artifacts in the format that
  `tools/refresh_scoreboard.py` can consume. The `scripts/bench_sop.py` local
  runner becomes a thin feeder into the same render path.

- **(b) Keep `scripts/` as a lightweight standalone path** (no external tools,
  no manifests, no ganak required) but document clearly that `tools/` is the
  authoritative scoreboard source. Rename to make the distinction obvious
  (e.g. `scripts/render_local_scoreboard.py`).

Option (a) eliminates ~400 lines of duplicate render logic. Option (b)
preserves a zero-dependency local smoke path.

**Acceptance:** `README.md` Benchmarks section describes one clear entry point
for the scoreboard; no two scripts produce conflicting scoreboard output for
the same data.
