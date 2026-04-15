IoStoreOnDemand 本地 MinIO 服务
==================================

目录结构:
  minio.exe           - MinIO 服务端 (需下载)
  mc.exe              - MinIO 客户端 (需下载)
  Data/               - MinIO 数据存储 (自动创建)
  start.bat           - 启动 MinIO 服务
  setup_bucket.bat    - 创建 IAS 内容桶
  download_minio.ps1  - 下载 minio.exe 和 mc.exe
  build_and_upload.bat - 打包Windows并自动上传OnDemand内容到MinIO

快速开始:
  1. PowerShell 运行: .\download_minio.ps1
  2. 双击: start.bat            (启动MinIO)
  3. 另开终端: setup_bucket.bat  (创建桶)
  4. 运行: build_and_upload.bat  (打包+自动上传)

默认凭证:
  User:     minioadmin
  Password: minioadmin

端口:
  API:      http://localhost:9000
  Console:  http://localhost:9001  (Web 管理界面)

打包参数说明:
  -ApplyIoStoreOnDemand           启用 OnDemand，自动编译 IasTool，强制 chunk manifest
                                  同时自动开启 CreateDefaultOnDemandPakRule（无需手动指定）
                                  会自动生成 BulkDataOnDemand 规则（*.uptnl, *.ubulk, 排除 .../Map/*）
                                  若未指定 -Upload，默认 Upload="localzen"（连接本机 Zen 服务器）
  -GenerateOnDemandPakForNonChunkedBuild  非 chunked 构建也生成 OnDemand pak
  -Upload=<值>                    控制打包后的上传行为：
                                  - 不传或 "localzen"：上传到本机 Zen 服务器（默认 http://127.0.0.1:8558）
                                  - 自定义S3：传入完整 IasTool 参数字符串，UAT 打包后自动调用

UAT 自动上传到 MinIO（推荐）:
  RunUAT BuildCookRun ^
    -project=IOStoreDemo.uproject ^
    -targetplatform=Win64 ^
    -build -cook -stage -pak -archive ^
    -archivedirectory=Archives\Windows ^
    -ApplyIoStoreOnDemand ^
    -GenerateOnDemandPakForNonChunkedBuild ^
    -Upload="Upload E:\UEProject\IOStoreDemo\Saved\StagedBuilds\Windows\IOStoreDemo\Content\Paks -ServiceUrl=http://localhost:9000 -Bucket=ias-content -AccessKey=minioadmin -SecretKey=minioadmin -StreamOnDemand -WriteTocToDisk -KeepContainerFiles -KeepPakFiles -TargetPlatform=Win64 -BuildVersion=1 -ConfigFilePath=E:\UEProject\IOStoreDemo\Saved\StagedBuilds\Windows\Cloud\IoStoreOnDemand.ini"

  注意: -Upload 的值（非 localzen 时）直接作为 IasTool.exe 的参数传递
  PakPath 基于 StageDirectory（Saved\StagedBuilds），不是 ArchiveDirectory

手动上传 (打包后单独执行，不推荐):
  IasTool.exe Upload "Paks\*ondemand*.utoc" ^
    -ServiceUrl=http://localhost:9000 ^
    -Bucket=ias-content ^
    -AccessKey=minioadmin ^
    -SecretKey=minioadmin ^
    -StreamOnDemand ^
    -WriteTocToDisk ^
    -KeepContainerFiles ^
    -KeepPakFiles ^
    -TargetPlatform=Win64 ^
    -BuildVersion=1 ^
    -ConfigFilePath="Cloud\IoStoreOnDemand.ini"

运行时 IoStoreOnDemand.ini 示例 (由 IasTool Upload 自动生成):
  [Endpoint]
  ServiceUrl="http://localhost:9000"
  TocPath="ias-content/83a7cd40bfffed2dc4a42820ddb5407322f1c6db.iochunktoc"
