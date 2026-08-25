#!/usr/bin/env bash
# CastMirror Linux Firewall Setup Script
# Opens necessary ports for Google Cast V2 Control and UDP Cast Streaming

set -euo pipefail

echo "==============================================="
echo "   CastMirror Firewall Configuration (Linux)   "
echo "==============================================="

if command -v ufw >/dev/null 2>&1; then
    echo "[*] Configuring UFW rules..."
    # Outbound Cast TLS control plane
    sudo ufw allow out 8009/tcp comment "CastMirror Cast V2 TLS Control"
    # Inbound/Outbound mDNS discovery
    sudo ufw allow 5353/udp comment "CastMirror mDNS Discovery"
    # Ephemeral Cast Streaming UDP ports
    sudo ufw allow 30000:60000/udp comment "CastMirror UDP Media Transport"
    echo "[+] UFW rules added successfully."
elif command -v iptables >/dev/null 2>&1; then
    echo "[*] Configuring iptables rules..."
    sudo iptables -A OUTPUT -p tcp --dport 8009 -j ACCEPT
    sudo iptables -A INPUT -p udp --dport 5353 -j ACCEPT
    sudo iptables -A INPUT -p udp --dport 30000:60000 -j ACCEPT
    sudo iptables -A OUTPUT -p udp --dport 30000:60000 -j ACCEPT
    echo "[+] iptables rules added successfully."
else
    echo "[!] Neither ufw nor iptables found. Please ensure port 8009/TCP and UDP media ports are open."
fi

echo "[+] Firewall configuration complete."
