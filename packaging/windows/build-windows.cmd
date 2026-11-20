@echo off
setlocal
if "%MSYS2_ROOT%"=="" set "MSYS2_ROOT=C:\msys64"
if not exist "%MSYS2_ROOT%\usr\bin\bash.exe" (
    echo MSYS2 was not found in %MSYS2_ROOT%. Install it from https://www.msys2.org/ or set MSYS2_ROOT.
    exit /b 1
)
set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
cd /d "%~dp0..\.."
"%MSYS2_ROOT%\usr\bin\bash.exe" -lc "bash packaging/windows/build-windows.sh --install-deps"
set "RESULT=%ERRORLEVEL%"
if not defined NO_PAUSE pause
exit /b %RESULT%
