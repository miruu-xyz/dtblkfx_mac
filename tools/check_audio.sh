#!/bin/bash
# Render every case through the DSP core and diff against the checked-in
# fingerprints. Run this before and after any change that touches src/core or
# the processor — it is the only thing standing between a refactor and a
# silently different-sounding plugin.
#
# Exit 0 = audio unchanged. Exit 1 = something moved; read the FAIL lines.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT}/build/tools/dtblkfx_render"
BASELINE="${ROOT}/tests/baseline/core.fingerprint"

if [ ! -x "${BIN}" ]; then
  echo "==> Building the harness"
  cmake --build "${ROOT}/build" --target dtblkfx_render
fi

exec "${BIN}" --check "${BASELINE}"
