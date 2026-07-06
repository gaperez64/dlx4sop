#!/usr/bin/env sh
# Bootstrap for the sop2wmc+ganak qccq-gauntlet adapter: build a venv with
# the pinned Python tooling, compile qasm2sop/sop2wmc, and fetch a pinned
# prebuilt ganak release binary from upstream meelgroup/ganak (mode 6,
# mpfr complex counting, is stock functionality -- no fork is needed).
# Runs once per checkout, from the repository root, and is not part of the
# timed per-case measurement.
set -eu

python3 -m venv .venv
export PATH="$PWD/.venv/bin:$PATH"
pip install --quiet -r .gauntlet/requirements.txt
meson setup build --buildtype=release -Db_lto=true
meson compile -C build qasm2sop sop2wmc

GANAK_TAG="release/v2.6.3"
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64)
    GANAK_ASSET="ganak-v2.6.3-linux-amd64.tar.gz"
    GANAK_SHA256="8e0a7d28c00b2e5bd5a92fdaa8c225053e5df81b09f4422722d551ce0e073fcc"
    ;;
  Linux-aarch64)
    GANAK_ASSET="ganak-v2.6.3-linux-arm64.tar.gz"
    GANAK_SHA256="b67f3619fefa945035d23e69b19beb50736a4002e6491651bf54a36e8daf49c6"
    ;;
  *)
    echo "error: no pinned ganak release for $(uname -s)-$(uname -m)" >&2
    exit 1
    ;;
esac

mkdir -p .gauntlet/ganak
curl -sL -o .gauntlet/ganak/ganak.tar.gz \
  "https://github.com/meelgroup/ganak/releases/download/${GANAK_TAG}/${GANAK_ASSET}"
echo "${GANAK_SHA256}  .gauntlet/ganak/ganak.tar.gz" | sha256sum -c -
tar xzf .gauntlet/ganak/ganak.tar.gz -C .gauntlet/ganak
chmod +x .gauntlet/ganak/ganak
