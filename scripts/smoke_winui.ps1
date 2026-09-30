<#
.SYNOPSIS
  Starts the real WinUI app and checks that it comes up, opens its windows, and logs no errors.

.DESCRIPTION
  Drives the running app through UI Automation: finds the main window, dismisses the
  welcome card if it is showing, opens Settings and Logs, opens and closes About, then
  checks the process is still alive and that gui-errors.log did not grow. Exits 1 on
  any failure.

  Needs an interactive desktop, so it runs on a developer machine, not on a CI runner.
  Build first (build-all.bat). Pass -Exe to test a published build instead.
#>
[CmdletBinding()]
param([string]$Exe = '')

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) {
    $Exe = Join-Path $root 'app\winui\bin\x64\Debug\net8.0-windows10.0.22621.0\win-x64\CastMirror.exe'
}
if (-not (Test-Path $Exe)) { throw "CastMirror.exe not found at '$Exe'. Build it first." }
# The debug build loads castcore.dll's MinGW dependencies from PATH; a published build carries them.
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

$failures = @()
function Check($ok, $what) {
    if ($ok) { Write-Host "  ok    $what" } else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:failures += $what }
}

$ui = [System.Windows.Automation.AutomationElement]
$scope = [System.Windows.Automation.TreeScope]
function Find-Window($title) {
    $c = New-Object System.Windows.Automation.PropertyCondition($ui::NameProperty, $title)
    $ui::RootElement.FindFirst($scope::Children, $c)
}
function Find-Button($window, $name) {
    $c = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition($ui::NameProperty, $name)),
        (New-Object System.Windows.Automation.PropertyCondition($ui::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Button)))
    $window.FindFirst($scope::Descendants, $c)
}
function Press($button) {
    $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}

Get-Process CastMirror -ErrorAction SilentlyContinue | Stop-Process -Force
$log = Join-Path $env:APPDATA 'CastMirror\gui-errors.log'
$before = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }

$process = Start-Process $Exe -PassThru
try {
    Start-Sleep -Seconds 6
    $main = Find-Window 'CastMirror'
    Check ($null -ne $main) 'main window appears'
    if ($main) {
        $notNow = Find-Button $main 'Not now'
        if ($notNow) { Press $notNow; Start-Sleep -Seconds 1 }

        foreach ($pair in @(@('Settings', 'CastMirror Settings'), @('Logs', 'CastMirror Logs'))) {
            $button = Find-Button $main $pair[0]
            Check ($null -ne $button) "$($pair[0]) button exists"
            if ($button) {
                Press $button; Start-Sleep -Seconds 3
                $window = Find-Window $pair[1]
                Check ($null -ne $window) "$($pair[1]) window opens"
                if ($window) {
                    try { $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close() } catch {}
                    Start-Sleep -Seconds 1
                }
            }
        }

        $about = Find-Button $main 'About'
        Check ($null -ne $about) 'About button exists'
        if ($about) {
            Press $about; Start-Sleep -Seconds 2
            $close = Find-Button $main 'Close'
            Check ($null -ne $close) 'About dialog opens'
            if ($close) { Press $close }
        }
    }
    Start-Sleep -Seconds 1
    Check (-not $process.HasExited) 'process is still running'

    # A second launch must wake this instance, not start another.
    $second = Start-Process $Exe -PassThru
    Start-Sleep -Seconds 4
    Check $second.HasExited 'second launch exits'
    Check (-not $process.HasExited) 'first instance survives the second launch'
}
finally {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
}

$after = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
# Only what this run appended counts. The log keeps history, and the single-instance
# wake line ("waking it") is expected.
$newText = ''
if ($after -gt $before) {
    $stream = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    try {
        [void]$stream.Seek($before, 'Begin')
        $newText = (New-Object IO.StreamReader($stream)).ReadToEnd()
    } finally { $stream.Dispose() }
}
Check (($newText -notmatch 'Exception') -and ($newText -notmatch '\[unhandled\]')) 'error log has no new exceptions'

if ($failures.Count -gt 0) { Write-Host "$($failures.Count) check(s) failed." -ForegroundColor Red; exit 1 }
Write-Host 'Smoke test passed.' -ForegroundColor Green
