<#
.SYNOPSIS
  Builds the native test suite with clang's UndefinedBehaviorSanitizer and runs it.

.DESCRIPTION
  The MinGW toolchain has no AddressSanitizer or ThreadSanitizer runtime, and
  libFuzzer does not link on it. UBSan in trap mode needs no runtime, so it works: any
  signed overflow, bad shift, misaligned or null access, or out-of-bounds array index
  stops the test with a trap instead of passing silently.

  Needs clang from MSYS2:  pacman -S mingw-w64-ucrt-x86_64-clang
#>
[CmdletBinding()]
param(
    [string]$Msys2Bin = $(if ($env:CASTMIRROR_MSYS2_BIN) { $env:CASTMIRROR_MSYS2_BIN } else { 'C:\msys64\ucrt64\bin' }),
    [string]$Filter = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$clang = Join-Path $Msys2Bin 'clang++.exe'
if (-not (Test-Path $clang)) { throw "clang++ not found at '$clang'. Install it with: pacman -S mingw-w64-ucrt-x86_64-clang" }

$env:PATH = "$Msys2Bin;$env:PATH"
$build = Join-Path $root 'build-ubsan'
$flags = '-fsanitize=undefined -fsanitize-trap=undefined -fno-omit-frame-pointer -g -O1'

& cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    "-DCMAKE_C_COMPILER=$($Msys2Bin -replace '\\','/')/clang.exe" `
    "-DCMAKE_CXX_COMPILER=$($Msys2Bin -replace '\\','/')/clang++.exe" `
    "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_CXX_FLAGS=$flags" `
    -DCASTMIRROR_BUILD_GUI=OFF -DCASTMIRROR_ENABLE_TRAY=OFF
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
& ninja -C $build castmirror_tests
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$args = @('--test-dir', $build, '--timeout', '180', '--output-on-failure')
if ($Filter) { $args += @('-R', $Filter) }
& ctest @args
exit $LASTEXITCODE
