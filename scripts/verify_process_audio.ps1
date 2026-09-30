<#
.SYNOPSIS
  Proves per-app audio works: captures one app's sound and nothing from another.

.DESCRIPTION
  Starts two throwaway PowerShell processes, one looping a 440 Hz tone and one that is
  silent, then runs the native test that captures each through WASAPI process loopback.
  The playing process must be audible and the silent one must not be. It plays a quiet
  tone on your default speakers for a few seconds.

  Needs Windows 10 version 2004 or later and an audio output device. Build first
  (build-all.bat).
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$tests = Join-Path $root 'build\tests\castmirror_tests.exe'
if (-not (Test-Path $tests)) { throw "castmirror_tests.exe not found at '$tests'. Build first." }

# 2 seconds of 440 Hz, 16-bit mono, at roughly a tenth of full volume.
$wav = Join-Path $env:TEMP 'castmirror-tone.wav'
$rate = 22050; $samples = $rate * 2
$pcm = New-Object byte[] ($samples * 2)
for ($i = 0; $i -lt $samples; $i++) {
    $v = [int](3000 * [math]::Sin(2 * [math]::PI * 440 * $i / $rate))
    [BitConverter]::GetBytes([int16]$v).CopyTo($pcm, $i * 2)
}
$header = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($header)
$w.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $w.Write([int](36 + $pcm.Length)); $w.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
$w.Write([int]16); $w.Write([int16]1); $w.Write([int16]1); $w.Write([int]$rate); $w.Write([int]($rate * 2)); $w.Write([int16]2); $w.Write([int16]16)
$w.Write([Text.Encoding]::ASCII.GetBytes('data')); $w.Write([int]$pcm.Length); $w.Write($pcm)
[IO.File]::WriteAllBytes($wav, $header.ToArray())

$player = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList '-NoProfile', '-Command',
    "`$p = New-Object Media.SoundPlayer '$wav'; for (`$n = 0; `$n -lt 6; `$n++) { `$p.PlaySync() }"
$silent = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList '-NoProfile', '-Command', 'Start-Sleep 30'
try {
    Start-Sleep -Milliseconds 1500   # let the tone start and its audio session appear
    $env:CASTMIRROR_TEST_AUDIO_PID_LOUD = "$($player.Id)"
    $env:CASTMIRROR_TEST_AUDIO_PID_QUIET = "$($silent.Id)"
    & $tests --gtest_filter='AudioSessionsTest.*'
    exit $LASTEXITCODE
}
finally {
    Stop-Process -Id $player.Id, $silent.Id -Force -ErrorAction SilentlyContinue
    Remove-Item $wav -Force -ErrorAction SilentlyContinue
}
