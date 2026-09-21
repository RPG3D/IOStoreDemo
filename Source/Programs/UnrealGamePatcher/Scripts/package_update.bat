@echo off
setlocal
set "BATCH_DIR=%~dp0"
REM ============================================================================
REM package_update.bat - official UAT cook/stage + UGP chunk build
REM
REM Cooks + stages the game (standard UAT BuildCookRun), then runs UGP build to
REM chunk the staged output and write UnrealGamePatcher.index.json + chunks/ to
REM the CloudDir. No BuildPatchServices / official manifest / BPT chunks.
REM
REM Engine path is hardcoded below; the .uproject is auto-detected from this
REM script's location (4 levels up). Keep this file ASCII-only: cmd reads .bat
REM as ANSI, non-ASCII comments break parsing on localized Windows.
REM
REM Usage:
REM   package_update.bat -Version <build version> [-AppName <name>]
REM                      [-Platform Win64] [-Config Shipping] [-CloudDir <out>]
REM                      [-BuildRoot <staged build>] [-AppLaunch <rel exe>]
REM
REM   -Version    build version (required; also used for BuildVersion + default out dir)
REM   -AppName    app id written into the self-designed index (default = .uproject name)
REM   -Platform   target platform (default Win64)
REM   -Config     client config (default Development)
REM   -CloudDir   CDN root output (default <project>\Saved\UGP_Package\CloudDir -
REM               one shared root for ALL versions: chunks/ is global/content-
REM               addressed, index/ holds each version's manifest + latest.json)
REM   -BuildRoot  an already staged build root (skip cook/stage, chunk it directly)
REM   -AppLaunch  launch exe relative to BuildRoot (default: first .exe found under BuildRoot)
REM ============================================================================

REM --- Engine root: hardcoded for this machine ---
set "ENGINE_ROOT=D:\Code\UE5"
set "RUNUAT=%ENGINE_ROOT%\Engine\Build\BatchFiles\RunUAT.bat"
if not exist "%RUNUAT%" (
  echo [ERROR] RunUAT.bat not found: %RUNUAT%
  exit /b 1
)

REM --- Arguments (space-separated; cmd batch args split at '=') ---
set "VERSION="
set "APPNAME="
set "PLATFORM=Win64"
set "CONFIG=Development"
set "CLOUDDIR="
set "BUILDROOT="
set "APP_LAUNCH="

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="-Version" set "VERSION=%~2" & shift & shift & goto parse
if /i "%~1"=="-AppName" set "APPNAME=%~2" & shift & shift & goto parse
if /i "%~1"=="-Platform" set "PLATFORM=%~2" & shift & shift & goto parse
if /i "%~1"=="-Config" set "CONFIG=%~2" & shift & shift & goto parse
if /i "%~1"=="-CloudDir" set "CLOUDDIR=%~2" & shift & shift & goto parse
if /i "%~1"=="-BuildRoot" set "BUILDROOT=%~2" & shift & shift & goto parse
if /i "%~1"=="-AppLaunch" set "APP_LAUNCH=%~2" & shift & shift & goto parse
echo ERROR: unknown argument %~1
exit /b 2
:parsed

if "%VERSION%"=="" goto usage

REM --- Auto-detect the project (.uproject). %~dp0 is only valid BEFORE any
REM     shift (shift moves %0), so BATCH_DIR is cached at the top. ---
set "PROJECT_DIR=%BATCH_DIR%..\..\..\.."
for %%I in ("%PROJECT_DIR%") do set "PROJECT_DIR=%%~fI"
set "PROJECT_NAME="
for %%F in ("%PROJECT_DIR%\*.uproject") do set "PROJECT_NAME=%%~nF"
if not defined PROJECT_NAME (
  echo [ERROR] No .uproject found in %PROJECT_DIR%
  exit /b 1
)
set "PROJECT_FILE=%PROJECT_DIR%\%PROJECT_NAME%.uproject"
if "%APPNAME%"=="" set "APPNAME=%PROJECT_NAME%"

REM --- Default output: ONE shared CDN root for all versions (chunks are
REM global/content-addressed; index/<version>/ + index/latest.json per version).
if "%CLOUDDIR%"=="" set "CLOUDDIR=%PROJECT_DIR%\Saved\UGP_Package\CloudDir"

echo ============================================================================
echo [package_update] Project : %PROJECT_FILE%
echo [package_update] Engine  : %ENGINE_ROOT%
echo [package_update] Version : %VERSION%  AppName: %APPNAME%
echo [package_update] CloudDir: %CLOUDDIR%
echo ============================================================================

REM --- 1. Cook + Stage (unless -BuildRoot was given) ---
if "%BUILDROOT%"=="" (
  REM Full build: -build compiles the editor + game target first (without it
  REM UAT only cooks and the stage step fails on the missing target receipt).
  echo [package_update] BuildCookRun: build + cook + stage + pak ^(%PLATFORM% / %CONFIG%^)...
  call "%RUNUAT%" BuildCookRun -project="%PROJECT_FILE%" -noP4 -platform=%PLATFORM% -clientconfig=%CONFIG% -build -cook -stage -pak
  if errorlevel 1 goto cook_failed
  REM Standard staged output: <project>\Saved\StagedBuilds\<Platform> - the
  REM platform dir name varies (Win64 in older UE, Windows in newer), so glob
  REM the first subdir instead of assuming %PLATFORM%.
  set "BUILDROOT="
  for /d %%D in ("%PROJECT_DIR%\Saved\StagedBuilds\*") do if not defined BUILDROOT set "BUILDROOT=%%D"
  if not defined BUILDROOT goto stage_missing
)
for %%I in ("%BUILDROOT%") do set "BUILDROOT=%%~fI"
if not exist "%BUILDROOT%" (
  echo [ERROR] BuildRoot not found: %BUILDROOT%
  exit /b 1
)

REM --- 2. AppLaunch: exe path relative to BuildRoot ---
set "PFX=%BUILDROOT%\"
if "%APP_LAUNCH%"=="" (
  set "EXE_ABS="
  for /r "%BUILDROOT%" %%F in (*.exe) do if not defined EXE_ABS set "EXE_ABS=%%F"
  if not defined EXE_ABS (
    echo [ERROR] no .exe under BuildRoot %BUILDROOT% - pass -AppLaunch explicitly
    exit /b 1
  )
  REM Relative path via TWO-PHASE (call set) substring replace:
  REM   %VAR:*<prefix>\=%  drops everything up to and including BuildRoot\,
  REM   then \ -> /. A plain %VAR:*%OTHER%\=% does not work (cmd has no
  REM   nested %-expansion inside a substitution).
  call set "APP_LAUNCH=%%EXE_ABS:*%PFX%=%%"
  call set "APP_LAUNCH=%%APP_LAUNCH:\=/%%
)
if "%APP_LAUNCH%"=="" (
  echo [ERROR] cannot resolve -AppLaunch under %BUILDROOT%
  exit /b 1
)

REM --- 3. UGP build: chunk + index directly from the staged build (self-
REM designed format - no BuildPatchTool, no official manifest).
set "PATCHER=%PROJECT_DIR%\Binaries\Win64\UnrealGamePatcher.exe"
if not exist "%PATCHER%" (
  echo [ERROR] UnrealGamePatcher.exe not found: %PATCHER% - build it first
  exit /b 1
)
echo [package_update] build: chunk %BUILDROOT% into %CLOUDDIR% ...
"%PATCHER%" -cli build -staged="%BUILDROOT%" -app="%APPNAME%" -version="%VERSION%" -out="%CLOUDDIR%"
if errorlevel 1 (
  echo [ERROR] UGP build failed ^(exit %errorlevel%^)
  exit /b 1
)

REM --- 4. Summary + next step ---
echo ============================================================================
echo [package_update] OK
echo   Index             : %CLOUDDIR%\index\%VERSION%\UnrealGamePatcher.index.json
echo   Latest            : %CLOUDDIR%\index\latest.json
echo   Chunks            : %CLOUDDIR%\chunks
echo   Next: point the client at -cdn="%CLOUDDIR%" [-version="%VERSION%"]
echo ============================================================================
exit /b 0

REM Error labels live AFTER the normal exit /b 0 (cmd labels do not stop
REM execution - a fall-through here would always fail).
:cook_failed
echo [ERROR] BuildCookRun failed ^(exit %errorlevel%^)
exit /b 1

:stage_missing
echo [ERROR] staged build not found at %BUILDROOT%
exit /b 1

:usage
echo Usage: package_update.bat -Version ^<build version^> [-AppName ^<name^>] [-Platform Win64] [-Config Shipping] [-CloudDir ^<out^>] [-BuildRoot ^<staged build^>] [-AppLaunch ^<rel exe^>]
exit /b 2
