#!/bin/sh
# Run a command under a hard memory cap so a runaway allocation cannot take the
# machine down with it (the pre-index-keyed CRT join maps could reach a ~140 GB
# footprint on a 5080-variable instance; see docs/table-representation-audit.md).
#
#   scripts/memguard.sh <budget-mib> <command> [args...]
#
# Linux: delegates to a real cgroup (systemd-run --user --scope -p MemoryMax=),
# which bounds the whole footprint and OOM-kills only the wrapped scope.
# macOS: there are no cgroups and the kernel rejects setrlimit(RLIMIT_DATA)
# outright (EINVAL), so a watchdog polls the resident set twice a second and
# SIGKILLs the process over budget.  Resident size undercounts pages the
# compressor has already taken, so choose budgets comfortably below free RAM.
set -eu
if [ "$#" -lt 2 ]; then
  echo "usage: $0 <budget-mib> <command> [args...]" >&2
  exit 2
fi
budget_mib=$1
shift
if [ "$(uname -s)" = Linux ] && command -v systemd-run >/dev/null 2>&1; then
  exec systemd-run --user --scope --quiet -p MemoryMax="${budget_mib}M" -p MemorySwapMax=0 "$@"
fi

"$@" &
pid=$!
trap 'kill -9 "$pid" 2>/dev/null || true' INT TERM
budget_kib=$((budget_mib * 1024))
while kill -0 "$pid" 2>/dev/null; do
  rss_kib=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ' || true)
  if [ -n "${rss_kib:-}" ] && [ "$rss_kib" -gt "$budget_kib" ]; then
    echo "memguard: rss ${rss_kib} KiB exceeds ${budget_mib} MiB budget; killing pid $pid" >&2
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    exit 137
  fi
  sleep 0.5
done
wait "$pid"
