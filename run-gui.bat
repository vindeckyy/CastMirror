@echo off
setlocal
if exist "%~dp0publish\CastMirror.exe" (
    start "" /D "%~dp0publish" "%~dp0publish\CastMirror.exe"
    exit /b 0
)
if not defined CASTMIRROR_DOTNET_ROOT set "CASTMIRROR_DOTNET_ROOT=%LOCALAPPDATA%\Microsoft\dotnet"
if not defined CASTMIRROR_MSYS2_BIN set "CASTMIRROR_MSYS2_BIN=C:\msys64\ucrt64\bin"
set "DOTNET_ROOT=%CASTMIRROR_DOTNET_ROOT%"
set "PATH=%CASTMIRROR_MSYS2_BIN%;%CASTMIRROR_DOTNET_ROOT%;%PATH%"
rem Resolve the Debug output dir from the csproj TargetFramework rather than a
rem hardcoded RID-shaped path.
set "EXE="
for /f "delims=" %%f in ('dir /b /s "%~dp0app\winui\bin\x64\Debug\CastMirror.exe" 2^>nul') do set "EXE=%%f"
if exist "%EXE%" (
    start "" "%EXE%"
) else (
    echo Error: CastMirror.exe not found. Please run package.ps1 or build-all.bat first.
    pause
)
endlocal
