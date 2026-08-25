#!/usr/bin/env bash
# Network simulation script using Linux Traffic Control (tc / netem)
# Simulates Wi-Fi packet loss, latency jitter, and reordering to test CastMirror's Adaptive Controller

set -euo pipefail

INTERFACE="${1:-lo}"
ACTION="${2:-start}"

if [ "$ACTION" = "start" ]; then
    echo "[*] Adding network impairment on interface $INTERFACE (2% loss, 30ms jitter, 25ms delay, 1% reorder)..."
    sudo tc qdisc add dev "$INTERFACE" root netem delay 25ms 15ms loss 2% reorder 1% 25% || \
    sudo tc qdisc change dev "$INTERFACE" root netem delay 25ms 15ms loss 2% reorder 1% 25%
    echo "[+] Network simulation active."
elif [ "$ACTION" = "stop" ]; then
    echo "[*] Restoring network on interface $INTERFACE..."
    sudo tc qdisc del dev "$INTERFACE" root || true
    echo "[+] Network restored to normal."
else
    echo "Usage: $0 [interface] [start|stop]"
    echo "Example: $0 lo start"
    exit 1
fi
