@echo off
setlocal
rem Toolchain locations. Override with CASTMIRROR_MSYS2_BIN / CASTMIRROR_DOTNET_ROOT.
if not defined CASTMIRROR_DOTNET_ROOT set "CASTMIRROR_DOTNET_ROOT=%LOCALAPPDATA%\Microsoft\dotnet"
if not defined CASTMIRROR_MSYS2_BIN set "CASTMIRROR_MSYS2_BIN=C:\msys64\ucrt64\bin"
set "DOTNET_ROOT=%CASTMIRROR_DOTNET_ROOT%"
set "PATH=%CASTMIRROR_MSYS2_BIN%;%CASTMIRROR_DOTNET_ROOT%;%PATH%"

echo ========================================================
echo Building CastMirror Native C++ Core and Tools (Ninja)...
echo ========================================================
ninja -C "%~dp0build"
if %ERRORLEVEL% neq 0 (
    echo Native build failed!
    exit /b %ERRORLEVEL%
)

echo.
echo ========================================================
echo Building CastMirror WinUI 3 Application (.NET 8)...
echo ========================================================
dotnet build "%~dp0app\winui\CastMirrorApp.csproj" -p:Platform=x64 -r win-x64 --self-contained true
if %ERRORLEVEL% neq 0 (
    echo WinUI 3 build failed!
    exit /b %ERRORLEVEL%
)

echo.
echo ========================================================
echo Build Complete!
echo Run 'run-cli.bat' for the command-line interface.
echo Run 'run-gui.bat' for the WinUI 3 graphical application.
echo ========================================================
endlocal
