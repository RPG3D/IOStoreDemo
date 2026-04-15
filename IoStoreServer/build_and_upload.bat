@echo off
setlocal enabledelayedexpansion

:: ============================================
:: IoStoreOnDemo - Windows 打包脚本（UAT 自动上传到 MinIO）
:: ============================================
:: 原理：
::   UAT 源码 CopyBuildToStagingDirectory.Automation.cs 中：
::   1. -ApplyIoStoreOnDemand 会设置 Params.Upload（默认为 "localzen"）
::   2. 如果显式传 -Upload=<值>，则不会默认为 localzen
::   3. 当 Upload 值非 "localzen" 时，UAT 直接将该值作为 IasTool 的参数
::      即: IasTool.exe <Upload的值>
::   4. 因此 -Upload 需要传入完整的 IasTool Upload 命令参数
::
::   IasTool Upload 用法: Upload <ContainerGlob> [Options]
::   例如: Upload E:\path\to\Paks -ServiceUrl=http://localhost:9000 ...

:: ---------- 自动检测项目目录 ----------
set "PROJECT_DIR=%~dp0.."
for %%I in ("%PROJECT_DIR%") do set "PROJECT_DIR=%%~fI"

:: 从 .uproject 文件获取项目名称
for %%F in ("%PROJECT_DIR%\*.uproject") do set "PROJECT_NAME=%%~nF"
set "PROJECT_FILE=%PROJECT_DIR%\%PROJECT_NAME%.uproject"

if not exist "%PROJECT_FILE%" (
    echo [ERROR] No .uproject file found in %PROJECT_DIR%
    pause
    exit /b 1
)

:: ---------- 自动检测引擎目录（使用 FindUE5.ps1 查找 5.7.* 版本）----------
echo [1/3] Detecting UE 5.7 engine...

set "FINDUE5_SCRIPT=%PROJECT_DIR%\Tools\FindUE5.ps1"
if not exist "%FINDUE5_SCRIPT%" (
    echo [ERROR] FindUE5.ps1 not found: %FINDUE5_SCRIPT%
    pause
    exit /b 1
)

for /f "delims=" %%E in ('powershell -NoProfile -ExecutionPolicy Bypass -File "%FINDUE5_SCRIPT%" -FindUE57 2^>nul') do set "ENGINE_DIR=%%E"

if not defined ENGINE_DIR (
    echo [ERROR] UE 5.7 engine not found
    echo [INFO] Run "powershell -File Tools\FindUE5.ps1 -List" to see available engines
    pause
    exit /b 1
)

echo       Found: %ENGINE_DIR%

:: MinIO S3 配置
set S3_URL=http://localhost:9000
set S3_BUCKET=ias-content
set S3_ACCESS_KEY=minioadmin
set S3_SECRET_KEY=minioadmin

:: 输出目录
set ARCHIVE_DIR=%PROJECT_DIR%\Archives\Windows

:: IasTool Upload 使用的路径（基于 StageDirectory，不是 ArchiveDirectory）
:: UAT 在 staging 完成后、archive 之前调用 IasTool Upload
:: StageDirectory = ProjectDir\Saved\StagedBuilds
:: PakPath = StageDirectory\Windows\ProjectName\Content\Paks
set STAGE_DIR=%PROJECT_DIR%\Saved\StagedBuilds
set PAK_PATH=%STAGE_DIR%\Windows\%PROJECT_NAME%\Content\Paks
set CLOUD_DIR=%STAGE_DIR%\Windows\Cloud

echo.
echo  Project:     %PROJECT_NAME%
echo  Project Dir: %PROJECT_DIR%
echo  Engine Dir:  %ENGINE_DIR%
echo.

:: ---------- 检查 MinIO ----------
echo [2/3] Checking MinIO connectivity...
curl -s -o nul -w "%%{http_code}" %S3_URL%/minio/health/live >nul 2>&1
if errorlevel 1 (
    echo [WARN] MinIO does not seem to be running at %S3_URL%
    echo [INFO] Start it first: IoStoreServer\start.bat
    echo.
    set /p CONTINUE="Continue without MinIO? (y/n): "
    if /i "!CONTINUE!" neq "y" exit /b 1
)

:: ---------- 运行 UAT 打包（含自动上传） ----------
echo [3/3] Running UAT BuildCookRun with auto-upload to MinIO...
echo.
echo  UAT will call IasTool Upload after staging:
echo    IasTool.exe Upload %PAK_PATH% -ServiceUrl=%S3_URL% -Bucket=%S3_BUCKET% ...
echo.

set UAT=%ENGINE_DIR%\Build\BatchFiles\RunUAT.bat

:: 注意：-Upload 的值（非 localzen）直接传给 IasTool 作为完整参数
:: 用引号包裹整个值，防止 cmd 拆分参数（路径无空格，不需要内部引号）
call %UAT% BuildCookRun ^
  -project=%PROJECT_FILE% ^
  -targetplatform=Win64 ^
  -clientconfig=Development ^
  -serverconfig=Development ^
  -build -cook -stage -pak -archive ^
  -archivedirectory=%ARCHIVE_DIR% ^
  -ApplyIoStoreOnDemand ^
  -GenerateOnDemandPakForNonChunkedBuild ^
  -GenerateOptionalPakForNonChunkedBuild ^
  -Upload="Upload %PAK_PATH% -ServiceUrl=%S3_URL% -Bucket=%S3_BUCKET% -AccessKey=%S3_ACCESS_KEY% -SecretKey=%S3_SECRET_KEY% -StreamOnDemand -WriteTocToDisk -KeepContainerFiles -KeepPakFiles -TargetPlatform=Win64 -BuildVersion=1 -ConfigFilePath=%CLOUD_DIR%\IoStoreOnDemand.ini"

set BUILD_RESULT=%errorlevel%

if %BUILD_RESULT% neq 0 (
    echo.
    echo [ERROR] Build or Upload failed with error code %BUILD_RESULT%
    pause
    exit /b %BUILD_RESULT%
)

echo.
echo ============================================
echo  Build & Auto-Upload Complete!
echo ============================================
echo  Archive:  %ARCHIVE_DIR%
echo  S3:       %S3_URL%/%S3_BUCKET%
echo.
echo  IoStoreOnDemand.ini:
echo    %CLOUD_DIR%\IoStoreOnDemand.ini
echo ============================================

pause
endlocal