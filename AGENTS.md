# AGENTS.md - 项目 AI 协作入口

## 项目概述

**IOStoreDemo** 是一个 Unreal Engine 5 项目，用于研究和验证 **IoStoreOnDemand** 按需下载功能。该功能允许将游戏资源（BulkData、贴图 mip 等）分离到云端，运行时按需下载，减小初始包体大小。

## 核心功能

- **IoStoreOnDemand**: UE5 的按需下载系统，支持将 BulkData 和贴图高精度 mip 分离到云端
- **MinIO**: 本地 S3 兼容存储服务，用于模拟云端存储
- **IasTool**: UE 提供的上传工具，用于将 OnDemand 内容上传到 S3 服务

## 两种 OnDemand 模式

| 模式 | 说明 | 使用场景 |
|------|------|----------|
| **StreamOnDemand** | 后台静默补齐缺失资源，访问时自动下载 | 基础游戏内容、贴图 mip |
| **InstallOnDemand** | 需主动调用命令下载，下载完成后才可用 | DLC、可选地图包 |

当前实现：
- `build_and_upload.bat` - 全部使用 StreamOnDemand
- `build_install_ondemand.bat` - 基础游戏 StreamOnDemand，DLC 使用 InstallOnDemand

## 命令

可用命令（位于 `.opencode/commands/`），所有脚本自动检测引擎和项目路径：

| 命令 | 说明 | 脚本 |
|------|------|------|
| BuildEditor | 编译 UE 编辑器 | `Tools/build_editor.bat` |
| RunEditor | 启动 UE 编辑器 | `Tools/run_editor.bat` |
| BuildClient | 打包并上传 OnDemand | `IoStoreServer/build_and_upload.bat` |
| RunClient | 启动游戏客户端 | `Tools/run_client.bat` |
| FindUE5 | 查找已安装引擎 | `Tools/FindUE5.ps1` |

## 关键文档

| 文档 | 说明 |
|------|------|
| [Docs/IoStoreOnDemand.md](Docs/IoStoreOnDemand.md) | IoStoreOnDemand 完整技术文档 |
| [Docs/Tools.md](Docs/Tools.md) | 工具使用说明 |
| [Docs/Source.md](Docs/Source.md) | 源码实现说明 |
| [IoStoreServer/README.txt](IoStoreServer/README.txt) | MinIO 服务快速启动指南 |

## 关键脚本

| 脚本 | 说明 |
|------|------|
| [Tools/build_editor.bat](Tools/build_editor.bat) | 编译编辑器（自动检测引擎） |
| [Tools/run_editor.bat](Tools/run_editor.bat) | 启动编辑器（自动检测引擎） |
| [Tools/run_client.bat](Tools/run_client.bat) | 启动打包后的游戏 |
| [IoStoreServer/start.bat](IoStoreServer/start.bat) | 启动 MinIO 服务 |
| [IoStoreServer/setup_bucket.bat](IoStoreServer/setup_bucket.bat) | 创建 IAS 内容桶 |
| [IoStoreServer/build_and_upload.bat](IoStoreServer/build_and_upload.bat) | 打包并上传 OnDemand（全部 StreamOnDemand） |
| [IoStoreServer/build_install_ondemand.bat](IoStoreServer/build_install_ondemand.bat) | 打包并上传（DLC 使用 InstallOnDemand） |

所有脚本均自动：
1. 从注册表查找 UE 5.7 引擎
2. 从当前目录获取项目路径和名称

## 关键配置

| 配置文件 | 说明 |
|----------|------|
| [Config/DefaultDeviceProfiles.ini](Config/DefaultDeviceProfiles.ini) | 贴图 LOD 设置，配置 `OptionalLODBias=1` 将 mip0 分离到 OnDemand |
| [Config/DefaultGame.ini](Config/DefaultGame.ini) | 项目打包配置，已启用 `bGenerateChunks=True` |

## 工具

| 工具 | 说明 |
|------|------|
| [Tools/FindUE5.ps1](Tools/FindUE5.ps1) | 查找已安装的 UE5 引擎 |
| [Tools/OnDemandCacheAnalyzer.py](Tools/OnDemandCacheAnalyzer.py) | 解析 OnDemand 下载缓存目录 |
| [IoStoreServer/download_minio.ps1](IoStoreServer/download_minio.ps1) | 下载 MinIO 工具（minio.exe、mc.exe） |

