@echo off
setlocal

:: ============================================
:: 创建 IAS 内容桶 & 测试连通性
:: 前提: MinIO 服务已启动 (start.bat)
:: 需要: mc.exe (MinIO Client)
:: ============================================

set MC=%~dp0mc.exe
set ALIAS=iaslocal
set ENDPOINT=http://localhost:9000
set BUCKET=ias-content

:: 检查 mc.exe
if not exist "%MC%" (
    echo [ERROR] mc.exe not found in %~dp0
    echo [INFO]  Please download from: https://dl.min.io/client/mc/release/windows-amd64/mc.exe
    echo [INFO]  Or run: download_minio.ps1
    pause
    exit /b 1
)

echo [1/3] Configuring mc alias...
"%MC%" alias set %ALIAS% %ENDPOINT% minioadmin minioadmin
if errorlevel 1 (
    echo [ERROR] Failed to connect to MinIO. Is the server running?
    pause
    exit /b 1
)

echo [2/3] Creating bucket '%BUCKET%'...
"%MC%" mb %ALIAS%/%BUCKET% 2>nul
if errorlevel 1 (
    echo [INFO] Bucket may already exist, continuing...
)

echo [3/3] Setting bucket policy (public read for runtime HTTP access)...
"%MC%" anonymous set download %ALIAS%/%BUCKET%
if errorlevel 1 (
    echo [WARN] Failed to set bucket policy. You may need to configure it manually.
)

echo.
echo ============================================
echo  Setup Complete
echo ============================================
echo  Bucket:     %BUCKET%
echo  Endpoint:   %ENDPOINT%
echo  S3 URL:     %ENDPOINT%/%BUCKET%
echo.
echo  IasTool Upload example:
echo    IasTool Upload *.utoc ^
echo      -ServiceUrl=%ENDPOINT% ^
echo      -Bucket=%BUCKET% ^
echo      -AccessKey=minioadmin ^
echo      -SecretKey=minioadmin ^
echo      -StreamOnDemand ^
echo      -WriteTocToDisk ^
echo      -ConfigFilePath=Config\IoStoreOnDemand.ini
echo ============================================

endlocal
