# CastMirror Windows Firewall Setup Script
# Run as Administrator in PowerShell

Write-Host "=================================================" -ForegroundColor Cyan
Write-Host "   CastMirror Windows Firewall Configuration    " -ForegroundColor Cyan
Write-Host "=================================================" -ForegroundColor Cyan

# 1. Outbound Cast V2 TLS Control (Port 8009)
New-NetFirewallRule -DisplayName "CastMirror Outbound Cast TLS (8009)" `
    -Direction Outbound -Protocol TCP -RemotePort 8009 -Action Allow `
    -Profile Private -Description "Allows CastMirror to connect to Google Cast devices on port 8009."

# 2. Inbound/Outbound mDNS (Port 5353)
New-NetFirewallRule -DisplayName "CastMirror mDNS Discovery (5353)" `
    -Direction Inbound -Protocol UDP -LocalPort 5353 -Action Allow `
    -Profile Private -Description "Allows CastMirror to discover Cast devices via mDNS."

# 3. Inbound/Outbound UDP Media Transport (Ports 30000-65535)
New-NetFirewallRule -DisplayName "CastMirror UDP Media Transport" `
    -Direction Outbound -Protocol UDP -RemotePort 30000-65535 -Action Allow `
    -Profile Private -Description "Allows CastMirror to stream low-latency RTP/RTCP media packets."

Write-Host "[+] CastMirror Windows Firewall rules added successfully." -ForegroundColor Green
