# Legacy benchmark scripts

The scripts in `tools/` that predate the local SOP corpus workflow are
considered legacy. They depend on external generators (MQT Bench, FeynmanDD,
PyZX, Qiskit) and native simulators (Stim, etc.) and are not required for
routine local backend tuning.

## Replacements

| Old script (tools/) | Replacement (scripts/) |
|---|---|
| `bench_qasm_corpus.py` | `bench_sop.py` + local SOP corpus |
| `run_corpus_benchmarks.py` | `bench_sop.py` |
| `refresh_scoreboard.py` | `refresh_scoreboard.py` |
| `render_scoreboard.py` | `render_scoreboard.py` |
| `build_corpus.py` | `build_sop_corpus.py` |
| `bench_wmc_ganak.py` | `refresh_scoreboard.py --refresh-ganak` |
| `compare_rankwidth_backends.py` | `bench_sop.py --backend branch --backend branch:from-treewidth` |

The old scripts remain in `tools/` for external scoreboard refresh workflows
that require third-party generators. They are not maintained as part of the
local tuning workflow.
