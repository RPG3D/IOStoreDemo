# IoStoreOnDemand 用法分析

## 一、系统概述

IoStoreOnDemand（也称为 IAS - IoStore On Demand / Install And Stream）是 UE5 的**内容按需加载系统**，允许将游戏部分内容（如 BulkData、可选资源）放在云端，在运行时按需下载/流式加载，而非随包全量分发。这能显著减小初始包体大小。

## 二、模块架构

```
IoStoreOnDemandCore (核心层，无HTTP依赖)
├── IoStoreOnDemand.h        — 公共接口 IOnDemandIoStore
├── OnDemandToc.h            — TOC文件格式定义
├── OnDemandHostGroup.h      — 主机组（CDN端点管理）
├── HttpIoDispatcher.h       — HTTP I/O调度器
└── OnDemandError.h          — 错误处理

IoStoreOnDemand (实现层，依赖HTTP/S3)
├── OnDemandIoStore          — 核心实现类
├── OnDemandContentInstaller — 内容安装器（下载+缓存）
├── OnDemandHttpThread       — HTTP下载线程
├── OnDemandInstallCache     — 安装缓存管理
├── OnDemandIoDispatcherBackend  — IoDispatcher后端（流式）
├── OnDemandPackageStoreBackend  — PackageStore后端
└── Tool/                    — IasTool 命令行工具
    ├── Upload.cpp           — 上传容器到S3
    ├── ChunkPlugin.cpp      — 分块插件生成
    ├── Download.cpp         — 下载工具
    └── ListTocs.cpp         — 列出TOC
```

## 三、打包阶段（CopyBuildToStagingDirectory）

### 3.1 Pak规则配置

在 `DefaultGame.ini` 中配置 PakFileRules，将特定内容标记为 OnDemand：

```ini
[PakFileRules]
+OnDemand_Rule=(Filter="*.ubulk", bOnDemand=true)
```

关键属性：
- `bOnDemand` — 标记该规则的文件进入 OnDemand 容器
- `OnDemandAllowedChunkFilters` — 限定允许的 chunk 名称正则
- `Filter` — 文件过滤器

### 3.2 自动生成默认规则

当启用 `CreateDefaultOnDemandPakRule` 时，系统自动创建 `BulkDataOnDemand` 规则：

```csharp
// CopyBuildToStagingDirectory.Automation.cs:3126-3147
PakRules.Name = "BulkDataOnDemand";
PakRules.bOnDemand = true;
PakRules.Filter.AddRule("*.uptnl");   // 位置数据
PakRules.Filter.AddRule("*.ubulk");   // BulkData
PakRules.Filter.AddRule("-.../Map/*"); // 排除地图目录（太大）
```

### 3.3 Pak文件生成

打包时，文件被分为四类：

| 字典 | 用途 | 输出Pak名 |
|------|------|-----------|
| `DefaultPak` | 常规内容 | `{Project}_p` |
| `OptionalPak` | 可选BulkData | `{Project}_optional` |
| `OnDemandPak` | OnDemand内容 | `{Project}_ondemand` |
| `OnDemandOptionalPak` | OnDemand可选内容 | `{Project}_ondemandoptional` |

OnDemand pak 创建时传入 `bOnDemand: true`，这会在容器上设置 `EIoContainerFlags::OnDemand` 标志。

### 3.4 关键打包参数

- `ApplyIoStoreOnDemand` — 启用 OnDemand 打包，会自动：
  1. 编译 IoStoreOnDemand 模块和 IasTool
  2. 开启 `CompileIoStoreOnDemand`、`Manifests=true`
  3. 自动创建 `CreateDefaultOnDemandPakRule`（无需手动在 ini 中配置）
  4. 若未指定 `-Upload`，默认设置 `Upload="localzen"`（连接本机 Zen 服务器）
  5. 启用 SSL 证书部署