详细用法参见 [Docs/Tools.md](Docs/Tools.md)。

## 打包流程

### 快速打包（推荐）

```powershell
# 1. 启动 MinIO
IoStoreServer\start.bat

# 2. 打包 + 自动上传
IoStoreServer\build_and_upload.bat
```

### 手动打包

详细 UAT 参数参见 [Docs/IoStoreOnDemand.md](Docs/IoStoreOnDemand.md)。关键参数：

```
-ApplyIoStoreOnDemand
-GenerateOnDemandPakForNonChunkedBuild
-GenerateOptionalPakForNonChunkedBuild
-Upload="Upload <PakPath> -ServiceUrl=... -Bucket=..."
```

## 输出目录

| 目录 | 说明 |
|------|------|
| `Saved/StagedBuilds/Windows/` | Staging 目录，包含完整包和 OnDemand 文件 |
| `Saved/StagedBuilds/Windows/Cloud/IoStoreOnDemand.ini` | OnDemand 配置文件 |
| `Archives/Windows/IOStoreDemo/` | 最终测试包（OnDemand 文件已剔除） |

## OnDemand 文件说明

打包后会生成以下 OnDemand 容器：

| 文件 | 内容 | 说明 |
|------|------|------|
| `pakchunk0ondemand-Windows.ucas` | BulkData (约 5 MB) | 主要 BulkData 数据 |
| `pakchunk0ondemandoptional-Windows.ucas` | 贴图 mip0 (约 7 MB) | 可选的高精度贴图 |
| `pakchunk1ondemand-Windows.ucas` | DLC BulkData (约 7 MB) | DLC 相关 BulkData |

## 运行时配置

运行时配置文件 `IoStoreOnDemand.ini`:

```ini
[Endpoint]
ServiceUrl="http://localhost:9000"
TocPath="ias-content/<hash>.iochunktoc"
```

## MinIO 配置

| 配置项 | 值 |
|--------|-----|
| API 端口 | 9000 |
| Console 端口 | 9001 |
| AccessKey | minioadmin |
| SecretKey | minioadmin |
| Bucket | ias-content |

## 游戏内控制台命令

打包后运行游戏，可使用以下控制台命令：

| 命令 | 说明 |
|------|------|
| `installdlc <tagset>` | 下载并安装指定标签的 DLC 内容 |
| `ondemandstatus` | 显示 OnDemand 缓存状态 |
| `getinstallsize <tagset>` | 获取指定标签内容的安装大小 |

示例：
```
installdlc NewMap       # 下载 NewMap 地图
ondemandstatus          # 查看缓存使用情况
getinstallsize NewMap   # 查看下载大小
```

## 常见问题

### 1. 打包后没有生成 OnDemand 文件

确保使用了 `-ApplyIoStoreOnDemand` 参数，且项目配置了 `bGenerateChunks=True`。

### 2. 运行游戏时无法下载 OnDemand 内容

检查 MinIO 是否运行，以及 `IoStoreOnDemand.ini` 中的 `ServiceUrl` 是否正确。

### 3. 贴图 mip0 没有分离

检查 `Config/DefaultDeviceProfiles.ini` 是否正确配置了 `OptionalLODBias=1`。

## 相关源码

| 文件 | 说明 |
|------|------|
| `Engine/Source/Programs/AutomationTool/Scripts/CopyBuildToStagingDirectory.Automation.cs` | UAT 打包逻辑，处理 OnDemand 和 Upload 参数 |
| `Engine/Source/Runtime/Engine/Classes/Engine/TextureLODSettings.h` | 贴图 LOD 设置定义，包含 `OptionalLODBias` |
| `Engine/Source/Runtime/Experimental/IoStore/OnDemand/` | OnDemand 运行时实现 |

## 日志位置

- **打包日志**: `Engine/Programs/AutomationTool/Saved/Logs/`
- **游戏日志**: `<游戏目录>/Saved/Logs/`

## 参考

- [Docs/IoStoreOnDemand.md](Docs/IoStoreOnDemand.md) - 完整技术文档
- UE 源码: `CopyBuildToStagingDirectory.Automation.cs` (约 4588 行)
