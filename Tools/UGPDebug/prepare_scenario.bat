@echo off
setlocal
REM ============================================================================
REM prepare_scenario.bat - reset the UGP debug scenario state.
REM
REM Builds a fresh file:// CDN (v1) from the real fixture and removes the
REM install dir, so a debugger-driven `update` runs the FRESH-INSTALL path:
REM   Update entry -> RebuildFileFromChunks -> FPakWriter::Commit -> verify.
REM
REM The debugmcp adapter launch (see .debugmcp registration, user scope) runs:
REM   UnrealGamePatcher.exe -cli update -cdn=file:///<CDN> -install=<INSTALL>
REM
REM Usage: Tools\UGPDebug\prepare_scenario.bat
REM ============================================================================
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "EXE=%ROOT%\Binaries\Win64\UnrealGamePatcher.exe"
set "STAGED=%ROOT%\.manifesttest\chunkinstall\Windows\1.0.0\pakchunk1"
set "CDN=%ROOT%\Saved\UGPDebug\cdn"
set "INSTALL=%ROOT%\Saved\UGPDebug\install"

if not exist "%EXE%" (
  echo [ERROR] UnrealGamePatcher.exe not found: %EXE%
  exit /b 1
)

rmdir /s /q "%CDN%" 2>nul
rmdir /s /q "%INSTALL%" 2>nul

"%EXE%" -cli build -staged="%STAGED%" -app=IOStoreDemo -version=1.0.0 -out="%CDN%"
if errorlevel 1 (
  echo [ERROR] ingest failed
  exit /b 1
)

echo [scenario] CDN     : %CDN%
echo [scenario] INSTALL : %INSTALL% ^(fresh, will be created by the debuggee^)
echo [scenario] OK - launch the debug session via debugmcp start_debugging
exit /b 0
