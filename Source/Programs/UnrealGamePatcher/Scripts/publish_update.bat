@echo off
REM ============================================================================
REM UnrealGamePatcher - build pipeline integration (task 9.2)
REM
REM Ingest an already-staged game build into the CDN layout, then optionally
REM diff it against a previous version. Fails with a non-zero exit code when
REM any step fails, so CI can propagate.
REM
REM (cook/stage itself lives in package_update.bat; this script only runs the
REM  UGP build/diff steps on a staged directory.)
REM
REM NOTE: cmd batch arguments split at '=', so use space-separated pairs.
REM Keep this file ASCII-only: cmd reads .bat as ANSI, non-ASCII comments break
REM parsing on localized Windows.
REM
REM Usage:
REM   publish_update.bat -Staged <staged dir> -App <name> -Version <v> -CdnRoot <CDN root> [-BaseIndex <prev index.json> -NewIndex <new index.json> -DeltaOut <delta dir>]
REM
REM Examples:
REM   publish_update.bat -Staged Saved/StagedBuilds/Windows -App IOStoreDemo -Version 1.0.0 -CdnRoot cdn
REM   publish_update.bat -Staged ... -App ... -Version ... -CdnRoot cdn -BaseIndex cdn/1.0.0/UnrealGamePatcher.index.json -NewIndex cdn/1.0.1/UnrealGamePatcher.index.json -DeltaOut deltas
REM ============================================================================

setlocal
set "PATCHER=%~dp0..\..\..\..\Binaries\Win64\UnrealGamePatcher.exe"

set "STAGED="
set "APP="
set "VERSION="
set "CDNROOT="
set "BASEINDEX="
set "NEWINDEX="
set "DELTAOUT="

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="-Staged" set "STAGED=%~2" & shift & shift & goto parse
if /i "%~1"=="-App" set "APP=%~2" & shift & shift & goto parse
if /i "%~1"=="-Version" set "VERSION=%~2" & shift & shift & goto parse
if /i "%~1"=="-CdnRoot" set "CDNROOT=%~2" & shift & shift & goto parse
if /i "%~1"=="-BaseIndex" set "BASEINDEX=%~2" & shift & shift & goto parse
if /i "%~1"=="-NewIndex" set "NEWINDEX=%~2" & shift & shift & goto parse
if /i "%~1"=="-DeltaOut" set "DELTAOUT=%~2" & shift & shift & goto parse
echo ERROR: unknown argument %~1
exit /b 2
:parsed

if "%STAGED%"=="" goto usage
if "%APP%"=="" goto usage
if "%VERSION%"=="" goto usage
if "%CDNROOT%"=="" goto usage

if not exist "%PATCHER%" (
  echo ERROR: patcher not found at %PATCHER%
  exit /b 2
)
if not exist "%STAGED%" (
  echo ERROR: staged dir not found: %STAGED%
  exit /b 2
)

echo [UnrealGamePatcher] Ingesting staged build %STAGED% into %CDNROOT% ...
"%PATCHER%" -cli build -staged="%STAGED%" -app="%APP%" -version="%VERSION%" -out="%CDNROOT%"
if errorlevel 1 (
  echo ERROR: ingest failed ^(exit %errorlevel%^)
  exit /b 1
)

if not "%BASEINDEX%"=="" (
  if "%DELTAOUT%"=="" (
    echo ERROR: -DeltaOut is required when -BaseIndex is given
    exit /b 2
  )
  if "%NEWINDEX%"=="" (
    echo ERROR: -NewIndex is required when -BaseIndex is given
    exit /b 2
  )
  echo [UnrealGamePatcher] Diffing %BASEINDEX% -^> %NEWINDEX% ...
  "%PATCHER%" -cli diff -base="%BASEINDEX%" -target="%NEWINDEX%" -out="%DELTAOUT%" -targetcdn="%CDNROOT%"
  if errorlevel 1 (
    echo ERROR: diff failed ^(exit %errorlevel%^)
    exit /b 1
  )
)

echo [UnrealGamePatcher] publish_update.bat OK
exit /b 0

:usage
echo Usage: publish_update.bat -Staged ^<staged dir^> -App ^<name^> -Version ^<v^> -CdnRoot ^<cdn root^> [-BaseIndex ^<prev index^> -NewIndex ^<new index^> -DeltaOut ^<dir^>]
exit /b 2
