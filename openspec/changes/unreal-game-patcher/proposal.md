# UnrealGamePatcher — UE5 Pak / Ucas 定制化增量更新器

## Why

Steam / WeGame / Epic 等通用平台对 UE5 游戏做更新时，采用"逐 block 对比 → 合并写入完整新版本"的策略：即使 1GB 的 pak（或 ucas）只更新 1 字节，也要完整写一遍整个文件。对机械硬盘，这意味着一场数十秒到数分钟的连续大块写入，体验极差。过去自研的方案是 `v1_to_v2.patch`、`v2_to_v3.patch` 链式补丁，patch 逐版本累积、无法跳版本、增量失效后必须全量重下，维护成本高。

本项目为 UE5 的 pak 与 IoStore（.ucas/.utoc）文件格式做**定制化增量更新器 UnrealGamePatcher**：CDN 侧保存 index（清单），patcher 下载 index，与本地文件对比得出差异 chunk，仅下载差异 chunk 并**原地覆盖 / 末尾追加**，最后刷新 index。未变化的数据零搬动、零重写，机械硬盘上更新耗时从"全量写入"降到"仅差异写入"。

## What Changes

- 新增 **UnrealGamePatcher**：一个 UE Program（UBT Program target），同时提供命令行（CLI）与 GUI（Slate）两种入口。
- 新增 **构建侧工具**：在 UnrealPak / IoStore 打包输出之上做后处理，生成 CDN index 与按 chunk 切分的下载产物（含差异计算、去重、版本化）。
- 新增 **pak 增量更新能力**：
  - 解析 pak 格式（footer / 目录索引 / 文件条目 / 压缩块），参考引擎 `Runtime/PakFile`、`D:\Code\repak`、`D:\Code\UAssetAPI`。
  - 对比本地 pak 末尾索引与远端 index，得出差异文件块。
  - 差异块尺寸不变 → 原地覆盖；尺寸变化 → 末尾追加并重写索引；记录空洞。
  - 支持紧凑回收（compact / 重打包）策略回收空洞；支持更新后重写签名（签名特性后续版本，首版不做）。
- 新增 **IoStore（.ucas/.utoc）增量更新能力**：
  - utoc 即现成 index（chunk id = 内容哈希），解析 container header / block entries / chunk entries。
  - 以 chunk 为粒度 diff，下载差异 chunk；block 尺寸变化时新 block 追加 + 重写 utoc 偏移表（数据不搬动）。
  - 处理"chunk id = 内容哈希"带来的更新闭包问题（引用方 chunk 需同步更新）。
- 新增 **客户端 patcher 流程**：index 下载 → 差异分析 → chunk 下载（含断点续传）→ 事务性应用（失败回滚）→ 完整性校验 → 刷新 index。
- 新增 **统一清单体系**：**复用 UE 打包产出的官方 manifest**（文件级清单 + hash，兼容 UE 生态 / BuildPatchTool）作为底座，扩展清单描述 pak/ucas 的 chunk 差异与更新策略（pak 用扩展 chunk 清单，IoStore 直接用 utoc 派生）。
- 新增 **普通文件全量更新**：dll / exe / 配置文件等非 pak/ucas 文件按官方 manifest 文件级 hash 检测变化，全量下载并原子替换，与 pak/ucas 更新纳入同一事务。
- 首版范围：标准压缩 pak（Oodle / zlib / LZ4）与未加密 IoStore；**不含** AES 加密与 .sig 签名校验（保留扩展点）。

## Capabilities

### New Capabilities

- `unreal-game-patcher/index-format`: 以 UE 官方 manifest 为底座的统一清单体系（文件级清单/hash 复用官方，扩展描述 pak/ucas chunk 差异、更新策略、版本化与跳版本）。
- `unreal-game-patcher/pak-patching`: pak 文件解析、差异分析、原地覆盖 / 末尾追加应用、空洞管理、compact 回收。
- `unreal-game-patcher/iostore-patching`: .ucas/.utoc 解析、chunk 级 diff 与应用、block 重排、更新闭包处理。
- `unreal-game-patcher/build-tooling`: 构建侧后处理工具，从 UnrealPak / IoStore 输出（含官方 manifest）生成扩展清单与 chunk 下载产物。
- `unreal-game-patcher/client-patcher`: 客户端流程：下载、断点续传、事务性应用与回滚、完整性校验、普通文件全量更新、CLI（`-cli`）与 GUI 入口。

### Modified Capabilities

无（全新系统，不改动既有 spec）。

## Impact

- **新工程**：`UnrealGamePatcher` 作为 UE Program 放在**项目代码 `Source/Programs/UnrealGamePatcher/`**（不新建独立工程、不复制引擎），**单 target**：默认形态为 GUI（Slate），命令行加 `-cli` 参数即以 CLI 模式运行（不创建窗口、输出文本日志，便于 AI / CI / 启动器调用与调试），目标为 `TargetType.Program` + `bCompileAgainstEngine=false`，模板参考引擎 `SlateViewer`（GUI 入口 `RunSlateViewer` + `IMPLEMENT_APPLICATION`）与 `UnrealPak`（CLI 选项）；在 target 的 `.Target.cs` 中设 `SolutionDirectory`（如 `"Programs/UnrealGamePatcher"`，参考 `UnrealBuildTool/Configuration/Rules/TestTargetRules.cs`）以控制 sln 中的分组位置。
- **引擎依赖**：`Core` / `CoreUObject` / `ApplicationCore`（Program 不链接 Engine）；格式解析与写回参考 `Runtime/PakFile`（FPakFile、FPakEntry、FPakInfo）、`Runtime/Core` 的 IoStore（FIoStoreReader、FIoChunkId）；可复用/参考 Developer 模块 `PakFileUtilities`（pak 创建/重打包、条目与压缩块序列化）与 `IoStoreUtilities`（引擎自带 **IoStoreWriter**，即 utoc/ucas 写入实现）。增量写入（原地覆盖/追加）在参考上述模块的基础上自研。
- **官方 manifest 依赖**：复用 `BuildPatchServices` 的 `FBuildPatchAppManifest`（文件 → MD5/SHA1 hash + 大小；chunk → GUID + SHA1 + 大小，JSON）作为清单底座；**打包管线需启用 manifest 产出**（BuildPatchTool / 对应打包配置），实施第一步即先开启并解析其实际格式。
- **外部参考代码**（仅作格式参考，不引入依赖）：`D:\Code\repak`（Rust，pak 读写/签名/加密）、`D:\Code\UAssetAPI`（C#，pak/ucas/utoc 解析）、云风博客《resource_pack》（大文件打包思路）。
- **CDN 侧**：静态托管 index 文件 + 按 chunk 切分的下载文件；支持版本化（多版本共存，支持跳版本更新）。
- **构建管线**：在现有 UnrealPak / IoStore 打包之后插入后处理步骤，产出更新产物。
- **风险点**：pak/utoc 原地写入引擎不原生支持，需要自行维护格式兼容；空洞膨胀需要 compact 机制；IoStore 更新闭包可能放大更新体积。