- `GenerateOnDemandPakForNonChunkedBuild` — 非 chunked 构建也生成 OnDemand pak
- `-Upload=<值>` — 控制 IasTool 上传行为：
  - 不传：默认 `"localzen"`，上传到本机 Zen 服务器（`http://127.0.0.1:8558`）
  - `"-Upload=localzen"`：同上
  - **自定义 S3**：传入完整的 IasTool 参数字符串，例如：
    ```
    -Upload="Upload E:\path\to\Paks -ServiceUrl=http://localhost:9000 -Bucket=ias-content -AccessKey=key -SecretKey=secret -StreamOnDemand -WriteTocToDisk -KeepContainerFiles -KeepPakFiles -TargetPlatform=Win64 -BuildVersion=1 -ConfigFilePath=E:\path\to\Cloud\IoStoreOnDemand.ini"
    ```
    注意：非 `localzen` 时，`-Upload` 的值直接作为 `IasTool.exe` 的参数传递（见源码 4648-4651 行的 `else` 分支）
- `.ucas/.utoc/.uondemandtoc` 文件被排除不随包分发

## 四、IasTool 命令行工具

`IasTool` 是独立的命令行程序，用于将 OnDemand 容器上传到云端。

### 4.1 Upload 命令

```
IasTool Upload <ContainerGlob> [options]
```

核心参数：

| 参数 | 说明 |
|------|------|
| `ContainerGlob` | 输入容器路径/glob |
| `-ServiceUrl` | S3服务URL |
| `-Bucket` | S3桶名 |
| `-Region` | AWS区域 |
| `-AccessKey / -SecretKey` | S3凭证 |
| `-BucketPrefix` | 对象前缀路径 |
| `-StreamOnDemand` | 标记为流式OnDemand |
| `-InstallOnDemand` | 标记为安装OnDemand |
| `-BuildVersion` | 嵌入TOC的构建版本 |
| `-TargetPlatform` | 目标平台 |
| `-HostGroupName` | 主机组名称 |
| `-WriteTocToDisk` | 同时将TOC写出到本地 |
| `-PerContainerTocs` | 每个容器生成独立TOC |
| `-CryptoKeys` | 加密密钥JSON文件 |
| `-KeepContainerFiles` | 上传后不删除容器文件 |
| `-KeepPakFiles` | 不删除pak跳板文件 |
| `-IgnoreContainerHeader` | 不将容器头序列化到TOC |
| `-MaxConcurrentUploads` | 并发上传数（默认16） |
| `-MaxTocListCount` | 最大TOC列表数量 |
| `-MaxTocDownloadCount` | 最大TOC下载数量 |

### 4.2 Upload 流程

1. **读取容器文件** — 打开 `.utoc`/`.ucas` 读取 OnDemand 标记的容器
2. **去重检查** — 从S3下载已有TOC，收集已上传chunk的hash，跳过重复
3. **上传chunk** — 每个chunk按 `{prefix}/Chunks/{hash前2位}/{hash}.iochunk` 路径上传
4. **上传utoc** — 将原始 `.utoc` 文件上传到S3
5. **生成FOnDemandToc** — 合并所有容器的元数据为统一的 `.iochunktoc` 文件
6. **上传TOC** — TOC以hash命名上传 `{hash}.iochunktoc`
7. **可选写配置** — 生成 `IoStoreOnDemand.ini` 配置文件
8. **可选写容器TOC** — 若启用 `-PerContainerTocs`，为每个容器生成 `.uondemandtoc` 文件

### 4.3 TOC文件格式（FOnDemandToc）

```cpp
// OnDemandToc.h
FOnDemandTocHeader {
    Magic = 0x6f6e64656d616e64;  // "ondemand"
    Version;           // 当前最新 = 12
    Flags;             // InstallOnDemand | StreamOnDemand
    BlockSize;
    CompressionFormat;
    ChunksDirectory;   // chunk在S3上的相对路径
    HostGroupName;     // 关联的主机组
}
FTocMeta { EpochTimestamp, BuildVersion, TargetPlatform }
TArray<FOnDemandTocContainerEntry> Containers;  // 各容器条目
TArray<FOnDemandTocAdditionalFile> AdditionalFiles;
TArray<FOnDemandTocTagSet> TagSets;             // 标签分组（用于按标签安装）
```

### 4.4 配置文件输出

Upload 时若指定 `-WriteTocToDisk -ConfigFilePath=...`，会生成 `IoStoreOnDemand.ini`：

