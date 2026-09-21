@echo off
setlocal
cd /d "%~dp0"

set "ADVMATCH_PRESET=%~1"
if "%ADVMATCH_PRESET%"=="" set "ADVMATCH_PRESET=release"
if /I "%ADVMATCH_PRESET%"=="debug" set "ADVMATCH_PRESET=debug"
if /I "%ADVMATCH_PRESET%"=="release" set "ADVMATCH_PRESET=release"

if /I not "%ADVMATCH_PRESET%"=="debug" if /I not "%ADVMATCH_PRESET%"=="release" (
    echo Usage: build.cmd [debug^|release]
    exit /b 2
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo CMake 3.20 or newer is required and must be available on PATH.
    exit /b 1
)

where git >nul 2>nul
if errorlevel 1 (
    echo Git is required to download the pinned dependencies and must be available on PATH.
    exit /b 1
)

where ninja >nul 2>nul
if errorlevel 1 (
    echo Ninja is required by the checked-in CMake presets and must be available on PATH.
    exit /b 1
)

cmake --preset "%ADVMATCH_PRESET%" || exit /b 1
cmake --build --preset "%ADVMATCH_PRESET%" || exit /b 1
ctest --preset "%ADVMATCH_PRESET%" || exit /b 1

if "%ADVMATCH_PRESET%"=="release" (
    cmake --build --preset release --target package || exit /b 1
)
