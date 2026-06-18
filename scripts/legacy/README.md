# Legacy benchmark scripts

The scripts in this subdirectory (`scripts/legacy/`) are superseded by the
unified `tools/bench.py` pipeline and are kept only for historical reference.

## Current benchmark entry points

| Purpose | Script |
|---|---|
| Local corpus smoke test (no external deps) | `scripts/bench_sop.py` |
| Full scoreboard refresh (Ganak + native) | `tools/bench.py full` |
| Render scoreboard from JSONL artifacts | `tools/bench.py render` |

## Two-track design

`scripts/` contains a **lightweight local path** that works without external
tools, manifests, or Ganak. It operates on the 12-instance synthetic corpus
in `benchmarks/corpus/sop/` and produces a simple local report. It is useful
for fast iteration during backend tuning.

`tools/bench.py` is the **authoritative full pipeline**: reads artifact JSONL
from all tiers, all backends, all WMC encodings, and native simulators, and
renders the complete multi-section scoreboard. Use this for public scoreboard
updates.

The two tracks produce different output formats and cover different corpus
scopes — they do not conflict but also do not share data files.
