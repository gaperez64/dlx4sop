#!/usr/bin/env sh
# Bootstrap for the qccq-gauntlet adapter: build a venv with the pinned
# Python tooling (meson/ninja/qiskit) and compile the release binaries the
# adapter shells out to. Runs once per checkout, from the repository root,
# and is not part of the timed per-case measurement.
set -eu

python3 -m venv .venv
export PATH="$PWD/.venv/bin:$PATH"
pip install --quiet -r .gauntlet/requirements.txt
meson setup build --buildtype=release -Db_lto=true
meson compile -C build qasm2sop sop-solve
