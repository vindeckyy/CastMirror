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

$visited = [System.Collections.Generic.HashSet[string]]::new()
$toCheck = [System.Collections.Generic.Queue[string]]::new()
$toCheck.Enqueue($castCoreDll)

while ($toCheck.Count -gt 0) {
    $current = $toCheck.Dequeue()
    $name = [System.IO.Path]::GetFileName($current)
    if ($visited.Contains($name)) { continue }
    $visited.Add($name) | Out-Null

    $dump = & (Join-Path $binDir "objdump.exe") -p $current | Select-String 'DLL Name:'
    foreach ($line in $dump) {
        if ($line -match 'DLL Name:\s*(.*)') {
            $dll = $matches[1].Trim()
            $dllPath = Join-Path $binDir $dll
            if (Test-Path $dllPath) {
                if (-not $visited.Contains($dll)) {
                    $toCheck.Enqueue($dllPath)
                    $destPath = Join-Path $publishDir $dll
                    if (-not (Test-Path $destPath)) {
                        Copy-Item $dllPath -Destination $destPath -Force
                    }
                }
            }
        }
    }
}

Write-Host "Copied $($visited.Count) native runtime libraries." -ForegroundColor Green

# Smoke check: the publish dir must contain the exe plus a castcore.dll that
# exports the ABI-version entrypoint CastCoreBridge requires. A stale or
# foreign DLL (e.g. copied from the wrong build dir) fails loudly here instead
# of shipping a binary the managed side refuses to talk to.
$exe = Join-Path $publishDir "CastMirror.exe"
$shippedDll = Join-Path $publishDir "castcore.dll"
if (-not (Test-Path $exe)) { throw "publish output missing CastMirror.exe" }
if (-not (Test-Path $shippedDll)) { throw "publish output missing castcore.dll" }
$exports = & (Join-Path $binDir "objdump.exe") -p $shippedDll | Select-String 'castmirror_abi_version'
if (-not $exports) { throw "publish\castcore.dll does not export castmirror_abi_version - wrong or stale DLL" }
Write-Host "Smoke check passed: CastMirror.exe + castcore.dll (ABI export present)." -ForegroundColor Green
Write-Host "`n========================================================" -ForegroundColor Green
Write-Host "Standalone CastMirror.exe is ready at:" -ForegroundColor Green
Write-Host (Join-Path $publishDir "CastMirror.exe") -ForegroundColor White
Write-Host "========================================================" -ForegroundColor Green
