#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

echo "[bootstrap] repo=$repo_root"

if [[ ! -d ".venv" ]]; then
  echo "[bootstrap] creating venv: .venv"
  python3 -m venv .venv
fi

echo "[bootstrap] activating venv"
# shellcheck disable=SC1091
source .venv/bin/activate

echo "[bootstrap] python=$(command -v python)"
python -m pip install -U pip >/dev/null

echo "[bootstrap] installing python deps"
python -m pip install -U py-clob-client-v2

if [[ ! -d "build" ]]; then
  echo "[bootstrap] configuring cmake build/"
  cmake -S . -B build
fi

echo "[bootstrap] building traders"
cmake --build build --target live_trader paper_trader

echo "[bootstrap] done"

