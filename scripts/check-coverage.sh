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
  # -Dsimd=scalar: the AVX-512/NEON kernels are gated behind runtime CPU
  # feature detection, so on a runner whose CPU lacks the extension (as is
  # the case for GitHub Actions' hosted x86_64 runners and AVX-512) the real
  # kernel bodies never execute no matter how much test coverage exists,
  # which tanks the line-coverage percentage on hardware the gate has no
  # control over. The scalar kernel is what "auto" already falls back to in
  # that situation, so this only affects what the coverage gate measures,
  # not correctness of the default (auto-detecting) build.
  meson setup "$builddir" -Db_coverage=true -Dsimd=scalar
fi

meson test -C "$builddir" --print-errorlogs
gcovr \
  --root . \
  --filter 'src' \
  --exclude 'tests/' \
  --gcov-ignore-parse-errors suspicious_hits.warn_once_per_file \
  --print-summary \
  --fail-under-line "$threshold"
