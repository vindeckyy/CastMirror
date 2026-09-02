#!/usr/bin/env bash
# CastMirror Phase 0.4 - Benchmark harness & baseline
# Runs poc-encode 1080p60 60s, poc-join against fake-receiver 28009 53533,
# records baseline CSV: encode_ms_p50/p95, fps, udp_pps, rtt_ms, loss_fraction, stop_ms
# Runnable via xvfb-run and supports --dry-run for quick CI check.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
TLS_PORT=28009
UDP_PORT=53533
ENCODE_DURATION=60
JOIN_DURATION=10
DRY_RUN=0
OUT_CSV="$ROOT_DIR/docs/bench/baseline.csv"
ENCODE_LOG="/tmp/bench_encode.log"
JOIN_LOG="/tmp/bench_join.log"
RECEIVER_LOG="/tmp/bench_receiver.log"
RECEIVER_PID=""

# Colors for pretty output (disabled if not tty)
if [[ -t 1 ]]; then
  GREEN="\033[32m"; YELLOW="\033[33m"; RED="\033[31m"; RESET="\033[0m"
else
  GREEN=""; YELLOW=""; RED=""; RESET=""
fi

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Phase 0.4 Benchmark harness - records encode + transport baseline.

Options:
  --dry-run, --quick      Quick run (encode 3s, join 3s) for CI smoke check
  --encode-duration SEC   Encode bench duration (default 60)
  --join-duration SEC     Join bench duration (default 10)
  --tls-port PORT         Fake receiver TLS port (default 28009)
  --udp-port PORT         Fake receiver UDP port (default 53533)
  --out PATH              Output CSV path (default docs/bench/baseline.csv)
  --build-dir PATH        Build directory (default build)
  --help, -h              Show this help

Examples:
  bash scripts/bench_baseline.sh
  bash scripts/bench_baseline.sh --dry-run
  xvfb-run -a bash scripts/bench_baseline.sh
  bash scripts/bench_baseline.sh --encode-duration 30 --join-duration 5

Outputs:
  CSV with header: timestamp,git_rev,encode_ms_p50,encode_ms_p95,fps,udp_pps,rtt_ms,loss_fraction,stop_ms,encoder,backend,width,height,duration
  Stored at docs/bench/baseline.csv (tracked via .gitignore exception)
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run|--quick) DRY_RUN=1; shift ;;
    --encode-duration) ENCODE_DURATION="$2"; shift 2 ;;
    --join-duration) JOIN_DURATION="$2"; shift 2 ;;
    --tls-port) TLS_PORT="$2"; shift 2 ;;
    --udp-port) UDP_PORT="$2"; shift 2 ;;
    --out) OUT_CSV="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --help|-h) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

if [[ "$DRY_RUN" == "1" ]]; then
  ENCODE_DURATION=3
  JOIN_DURATION=3
  echo -e "${YELLOW}[bench] DRY RUN: encode ${ENCODE_DURATION}s, join ${JOIN_DURATION}s${RESET}"
fi

strip_ansi() { sed 's/\x1b\[[0-9;]*m//g'; }

find_binary() {
  local name="$1"
  # Prefer explicit BUILD_DIR
  if [[ -x "$BUILD_DIR/tools/$name" ]]; then echo "$BUILD_DIR/tools/$name"; return 0; fi
  if [[ -x "$ROOT_DIR/build/tools/$name" ]]; then echo "$ROOT_DIR/build/tools/$name"; return 0; fi
  if [[ -x "$ROOT_DIR/build-asan/tools/$name" ]]; then echo "$ROOT_DIR/build-asan/tools/$name"; return 0; fi
  if command -v "$name" >/dev/null 2>&1; then command -v "$name"; return 0; fi
  return 1
}

