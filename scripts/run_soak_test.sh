#!/usr/bin/env bash
# Soak test script for CastMirror: runs live loopback casting and monitors stability

set -euo pipefail

DURATION="${1:-120}" # Duration in seconds (default 120s)
TLS_PORT=29009
UDP_PORT=54533

echo "==============================================="
echo "   CastMirror Soak Test (${DURATION}s run)     "
echo "==============================================="

# 1. Start fake receiver in background
echo "[*] Launching Fake Cast Receiver on TLS :$TLS_PORT and UDP :$UDP_PORT..."
./build/tools/fake-receiver "$TLS_PORT" "$UDP_PORT" &
RECEIVER_PID=$!

cleanup() {
    echo "[*] Cleaning up background processes..."
    kill -9 "$RECEIVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

sleep 1

# 2. Run End-to-End join tool with specified duration
echo "[*] Starting live capture & streaming session..."
timeout "$((DURATION + 10))" ./build/tools/poc-join 127.0.0.1 "$TLS_PORT" "$DURATION"
echo "[+] Soak test finished successfully!"
