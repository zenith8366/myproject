@echo off
REM ==========================================================================
REM One-click build script (double-click to run, or execute build.bat in cmd)
REM   - Adds cmake / ninja to PATH (STM32CubeCLT bundles)
REM   - Auto-configures on first run (Ninja + Debug)
REM   - Output: build\Debug\software.hex / .bin / .elf
REM NOTE: keep this file ASCII-only; cmd mangles UTF-8 Chinese comments.
REM ==========================================================================
setlocal

set "PATH=C:\Users\lyh35\AppData\Local\stm32cube\bundles\cmake\4.3.1+st.1\bin;C:\Users\lyh35\AppData\Local\stm32cube\bundles\ninja\1.13.2+st.1\bin;%PATH%"

cd /d "%~dp0"

REM First run (or build dir deleted): configure
if not exist "build\Debug\CMakeCache.txt" (
    echo ==^> First build, configuring...
    cmake -S . -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
    if errorlevel 1 goto :fail
)

cmake --build build/Debug
if errorlevel 1 goto :fail

echo.
echo ==^> Build complete: %~dp0build\Debug\software.hex
pause
exit /b 0

:fail
echo.
echo ==^> BUILD FAILED - check the errors above
pause
exit /b 1