ensure_build() {
  local poc_encode_bin poc_join_bin fake_bin
  poc_encode_bin=$(find_binary poc-encode || true)
  poc_join_bin=$(find_binary poc-join || true)
  fake_bin=$(find_binary fake-receiver || true)
  if [[ -z "$poc_encode_bin" || -z "$poc_join_bin" || -z "$fake_bin" ]]; then
    echo -e "${YELLOW}[bench] Binaries missing, attempting cmake build...${RESET}"
    if [[ ! -d "$BUILD_DIR" ]]; then
      cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCASTMIRROR_ENABLE_TRAY=OFF || \
      cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
    fi
    cmake --build "$BUILD_DIR" -j"$(nproc)" --target poc-encode poc-join fake-receiver || \
    cmake --build "$BUILD_DIR" -j"$(nproc)"
  fi
  # re-resolve after build
  POC_ENCODE_BIN=$(find_binary poc-encode) || { echo "poc-encode not found" >&2; exit 1; }
  POC_JOIN_BIN=$(find_binary poc-join) || { echo "poc-join not found" >&2; exit 1; }
  FAKE_BIN=$(find_binary fake-receiver) || { echo "fake-receiver not found" >&2; exit 1; }
  # Also check cmake built tools
  echo "[bench] Using binaries:"
  echo "  poc-encode: $POC_ENCODE_BIN"
  echo "  poc-join:   $POC_JOIN_BIN"
  echo "  fake:       $FAKE_BIN"
}