```ini
[Endpoint]
DistributionUrl="https://cdn.example.com"
FallbackUrl="https://fallback.example.com"
ServiceUrl="https://s3.amazonaws.com"
TocPath="bucket/prefix/{toc-hash}.iochunktoc"
ContentKey="{GUID}:{Base64-AES-Key}"
```

## 五、运行时使用

### 5.1 初始化

`IoStoreOnDemand` 模块启动时注册 `IOnDemandIoStoreFactory` 作为 ModularFeature：

```cpp
// OnDemandModule.cpp
void StartupModule() {
    PlatformSocketSystem.Startup();
    IModularFeatures::Get().RegisterModularFeature(
        IOnDemandIoStoreFactory::FeatureName, Factory.Get());
}
```

获取实例：

```cpp
#include "IO/IoStoreOnDemand.h"

IOnDemandIoStore* OnDemandIoStore = UE::IoStore::TryGetOnDemandIoStore();
```

### 5.2 注册主机组

```cpp
FOnDemandRegisterHostGroupArgs Args;
Args.HostGroupName = "MyCDN";
Args.HostNames = {"https://cdn1.example.com", "https://cdn2.example.com"};
Args.bUseSecureHttp = true;
auto Result = OnDemandIoStore->RegisterHostGroup(MoveTemp(Args));
if (Result.IsOk()) {
    // Result.HostGroup 可用于后续挂载
}
```

### 5.3 挂载OnDemand容器

```cpp
FOnDemandMountArgs Args;
Args.MountId = "MyContent";
Args.HostGroup = Result.HostGroup;  // 或指定 HostGroupName
Args.TocRelativeUrl = "path/to/content.iochunktoc";
Args.Options = EOnDemandMountOptions::StreamOnDemand
             | EOnDemandMountOptions::InstallOnDemand;

OnDemandIoStore->Mount(MoveTemp(Args),
    [](FOnDemandMountResult Result) {
        if (Result.Status.IsOk()) {
            // 挂载成功
        }
    });
```

挂载选项 `EOnDemandMountOptions`：

| 选项 | 说明 |
|------|------|
| `StreamOnDemand` | 流式按需加载（按chunk请求） |
| `InstallOnDemand` | 安装按需加载（先下载到本地缓存） |
| `CallbackOnGameThread` | 回调在游戏线程 |
| `WithSoftReferences` | 使软引用可用 |
| `ForceNonSecureHttp` | 强制HTTP |

也可以通过文件路径或内存中的TOC挂载：

```cpp
// 从本地文件挂载
Args.FilePath = "C:/Path/to/content.uondemandtoc";

// 从内存中的序列化TOC挂载
Args.Toc = MakeUnique<FOnDemandToc>(...);
```

### 5.4 安装内容

```cpp
FOnDemandInstallArgs Args;
Args.MountId = "MyContent";
Args.PackageIds = {PackageId1, PackageId2};  // 按包ID
// 或 Args.TagSets = {"HD_Textures"};         // 按标签
Args.ContentHandle = FOnDemandContentHandle::Create("MyInstall");
Args.Options = EOnDemandInstallOptions::InstallSoftReferences;

auto Request = OnDemandIoStore->Install(
    MoveTemp(Args),
    [](FOnDemandInstallResult Result) {
        if (Result.IsOk()) {
            // 安装完成
            uint64 Downloaded = Result.Progress.CurrentInstallSize;
        }
    },
    [](FOnDemandInstallProgress Progress) {
        float Pct = Progress.GetRelativeProgress(); // 0.0 - 1.0
        uint64 CachedBytes = Progress.GetCachedSize();
        uint64 TotalBytes = Progress.GetTotalSize();
    }
);

// 可取消
Request.Cancel();

// 可调整优先级
Request.UpdatePriority(10);
```

安装选项 `EOnDemandInstallOptions`：

| 选项 | 说明 |
|------|------|
| `CallbackOnGameThread` | 回调在游戏线程 |
| `InstallSoftReferences` | 跟随软引用 |
| `InstallOptionalBulkData` | 安装可选BulkData |
| `DoNotDownload` | 仅检查缓存，不下载 |
| `AllowMissingDependencies` | 允许缺失依赖 |

### 5.5 获取安装大小

