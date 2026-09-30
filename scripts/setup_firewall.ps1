<#
.SYNOPSIS
  Allows CastMirror through Windows Defender Firewall, or removes the rules again.

.DESCRIPTION
  The installer does this for you. Run this script by hand only for a portable copy.
  Discovery (mDNS) and the media stream both need inbound UDP, so the rules are
  scoped to the CastMirror executable on the Private and Domain profiles, not to a
  port range, and they cover both directions. Public networks stay closed on
  purpose: on cafe or hotel Wi-Fi other devices can see the PC.

.PARAMETER Program
  Full path to CastMirror.exe. Defaults to the copy next to this script's parent folder.

.PARAMETER Remove
  Delete the rules instead of adding them.

.EXAMPLE
  # From an elevated PowerShell:
  .\setup_firewall.ps1 -Program "C:\Tools\CastMirror\CastMirror.exe"
  .\setup_firewall.ps1 -Remove
#>
[CmdletBinding()]
param(
    [string]$Program = '',
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
if (-not $Program) { $Program = Join-Path (Split-Path -Parent $PSScriptRoot) 'CastMirror.exe' }
$ruleNames = @('CastMirror (inbound)', 'CastMirror (outbound)')

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    throw 'Run this script from an elevated (Administrator) PowerShell.'
}

# Idempotent: drop any earlier copy first so running twice never stacks duplicates.
foreach ($name in $ruleNames) {
    Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
}

if ($Remove) {
    Write-Host 'CastMirror firewall rules removed.' -ForegroundColor Green
    return
}

if (-not (Test-Path $Program)) {
    throw "CastMirror.exe not found at '$Program'. Pass -Program with the full path."
}

New-NetFirewallRule -DisplayName $ruleNames[0] -Direction Inbound -Action Allow `
    -Program $Program -Profile Private, Domain `
    -Description 'Lets CastMirror find Cast devices (mDNS) and receive their feedback.' | Out-Null
New-NetFirewallRule -DisplayName $ruleNames[1] -Direction Outbound -Action Allow `
    -Program $Program -Profile Private, Domain `
    -Description 'Lets CastMirror connect to Cast devices and send the video and audio stream.' | Out-Null

Write-Host "CastMirror allowed through Windows Firewall on Private and Domain networks: $Program" -ForegroundColor Green
