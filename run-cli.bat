@echo off
setlocal
if not defined CASTMIRROR_MSYS2_BIN set "CASTMIRROR_MSYS2_BIN=C:\msys64\ucrt64\bin"
set "PATH=%CASTMIRROR_MSYS2_BIN%;%PATH%"
if exist "%~dp0build\app\castmirror.exe" (
    "%~dp0build\app\castmirror.exe" %*
) else (
    echo Error: castmirror.exe not found in build\app. Please run build-all.bat first.
    pause
)
endlocal
