#!/usr/bin/env bash
#
# CastMirror REAL-DEVICE SOAK PROTOCOL (orchestrator).
#
# Runs one soak session against physical Cast hardware, captures the console
# log, polls live stats once per second, and emits a machine-readable run
# record plus a human-readable summary.
#
# The protocol is documented in docs/SOAK_PROTOCOL.md. READ IT. In short:
#   * this script records what the sender observes (fps, bitrate, RTT, loss,
#     NACK/PLI, adaptive rung transitions, disconnects);
#   * it does NOT and CANNOT measure glass-to-glass latency -- that requires an
#     external camera. You must pass the observation in via --g2g-* or it is
#     recorded as null ("not measured"), never as a plausible number.
#
# Before touching a device it runs `scripts/fetch_cast_appids.sh --check` and
# ABORTS if the Cast app IDs have drifted from upstream Chromium, so a
# real-device run is never performed against stale app IDs.
#
# Usage:
#   scripts/soak_real_device.sh --device <ip> [options]
#
# See --help for the full option list.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BASH_BIN="${BASH:-bash}"

usage() {
  cat <<'EOF'
Usage: scripts/soak_real_device.sh --device <ip> [options]

Runs ONE real-device soak session and writes a JSON run record + summary.

Required:
  --device <ip>            Cast device IP (or set CASTMIRROR_TEST_DEVICE)

Session:
  --preset <name>          Auto|High|Balanced|Smooth|Game|Cinema (default Balanced)
  --bitrate <kbps>         Override the preset default video bitrate
  --delay <ms>             Override the preset default target delay
  --window <id>            Capture a single window instead of the monitor
  --duration <minutes>     Soak duration in minutes (default 5)

Hardware facts recorded verbatim (omit -> recorded as "not measured"):
  --model <string>         Device model, e.g. "Google TV Streamer (4K)"
  --md <string>            mDNS `md` model string, e.g. G3MYX
  --firmware <string>      Firmware / app version, e.g. "Android 14 (UTTC.240618)"
  --band <string>          2.4GHz | 5GHz | 6GHz | ethernet
  --distance <meters>      Approximate sender<->receiver distance
  --app-id <id>            Mirroring app ID actually launched

Glass-to-glass latency (EXTERNAL CAMERA ONLY -- not measured by this tool):
  --g2g-ms <ms>            Observed latency
  --g2g-method <string>    How it was observed, e.g. "external 120fps camera"
  --g2g-uncertainty-ms <ms> Measurement uncertainty

Output:
  --out-dir <dir>          Run record directory (default docs/bench/device_runs)
  --meta <file.json>       Metadata sidecar; overrides the flags above

Control:
  --skip-appid-check       Do NOT run the app-ID drift preflight (loud warning)
  --dry-run                Run the preflight and print the plan, then exit
  --help, -h               Show this help
EOF
}

TARGET_DEVICE="${CASTMIRROR_TEST_DEVICE:-}"
PRESET="Balanced"
BITRATE=""
DELAY=""
WINDOW_ID=""
DURATION_MIN="5"
MODEL=""
MD=""
FIRMWARE=""
BAND=""
DISTANCE=""
APP_ID=""
G2G_MS=""
G2G_METHOD=""
G2G_UNCERTAINTY_MS=""
OUT_DIR="${REPO_ROOT}/docs/bench/device_runs"
META=""
SKIP_APPID_CHECK=0
DRY_RUN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --device) TARGET_DEVICE="$2"; shift 2 ;;
    --preset) PRESET="$2"; shift 2 ;;
    --bitrate) BITRATE="$2"; shift 2 ;;
    --delay) DELAY="$2"; shift 2 ;;
    --window) WINDOW_ID="$2"; shift 2 ;;
    --duration) DURATION_MIN="$2"; shift 2 ;;
    --model) MODEL="$2"; shift 2 ;;
    --md) MD="$2"; shift 2 ;;
    --firmware) FIRMWARE="$2"; shift 2 ;;
    --band) BAND="$2"; shift 2 ;;
    --distance) DISTANCE="$2"; shift 2 ;;
    --app-id) APP_ID="$2"; shift 2 ;;
    --g2g-ms) G2G_MS="$2"; shift 2 ;;
    --g2g-method) G2G_METHOD="$2"; shift 2 ;;
    --g2g-uncertainty-ms) G2G_UNCERTAINTY_MS="$2"; shift 2 ;;
    --out-dir) OUT_DIR="$2"; shift 2 ;;
    --meta) META="$2"; shift 2 ;;
    --skip-appid-check) SKIP_APPID_CHECK=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "${TARGET_DEVICE}" ]]; then
  echo "Error: --device <ip> is required (or set CASTMIRROR_TEST_DEVICE)." >&2
  usage >&2
  exit 2
fi