```cpp
FOnDemandGetInstallSizeArgs Args;
Args.MountId = "MyContent";
Args.TagSets = {"HD_Textures"};
auto SizeResult = OnDemandIoStore->GetInstallSize(Args);
if (SizeResult.IsOk()) {
    uint64 TotalSize = SizeResult.ValueOrDie().InstallSize;
    uint64 DownloadSize = SizeResult.ValueOrDie().DownloadSize.GetValue();  // 需ContentHandle
}
```

### 5.6 缓存管理

```cpp
// 清理缓存
OnDemandIoStore->Purge(
    FOnDemandPurgeArgs{.BytesToPurge = 1024*1024*100}, // 清理100MB
    [](FOnDemandPurgeResult Result) {}
);

// 碎片整理
OnDemandIoStore->Defrag(
    FOnDemandDefragArgs{.BytesToFree = 1024*1024*50},
    [](FOnDemandDefragResult Result) {}
);

// 验证缓存
OnDemandIoStore->Verify([](FOnDemandVerifyCacheResult Result) {});

// 查看缓存使用情况
FOnDemandCacheUsage Usage = OnDemandIoStore->GetCacheUsage({});
// Usage.InstallCache.MaxSize / TotalSize / ReferencedSize / FragmentedChunksSize
// Usage.StreamingCache.MaxSize / TotalSize
```

### 5.7 内容句柄（ContentHandle）

`FOnDemandContentHandle` 用于 pin 住缓存中的内容，防止被清理：

```cpp
// 创建句柄
FOnDemandContentHandle Handle = FOnDemandContentHandle::Create("MyDLC");

// 安装时关联句柄
InstallArgs.ContentHandle = Handle;

// 获取安装大小时 pin 现有chunk
GetInstallSizeArgs.ContentHandle = Handle;

// 释放句柄（允许缓存清理）
Handle.Reset();
```

### 5.8 卸载

```cpp
OnDemandIoStore->Unmount("MyContent");
```

## 六、两种加载模式

| 模式 | 说明 | TOC Flag | 实现类 |
|------|------|----------|--------|
| **StreamOnDemand** | 通过 `OnDemandIoDispatcherBackend` 拦截 IoDispatcher 请求，实时 HTTP 下载缺失 chunk 到内存 | `EOnDemandTocFlags::StreamOnDemand` | `FOnDemandIoDispatcherBackend` |
| **InstallOnDemand** | 通过 `OnDemandContentInstaller` 预下载到磁盘缓存 `OnDemandInstallCache`，之后从本地读取 | `EOnDemandTocFlags::InstallOnDemand` | `FOnDemandContentInstaller` |

### StreamOnDemand 流程

```
IoDispatcher 请求 chunk
  → OnDemandIoDispatcherBackend 拦截未命中
    → OnDemandHttpThread 发起 HTTP 请求下载 chunk
      → 下载后解码（解压+解密）返回给 IoDispatcher
```

### InstallOnDemand 流程

```
调用 Install()
  → 解析 PackageId/TagSet → 找到对应容器和 chunk
    → FOnDemandContentInstaller 排队安装请求
      → HTTP 批量下载 chunk → 写入 OnDemandInstallCache
        → 完成回调通知
```

## 七、Chunk URL 构造规则

```
http(s)://<Host>/<ChunksDirectory>/chunks/<Hash前2位>/<Hash>.iochunk
```

示例：
```
https://cdn.example.com/bucket/prefix/Chunks/3a/3a7f8b...c2.iochunk
```

## 八、端到端工作流

```
1. 打包配置
   DefaultGame.ini → PakFileRules → bOnDemand=true
   （或使用 -ApplyIoStoreOnDemand 自动创建默认规则，无需手动配置）
   
2. 项目打包
   UAT BuildCookRun -ApplyIoStoreOnDemand
   → 生成 ondemand .ucas/.utoc 容器
   → 容器带 EIoContainerFlags::OnDemand 标志
   
3. 上传云端（两种方式）
   
   方式A：UAT 自动上传（推荐）
   UAT BuildCookRun -ApplyIoStoreOnDemand -Upload="Upload <PakPath> -ServiceUrl=... -Bucket=... ..."
   → 打包完成后 UAT 自动调用 IasTool Upload
   
   方式B：手动上传
   IasTool Upload *.utoc -ServiceUrl=... -Bucket=... -StreamOnDemand
   → 生成 .iochunktoc 上传到S3
   → chunk文件上传到S3
   → 生成 IoStoreOnDemand.ini 配置文件
   
4. 客户端配置
   将 IoStoreOnDemand.ini 随包分发
   
5. 运行时
   模块初始化 → 读取 IoStoreOnDemand.ini
   → RegisterHostGroup 注册CDN端点
   → Mount 挂载TOC（自动从HTTP下载TOC）
   → Install 或 StreamOnDemand 按需加载内容
```

