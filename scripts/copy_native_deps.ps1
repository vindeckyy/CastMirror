<#
.SYNOPSIS
  Copies castcore.dll's dependency closure next to the app.

.DESCRIPTION
  castcore.dll is linked against FFmpeg, OpenSSL, protobuf and the MinGW runtime.
  All of those live in the MSYS2 toolchain, and all of them are hard imports: the
  Windows loader resolves the entire closure before the app's managed entry point
  runs, so a missing member surfaces as DllNotFoundException on the very first
  native call and the app comes up with no sources and no devices.

  run-gui.bat used to hide this by putting the toolchain on PATH, which meant the
  shipped app only worked when launched through that batch file. A shortcut, a
  double-click, or the Start menu all bypass it. Copying the closure into the
  output directory removes the launcher from the equation.

  The import tables are read with objdump from the same toolchain that built
  castcore, so this stays in step with whatever the linker actually recorded.
  Anything the loader would find in System32 is left alone; everything else is
  copied.

.PARAMETER Destination
  Directory to copy into, typically the app's output or publish directory.

.PARAMETER CoreDll
  Path to castcore.dll. Defaults to build\core\castcore.dll.

.PARAMETER ToolchainBin
  MSYS2 UCRT64 bin directory the native libraries come from.


.OUTPUTS
  Exit code 0 when the closure is in place (including the "castcore.dll is not
  built yet" case, which is not an error). Exit code 2 when the imports cannot be
  read at all - no objdump, or a closure that came back empty - because that is
  indistinguishable from success and must not be allowed to pass silently.
.EXAMPLE
  scripts\copy_native_deps.ps1 -Destination app\winui\bin\x64\Debug
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Destination,
    # Resolved in the body, not here: $PSScriptRoot is not yet populated when a
    # param default is evaluated under `powershell -File`.
    [string]$CoreDll = '',
    [string]$ToolchainBin = $(if ($env:CASTMIRROR_MSYS2_BIN) { $env:CASTMIRROR_MSYS2_BIN } else { 'C:\msys64\ucrt64\bin' })
)

$ErrorActionPreference = 'Stop'
# Resolved in the body, not in the param default: $PSScriptRoot is not yet
# populated when a default is evaluated under `powershell -File`.
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $CoreDll) { $CoreDll = Join-Path $repoRoot 'build\core\castcore.dll' }
$objdump = Join-Path $ToolchainBin 'objdump.exe'
$system32 = Join-Path $env:SystemRoot 'System32'
# objdump is itself a UCRT64 binary and dies silently when its own toolchain is
# absent from PATH, which would look exactly like castcore having no imports.
# Put the toolchain first for the duration of this script.
$env:PATH = "$ToolchainBin;$env:PATH"
$system32 = Join-Path $env:SystemRoot 'System32'

if (-not (Test-Path $CoreDll)) {
    Write-Verbose "castcore.dll not built yet at $CoreDll; nothing to resolve."
    return
}
if (-not (Test-Path $objdump)) {
    # We cannot see the imports, and guessing is worse than failing: packaging
    # would otherwise ship a publish dir with no closure, which is the exact
    # regression this script exists to prevent.
    # $ErrorActionPreference is Stop, so Write-Error would terminate with code 1
    # before reaching the exit and the documented code would never be seen.
    [Console]::Error.WriteLine("copy_native_deps: objdump.exe not found under $ToolchainBin; cannot resolve castcore's DLL closure. Set CASTMIRROR_MSYS2_BIN to the MSYS2 UCRT64 bin directory.")
    exit 2
}

function Get-ImportedDlls([string]$Path) {
    # objdump chatters on stderr even on success, and $ErrorActionPreference='Stop'
    # would turn that into a terminating error. Relax it just for the call, and
    # keep stderr out of the stream the regex runs against.
    $strict = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $out = & $objdump -p $Path 2>&1
    $ErrorActionPreference = $strict
    if ($LASTEXITCODE -ne 0) { return @() }
    return [regex]::Matches(($out -join "`n"), 'DLL Name:\s*(\S+)') |
        ForEach-Object { $_.Groups[1].Value }
}

# Walks imports breadth-first. A name already seen is never re-resolved, so
# diamond dependencies and cycles terminate without extra bookkeeping.
$closure = @{}
$seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$null = $seen.Add([System.IO.Path]::GetFileName($CoreDll))
$queue = [System.Collections.Generic.Queue[string]]::new()
$queue.Enqueue($CoreDll)

while ($queue.Count -gt 0) {
    $current = $queue.Dequeue()
    $currentDir = Split-Path -Parent $current
    foreach ($dep in Get-ImportedDlls $current) {
        if (-not $seen.Add($dep)) { continue }
        # Probe order mirrors the loader: the importing module's own directory
        # first, then the toolchain, then System32.
        # @() wraps the pipeline as well: with a single survivor the result would
        # be a bare string, and $candidates[0] would then yield its first
        # character ("C") instead of the path.
        $candidates = @(@(
            (Join-Path $currentDir $dep),
            (Join-Path $ToolchainBin $dep),
            (Join-Path $system32 $dep)
        ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
        if ($candidates.Count -eq 0) { continue }
        $resolved = $candidates[0]
        $inToolchain = Test-Path -LiteralPath (Join-Path $ToolchainBin $dep) -PathType Leaf
        # A DLL that lives in System32 and not in the toolchain is a Windows
        # component; the loader already has it and copying it would be wrong.
        if ((Split-Path -Parent $resolved) -ieq $system32 -and -not $inToolchain) { continue }
        $closure[$dep] = $resolved
        $queue.Enqueue($resolved)
    }
}

if ($closure.Count -eq 0) {
    # Either castcore genuinely has no external imports or objdump ran but
    # produced nothing. Both mean we cannot vouch for the output directory.
    [Console]::Error.WriteLine("copy_native_deps: resolved no native dependencies for '$CoreDll'. If it really imports none the closure is empty; otherwise objdump did not run.")
    exit 2
}

$null = New-Item -ItemType Directory -Path $Destination -Force
$copied = 0
foreach ($name in $closure.Keys) {
    $target = Join-Path $Destination $name
    $source = $closure[$name]
    # Skip an identical copy so an incremental build does not rewrite 200 MB.
    if ((Test-Path -LiteralPath $target -PathType Leaf) -and
        (Get-Item -LiteralPath $target).Length -eq (Get-Item -LiteralPath $source).Length -and
        (Get-Item -LiteralPath $target).LastWriteTimeUtc -ge (Get-Item -LiteralPath $source).LastWriteTimeUtc) {
        continue
    }
    try {
        Copy-Item -LiteralPath $source -Destination $target -Force
    } catch {
        # Name the pair: a bare Copy-Item error only says "item not found", and
        # the interesting question is which dependency and where it was looked for.
        throw "Failed to copy native dependency '$name' from '$source' to '$target': $($_.Exception.Message)"
    }
    $copied++
}

$bytes = (Get-ChildItem -LiteralPath $Destination -Filter *.dll -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("Native deps: {0} DLLs ({1:N0} MB total) in {2}; {3} copied this run." -f `
    $closure.Count, ($bytes / 1MB), $Destination, $copied)

exit 0