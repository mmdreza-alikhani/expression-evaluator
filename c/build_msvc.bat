@echo off
rem Build the C evaluator with MSVC (x64). Output: ..\bin\evaluator.exe
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo error: vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo error: no MSVC installation found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if not exist "..\bin" mkdir "..\bin"
cl /nologo /std:c11 /O2 /W4 /Fe:..\bin\evaluator.exe evaluator.c
exit /b %errorlevel%
