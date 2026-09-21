@echo off
REM ============================================================================
REM install_update_module.bat - copy the UnrealGamePatcher update module into
REM another UE project with one command.
REM
REM What gets copied (into the TARGET project):
REM   Source\Programs\UnrealGamePatcher\   C++ Program modules + Scripts + docs
REM   Binaries\Win64\UnrealGamePatcher.exe  prebuilt client (or rebuilt with -Build)
REM
REM The module is location-portable: package_update.bat / publish_update.bat /
REM start_rustfs.bat all derive the project root from their own location
REM (Scripts\..\..\..\..), so they work unchanged inside the new project.
REM
REM Usage:
REM   install_update_module.bat -Project <target UE project dir>
REM                            [-WithRustfs] [-Build] [-Config Development]
REM
REM   -Project     target project root (the dir containing the .uproject).
REM                Required.
REM   -WithRustfs  also copy rustfs.exe (the 1.4MB S3 server binary) into Scripts\
REM   -Build       rebuild UnrealGamePatcher for the TARGET project (engine
REM                path hardcoded below) instead of copying the prebuilt exe -
REM                use this when the target uses a different engine version.
REM
REM Keep this file ASCII-only: cmd reads .bat as ANSI.
REM ============================================================================
setlocal
set "BATCH_DIR=%~dp0"
set "ENGINE_ROOT=D:\Code\UE5"

REM --- Source module root = this script's own project --------------------------
set "SRC_PROJECT=%BATCH_DIR%..\..\..\.."
for %%I in ("%SRC_PROJECT%") do set "SRC_PROJECT=%%~fI"
set "SRC_MODULE=%SRC_PROJECT%\Source\Programs\UnrealGamePatcher"
set "SRC_EXE=%SRC_PROJECT%\Binaries\Win64\UnrealGamePatcher.exe"

set "TARGET="
set "WITH_RUSTFS=0"
set "BUILD=0"
set "CONFIG=Development"

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="-Project" set "TARGET=%~2" & shift & shift & goto parse
if /i "%~1"=="-WithRustfs" set "WITH_RUSTFS=1" & shift & goto parse
if /i "%~1"=="-Build" set "BUILD=1" & shift & goto parse
if /i "%~1"=="-Config" set "CONFIG=%~2" & shift & shift & goto parse
echo ERROR: unknown argument %~1
exit /b 2
:parsed

if "%TARGET%"=="" goto usage
for %%I in ("%TARGET%") do set "TARGET=%%~fI"
if not exist "%TARGET%" (
  echo [ERROR] target dir not found: %TARGET%
  exit /b 1
)

REM --- Locate the target .uproject ---------------------------------------------
set "UPROJECT="
for %%F in ("%TARGET%\*.uproject") do set "UPROJECT=%%~fF"
if not defined UPROJECT (
  echo [ERROR] no .uproject found in %TARGET%
  exit /b 1
)
for %%N in ("%UPROJECT%") do set "UPROJECT_NAME=%%~nN"

REM --- Guard: target must not already host the module -------------------------
set "DST_MODULE=%TARGET%\Source\Programs\UnrealGamePatcher"
if exist "%DST_MODULE%\UnrealGamePatcher.Target.cs" (
  echo [ERROR] target already has Source\Programs\UnrealGamePatcher - remove it first
  exit /b 1
)

echo ============================================================================
echo [install] source project : %SRC_PROJECT%
echo [install] target project : %UPROJECT_NAME% ^(%UPROJECT%^)
echo [install] module         : %SRC_MODULE%  -^>  %DST_MODULE%
echo ============================================================================

REM --- 1. Copy the module (C++ + Scripts + docs) ------------------------------
if not exist "%SRC_MODULE%" (
  echo [ERROR] source module not found: %SRC_MODULE%
  exit /b 1
)
robocopy "%SRC_MODULE%" "%DST_MODULE%" /E /XD .git /XF "*.bak" /NFL /NDL /NJH /NJS /NP
if %errorlevel% GEQ 8 (
  echo [ERROR] robocopy failed ^(code %errorlevel%^)
  exit /b 1
)

REM Drop rustfs.exe unless explicitly requested (1.4MB third-party binary).
if "%WITH_RUSTFS%"=="0" (
  if exist "%DST_MODULE%\Scripts\rustfs.exe" del "%DST_MODULE%\Scripts\rustfs.exe"
)

REM --- 2. Binary: rebuild for the target, or copy the prebuilt exe -------------
if "%BUILD%"=="1" (
  echo [install] building UnrealGamePatcher %CONFIG% for %UPROJECT_NAME% ...
  call "%ENGINE_ROOT%\Engine\Build\BatchFiles\Build.bat" UnrealGamePatcher Win64 %CONFIG% -Project="%UPROJECT%" -WaitMutex -FromMsBuild -architecture=x64
  if errorlevel 1 (
    echo [ERROR] build failed ^(exit %errorlevel%^)
    exit /b 1
  )
) else (
  if not exist "%SRC_EXE%" (
    echo [ERROR] prebuilt exe not found: %SRC_EXE%
    echo         Use -Build to compile for the target project instead.
    exit /b 1
  )
  if not exist "%TARGET%\Binaries\Win64" mkdir "%TARGET%\Binaries\Win64"
  copy /y "%SRC_EXE%" "%TARGET%\Binaries\Win64\" >nul
  if errorlevel 1 (
    echo [ERROR] failed copying %SRC_EXE%
    exit /b 1
  )
)

REM --- 3. Summary + next steps -------------------------------------------------
echo ============================================================================
echo [install] OK - UnrealGamePatcher installed into %UPROJECT_NAME%
echo.
echo   Module  : %DST_MODULE%
echo   Binary  : %TARGET%\Binaries\Win64\UnrealGamePatcher.exe
echo.
echo   Next steps:
echo     1. Publish a version:
echo        Scripts\package_update.bat -Version 1.0.0
echo        ^(cooks/stages the game, chunks it, writes index + latest.json^)
echo     2. Serve it over S3 ^(rustfs/MinIO/cloud^):
echo        python Scripts\s3_sync.py --endpoint ... --bucket ugp-cdn --create --public --sync Saved\UGP_Package\CloudDir
echo     3. Point the client at it:
echo        Binaries\Win64\UnrealGamePatcher.exe -cli update -cdn=http://.../ugp-cdn -install=^<dir^>
echo ============================================================================
exit /b 0

:usage
echo Usage: install_update_module.bat -Project ^<target UE project dir^> [-WithRustfs] [-Build] [-Config Development]
exit /b 2