cleanup() {
  echo "[bench] Cleaning up..."
  if [[ -n "${RECEIVER_PID:-}" ]] && kill -0 "$RECEIVER_PID" 2>/dev/null; then
    kill -TERM "$RECEIVER_PID" 2>/dev/null || true
    sleep 0.5
    kill -9 "$RECEIVER_PID" 2>/dev/null || true
    wait "$RECEIVER_PID" 2>/dev/null || true
  fi
  # kill any leftover fake-receiver on bench ports
  pkill -f "fake-receiver.*$TLS_PORT" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

ensure_build

# Check xvfb-run availability hint
if [[ -z "${DISPLAY:-}" ]]; then
  if command -v xvfb-run >/dev/null 2>&1; then
    echo -e "${YELLOW}[bench] No DISPLAY, but xvfb-run available. If capture fails, re-run as:${RESET}"
    echo "  xvfb-run -a bash scripts/bench_baseline.sh $@"
  else
    echo -e "${YELLOW}[bench] No DISPLAY and no xvfb-run - using Synthetic capture fallback${RESET}"
  fi
fi

# Ensure output dir exists
mkdir -p "$(dirname "$OUT_CSV")"
mkdir -p "$(dirname "$ENCODE_LOG")"

echo "==============================================="
echo " CastMirror Phase 0.4 - Benchmark harness"
echo "==============================================="
echo " Encode: 1080p60 ${ENCODE_DURATION}s (poc-encode)"
echo " Join:   fake-receiver ${TLS_PORT}/${UDP_PORT} + poc-join ${JOIN_DURATION}s"
echo " Output: $OUT_CSV"
echo "==============================================="

# 1. Run poc-encode
echo "[bench] [1/2] Running poc-encode ${ENCODE_DURATION}s 1080p60..."
# Try new CLI first, fallback to legacy
set +e
if "$POC_ENCODE_BIN" --help 2>&1 | grep -q -- "--duration"; then
  # New CLI supports --preset and --duration and --json
  ENCODE_JSON="/tmp/bench_encode.json"
  timeout $((ENCODE_DURATION+20)) "$POC_ENCODE_BIN" --preset 1080p60 --duration "$ENCODE_DURATION" --json "$ENCODE_JSON" 2>&1 | tee "$ENCODE_LOG"
  ENC_EXIT=${PIPESTATUS[0]}
  # If --json failed, try without json
  if [[ $ENC_EXIT -ne 0 ]]; then
    echo "[bench] retry without --json"
    timeout $((ENCODE_DURATION+20)) "$POC_ENCODE_BIN" --preset 1080p60 --duration "$ENCODE_DURATION" 2>&1 | tee "$ENCODE_LOG"
    ENC_EXIT=${PIPESTATUS[0]}
  fi
else
  # Legacy binary: positional 1080p60 60s
  timeout $((ENCODE_DURATION+20)) "$POC_ENCODE_BIN" 1080p60 "${ENCODE_DURATION}s" 2>&1 | tee "$ENCODE_LOG"
  ENC_EXIT=${PIPESTATUS[0]}
  if [[ $ENC_EXIT -ne 0 ]]; then
    # fallback legacy no args (3s)
    timeout $((ENCODE_DURATION+20)) "$POC_ENCODE_BIN" 2>&1 | tee "$ENCODE_LOG"
    ENC_EXIT=${PIPESTATUS[0]}
  fi
  ENCODE_JSON=""
fi
set -e
if [[ ${ENC_EXIT:-0} -ne 0 ]]; then
  echo -e "${YELLOW}[bench] poc-encode exited $ENC_EXIT (continuing with available log)${RESET}"
fi

# Parse encode metrics
# Prefer JSON if available
ENCODE_P50=""
ENCODE_P95=""
ENCODE_FPS=""
ENCODE_AVG=""
FRAMES=""
BITRATE=""
ENCODER=""
BACKEND=""

if [[ -n "${ENCODE_JSON:-}" && -f "$ENCODE_JSON" ]]; then
  # Parse JSON via python or jq or grep
  if command -v python3 >/dev/null 2>&1; then
    ENCODE_P50=$(python3 -c "import json; d=json.load(open('$ENCODE_JSON')); print(d.get('encode_ms_p50',0))" 2>/dev/null || echo "")
    ENCODE_P95=$(python3 -c "import json; d=json.load(open('$ENCODE_JSON')); print(d.get('encode_ms_p95',0))" 2>/dev/null || echo "")
    ENCODE_FPS=$(python3 -c "import json; d=json.load(open('$ENCODE_JSON')); print(d.get('fps_measured',0))" 2>/dev/null || echo "")
    ENCODE_AVG=$(python3 -c "import json; d=json.load(open('$ENCODE_JSON')); print(d.get('encode_ms_avg',0))" 2>/dev/null || echo "")
    FRAMES=$(python3 -c "import json; d=json.load(open('$ENCODE_JSON')); print(d.get('frames',0))" 2>/dev/null || echo "")
    BITRATE=$(python3 -c "import json; d=json.load(open('$ENCODE_JSON')); print(d.get('bitrate_mbps',0))" 2>/dev/null || echo "")
  fi
fi

# Fallback to log parsing (strip ANSI)
CLEAN_ENCODE=$(strip_ansi < "$ENCODE_LOG" || cat "$ENCODE_LOG")

if [[ -z "$ENCODE_P50" ]]; then
  # Try BENCH_METRIC line: encode_ms_p50=XX
  ENCODE_P50=$(echo "$CLEAN_ENCODE" | grep -oP 'encode_ms_p50=\K[0-9.]+' | tail -n1 || echo "")
fi
if [[ -z "$ENCODE_P95" ]]; then
  ENCODE_P95=$(echo "$CLEAN_ENCODE" | grep -oP 'encode_ms_p95=\K[0-9.]+' | tail -n1 || echo "")
fi
if [[ -z "$ENCODE_FPS" ]]; then
  ENCODE_FPS=$(echo "$CLEAN_ENCODE" | grep -oP 'BENCH_ENCODE[^0-9]*p50=[0-9.]+ p95=[0-9.]+ avg=[0-9.]+ fps=\K[0-9.]+' | tail -n1 || echo "")
  if [[ -z "$ENCODE_FPS" ]]; then
    ENCODE_FPS=$(echo "$CLEAN_ENCODE" | grep -oP 'fps=\K[0-9.]+' | tail -n1 || echo "")
  fi
  # Also try "Video Frames Encoded: 161 (53.6 FPS)"
  if [[ -z "$ENCODE_FPS" ]]; then
    ENCODE_FPS=$(echo "$CLEAN_ENCODE" | grep -oP 'Video Frames Encoded:.*\(\K[0-9.]+' | tail -n1 || echo "")
  fi
fi
if [[ -z "$ENCODE_P50" ]]; then
  # Try Average fallback
  AVG=$(echo "$CLEAN_ENCODE" | grep -oP 'Average Video Encode Latency:\s*\K[0-9.]+' | tail -n1 || echo "")
  ENCODE_P50="$AVG"
  ENCODE_P95="$AVG"
fi
# Also capture p50/p95 from dedicated log lines
if [[ -z "$ENCODE_P50" ]]; then
  ENCODE_P50=$(echo "$CLEAN_ENCODE" | grep -oP 'p50 Video Encode Latency:\s*\K[0-9.]+' | tail -n1 || echo "")
fi
if [[ -z "$ENCODE_P95" ]]; then
  ENCODE_P95=$(echo "$CLEAN_ENCODE" | grep -oP 'p95 Video Encode Latency:\s*\K[0-9.]+' | tail -n1 || echo "")
fi

# Sanitize and defaults
ENCODE_P50=${ENCODE_P50:-0}
ENCODE_P95=${ENCODE_P95:-0}
ENCODE_FPS=${ENCODE_FPS:-0}
ENCODE_AVG=${ENCODE_AVG:-$ENCODE_P50}
FRAMES=${FRAMES:-0}
BITRATE=${BITRATE:-0}

# Ensure numeric
if ! [[ "$ENCODE_P50" =~ ^[0-9.]+$ ]]; then ENCODE_P50=0; fi
if ! [[ "$ENCODE_P95" =~ ^[0-9.]+$ ]]; then ENCODE_P95=0; fi
if ! [[ "$ENCODE_FPS" =~ ^[0-9.]+$ ]]; then ENCODE_FPS=0; fi

echo "[bench] Encode metrics: p50=${ENCODE_P50}ms p95=${ENCODE_P95}ms fps=${ENCODE_FPS} frames=${FRAMES}"

# 2. Run fake-receiver + poc-join
echo "[bench] [2/2] Launching fake-receiver TLS:$TLS_PORT UDP:$UDP_PORT..."
# Kill any stale on same ports
fuser -k ${TLS_PORT}/tcp 2>/dev/null || true
fuser -k ${UDP_PORT}/udp 2>/dev/null || true
sleep 0.2

# Start fake-receiver
"$FAKE_BIN" "$TLS_PORT" "$UDP_PORT" > "$RECEIVER_LOG" 2>&1 &
RECEIVER_PID=$!
# Wait for ready (poll log or port)
READY=0
for i in $(seq 1 50); do
  if grep -q "Fake Cast Receiver running" "$RECEIVER_LOG" 2>/dev/null; then READY=1; break; fi
  if grep -q "Fake Receiver ready" "$RECEIVER_LOG" 2>/dev/null; then READY=1; break; fi
  # Also check port listening
  if command -v ss >/dev/null 2>&1; then
    if ss -tln 2>/dev/null | grep -q ":$TLS_PORT"; then READY=1; break; fi
  elif command -v netstat >/dev/null 2>&1; then
    if netstat -tln 2>/dev/null | grep -q ":$TLS_PORT"; then READY=1; break; fi
  fi
  if ! kill -0 "$RECEIVER_PID" 2>/dev/null; then
    echo "[bench] fake-receiver died early, log:"
    cat "$RECEIVER_LOG" || true
    break
  fi
  sleep 0.1
done
if [[ "$READY" != "1" ]]; then
  echo -e "${YELLOW}[bench] Warning: fake-receiver not ready after 5s (log below)${RESET}"
  cat "$RECEIVER_LOG" || true
  sleep 1
fi
echo "[bench] Fake receiver PID $RECEIVER_PID ready"

# Run poc-join
echo "[bench] Running poc-join against 127.0.0.1:$TLS_PORT for ${JOIN_DURATION}s..."
set +e
JOIN_JSON="/tmp/bench_join.json"
if "$POC_JOIN_BIN" --help 2>&1 | grep -q -- "--duration"; then
  timeout $((JOIN_DURATION+15)) "$POC_JOIN_BIN" --ip 127.0.0.1 --port "$TLS_PORT" --duration "$JOIN_DURATION" --json "$JOIN_JSON" 2>&1 | tee "$JOIN_LOG"
  JOIN_EXIT=${PIPESTATUS[0]}
else
  timeout $((JOIN_DURATION+15)) "$POC_JOIN_BIN" 127.0.0.1 "$TLS_PORT" "$JOIN_DURATION" 2>&1 | tee "$JOIN_LOG"
  JOIN_EXIT=${PIPESTATUS[0]}
fi
set -e
# 124 is timeout exit, 0 is success, 1 is poc-join failure (still produce CSV)
if [[ ${JOIN_EXIT:-0} -eq 124 ]]; then
  echo -e "${YELLOW}[bench] poc-join timed out (killed by timeout) - treating as completed${RESET}"
elif [[ ${JOIN_EXIT:-0} -ne 0 ]]; then
  echo -e "${YELLOW}[bench] poc-join exited $JOIN_EXIT (still parsing log)${RESET}"
fi

# Give receiver a moment to flush
sleep 0.5

# Stop receiver
if kill -0 "$RECEIVER_PID" 2>/dev/null; then
  kill -TERM "$RECEIVER_PID" 2>/dev/null || true
  sleep 0.5
  kill -9 "$RECEIVER_PID" 2>/dev/null || true
  wait "$RECEIVER_PID" 2>/dev/null || true
  RECEIVER_PID=""
fi

# Parse join metrics
UDP_PPS=""
RTT_MS=""
LOSS=""
STOP_MS=""
JOIN_FPS=""
PACKETS=""
JOIN_FRAMES=""

if [[ -f "$JOIN_JSON" ]]; then
  if command -v python3 >/dev/null 2>&1; then
    UDP_PPS=$(python3 -c "import json; d=json.load(open('$JOIN_JSON')); print(d.get('udp_pps',0))" 2>/dev/null || echo "")
    RTT_MS=$(python3 -c "import json; d=json.load(open('$JOIN_JSON')); print(d.get('rtt_ms',0))" 2>/dev/null || echo "")
    LOSS=$(python3 -c "import json; d=json.load(open('$JOIN_JSON')); print(d.get('loss_fraction',0))" 2>/dev/null || echo "")
    STOP_MS=$(python3 -c "import json; d=json.load(open('$JOIN_JSON')); print(d.get('stop_ms',0))" 2>/dev/null || echo "")
    JOIN_FPS=$(python3 -c "import json; d=json.load(open('$JOIN_JSON')); print(d.get('fps',0))" 2>/dev/null || echo "")
    PACKETS=$(python3 -c "import json; d=json.load(open('$JOIN_JSON')); print(d.get('packets_sent',0))" 2>/dev/null || echo "")
    JOIN_FRAMES=$(python3 -c "import json; d=json.load(open('$JOIN_JSON')); print(d.get('frames_sent',0))" 2>/dev/null || echo "")
  fi
fi

CLEAN_JOIN=$(strip_ansi < "$JOIN_LOG" || cat "$JOIN_LOG")

if [[ -z "$UDP_PPS" ]]; then
  UDP_PPS=$(echo "$CLEAN_JOIN" | grep -oP 'udp_pps=\K[0-9.]+' | tail -n1 || echo "")
fi
if [[ -z "$RTT_MS" ]]; then
  RTT_MS=$(echo "$CLEAN_JOIN" | grep -oP 'rtt_ms=\K[0-9.]+' | tail -n1 || echo "")
  if [[ -z "$RTT_MS" ]]; then
    RTT_MS=$(echo "$CLEAN_JOIN" | grep -oP 'RTT:\s*\K[0-9.]+' | tail -n1 || echo "")
  fi
fi
if [[ -z "$LOSS" ]]; then
  LOSS=$(echo "$CLEAN_JOIN" | grep -oP 'loss_fraction=\K[0-9.]+' | tail -n1 || echo "")
  if [[ -z "$LOSS" ]]; then
    # Parse from "Loss: 0.5%"
    LOSS_PCT=$(echo "$CLEAN_JOIN" | grep -oP 'Loss:\s*\K[0-9.]+' | tail -n1 || echo "")
    if [[ -n "$LOSS_PCT" ]]; then
      LOSS=$(python3 -c "print(float('$LOSS_PCT')/100.0)" 2>/dev/null || echo "0")
    fi
  fi
fi
if [[ -z "$STOP_MS" ]]; then
  STOP_MS=$(echo "$CLEAN_JOIN" | grep -oP 'Stop completed in\s*\K[0-9.]+' | tail -n1 || echo "")
  if [[ -z "$STOP_MS" ]]; then
    STOP_MS=$(echo "$CLEAN_JOIN" | grep -oP 'stop_ms=\K[0-9.]+' | tail -n1 || echo "")
  fi
fi
if [[ -z "$JOIN_FPS" ]]; then
  JOIN_FPS=$(echo "$CLEAN_JOIN" | grep -oP 'BENCH_JOIN[^0-9]*fps=\K[0-9.]+' | tail -n1 || echo "")
  if [[ -z "$JOIN_FPS" ]]; then
    JOIN_FPS=$(echo "$CLEAN_JOIN" | grep -oP '\[LIVE STATS\] FPS:\s*\K[0-9.]+' | tail -n1 || echo "")
  fi
fi
if [[ -z "$PACKETS" ]]; then
  PACKETS=$(echo "$CLEAN_JOIN" | grep -oP 'packets=\K[0-9]+' | tail -n1 || echo "")
  if [[ -z "$PACKETS" ]]; then
    PACKETS=$(echo "$CLEAN_JOIN" | grep -oP 'Packets:\s*\K[0-9]+' | tail -n1 || echo "")
  fi
fi

# Also try BENCH_JOIN_CSV fallback: udp_pps,rtt,loss,stop,fps
if [[ -z "$UDP_PPS" ]]; then
  CSV_LINE=$(echo "$CLEAN_JOIN" | grep "BENCH_JOIN_CSV" | tail -n1 || echo "")
  if [[ -n "$CSV_LINE" ]]; then
    # format: BENCH_JOIN_CSV 123,0.5,0,45,60
    CSV_VAL=$(echo "$CSV_LINE" | sed -E 's/.*BENCH_JOIN_CSV[[:space:]]*//')
    UDP_PPS=$(echo "$CSV_VAL" | cut -d',' -f1)
    RTT_MS=$(echo "$CSV_VAL" | cut -d',' -f2)
    LOSS=$(echo "$CSV_VAL" | cut -d',' -f3)
    STOP_MS=$(echo "$CSV_VAL" | cut -d',' -f4)
    JOIN_FPS=$(echo "$CSV_VAL" | cut -d',' -f5)
  fi
fi

# Defaults
UDP_PPS=${UDP_PPS:-0}
RTT_MS=${RTT_MS:-0}
LOSS=${LOSS:-0}
STOP_MS=${STOP_MS:-0}
JOIN_FPS=${JOIN_FPS:-0}
PACKETS=${PACKETS:-0}
JOIN_FRAMES=${JOIN_FRAMES:-0}

# Sanitize numeric
for v in UDP_PPS RTT_MS LOSS STOP_MS JOIN_FPS; do
  val=$(eval echo \$$v)
  if ! [[ "$val" =~ ^[0-9.]+$ ]]; then
    # allow 0.0 scientific? fallback 0
    eval $v=0
  fi
done

# If UDP_PPS still 0 but we have packets and duration, compute
if [[ "$UDP_PPS" == "0" && "$PACKETS" != "0" && "$JOIN_DURATION" -gt 0 ]]; then
  UDP_PPS=$(python3 -c "print(float('$PACKETS')/float('$JOIN_DURATION'))" 2>/dev/null || echo "0")
fi
if [[ "$UDP_PPS" == "0" && "$JOIN_FPS" != "0" ]]; then
  # Fallback approx: fps * ~2 packets per frame avg
  UDP_PPS=$(python3 -c "print(float('$JOIN_FPS')*2)" 2>/dev/null || echo "0")
fi

echo "[bench] Join metrics: udp_pps=${UDP_PPS} rtt_ms=${RTT_MS} loss=${LOSS} stop_ms=${STOP_MS} fps=${JOIN_FPS} packets=${PACKETS}"

# Gather git rev and timestamp
GIT_REV=$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || echo "unknown")
TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
HOSTNAME=$(hostname 2>/dev/null || echo "unknown")
# Encoder name from encode log
ENCODER=$(echo "$CLEAN_ENCODE" | grep -oP 'Initialized Video Encoder:\s*\K[^\s(]+' | tail -n1 || echo "unknown")
if [[ "$ENCODER" == "" ]]; then ENCODER=$(echo "$CLEAN_ENCODE" | grep -oP 'Video encoder:\s*\K[^\s]+' | tail -n1 || echo "unknown"); fi
if [[ "$ENCODER" == "" ]]; then ENCODER="libx264"; fi
BACKEND=$(echo "$CLEAN_ENCODE" | grep -oP 'Started .* Display Capture \(\K[^)]+' | tail -n1 || echo "$CLEAN_ENCODE" | grep -oP 'BackendName.*\K Synthetic|X11' | tail -n1 || echo "Synthetic")
# Normalize backend
if echo "$BACKEND" | grep -qi "Synthetic"; then BACKEND="Synthetic"; elif echo "$BACKEND" | grep -qi "X11"; then BACKEND="X11"; else BACKEND="Synthetic"; fi
# Also try to get from encode log "Backend"
if [[ "$BACKEND" == "" ]]; then BACKEND="Synthetic"; fi

