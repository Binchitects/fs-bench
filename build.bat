@echo off
rem Builds fsbench on Windows. Run from a "Developer Command Prompt for VS"
rem (or any shell where cl.exe / cmake is on PATH).
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if %errorlevel%==0 (
    cmake -S . -B build
    if errorlevel 1 exit /b 1
    cmake --build build --config Release
    if errorlevel 1 exit /b 1
    echo built: build\Release\fsbench.exe
    exit /b 0
)

where cl >nul 2>nul
if %errorlevel%==0 (
    if not exist build mkdir build
    cl /nologo /std:c++20 /O2 /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /Isrc ^
       src\*.cpp src\tests\*.cpp src\bench\*.cpp ^
       /Fobuild\ /Fe:build\fsbench.exe advapi32.lib
    if errorlevel 1 exit /b 1
    echo built: build\fsbench.exe
    exit /b 0
)

echo Neither cmake nor cl.exe found. Open a "Developer Command Prompt for VS" and retry.
exit /b 1