## 九、关键源码文件索引

| 文件 | 说明 |
|------|------|
| `Engine/Source/Runtime/Experimental/IoStore/OnDemandCore/Public/IO/IoStoreOnDemand.h` | 核心公共接口 |
| `Engine/Source/Runtime/Experimental/IoStore/OnDemandCore/Public/IO/OnDemandToc.h` | TOC格式定义 |
| `Engine/Source/Runtime/Experimental/IoStore/OnDemandCore/Public/IO/OnDemandHostGroup.h` | 主机组定义 |
| `Engine/Source/Runtime/Experimental/IoStore/OnDemandCore/Public/IO/HttpIoDispatcher.h` | HTTP调度器 |
| `Engine/Source/Runtime/Experimental/IoStore/OnDemand/Private/OnDemandIoStore.h` | 核心实现类 |
| `Engine/Source/Runtime/Experimental/IoStore/OnDemand/Private/OnDemandIoStore.cpp` | 核心实现 |
| `Engine/Source/Runtime/Experimental/IoStore/OnDemand/Private/OnDemandContentInstaller.h` | 内容安装器 |
| `Engine/Source/Runtime/Experimental/IoStore/OnDemand/Private/OnDemandConfig.cpp` | 运行时配置解析 |
| `Engine/Source/Runtime/Experimental/IoStore/OnDemand/Private/OnDemandModule.cpp` | 模块注册 |
| `Engine/Source/Programs/IoStoreOnDemand/Private/IasTool.cpp` | IasTool入口 |
| `Engine/Source/Programs/IoStoreOnDemand/Private/Tool/Upload.cpp` | Upload命令实现 |
| `Engine/Source/Programs/IoStoreOnDemand/Private/Tool/ChunkPlugin.cpp` | ChunkPlugin命令 |
| `Engine/Source/Programs/AutomationTool/Scripts/CopyBuildToStagingDirectory.Automation.cs` | 打包阶段OnDemand处理 |

## 十、实践验证（IOStoreDemo 项目）

### 10.1 项目配置

**DefaultGame.ini**:
```ini
[/Script/UnrealEd.ProjectPackagingSettings]
bCookAll=True
bGenerateChunks=True
```

**DefaultDeviceProfiles.ini** (将贴图 mip0 分离到 OnDemand):
```ini
[GlobalDefaults DeviceProfile]
+TextureLODGroups=(Group=TEXTUREGROUP_World,OptionalLODBias=1)
+TextureLODGroups=(Group=TEXTUREGROUP_Character,OptionalLODBias=1)
; ... 其他贴图组
```

> **注意**: `OptionalLODBias=1` 表示将最高一级 mip (mip0) 剥离到 optional bulkdata package，这些会被打包到 `OnDemandOptionalPak`。

### 10.2 MinIO 本地服务

使用 MinIO 作为本地 S3 兼容存储，模拟云端 CDN：

| 配置 | 值 |
|------|-----|
| API 端口 | 9000 |
| Console 端口 | 9001 |
| AccessKey | minioadmin |
| SecretKey | minioadmin |
| Bucket | ias-content |

启动命令:
```powershell
IoStoreServer\start.bat
```

### 10.3 打包命令