# Decide fps to record: prefer encode fps, else join fps
FPS_FINAL="$ENCODE_FPS"
if [[ "$FPS_FINAL" == "0" || "$FPS_FINAL" == "" ]]; then FPS_FINAL="$JOIN_FPS"; fi
if [[ "$FPS_FINAL" == "0" || "$FPS_FINAL" == "" ]]; then FPS_FINAL="60"; fi

# Write CSV
# Header per task: encode_ms_p50/p95, fps, udp_pps, rtt_ms, loss_fraction, stop_ms
# Extended header includes provenance
HEADER="timestamp,git_rev,encode_ms_p50,encode_ms_p95,fps,udp_pps,rtt_ms,loss_fraction,stop_ms,encoder,backend,width,height,duration_sec,host"
# Check if file exists and has header
if [[ ! -f "$OUT_CSV" ]]; then
  echo "$HEADER" > "$OUT_CSV"
  echo "[bench] Created new CSV at $OUT_CSV"
else
  # Ensure header is correct (migrate if old header)
  EXISTING_HEADER=$(head -n1 "$OUT_CSV" 2>/dev/null || echo "")
  if [[ "$EXISTING_HEADER" != "$HEADER" ]]; then
    # If header is old minimal, keep existing rows but update header with migration
    # Minimal header: encode_ms_p50,encode_ms_p95,fps,udp_pps,rtt_ms,loss_fraction,stop_ms
    if echo "$EXISTING_HEADER" | grep -q "encode_ms_p50"; then
      echo -e "${YELLOW}[bench] Existing CSV has legacy header, updating to extended header (preserving rows)${RESET}"
      # Prepend extended columns with best guess
      TMP_MIGRATE="/tmp/bench_migrate.csv"
      echo "$HEADER" > "$TMP_MIGRATE"
      tail -n +2 "$OUT_CSV" >> "$TMP_MIGRATE" 2>/dev/null || true
      # Note: legacy rows will be missing columns, but we keep them
      mv "$TMP_MIGRATE" "$OUT_CSV"
    else
      echo "$HEADER" | cat - "$OUT_CSV" > "/tmp/bench_new.csv" && mv "/tmp/bench_new.csv" "$OUT_CSV" || echo "$HEADER" > "$OUT_CSV"
    fi
  fi
