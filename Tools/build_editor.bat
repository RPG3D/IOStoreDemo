@echo off
setlocal

:: ============================================
:: Build UE Editor for IOStoreDemo
:: ============================================

:: Detect project
set "PROJECT_DIR=%~dp0.."
for %%I in ("%PROJECT_DIR%") do set "PROJECT_DIR=%%~fI"
for %%F in ("%PROJECT_DIR%\*.uproject") do set "PROJECT_NAME=%%~nF"
set "PROJECT_FILE=%PROJECT_DIR%\%PROJECT_NAME%.uproject"

:: Detect UE 5.7 engine
set "FINDUE5_SCRIPT=%PROJECT_DIR%\Tools\FindUE5.ps1"
for /f "delims=" %%E in ('powershell -NoProfile -ExecutionPolicy Bypass -File "%FINDUE5_SCRIPT%" -FindUE57 -EnginePath 2^>nul') do set "ENGINE_DIR=%%E"

if not defined ENGINE_DIR (
    echo [ERROR] UE 5.7 engine not found
    exit /b 1
)

echo Engine: %ENGINE_DIR%
echo Project: %PROJECT_FILE%
echo.

:: Build
call "%ENGINE_DIR%\Build\BatchFiles\Build.bat" %PROJECT_NAME%Editor Win64 Development "%PROJECT_FILE%" -waitmutex

endlocal