# ---------------------------------------------------------------------------
# Interpreter / binary resolution
# ---------------------------------------------------------------------------
PYTHON=()
resolve_python() {
  if command -v python3 >/dev/null 2>&1; then PYTHON=(python3); return 0; fi
  if command -v python  >/dev/null 2>&1; then PYTHON=(python);  return 0; fi
  if command -v py      >/dev/null 2>&1; then PYTHON=(py -3);   return 0; fi
  return 1
}
if ! resolve_python; then
  echo "Error: no python interpreter found (need python3, python or py)." >&2
  exit 1
fi

find_binary() {
  local name="$1"
  for cand in \
      "${CASTMIRROR_BIN:-}" \
      "${REPO_ROOT}/build/app/${name}" \
      "${REPO_ROOT}/build/app/${name}.exe" \
      "${REPO_ROOT}/build-asan/app/${name}" \
      "${REPO_ROOT}/build-asan/app/${name}.exe"; do
    if [[ -n "${cand}" && -x "${cand}" ]]; then echo "${cand}"; return 0; fi
  done
  return 1
}

BIN="$(find_binary castmirror || true)"

# ---------------------------------------------------------------------------
# Preflight: Cast app-ID drift check (DELIVERABLE 3 wiring)
# ---------------------------------------------------------------------------
echo "==============================================================="
echo " CastMirror real-device soak protocol"
echo "==============================================================="
echo " device        : ${TARGET_DEVICE}"
echo " preset        : ${PRESET}"
echo " duration      : ${DURATION_MIN} minute(s)"
echo " output dir    : ${OUT_DIR}"
echo "---------------------------------------------------------------"

if [[ "${SKIP_APPID_CHECK}" == "1" ]]; then
  echo "!! WARNING: app-ID drift preflight SKIPPED (--skip-appid-check)." >&2
  echo "!! A real-device run against stale Cast app IDs is not evidence." >&2
else
  echo "[soak] Preflight 1/1: checking Cast app IDs against upstream Chromium..."
  set +e
  "${BASH_BIN}" "${SCRIPT_DIR}/fetch_cast_appids.sh" --check
  APPID_STATUS=$?
  set -e
  if [[ ${APPID_STATUS} -ne 0 ]]; then
    echo "" >&2
    echo "ABORT: Cast app-ID drift detected (exit ${APPID_STATUS})." >&2
    echo "       core/include/castcore/cast_app_ids.h no longer matches the IDs" >&2
    echo "       regenerated from Chromium HEAD. A real-device soak run would be" >&2
    echo "       performed against stale app IDs, so it is refused." >&2
    echo "       Review and run: scripts/fetch_cast_appids.sh" >&2
    echo "       (or pass --skip-appid-check to override -- do not ship that)." >&2
    exit 3
  fi
  echo "[soak] Preflight OK: Cast app IDs match upstream Chromium."
fi

if [[ -z "${BIN}" ]]; then
  echo "Error: castmirror binary not found under ${REPO_ROOT}/build/app." >&2
  echo "       Build it first: cmake --build build --target castmirror" >&2
  exit 1
fi
echo "[soak] binary: ${BIN}"

if [[ "${DRY_RUN}" == "1" ]]; then
  echo "[soak] --dry-run: preflight passed, not launching a session."
  exit 0
fi

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
mkdir -p "${OUT_DIR}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
SAFE_TARGET="$(printf '%s' "${TARGET_DEVICE}" | tr -c 'A-Za-z0-9._-' '-')"
RUN_ID="${STAMP}-${SAFE_TARGET}"
LOG="${OUT_DIR}/${RUN_ID}.log"
SAMPLES="${OUT_DIR}/${RUN_ID}.samples.tsv"
OUT_JSON="${OUT_DIR}/${RUN_ID}.json"
OUT_SUMMARY="${OUT_DIR}/${RUN_ID}.summary.txt"

DURATION_SECONDS=$(( DURATION_MIN * 60 ))

LAUNCH_ARGS=( --device "${TARGET_DEVICE}" --preset "${PRESET}" )
[[ -n "${BITRATE}" ]] && LAUNCH_ARGS+=( --bitrate "${BITRATE}" )
[[ -n "${WINDOW_ID}" ]] && LAUNCH_ARGS+=( --window "${WINDOW_ID}" )

echo "[soak] launching: ${BIN} ${LAUNCH_ARGS[*]}"
echo "[soak] log: ${LOG}"

"${BIN}" "${LAUNCH_ARGS[@]}" > "${LOG}" 2>&1 &
CAST_PID=$!

