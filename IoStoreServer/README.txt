IoStoreOnDemand 本地 MinIO 服务
==================================

目录结构:
  minio.exe           - MinIO 服务端 (需下载)
  mc.exe              - MinIO 客户端 (需下载)
  Data/               - MinIO 数据存储 (自动创建)
  start.bat           - 启动 MinIO 服务
  setup_bucket.bat    - 创建 IAS 内容桶
  download_minio.ps1  - 下载 minio.exe 和 mc.exe
  build_and_upload.bat - 打包Windows并上传OnDemand内容

快速开始:
  1. PowerShell 运行: .\download_minio.ps1
  2. 双击: start.bat            (启动MinIO)
  3. 另开终端: setup_bucket.bat  (创建桶)
  4. 运行: build_and_upload.bat  (打包+上传)

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
  -GenerateOnDemandPakForNonChunkedBuild  非 chunked 构建也生成 OnDemand pak

也可直接用 RunUAT 手动打包:
  RunUAT BuildCookRun ^
    -project=IOStoreDemo.uproject ^
    -targetplatform=Win64 ^
    -build -cook -stage -pak -archive ^
    -archivedirectory=Archives\Windows ^
    -ApplyIoStoreOnDemand ^
    -GenerateOnDemandPakForNonChunkedBuild

手动上传 (打包后):
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

运行时 IoStoreOnDemand.ini 示例:
  [Endpoint]
  ServiceUrl="http://localhost:9000/ias-content"
  TocPath="prefix/{toc-hash}.iochunktoc"
