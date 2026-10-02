$ErrorActionPreference = "Stop"
$rootDir = $PSScriptRoot
$publishDir = Join-Path $rootDir "publish"
# Toolchain locations. Override with CASTMIRROR_MSYS2_BIN / CASTMIRROR_DOTNET_ROOT
# instead of editing this script.
$binDir = if ($env:CASTMIRROR_MSYS2_BIN) { $env:CASTMIRROR_MSYS2_BIN } else { "C:\msys64\ucrt64\bin" }
$dotnetRoot = if ($env:CASTMIRROR_DOTNET_ROOT) { $env:CASTMIRROR_DOTNET_ROOT } else { "$env:LOCALAPPDATA\Microsoft\dotnet" }
$dotnet = Join-Path $dotnetRoot "dotnet.exe"

if (-not (Test-Path $binDir)) { throw "MSYS2 UCRT64 bin not found at '$binDir'. Set CASTMIRROR_MSYS2_BIN." }
if (-not (Test-Path $dotnet)) { throw "dotnet not found at '$dotnet'. Set CASTMIRROR_DOTNET_ROOT." }

Write-Host "========================================================" -ForegroundColor Cyan
Write-Host "1. Building the whole tree (core, CLI, tests)..." -ForegroundColor Cyan
Write-Host "========================================================" -ForegroundColor Cyan
$env:PATH = "$binDir;$env:PATH"
# The canonical dev tree lives in build\. Configure it if absent so a clean
# checkout packages the same binaries the tests ran against.
$buildDir = Join-Path $rootDir "build"
if (-not (Test-Path (Join-Path $buildDir "CMakeCache.txt"))) {
    & cmake -S $rootDir -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_C_COMPILER="$binDir\gcc.exe" -DCMAKE_CXX_COMPILER="$binDir\g++.exe"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
}
& ninja -C $buildDir
if ($LASTEXITCODE -ne 0) { throw "Ninja build failed" }

Write-Host "`n========================================================" -ForegroundColor Cyan
Write-Host "2. Running the test suite..." -ForegroundColor Cyan
Write-Host "========================================================" -ForegroundColor Cyan
# Cases that need a capturable desktop skip themselves; anything else failing
# means the DLL about to ship is broken, so package nothing in that case.
& ctest --test-dir $buildDir --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "ctest failed - refusing to package a failing tree" }

Write-Host "`n========================================================" -ForegroundColor Cyan
Write-Host "3. Publishing self-contained WinUI 3 App..." -ForegroundColor Cyan
Write-Host "========================================================" -ForegroundColor Cyan
if (Test-Path $publishDir) { Remove-Item -Recurse -Force $publishDir }

$env:DOTNET_ROOT = $dotnetRoot
& $dotnet publish (Join-Path $rootDir "app\winui\CastMirrorApp.csproj") `
    -c Release `
    -r win-x64 `
    --self-contained true `
    -p:Platform=x64 `
    -o $publishDir

if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed" }

Write-Host "`n========================================================" -ForegroundColor Cyan
Write-Host "4. Bundling native DLLs into publish directory..." -ForegroundColor Cyan
Write-Host "========================================================" -ForegroundColor Cyan
$castCoreDll = Join-Path $buildDir "core\castcore.dll"
Copy-Item $castCoreDll -Destination $publishDir -Force

# castcore.dll's DLL closure has to travel with it: FFmpeg, OpenSSL, protobuf
# and the MinGW runtime are hard imports, so a missing member surfaces as
# DllNotFoundException on the first native call rather than at startup. The walk
# lives in one script because the Debug output directory needs the same closure
# (see CopyCastCoreDependencies in CastMirrorApp.csproj).
& (Join-Path $rootDir "scripts\copy_native_deps.ps1") -Destination $publishDir -ToolchainBin $binDir -CoreDll $castCoreDll
if ($LASTEXITCODE -ne 0) { throw "failed to copy castcore's native dependencies into the publish dir" }

# Licence texts travel with the binaries. libx264 is GPL, so shipping the DLLs
# without THIRD-PARTY-LICENSES.txt (which carries the GPL notice and the source
# offer) would be a compliance failure, not just untidiness.
foreach ($doc in 'LICENSE', 'NOTICE', 'THIRD-PARTY-LICENSES.txt') {
    Copy-Item (Join-Path $rootDir $doc) -Destination $publishDir -Force
}
# Symbols are for crash analysis, not for the download.
Get-ChildItem $publishDir -Recurse -Filter *.pdb | Remove-Item -Force
# The Windows App SDK ships ~85 per-language resource folders. The app is English
# only for now and Windows falls back to en-US, so the rest is clutter in the install.
Get-ChildItem $publishDir -Directory |
    Where-Object { $_.Name -match '^[a-z]{2,3}(-[A-Za-z0-9]+)+$' -and $_.Name -ne 'en-US' } |
    Remove-Item -Recurse -Force

# Smoke check: the publish dir must contain the exe plus a castcore.dll that
# exports the ABI-version entrypoint CastCoreBridge requires. A stale or
# foreign DLL (e.g. copied from the wrong build dir) fails loudly here instead
# of shipping a binary the managed side refuses to talk to.
$exe = Join-Path $publishDir "CastMirror.exe"
$shippedDll = Join-Path $publishDir "castcore.dll"
if (-not (Test-Path $exe)) { throw "publish output missing CastMirror.exe" }
if (-not (Test-Path $shippedDll)) { throw "publish output missing castcore.dll" }
$exports = & (Join-Path $binDir "objdump.exe") -p $shippedDll 2>&1 | Select-String 'castmirror_abi_version'
if (-not $exports) { throw "publish\castcore.dll does not export castmirror_abi_version - wrong or stale DLL" }

# Presence is not loadability, but the closure copied above is what makes it
# loadable: every non-system DLL castcore imports is copied in, recursively, and
# Windows components are left to the loader. check_windows_package.ps1
# re-checks the shipped layout from the outside.
Write-Host "Smoke check passed: CastMirror.exe + castcore.dll (ABI export present)." -ForegroundColor Green

& (Join-Path $rootDir "scripts\check_windows_package.ps1") -PublishDir $publishDir
if ($LASTEXITCODE -ne 0) { throw "package checks failed" }
Write-Host "`n========================================================" -ForegroundColor Green
Write-Host "Standalone CastMirror.exe is ready at:" -ForegroundColor Green
Write-Host (Join-Path $publishDir "CastMirror.exe") -ForegroundColor White
Write-Host "========================================================" -ForegroundColor Green