cleanup() {
  if kill -0 "${CAST_PID}" 2>/dev/null; then
    echo "[soak] stopping session (PID ${CAST_PID})..."
    kill -INT "${CAST_PID}" 2>/dev/null || true
    sleep 1
    kill -TERM "${CAST_PID}" 2>/dev/null || true
    wait "${CAST_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

printf 'elapsed_ms\tfps\tbitrate_kbps\trtt_ms\tloss_percent\ttarget_delay_ms\n' > "${SAMPLES}"

SECONDS=0
DIED_EARLY=0
while kill -0 "${CAST_PID}" 2>/dev/null; do
  if [[ ${SECONDS} -ge ${DURATION_SECONDS} ]]; then
    echo "[soak] duration of ${DURATION_MIN}m reached."
    break
  fi
  LAST_LINE="$(tr '\r' '\n' < "${LOG}" 2>/dev/null | awk '/\[LIVE\]/{l=$0} END{if(l!="")print l}' || true)"
  if [[ -n "${LAST_LINE}" ]]; then
    FPS="$(printf '%s' "${LAST_LINE}" | grep -oP 'FPS:\s*\K[0-9.]+' || true)"
    MBPS="$(printf '%s' "${LAST_LINE}" | grep -oP 'Bitrate:\s*\K[0-9.]+' || true)"
    RTT="$(printf '%s' "${LAST_LINE}" | grep -oP 'RTT:\s*\K[0-9.]+' || true)"
    LOSS="$(printf '%s' "${LAST_LINE}" | grep -oP 'Loss:\s*\K[0-9.]+' || true)"
    DELAY_OBS="$(printf '%s' "${LAST_LINE}" | grep -oP 'Target Delay:\s*\K[0-9]+' || true)"
    if [[ -n "${MBPS}" ]]; then
      KBPS="$("${PYTHON[@]}" -c "print(int(float('${MBPS}')*1000))" 2>/dev/null || echo "")"
    else
      KBPS=""
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$(( SECONDS * 1000 ))" "${FPS:-}" "${KBPS:-}" "${RTT:-}" "${LOSS:-}" "${DELAY_OBS:-}" \
      >> "${SAMPLES}"
  fi
  sleep 1
done

if ! kill -0 "${CAST_PID}" 2>/dev/null; then
  DIED_EARLY=1
  echo "[soak] WARNING: castmirror exited before the soak duration elapsed." >&2
fi

cleanup
trap - EXIT INT TERM

# ---------------------------------------------------------------------------
# Emit the machine-readable run record
# ---------------------------------------------------------------------------
PARSE_ARGS=(
  "${SCRIPT_DIR}/soak_parse.py" parse
  --log "${LOG}"
  --samples "${SAMPLES}"
  --out "${OUT_JSON}"
  --summary "${OUT_SUMMARY}"
  --run-id "${RUN_ID}"
  --device "${TARGET_DEVICE}"
  --preset "${PRESET}"
  --duration "${SECONDS}"
  --git-rev "$(git -C "${REPO_ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
)
[[ -n "${META}" ]] && PARSE_ARGS+=( --meta "${META}" )
[[ -n "${MODEL}" ]] && PARSE_ARGS+=( --model "${MODEL}" )
[[ -n "${MD}" ]] && PARSE_ARGS+=( --md "${MD}" )
[[ -n "${FIRMWARE}" ]] && PARSE_ARGS+=( --firmware "${FIRMWARE}" )
[[ -n "${BAND}" ]] && PARSE_ARGS+=( --band "${BAND}" )
[[ -n "${DISTANCE}" ]] && PARSE_ARGS+=( --distance "${DISTANCE}" )
[[ -n "${APP_ID}" ]] && PARSE_ARGS+=( --app-id "${APP_ID}" )
[[ -n "${BITRATE}" ]] && PARSE_ARGS+=( --bitrate "${BITRATE}" )
[[ -n "${DELAY}" ]] && PARSE_ARGS+=( --delay "${DELAY}" )
[[ -n "${G2G_MS}" ]] && PARSE_ARGS+=( --g2g-ms "${G2G_MS}" )
[[ -n "${G2G_METHOD}" ]] && PARSE_ARGS+=( --g2g-method "${G2G_METHOD}" )
[[ -n "${G2G_UNCERTAINTY_MS}" ]] && PARSE_ARGS+=( --g2g-uncertainty-ms "${G2G_UNCERTAINTY_MS}" )
if [[ -n "${WINDOW_ID}" ]]; then PARSE_ARGS+=( --capture window ); else PARSE_ARGS+=( --capture monitor ); fi
if [[ "${DIED_EARLY}" == "1" ]]; then PARSE_ARGS+=( --notes "sender process exited before the soak duration elapsed" ); fi

"${PYTHON[@]}" "${PARSE_ARGS[@]}"

echo "[soak] run record : ${OUT_JSON}"
echo "[soak] summary    : ${OUT_SUMMARY}"
echo "[soak] raw log    : ${LOG}"
echo ""
cat "${OUT_SUMMARY}"
echo "[soak] Regenerate the device matrix with: scripts/render_device_matrix.sh"
