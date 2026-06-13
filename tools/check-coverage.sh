#!/usr/bin/env sh
set -eu

builddir="${1:-build-coverage}"
threshold="${DLX4SOP_COVERAGE_THRESHOLD:-75}"

if ! command -v meson >/dev/null 2>&1; then
  echo "error: meson is required for coverage checks" >&2
  exit 1
fi

if ! command -v gcovr >/dev/null 2>&1; then
  echo "error: gcovr is required for coverage checks" >&2
  exit 1
fi

if [ ! -f "$builddir/build.ninja" ]; then
  meson setup "$builddir" -Db_coverage=true
fi

meson test -C "$builddir" --print-errorlogs
gcovr \
  --root . \
  --filter 'src' \
  --exclude 'tests/' \
  --print-summary \
  --fail-under-line "$threshold"
