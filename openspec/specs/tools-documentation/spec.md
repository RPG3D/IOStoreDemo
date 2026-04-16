# Tools Documentation

## Requirements

### Requirement: FindUE5.ps1 文档
Docs/Tools.md SHALL 包含 FindUE5.ps1 的完整使用说明，包括所有参数（-List, -Json, -EnginePath, -FindUE53/54/55/56/57）和示例。

#### Scenario: 用户查找引擎
- **WHEN** 用户需要查找已安装的 UE5 引擎
- **THEN** 可参考 Docs/Tools.md 获取 FindUE5.ps1 的使用方法

### Requirement: download_minio.ps1 文档
Docs/Tools.md SHALL 包含 download_minio.ps1 的使用说明，说明其用途是自动下载 minio.exe 和 mc.exe。

#### Scenario: 新用户设置环境
- **WHEN** 新用户首次克隆项目，IoStoreServer 目录无 minio.exe
- **THEN** 可参考 Docs/Tools.md 运行 download_minio.ps1 下载所需工具

### Requirement: OnDemandCacheAnalyzer.py 文档
Docs/Tools.md SHALL 包含 OnDemandCacheAnalyzer.py 的使用说明，包括命令行参数和输出格式。

#### Scenario: 分析缓存状态
- **WHEN** 用户需要分析 OnDemand 下载缓存
- **THEN** 可参考 Docs/Tools.md 运行 OnDemandCacheAnalyzer.py

### Requirement: mc.exe 用途说明
Docs/Tools.md SHALL 说明 mc.exe（MinIO Client）的用途，即在 setup_bucket.bat 中用于创建和配置 S3 桶。

#### Scenario: 理解桶设置流程
- **WHEN** 用户运行 setup_bucket.bat
- **THEN** 可参考 Docs/Tools.md 理解 mc.exe 在其中的作用
