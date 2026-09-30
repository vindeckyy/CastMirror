#!/usr/bin/env bash
#
# Regenerate docs/DEVICE_MATRIX.md from the recorded soak runs.
#
# Reads every castmirror.device_soak.run.v1 record under
# docs/bench/device_runs/ and rewrites ONLY the block between
#   <!-- BEGIN GENERATED:DEVICE_MATRIX -->
#   <!-- END GENERATED:DEVICE_MATRIX -->
# in docs/DEVICE_MATRIX.md, leaving the hand-written sections untouched.
#
# Every number it writes comes from a recorded run. A device with no recorded
# run is rendered as "not measured". Synthetic runs are kept in a separate,
# loudly-labelled section and can never be mistaken for hardware evidence.
#
# Usage:
#   scripts/render_device_matrix.sh                 # update docs/DEVICE_MATRIX.md
#   scripts/render_device_matrix.sh --stdout        # print the block, write nothing
#   scripts/render_device_matrix.sh --check         # exit 1 if the file is stale
#   scripts/render_device_matrix.sh --runs-dir DIR --out FILE

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PYTHON=()
if command -v python3 >/dev/null 2>&1; then PYTHON=(python3)
elif command -v python >/dev/null 2>&1; then PYTHON=(python)
elif command -v py >/dev/null 2>&1; then PYTHON=(py -3)
else
  echo "Error: no python interpreter found (need python3, python or py)." >&2
  exit 1
fi

RUNS_DIR="${REPO_ROOT}/docs/bench/device_runs"
OUT_FILE="${REPO_ROOT}/docs/DEVICE_MATRIX.md"
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --stdout) EXTRA_ARGS+=( --stdout ); shift ;;
    --check) EXTRA_ARGS+=( --check ); shift ;;
    --runs-dir) RUNS_DIR="$2"; shift 2 ;;
    --out) OUT_FILE="$2"; shift 2 ;;
    --help|-h)
      sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

if [[ ${#EXTRA_ARGS[@]} -gt 0 && " ${EXTRA_ARGS[*]} " == *" --stdout "* ]]; then
  "${PYTHON[@]}" "${SCRIPT_DIR}/render_device_matrix.py" \
      --runs-dir "${RUNS_DIR}" "${EXTRA_ARGS[@]}"
else
  "${PYTHON[@]}" "${SCRIPT_DIR}/render_device_matrix.py" \
      --runs-dir "${RUNS_DIR}" --out "${OUT_FILE}" "${EXTRA_ARGS[@]}"
fi
