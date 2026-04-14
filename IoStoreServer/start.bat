@echo off
setlocal

:: ============================================
:: IoStoreOnDemand - MinIO 本地服务启动脚本
:: ============================================

:: 数据存储目录
set DATA_DIR=%~dp0Data

:: S3 API 端口
set API_PORT=9000

:: MinIO 控制台端口
set CONSOLE_PORT=9001

:: 默认凭证（仅开发环境使用，生产环境请更换）
set MINIO_ROOT_USER=minioadmin
set MINIO_ROOT_PASSWORD=minioadmin

:: 创建数据目录
if not exist "%DATA_DIR%" (
    mkdir "%DATA_DIR%"
    echo [INFO] Created data directory: %DATA_DIR%
)

:: 检查 minio.exe 是否存在
if not exist "%~dp0minio.exe" (
    echo [ERROR] minio.exe not found in %~dp0
    echo [INFO]  Please download from: https://dl.min.io/server/minio/release/windows-amd64/minio.exe
    echo [INFO]  Or run: download_minio.ps1
    pause
    exit /b 1
)

echo ============================================
echo  MinIO Server for IoStoreOnDemand
echo ============================================
echo  API:       http://localhost:%API_PORT%
echo  Console:   http://localhost:%CONSOLE_PORT%
echo  User:      %MINIO_ROOT_USER%
echo  Password:  %MINIO_ROOT_PASSWORD%
echo  Data:      %DATA_DIR%
echo ============================================
echo.

:: 启动 MinIO
"%~dp0minio.exe" server "%DATA_DIR%" --address ":%API_PORT%" --console-address ":%CONSOLE_PORT%"

endlocal
