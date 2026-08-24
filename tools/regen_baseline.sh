#!/bin/bash
# Overwrite the audio baseline with what the code does *right now*.
#
# Only run this when you have listened to the change and decided the new
# output is correct. Regenerating to make a red check go green throws away the
# entire point of having a baseline — commit the regenerated file as its own
# change, with a message saying what moved and why.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT}/build/tools/dtblkfx_render"
BASELINE="${ROOT}/tests/baseline/core.fingerprint"

cmake --build "${ROOT}/build" --target dtblkfx_render

"${BIN}" --write "${BASELINE}"

echo
echo "Baseline rewritten. Review the diff before committing:"
echo "  git diff -- tests/baseline/core.fingerprint"
