@echo on

:: Auto-detect the .uproject in this directory (cross-project usable).
set "PROJECT_NAME="
for %%F in ("%~dp0*.uproject") do set "PROJECT_NAME=%%~nF"
if not defined PROJECT_NAME (
    echo [ERROR] No .uproject found in %~dp0
    pause
    exit /b 1
)

call "D:\Code\UE5\Engine\Build\BatchFiles\GenerateProjectFiles.bat" "%~dp0%PROJECT_NAME%.uproject" -game -engine -VSCode
call "D:\Code\UE5\Engine\Build\BatchFiles\GenerateProjectFiles.bat" "%~dp0%PROJECT_NAME%.uproject" -game -engine -VS2026

pause
