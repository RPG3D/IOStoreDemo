@echo off
REM ============================================================================
REM start_rustfs.bat - start the local rustfs S3 server for the UGP CDN.
REM
REM rustfs is an S3-compatible object store (MinIO-class, written in Rust).
REM Put rustfs.exe next to this script, then run:
REM   start_rustfs.bat
REM and publish the CDN into it with:
REM   python s3_sync.py --endpoint http://127.0.0.1:9000 --access-key ugpadmin
REM       --secret-key ugpadmin123 --bucket ugp-cdn --create --public
REM       --sync <project>\Saved\UGP_Package\CloudDir
REM Then point the client at: -cdn=http://127.0.0.1:9000/ugp-cdn (or -cdn=file:///... for the local dir)
REM (the bucket must be public-read: the client GETs without signing).
REM
REM Defaults (override via env vars): RUSTFS_DATA / RUSTFS_ADDR / RUSTFS_KEY /
REM RUSTFS_SECRET. Keep this file ASCII-only: cmd reads .bat as ANSI.
REM ============================================================================
setlocal
set "BATCH_DIR=%~dp0"

REM --- rustfs.exe sits next to this script ---
set "RUSTFS=%BATCH_DIR%rustfs.exe"
if not exist "%RUSTFS%" (
  echo [ERROR] rustfs.exe not found next to this script: %RUSTFS%
  exit /b 1
)

REM --- Data volume: project Saved\rustfs-data (auto-derived from script location) ---
set "PROJECT_DIR=%BATCH_DIR%..\..\..\.."
for %%I in ("%PROJECT_DIR%") do set "PROJECT_DIR=%%~fI"
if "%RUSTFS_DATA%"=="" set "RUSTFS_DATA=%PROJECT_DIR%\Saved\rustfs-data"
if "%RUSTFS_ADDR%"=="" set "RUSTFS_ADDR=127.0.0.1:9000"
if "%RUSTFS_KEY%"=="" set "RUSTFS_KEY=ugpadmin"
if "%RUSTFS_SECRET%"=="" set "RUSTFS_SECRET=ugpadmin123"

if not exist "%RUSTFS_DATA%" mkdir "%RUSTFS_DATA%"

REM --- Port check: refuse to start a SECOND CDN server on the same port.
REM Extract the port (text after the LAST ':') from RUSTFS_ADDR, then look
REM for a LISTENING socket on it - another rustfs/CDN instance would
REM conflict and silently shadow this one.
set "PORT="
set "TMP=%RUSTFS_ADDR%"
:port_seg
if "%TMP%"=="" goto port_done
for /f "tokens=1,* delims=:" %%A in ("%TMP%") do (
  set "PORT=%%A"
  set "TMP=%%B"
)
if not "%TMP%"=="" goto port_seg
:port_done
netstat -ano | findstr /C:":%PORT% " | findstr /C:"LISTENING" >nul
if not errorlevel 1 (
  echo [ERROR] port %PORT% is already LISTENING - a rustfs/CDN server seems
  echo         to be running. Stop it first, or set RUSTFS_ADDR to another
  echo         port, then start this one.
  exit /b 2
)

echo ============================================================================
echo [rustfs] binary : %RUSTFS%
echo [rustfs] data   : %RUSTFS_DATA%
echo [rustfs] address: %RUSTFS_ADDR%   console: http://127.0.0.1:9001
echo [rustfs] keys   : %RUSTFS_KEY% / %RUSTFS_SECRET%
echo [rustfs] client : -cdn=http://%RUSTFS_ADDR%/^<bucket^>   (or -cdn=file:///^<local CDN dir^>)
echo ============================================================================
"%RUSTFS%" server "%RUSTFS_DATA%" --address %RUSTFS_ADDR% --access-key %RUSTFS_KEY% --secret-key %RUSTFS_SECRET% %*
exit /b %errorlevel%
