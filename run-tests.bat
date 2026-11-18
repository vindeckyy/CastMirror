@echo off
setlocal
if not defined CASTMIRROR_MSYS2_BIN set "CASTMIRROR_MSYS2_BIN=C:\msys64\ucrt64\bin"
set "PATH=%CASTMIRROR_MSYS2_BIN%;%PATH%"
if exist "%~dp0build\tests\castmirror_tests.exe" (
    echo Running CastMirror Test Suite...
    "%~dp0build\tests\castmirror_tests.exe" %*
    set "RC=%ERRORLEVEL%"
) else (
    echo Error: castmirror_tests.exe not found in build\tests. Please run build-all.bat first.
    pause
    set "RC=1"
)
endlocal & exit /b %RC%
