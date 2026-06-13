# dlx4sop Task Tracker

This file tracks local project state so work can resume after a pause or stop.
Completed implementation history has been flushed into `README.md`.

## Last Checkpoint

- Rewrote `README.md` to describe the currently implemented utilities and
  include concrete usage examples.
- Moved the still-relevant QSOP definition, syntax, examples, normalization
  rules, and backend design notes into `ARCHITECTURE.md`.
- Copied `.github/FUNDING.yml` from `gaperez64/acacia-bonsai`, including the
  `buy_me_a_coffee: gaperez64` entry.
- Implemented `sop-solve --format stats`:
  - additive stats-aware solver entry points;
  - branch `search_nodes` and `leaf_assignments`;
  - component count and component subproblem leaf work;
  - golden CLI tests for branch and component stats output.
- Verified with:
  - `meson test -C build --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 76.2% line coverage over `src`.
- Implemented component-level solve caching:
  - local cache inside the component backend;
  - deterministic cache key from canonical component subinstance data;
  - cache hit/miss counters in `sop-solve --format stats`;
  - golden CLI coverage for a repeated-component cache hit.
- Verified with:
  - `meson test -C build --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 75.6% line coverage over `src`.
- Implemented split-aware branch selection:
  - residual helper to estimate active component count after removing a candidate variable;
  - branch selector now prioritizes split count, then active degree, then unary-label presence;
  - residual unit tests for articulation, non-articulation, post-branch, and inactive-variable cases.
- Verified with:
  - `meson test -C build --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 75.8% line coverage over `src`.
- Added deterministic algebraic invariant tests:
  - canonicalization idempotence;
  - raw-vs-canonical solver agreement across available backends;
  - constant-phase residue-vector rotation.
- Registered the invariant test in Meson without adding new runtime dependencies.
- Verified with:
  - `meson test -C build --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 75.8% line coverage over `src`.
- Added optional parser fuzz target:
  - `tests/fuzz/fuzz_qsop_parse.c` with libFuzzer-compatible entry point;
  - standalone replay mode for corpus files and stdin;
  - parser plus canonical writer idempotence oracle;
  - Meson target behind `-Dbuild_fuzzers=true`, outside normal CI.
- Verified with:
  - `meson test -C build --print-errorlogs`
  - `meson setup --wipe build-fuzz -Dbuild_fuzzers=true`
  - `meson test -C build-fuzz --print-errorlogs`
- Added initial OpenQASM static-subset importer:
  - new `qasm2sop` utility;
  - OpenQASM 2.0, `qreg`, ignored `include` and `barrier`;
  - gates `h`, `t`, `tdg`, `s`, `sdg`, `z`, and `cz`;
  - generated raw QSOP with 0-to-0 boundary pins, then canonicalized through the QSOP parser/writer;
  - golden tests for `H-T-H`, `H-CZ-H`, stdin/help, and unsupported dynamic features.
- Verified with:
  - `meson test -C build --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 75.2% line coverage over `src`.
- Extended direct OpenQASM static gate support:
  - `id` as a no-op after operand validation;
  - `swap` as a wire-state permutation;
  - golden test covering `id` and `swap`.
- Added explicit input/output boundary options to `qasm2sop`:
  - `--input BITS` and `--output BITS`;
  - omitted boundaries default to all-zero bits;
  - boundary pins are emitted explicitly and contradictory fixed boundaries
    produce a valid zero-amplitude QSOP;
  - golden and CLI tests cover nonzero boundaries, zero boundaries, and option
    validation.
- Verified with:
  - `meson test -C build --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 75.9% line coverage over `src`.
- Added decomposition-backed OpenQASM imports:
  - `x` lowered as `h; z; h`;
  - `cx` lowered as `h` on the target, `cz`, then `h` on the target;
  - boundary-level golden tests cover `x: 0 -> 1` and `cx: 10 -> 11`.
- Verified with:
  - `meson test -C build --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 76.1% line coverage over `src`.

## Current Task

- Continue broadening OpenQASM importer coverage:
  - add only gates that lower cleanly to supported primitives or have a clear
    QSOP representation;
  - keep coverage above the 75% CI gate.

## Future Tasks

- Add more OpenQASM syntax compatibility as importer scope grows.
