<#
.SYNOPSIS
  Checks that publish\ is fit to ship before it goes into the installer or zip.

.DESCRIPTION
  Catches the mistakes that only show up on a user's machine: a missing engine DLL,
  a missing runtime dependency, debug symbols in the download, and missing licence
  files. The GPL notice is required because castcore.dll links libx264.
#>
[CmdletBinding()]
param([string]$PublishDir = '')

$ErrorActionPreference = 'Stop'
if (-not $PublishDir) { $PublishDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'publish' }
$problems = @()

foreach ($required in 'CastMirror.exe', 'castcore.dll', 'LICENSE', 'NOTICE', 'THIRD-PARTY-LICENSES.txt') {
    if (-not (Test-Path (Join-Path $PublishDir $required))) { $problems += "missing $required" }
}

$symbols = Get-ChildItem $PublishDir -Recurse -Filter *.pdb -ErrorAction SilentlyContinue
if ($symbols) { $problems += "debug symbols in the package: $($symbols.Count) .pdb file(s)" }

$localeDirs = Get-ChildItem $PublishDir -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^[a-z]{2,3}(-[A-Za-z0-9]+)+$' -and $_.Name -ne 'en-US' }
if ($localeDirs.Count -gt 0) { $problems += "$($localeDirs.Count) unused locale folders shipped; package.ps1 should prune them" }

if ((Test-Path (Join-Path $PublishDir 'THIRD-PARTY-LICENSES.txt')) -and
    ((Get-ChildItem $PublishDir -Filter 'libx264*.dll').Count -gt 0)) {
    $notice = Get-Content (Join-Path $PublishDir 'THIRD-PARTY-LICENSES.txt') -Raw
    if ($notice -notmatch 'GPL') { $problems += 'libx264 is bundled but THIRD-PARTY-LICENSES.txt has no GPL notice' }
}

$sizeMb = [math]::Round(((Get-ChildItem $PublishDir -Recurse -File | Measure-Object Length -Sum).Sum) / 1MB, 1)
Write-Host "Package size: $sizeMb MB"
# Most of the weight is FFmpeg's codec dependencies and the self-contained .NET
# runtime. The cap catches an accidental jump (a stray build tree, a second copy).
$maxMb = 400
if ($sizeMb -gt $maxMb) { $problems += "package is $sizeMb MB, over the $maxMb MB budget" }

if ($problems) {
    $problems | ForEach-Object { Write-Error $_ -ErrorAction Continue }
    exit 1
}
Write-Host 'Package checks passed.' -ForegroundColor Green
