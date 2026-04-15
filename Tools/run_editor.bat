@echo off
setlocal

:: ============================================
:: Run UE Editor for IOStoreDemo
:: ============================================

:: Detect project
set "PROJECT_DIR=%~dp0.."
for %%I in ("%PROJECT_DIR%") do set "PROJECT_DIR=%%~fI"
for %%F in ("%PROJECT_DIR%\*.uproject") do set "PROJECT_FILE=%PROJECT_DIR%\%%~nF.uproject"

:: Detect UE 5.7 engine
set "FINDUE5_SCRIPT=%PROJECT_DIR%\Tools\FindUE5.ps1"
for /f "delims=" %%E in ('powershell -NoProfile -ExecutionPolicy Bypass -File "%FINDUE5_SCRIPT%" -FindUE57 -EnginePath 2^>nul') do set "ENGINE_DIR=%%E"

if not defined ENGINE_DIR (
    echo [ERROR] UE 5.7 engine not found
    exit /b 1
)

echo Starting: %ENGINE_DIR%\Binaries\Win64\UnrealEditor.exe
echo Project: %PROJECT_FILE%

start "" "%ENGINE_DIR%\Binaries\Win64\UnrealEditor.exe" "%PROJECT_FILE%"

endlocal
