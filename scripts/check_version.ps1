<#
.SYNOPSIS
  Fails when the version numbers that must agree do not.

.DESCRIPTION
  The version lives in CMakeLists.txt (project VERSION) and app\winui\CastMirrorApp.csproj
  (<Version>). A release tag must match both. Run with -Tag windows-v1.2.3 (or linux-v1.2.3) in the release
  workflow, or without it to check that the two files agree with each other.
#>
[CmdletBinding()]
param([string]$Tag = '')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(\s*CastMirror\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Could not read project(CastMirror VERSION ...) from CMakeLists.txt.'
}
$cmakeVersion = $Matches[1]

$csproj = Get-Content (Join-Path $root 'app\winui\CastMirrorApp.csproj') -Raw
if ($csproj -notmatch '<Version>\s*([0-9]+\.[0-9]+\.[0-9]+)\s*</Version>') {
    throw 'Could not read <Version> from CastMirrorApp.csproj.'
}
$csprojVersion = $Matches[1]

$problems = @()
if ($cmakeVersion -ne $csprojVersion) {
    $problems += "CMakeLists.txt says $cmakeVersion but CastMirrorApp.csproj says $csprojVersion."
}
if ($Tag) {
    # Windows and Linux release separately: windows-v1.0.0, linux-v1.0.0 (plain v1.0.0 also works).
    $tagVersion = $Tag -replace '^(windows-|linux-)?[vV]', ''
    if ($tagVersion -ne $cmakeVersion) {
        $problems += "Tag $Tag does not match version $cmakeVersion. Bump both files before tagging."
    }
}

if ($problems) {
    $problems | ForEach-Object { Write-Error $_ -ErrorAction Continue }
    exit 1
}
Write-Host "Version $cmakeVersion is consistent." -ForegroundColor Green
