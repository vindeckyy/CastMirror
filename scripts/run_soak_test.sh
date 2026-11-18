#!/usr/bin/env bash
#
# CastMirror LOCAL / SYNTHETIC soak harness.
#
# THIS IS NOT REAL-DEVICE EVIDENCE. It exercises the soak *protocol* against the
# loopback fake-receiver + tools/poc-join pair (or, with --fixture, replays a
# bundled synthetic log) and emits a run record with "synthetic": true. The
# matrix generator keeps synthetic runs in a separate, loudly-labelled section
# so they can never be read as hardware validation.
#
# It writes the SAME schema (castmirror.device_soak.run.v1) as
# scripts/soak_real_device.sh, so both surfaces share one output format.
#
# Usage:
#   scripts/run_soak_test.sh [--duration SECONDS] [--out-dir DIR] [--build-dir DIR]
#   scripts/run_soak_test.sh --fixture        # replay the bundled synthetic log

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DURATION=120
DURATION_SET=0
OUT_DIR="${REPO_ROOT}/docs/bench/device_runs"
BUILD_DIR="${BUILD_DIR:-}"
FIXTURE=0
TLS_PORT=29009
UDP_PORT=54533

while [[ $# -gt 0 ]]; do
  case "$1" in
    --duration)
      [[ $# -ge 2 ]] || { echo "Error: --duration requires a value." >&2; exit 2; }
      DURATION="$2"; DURATION_SET=1; shift 2 ;;
    --out-dir)
      [[ $# -ge 2 ]] || { echo "Error: --out-dir requires a value." >&2; exit 2; }
      OUT_DIR="$2"; shift 2 ;;
    --build-dir)
      [[ $# -ge 2 ]] || { echo "Error: --build-dir requires a value." >&2; exit 2; }
      BUILD_DIR="$2"; shift 2 ;;
    --fixture) FIXTURE=1; shift ;;
    --tls-port)
      [[ $# -ge 2 ]] || { echo "Error: --tls-port requires a value." >&2; exit 2; }
      TLS_PORT="$2"; shift 2 ;;
    --udp-port)
      [[ $# -ge 2 ]] || { echo "Error: --udp-port requires a value." >&2; exit 2; }
      UDP_PORT="$2"; shift 2 ;;
    --help|-h) sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

PYTHON=()
if command -v python3 >/dev/null 2>&1; then PYTHON=(python3)
elif command -v python >/dev/null 2>&1; then PYTHON=(python)
elif command -v py >/dev/null 2>&1; then PYTHON=(py -3)
else
  echo "Error: no python interpreter found (need python3, python or py)." >&2
  exit 1
fi

find_tool() {
  local name="$1"
  local cands=()
  if [[ -n "${BUILD_DIR}" ]]; then
    cands+=(
      "${BUILD_DIR}/tools/${name}"
      "${BUILD_DIR}/tools/${name}.exe"
      "${REPO_ROOT}/${BUILD_DIR}/tools/${name}"
      "${REPO_ROOT}/${BUILD_DIR}/tools/${name}.exe"
    )
  fi
  cands+=(
    "${REPO_ROOT}/build/tools/${name}"
    "${REPO_ROOT}/build/tools/${name}.exe"
    "${REPO_ROOT}/build-swarm/tools/${name}"
    "${REPO_ROOT}/build-swarm/tools/${name}.exe"
    "${REPO_ROOT}/build-asan/tools/${name}"
    "${REPO_ROOT}/build-asan/tools/${name}.exe"
  )
  for cand in "${cands[@]}"; do
    if [[ -x "${cand}" ]]; then echo "${cand}"; return 0; fi
  done
  return 1
}

mkdir -p "${OUT_DIR}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
RUN_ID="synthetic-${STAMP}"
LOG="${OUT_DIR}/${RUN_ID}.log"
OUT_JSON="${OUT_DIR}/${RUN_ID}.json"
OUT_SUMMARY="${OUT_DIR}/${RUN_ID}.summary.txt"

echo "==============================================================="
echo " CastMirror LOCAL SYNTHETIC soak harness (NOT real hardware)"
echo "==============================================================="

FIXTURE_LOG="${SCRIPT_DIR}/fixtures/soak_synthetic.log"
FAKE_BIN="$(find_tool fake-receiver || true)"
JOIN_BIN="$(find_tool poc-join || true)"

USED_FIXTURE=0
if [[ "${FIXTURE}" == "1" || -z "${FAKE_BIN}" || -z "${JOIN_BIN}" ]]; then
  USED_FIXTURE=1
  if [[ "${FIXTURE}" != "1" ]]; then
    echo "[soak-test] loopback binaries not found; falling back to --fixture mode."
  fi
  if [[ ! -f "${FIXTURE_LOG}" ]]; then
    echo "Error: fixture log ${FIXTURE_LOG} missing." >&2
    exit 1
  fi
  echo "[soak-test] replaying bundled synthetic log: ${FIXTURE_LOG}"
  cp "${FIXTURE_LOG}" "${LOG}"
else
  echo "[soak-test] fake-receiver: ${FAKE_BIN}"
  echo "[soak-test] poc-join:      ${JOIN_BIN}"
  : > "${LOG}"
  "${FAKE_BIN}" "${TLS_PORT}" "${UDP_PORT}" >> "${LOG}" 2>&1 &
  RECEIVER_PID=$!
  cleanup() {
    if kill -0 "${RECEIVER_PID}" 2>/dev/null; then
      kill -TERM "${RECEIVER_PID}" 2>/dev/null || true
      sleep 0.3
      kill -9 "${RECEIVER_PID}" 2>/dev/null || true
      wait "${RECEIVER_PID}" 2>/dev/null || true
    fi
  }
  trap cleanup EXIT INT TERM
  sleep 1
  if "${JOIN_BIN}" --help 2>&1 | grep -q -- "--duration"; then
    timeout "$((DURATION + 10))" "${JOIN_BIN}" --ip 127.0.0.1 --port "${TLS_PORT}" \
        --duration "${DURATION}" >> "${LOG}" 2>&1 || true
  else
    timeout "$((DURATION + 10))" "${JOIN_BIN}" 127.0.0.1 "${TLS_PORT}" "${DURATION}" \
        >> "${LOG}" 2>&1 || true
  fi
  cleanup
  trap - EXIT INT TERM
fi

PARSE_ARGS=(
  "${SCRIPT_DIR}/soak_parse.py" parse
  --log "${LOG}"
  --out "${OUT_JSON}"
  --summary "${OUT_SUMMARY}"
  --run-id "${RUN_ID}"
  --synthetic
  --preset Balanced
  --capture monitor
  --model "Synthetic Capture Backend (loopback)"
  --md "SYNTHETIC"
  --firmware "n/a - synthetic harness"
  --band "loopback"
  --app-id "0F5096E8"
  --notes "synthetic harness run - not real hardware, not device validation"
)
if [[ "${USED_FIXTURE}" == "0" || "${DURATION_SET}" == "1" ]]; then
  PARSE_ARGS+=( --duration "${DURATION}" )
fi

"${PYTHON[@]}" "${PARSE_ARGS[@]}"

echo "[soak-test] run record : ${OUT_JSON}"
echo "[soak-test] summary    : ${OUT_SUMMARY}"
cat "${OUT_SUMMARY}"
