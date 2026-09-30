<#
.SYNOPSIS
  Streams to the simulated receiver for a while and checks memory and handles level off.

.DESCRIPTION
  Runs the CastE2ETest.SoakStreamDoesNotLeakMemoryOrHandles test for -Minutes. It
  captures the real desktop, encodes, encrypts and sends to the in-process test
  receiver, sampling the working set and handle count every 10 seconds. It fails if the
  stream drops, frames stop, or either number keeps climbing after a 2-minute warm-up.

  Ten minutes is a useful smoke soak. For a release, run it for 8 hours (-Minutes 480)
  on the hardware you ship for. Build first (build-all.bat).
#>
[CmdletBinding()]
param([int]$Minutes = 10)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$tests = Join-Path $root 'build\tests\castmirror_tests.exe'
if (-not (Test-Path $tests)) { throw "castmirror_tests.exe not found at '$tests'. Build first." }

$env:CASTMIRROR_SOAK_MINUTES = "$Minutes"
# The test reports on stderr. Windows PowerShell 5.1 turns native stderr into errors, so
# run it through cmd, which merges the streams into plain text first.
$ErrorActionPreference = 'Continue'
cmd /c "`"$tests`" --gtest_filter=CastE2ETest.SoakStreamDoesNotLeakMemoryOrHandles 2>&1" |
    Select-String -Pattern '\[soak\]|\[  (PASSED|FAILED)|Failure|Expected|Which is'
exit $LASTEXITCODE
