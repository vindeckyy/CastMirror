@echo off
setlocal
if not defined CASTMIRROR_MSYS2_BIN set "CASTMIRROR_MSYS2_BIN=C:\msys64\ucrt64\bin"
set "PATH=%CASTMIRROR_MSYS2_BIN%;%PATH%"
echo Running CastMirror Test Suite...
"%~dp0build\tests\castmirror_tests.exe" %*
endlocal
