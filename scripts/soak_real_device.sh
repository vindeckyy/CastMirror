#!/usr/bin/env bash
set -euo pipefail

TARGET_DEVICE="${CASTMIRROR_TEST_DEVICE:-${1:-}}"
DURATION_MINUTES="${2:-5}"
OUTPUT_CSV="${3:-docs/bench/real_${TARGET_DEVICE//./_}.csv}"

if [[ -z "${TARGET_DEVICE}" ]]; then
  echo "Usage: CASTMIRROR_TEST_DEVICE=<ip> $0 [ip] [duration_minutes] [output_csv]" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BIN="${REPO_ROOT}/build/app/castmirror"

if [[ ! -x "${BIN}" ]]; then
  echo "Error: ${BIN} not found or not executable. Run cmake --build build first." >&2
  exit 1
fi

mkdir -p "$(dirname "${OUTPUT_CSV}")"
echo "timestamp_iso,fps,bitrate_kbps,rtt_ms,loss_fraction,target_delay_ms,overruns" > "${OUTPUT_CSV}"

echo "Starting soak test against real Cast device ${TARGET_DEVICE} for ${DURATION_MINUTES} minutes..."
echo "Writing telemetry CSV to ${OUTPUT_CSV}..."

DURATION_SECONDS=$(( DURATION_MINUTES * 60 ))
START_TIME=$(date +%s)

# Launch castmirror in non-interactive background process
"${BIN}" --device "${TARGET_DEVICE}" --preset Balanced > /tmp/castmirror_soak.log 2>&1 &
CAST_PID=$!

cleanup() {
  echo "Stopping soak test (PID ${CAST_PID})..."
  kill -INT "${CAST_PID}" 2>/dev/null || true
  wait "${CAST_PID}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

while kill -0 "${CAST_PID}" 2>/dev/null; do
  NOW=$(date +%s)
  ELAPSED=$(( NOW - START_TIME ))
  if [[ ${ELAPSED} -ge ${DURATION_SECONDS} ]]; then
    echo "Soak duration of ${DURATION_MINUTES}m reached."
    break
  fi

  # Sample current live stats line from log
  LAST_LINE=$(grep -F "[LIVE]" /tmp/castmirror_soak.log | tail -n 1 || echo "")
  if [[ -n "${LAST_LINE}" ]]; then
    ISO_TS=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
    FPS=$(echo "${LAST_LINE}" | grep -oP 'FPS:\s*\K[0-9.]+' || echo "0")
    BITRATE=$(echo "${LAST_LINE}" | grep -oP 'Bitrate:\s*\K[0-9.]+' || echo "0")
    RTT=$(echo "${LAST_LINE}" | grep -oP 'RTT:\s*\K[0-9.]+' || echo "0")
    LOSS=$(echo "${LAST_LINE}" | grep -oP 'Loss:\s*\K[0-9.]+' || echo "0")
    DELAY=$(echo "${LAST_LINE}" | grep -oP 'Target Delay:\s*\K[0-9]+' || echo "200")
    OVERRUNS=$(echo "${LAST_LINE}" | grep -oP 'Drops:\s*\K[0-9]+' || echo "0")
    echo "${ISO_TS},${FPS},${BITRATE},${RTT},${LOSS},${DELAY},${OVERRUNS}" >> "${OUTPUT_CSV}"
  fi

  sleep 1
done

echo "Soak test completed successfully. Log recorded in ${OUTPUT_CSV}."
