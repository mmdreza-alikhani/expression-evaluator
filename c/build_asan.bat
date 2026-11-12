@echo off
rem Build the C evaluator with AddressSanitizer and run the shared test cases.
rem This catches buffer overflows, use-after-free and similar memory bugs.
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
cl /nologo /std:c11 /Od /Zi /W4 /fsanitize=address /Fe:..\bin\evaluator-asan.exe evaluator.c
if errorlevel 1 exit /b 1

set "ASAN_OPTIONS=detect_leaks=0"
..\bin\evaluator-asan.exe --test ..\tests\cases.txt
exit /b %errorlevel%