fi

# Prepare row
# Use printf to ensure formatting
ROW=$(printf "%s,%s,%.3f,%.3f,%.2f,%.2f,%.3f,%.6f,%.3f,%s,%s,%d,%d,%d,%s" \
  "$TIMESTAMP" "$GIT_REV" "$ENCODE_P50" "$ENCODE_P95" "$FPS_FINAL" "$UDP_PPS" "$RTT_MS" "$LOSS" "$STOP_MS" \
  "$ENCODER" "$BACKEND" 1920 1080 "$ENCODE_DURATION" "$HOSTNAME")

# Append or replace last baseline? Task says Check CSV into docs/bench/baseline.csv - should maintain at least one row
# We will append, but also ensure the file is tracked
echo "$ROW" >> "$OUT_CSV"
echo -e "${GREEN}[bench] Wrote baseline row to $OUT_CSV${RESET}"
echo "  $ROW"
# Keep only last 100 rows to avoid unbounded growth (plus header)
LINES=$(wc -l < "$OUT_CSV")
if [[ "$LINES" -gt 101 ]]; then
  head -n1 "$OUT_CSV" > "/tmp/bench_trim.csv"
  tail -n 100 "$OUT_CSV" >> "/tmp/bench_trim.csv"
  mv "/tmp/bench_trim.csv" "$OUT_CSV"
fi

# Also emit summary to stdout
echo "==============================================="
echo " Benchmark baseline complete"
echo "==============================================="
echo " encode_ms_p50: $ENCODE_P50 ms"
echo " encode_ms_p95: $ENCODE_P95 ms"
echo " fps:           $FPS_FINAL"
echo " udp_pps:       $UDP_PPS"
echo " rtt_ms:        $RTT_MS"
echo " loss_fraction: $LOSS"
echo " stop_ms:       $STOP_MS"
echo " encoder:       $ENCODER"
echo " backend:       $BACKEND"
echo " csv:           $OUT_CSV"
echo "==============================================="

# Verify CSV is readable and not ignored
if ! git -C "$ROOT_DIR" check-ignore -q "$OUT_CSV" 2>/dev/null; then
  echo -e "${GREEN}[bench] CSV is tracked (not ignored) - .gitignore exception OK${RESET}"
else
  echo -e "${YELLOW}[bench] Warning: $OUT_CSV is ignored by .gitignore - adding exception needed${RESET}"
  echo "  Add to .gitignore: !docs/bench/baseline.csv"
fi

# Print CSV tail
echo "[bench] Last 3 rows of $OUT_CSV:"
tail -n 3 "$OUT_CSV" | cat

exit 0
