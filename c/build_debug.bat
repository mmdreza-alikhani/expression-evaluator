@echo off
rem Build the C evaluator against the debug CRT, which prints a leak report at
rem exit if any allocation was not freed.
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo error: no MSVC installation found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if not exist "..\bin" mkdir "..\bin"
cl /nologo /std:c11 /Od /Zi /W4 /MDd /D_DEBUG /Fe:..\bin\evaluator-debug.exe evaluator.c
if errorlevel 1 exit /b 1

..\bin\evaluator-debug.exe --test ..\tests\cases.txt >nul
echo.
echo --- leak check (expect "no leaks" / no report below) ---
..\bin\evaluator-debug.exe "3 + 4 * (2 - 1)"
exit /b %errorlevel%
