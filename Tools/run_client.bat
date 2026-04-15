@echo off
setlocal

:: ============================================
:: Run packaged game client
:: ============================================

:: Detect project
set "PROJECT_DIR=%~dp0.."
for %%I in ("%PROJECT_DIR%") do set "PROJECT_DIR=%%~fI"
for %%F in ("%PROJECT_DIR%\*.uproject") do set "PROJECT_NAME=%%~nF"

set "CLIENT_EXE=%PROJECT_DIR%\Archives\Windows\%PROJECT_NAME%\Binaries\Win64\%PROJECT_NAME%.exe"

if not exist "%CLIENT_EXE%" (
    echo [ERROR] Client not found: %CLIENT_EXE%
    echo [INFO] Run BuildClient first
    exit /b 1
)

echo Starting: %CLIENT_EXE%
start "" "%CLIENT_EXE%"

endlocal