```powershell
RunUAT BuildCookRun ^
  -project=IOStoreDemo.uproject ^
  -platform=Win64 ^
  -build -cook -stage -pak -archive ^
  -archivedirectory=Archives\Windows ^
  -ApplyIoStoreOnDemand ^
  -GenerateOnDemandPakForNonChunkedBuild ^
  -GenerateOptionalPakForNonChunkedBuild ^
  -Upload="Upload E:\UEProject\IOStoreDemo\Saved\StagedBuilds\Windows\IOStoreDemo\Content\Paks -ServiceUrl=http://localhost:9000 -Bucket=ias-content -AccessKey=minioadmin -SecretKey=minioadmin -StreamOnDemand -WriteTocToDisk -KeepContainerFiles -KeepPakFiles -TargetPlatform=Win64 -BuildVersion=1 -ConfigFilePath=E:\UEProject\IOStoreDemo\Saved\StagedBuilds\Windows\Cloud\IoStoreOnDemand.ini"
```

### 10.4 打包输出

生成的 OnDemand 容器：

| 文件 | 内容 | 大小 | 条目数 |
|------|------|------|--------|
| `pakchunk0ondemand-Windows.ucas` | BulkData | ~5 MB | 140 |
| `pakchunk0ondemandoptional-Windows.ucas` | 贴图 mip0 | ~7 MB | 35 |
| `pakchunk1ondemand-Windows.ucas` | DLC BulkData | ~7 MB | 65 |

### 10.5 运行时验证

日志关键输出：
```
LogIoStoreOnDemand: Loading TOC from file 'Cloud/39a6885472af60eaa36ffdadda30616ee18a67ad.iochunktoc'
LogIoStoreOnDemand: Mounting container 'pakchunk0ondemand-Windows', Entries=140, Flags='StreamOnDemand'
LogIoStoreOnDemand: Mounting container 'pakchunk0ondemandoptional-Windows', Entries=35, Flags='StreamOnDemand'
LogIoStoreOnDemand: Mounting container 'pakchunk1ondemand-Windows', Entries=65, Flags='StreamOnDemand'
LogIas: Endpoint 'http://localhost:9000' latency test (ms): 7 1 0 0
LogIas: Display: HostGroup init took 0.010 seconds (Succeeded)
LogIas: HTTP streaming enabled 'true'
LogIas: IAS - HttpStats: DownloadedKiB=11699, Get=172, Retry=2, Error=0
```

### 10.6 缓存目录结构

```
Saved/PersistentDownloadDir/
├── ias/
│   ├── ias.cache.0          # IAS 缓存数据 (~6 MB)
│   └── ias.cache.0.jrn      # 缓存日志
└── IoStore/InstallCache/
    ├── blocks/              # 缓存块目录
    └── cas.jrn              # CAS 日志 (CASJOURNALHEADER + entries + CASJOURNALFOOTER)
```

### 10.7 常见问题排查

**问题**: 打包后没有生成 `pakchunk*ondemand*` 文件
- 检查是否使用 `-ApplyIoStoreOnDemand` 参数
- 检查 `DefaultGame.ini` 是否配置 `bGenerateChunks=True`

**问题**: 运行时无法下载 OnDemand 内容
- 检查 MinIO 服务是否运行 (`http://localhost:9000/minio/health/live`)
- 检查 `IoStoreOnDemand.ini` 中的 `ServiceUrl` 是否正确
- 检查 `ias-content` bucket 是否存在

**问题**: 贴图 mip0 没有分离
- 检查 `DefaultDeviceProfiles.ini` 配置是否正确
- 注意 section 名称应为 `[GlobalDefaults DeviceProfile]`（UE 5.3+）

**问题**: 运行日志出现 Warning `deprecated ini section name`
- 将 `[/Script/Engine.TextureLODSettings]` 改为 `[GlobalDefaults DeviceProfile]`

## 十一、工具

### 11.1 OnDemandCacheAnalyzer

解析下载缓存目录的工具：

```powershell
python Tools\OnDemandCacheAnalyzer.py "Archives\Windows\IOStoreDemo\Saved\PersistentDownloadDir" [-v]
```

输出示例：
```
============================================================
IoStoreOnDemand Cache Analysis Report
============================================================

Directory: E:\UEProject\IOStoreDemo\Archives\Windows\IOStoreDemo\Saved\PersistentDownloadDir

----------------Summary-----------------
  Total Files: 3
  Total Size:  6.36 MB

---------CAS Journal Statistics---------
  Total Entries:      0
  ...
```
