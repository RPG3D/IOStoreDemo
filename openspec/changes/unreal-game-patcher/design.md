# UnrealGamePatcher 设计

## Context

动机见 proposal.md - Why。当前项目为 UE5 工程（IOStoreDemo），本次新增独立的 UE Program `UnrealGamePatcher`，针对 pak 与 IoStore（.ucas/.utoc）做增量更新。格式细节以引擎源码为准，参考 `D:\Code\repak`（Rust：pak 读写/签名/加密）、`D:\Code\UAssetAPI`（C#：pak/ucas/utoc 解析）、云风《resource_pack》（大文件打包与索引思路）。

关键格式事实（决定设计）：
- **pak**：数据区（文件条目按 32 字节对齐连续存储）→ 目录索引 → footer（含版本、索引偏移/大小）。条目含压缩块数组（`FPakEntry::CompressionBlocks`），数据按文件块随机读取。
- **IoStore**：utoc = container header + block/chunk entries；chunk id 是**内容 hash**；chunk 的 block 序列**必须自 `OffsetInContainer` 起连续**（因此变化 chunk 必须整体连续重写，不能只搬单 block）。
- 引擎运行时**不提供** pak/utoc 的写入 API；读取可参考 `Runtime/PakFile` 与 `Runtime/Core` 的 IoStore。

## Goals / Non-Goals

**Goals:**
- 只下载差异 chunk、只写差异数据；未变化数据零搬动、零重写。
- pak 与 IoStore 两条路径共用同一套 index / 下载 / 事务 / CLI / GUI 框架。
- 构建侧工具与客户端 patcher 在同一 Program 内，子命令区分。
- 机械硬盘友好的写策略（差异小 → 原地写；差异分散且大 → 可选顺序重写回退）。

**Non-Goals:**
- 首版不支持 AES 加密与 .sig 签名（保留扩展点：格式解析预留加密标志位处理与报错）。
- 不做 CDN 服务器/上传管线本身，只产出 CDN 静态文件布局。
- 不做游戏启动器（更新器只负责内容更新与"完成后启动游戏"）。
- 移动端/主机平台首版不验证（Program 结构保持跨平台，Windows 优先）。
- 不做 IoStore 部分下载 / on-demand 虚拟化（仅本地全量 chunk 处理）。

## Decisions

### D1. 工程结构与模块划分

**单 Program target**：默认形态为 GUI（Slate 窗口）；命令行加 `-cli` 参数即以 CLI 模式运行（不创建窗口、输出文本日志），便于 AI / CI / 启动器调用与调试。Program 放**项目代码** `Source/Programs/UnrealGamePatcher/`（不新建独立工程、不复制引擎），在项目 sln 中以 `SolutionDirectory` 归组。布局：

```
<Project>/Source/Programs/UnrealGamePatcher/
  UnrealGamePatcher.Target.cs    # 单 Program target（GUI 默认，-cli 切换 CLI 模式）
  UnrealGamePatcher.Build.cs     # ModuleRules（Slate 系 + Core 系 + Launch）
  Public/  Private/              # 实现
```

- **target 模板**：参考 `SlateViewer.Target.cs` / `SlateViewer.Build.cs`（`Type = TargetType.Program`、`bCompileAgainstEngine = false`、`StandaloneRenderer` + Slate 依赖）与 `SlateViewerApp.cpp` 的 `RunSlateViewer(CommandLine)` + `IMPLEMENT_APPLICATION` 入口模式；CLI 侧行为参考 `UnrealPak`（`-cli` 时按 console 输出与退出码约定运行）。主入口 `RunUnrealGamePatcher(CommandLine)` 内以 `FParse::Param(CommandLine, TEXT("cli"))` 分流：CLI 路径不初始化 Slate 渲染、直接跑 orchestrator 并输出文本；GUI 路径走 Slate 窗口。
- **sln 位置**：在 target `.Target.cs` 中设 `SolutionDirectory`（用法参考 `UnrealBuildTool/Configuration/Rules/TestTargetRules.cs` 的 `SolutionDirectory = "Programs/LowLevelTests"`），本工程设为 `"Programs/UnrealGamePatcher"`。
- **模块划分**（单 target 内，逻辑仍分层、无 UI 逻辑下沉）：
  - `Core`：格式层 / 应用引擎 / 下载 / 事务 / 状态（无 UI 依赖）
  - `Build`：构建侧 ingest / diff / 产物布局
  - `CLI`：CLI 模式入口与文本输出（`-cli` 时启用）
  - `GUI`：Slate 界面（worker 线程跑 orchestrator，轮询进度）
  - 主模块（Launch）：`RunUnrealGamePatcher` 分流

**为什么**：单 target 避免双 exe 的构建/分发/维护成本；`-cli` 参数让 AI/CI 以同一 exe 直接调试核心逻辑而无需 GUI 交互，且 Slate 依赖对单 exe 无额外成本（CLI 模式只是不开窗口，不启动渲染）。模块仍分层，CLI 与 GUI 仅入口差异，orchestrator 全程共享。**备选**：双 target（维护两个 exe、入口重复，用户已否决）；`-cli` 只作为 GUI 内部隐藏参数而非一等入口（不利于 AI/CI 发现与稳定输出，否）。

### D2. 自研格式层（读+写一体），不复用引擎只读 API

pak / utoc 的解析与写回**全部自研**（`Core/Formats/`），引擎仅用于低级能力（`FCompression` 解压/压缩、`FIoHash`/hash、`FPaths`、`FArchive`）。参考来源：

- 引擎只读 API 与数据结构：`Runtime/PakFile`（FPakFile、FPakEntry、FPakInfo）、`Runtime/Core` IoStore（FIoStoreReader、FIoChunkId、FIoContainerHeader）。
- 引擎 Developer 模块（可直接复用或移植）：**`PakFileUtilities`**（pak 创建/重打包、条目与压缩块序列化、SignedArchiveWriter）、**`IoStoreUtilities`**（引擎自带 **`IoStoreWriter`**，即 utoc/ucas 写入实现——我们的 IoStore 写入器直接参考其 block/chunk 布局、对齐与序列化逻辑）。这两个模块由 UnrealPak 依赖，与 Program 构建体系兼容。
- 外部参考（仅格式参考）：`D:\Code\repak`（Rust，pak 读写/签名/加密）、`D:\Code\UAssetAPI`（C#，pak/ucas/utoc 解析）。

**为什么**：diff 需要访问原始字节与块级偏移，引擎 `FPakFile`/`FIoStoreReader` 是只读高层 API 且绑定自身 `FArchive` 生命周期；读用引擎、写用自研会造成两套条目模型不一致的风险。自研格式层同时服务构建侧与客户端，逻辑唯一；但条目序列化、压缩块布局、对齐等"格式事实"尽量从 PakFileUtilities/IoStoreWriter 移植而非重发明，降低兼容性风险。**备选**：复用引擎只读 API + 自研写入器——两套模型，否决；直接改引擎 `Runtime/PakFile` 加写入 API——改动引擎源码，污染升级路径，否。

格式层直接产出：
- `FPakReader/FPakWriter`：footer/索引/条目/压缩块的读与写（支持多 pak 版本，从 footer version 分发）。
- `FIoStoreReader/FIoStoreWriter`：utoc/ucas 的读与写（支持 ContainerVersion 分发）。
- 压缩方法：首版 Oodle/zlib/LZ4（`FCompression` 统一入口）。

### D3. 清单体系：UE 官方 manifest 为底座 + 扩展清单

**复用 UE 打包产出的官方 manifest** 作为文件级清单与文件 hash 的唯一来源，最大化兼容性（清单可被 UE 生态/BuildPatchTool/SteamPipe 等读取）；我们的扩展只描述 pak/ucas 差异与更新策略。**实施第一步即启用 manifest 打包并 dump 其实际格式**（见 tasks 3.1），字段以实测为准落地解析。

- **底座**：官方 manifest——`BuildPatchServices` 的 `FBuildPatchAppManifest`（JSON：文件 → MD5/SHA1 hash + 大小；chunk → GUID + SHA1 + 大小），由 `BuildPatchTool` 生成（`PackageChunksMode` 等）。文件级完整性以官方 hash（MD5/SHA1）为准。
- **扩展清单** `UnrealGamePatcher.index.json`（JSON，自研）：
  - 头：`formatVersion` / `contentVersion` / `baseVersion`（跳版本关联）/ 生成信息 / 自校验。
  - `targets[]`：每个 pak/ucas 目标 → 更新策略（`incremental`）+ chunk 列表（id / offset / size / rawSize；**pak 复用条目内置每文件 hash**（`FPakEntry::Hash`，打包时对未压缩数据计算的 SHA1，粒度即文件块），**IoStore 直接用 utoc chunk id**——两者均不自算内容 hash；xxhash64 仅用于扩展清单自校验与 CDN chunk 文件下载校验）。
  - 普通文件（dll/exe 等）不在此列：策略即"全量"，由底座 manifest 驱动。
- **拆两层而非在官方 manifest JSON 里塞自定义字段**：官方 schema 严格（`FeatureLevel` 序列化），加字段破坏兼容；两层清单各司其职，官方层保持纯净。
- **差异判断**：pak 按条目内置 hash（每文件 SHA1）对比本地条目（同 hash ⇒ 同内容，天然去重）；IoStore 按 utoc chunk id 对比（id 即内容 hash）；普通文件按底座 manifest 文件级 hash 对比。**本地真相**：pak 目录 / utoc 即本地当前版本（sidecar 只存空洞与事务，见 D5），diff = 本地解析出的 chunk 清单 vs 远端扩展清单。边界：若某 pak 条目缺失内置 hash（异常/旧产物），扩展清单记录构建侧计算的 hash 兜底（spec 允许）。
- **完整性校验语义**：pak chunk 下载后解压 → 按未压缩数据 SHA1 校验（与内置 hash 语义一致）；IoStore chunk 按 chunk id 对应 hash 校验；CDN 传输层校验用扩展清单记录的 hash。

### D4. pak 增量应用算法

```
pak = [ 文件数据区（文件块连续、32B 对齐） ][ 目录索引 ][ footer ]
```

1. **差异分析**：解析本地 pak 目录 → chunk 清单；与远端 index 对比 → changed/added/removed。
2. **应用**：
   - changed 块压缩尺寸 == 原尺寸（且 32B 对齐不变）→ **原地覆盖**（`pwrite` 到原偏移），零搬动。
   - 其余（尺寸变化 / added）→ **append 到文件末尾**（旧目录之后），按 32B 对齐；旧目录区成为空洞。
3. **索引刷新**：读取旧目录 → 内存中重建目录（新条目偏移）→ 追加的新数据之后写入新目录索引 + 新 footer。
   - 顺序：新数据 append → 新目录 + footer append。全程无数据搬动；文件末尾的旧目录/旧 footer 成为空洞（紧凑回收见 D7）。
4. 空洞与事务日志写入 sidecar（D5）。

**为什么 append 在"旧目录之后"而非"目录之前"**：零搬动、实现最简、崩溃安全（旧目录一直可用到新目录写入完成）。代价是每次尺寸变化的更新产生一段尾部空洞，由 compact 回收。**备选**：新数据插入目录位置并搬移旧目录——多一次搬动与失败风险，否。

### D5. sidecar 状态与事务

每安装目录一个 sidecar：`UnrealGamePatcher.state.json`，记录：
- 空洞列表（offset/size，供 compact 与统计）。
- 事务日志（本次更新每个目标文件的操作：覆盖偏移/原始块 hash、追加偏移/大小）。
- 进行中标记（pending/committed/rolled-back）+ 崩溃恢复状态。

事务模型（两阶段）：
1. **DOWNLOAD**：差异 chunk 全部下载到临时区并通过 hash 校验（spec client-patcher 下载要求）。
2. **APPLY**：对每个目标文件，先备份将被覆盖的原始块到临时区（记录 hash），执行覆盖/追加，写 sidecar 日志。
3. **VERIFY**：对写入块 hash 校验。
4. **COMMIT**：全部通过 → 刷新本地 index 副本、清空洞记录更新、删除备份 → 标记完成。任一失败 → **ROLLBACK**：从备份恢复覆盖块、按日志截断追加区、恢复旧目录/旧 footer。

崩溃恢复：下次启动读到 pending 事务 → 依据日志回滚或（若备份完整且远端可再取）重做。spec 的"恢复至更新前状态或标记为可修复"即此机制。

### D6. IoStore 增量应用算法

1. **差异分析**：解析 utoc → chunk 清单（id=hash）；与远端 index 对比 → 差异 chunk 集合（构建侧已算好闭包，见 D8，客户端无脑应用清单）。
2. **应用**（按 chunk 整体）：
   - 差异 chunk 压缩数据尺寸 == 原尺寸 → **原地覆盖**。
   - 尺寸变化 / added → 该 chunk 的 **block 序列整体重写**：从 ucas 现有内容（或本地其它 chunk 数据）读未变化部分 → 解压 → 合并修改 → 重新压缩 → **append 到 ucas 末尾（按 block 对齐）** → 重写 utoc 中该 chunk 条目（OffsetInContainer / Size / 压缩块偏移数组）。未变化 chunk 数据零搬动。
   - 对齐：container block size（默认 1MB 量级）对齐约束在 Writer 内统一处理。
3. **索引刷新**：重写后的 utoc append（或整体重写 utoc——chunk 数万级、几十 MB，追加后重写 utoc 文件本身）→ 旧 utoc 区域空洞记录。
   - 决策：**utoc 整体重写**（新 utoc = 重写条目 + 未变条目，写为临时文件再原子替换），因为 utoc 是纯索引、体积小，整体重写比"追加重写部分"更简单可靠。旧 utoc 备份用于回滚。
4. 空洞与事务入 sidecar（D5）。

**关键约束**：IoStore chunk 的 block 序列必须连续（自 OffsetInContainer），故不能"只搬单 block"——变化 chunk 整体连续重写（见 spec iostore-patching 修正版）。

### D7. 空洞管理 & compact

- 空洞只记录、不自动复用（首版）：追加优先于复用，避免碎片化与"空洞内放不下"的复杂匹配。空洞由 **compact** 统一回收。
- **compact**：读取当前有效数据（解析目录/utoc，按有效块流式复制）→ 写紧凑新文件（临时文件）→ 原子替换 → 清空空洞记录。compact 后文件与更新前逻辑等价（spec pak-patching 紧凑回收要求）。
- 触发策略（可配置）：空洞总字节 > 阈值（默认：> 目标文件大小 10% 或 > 512MB）时，check 命令提示/update 命令可选自动执行。

### D8. 构建侧工具

同一 Program 的 `build` / `diff` 子命令，输入为打包输出目录（UnrealPak 的 .pak 或 IoStore 的 .ucas/.utoc），输出为 CDN 布局：

```
<contentVersion>/
  index.json
  chunks/                  # chunk id 命名（xxhash64 / FIoChunkId hex）
    <chunkid>.bin
  manifest.txt             # 全量 chunk 清单（供上传/校验脚本）
```

- **ingest（build）**：解析打包输出 → 生成 index + 按 chunk 切分文件（pak 按文件块，IoStore 按 chunk）。确定性构建（spec build-tooling 确定性要求）：稳定遍历顺序、固定对齐、无时间戳嵌入 index（generatedAt 放 manifest 而非 index，保证 index 字节稳定）。
- **diff**：两 index 对比 → 差异 chunk 集合 + 差异产物（增量目录，含差异清单）。**IoStore 闭包在此计算**：利用引擎的引用链（或保守策略：变化 package 的依赖 chunk 全部纳入）生成闭包，保证"更新集无悬垂引用"（spec iostore-patching 更新闭包要求）——客户端只按清单应用。
- 版本共存与跳版本：产物按 contentVersion 目录并存；diff 支持任意两个已发布版本。

### D9. 下载层：可插拔下载器 + 默认 HTTP 实现

- **抽象接口** `IChunkDownloader`（Core 内，业务层可扩展）：
  - `DownloadRange(const FString& Url, int64 Offset, int64 Size, ...)`——按 URL + Range 请求字节区间（覆盖 chunk 请求与断点续传）；含进度回调、取消、失败语义。
  - 更新流程（下载调度、并发、断点续传、hash 校验）**只依赖该接口**，与具体后端解耦（spec client-patcher 可插拔下载后端要求）。
- **默认实现** `FHttpChunkDownloader`：基于引擎 **FHttpModule**（内置、成熟、异步 + Range 头）。并发下载数默认 8，可配置；chunk 先写 `<chunkid>.part`，完成校验后改名 `.bin`；重启复用已下载字节（断点续传）。
- **扩展点**：业务层通过注册/工厂注入自定义 `IChunkDownloader`（私有 CDN 协议、P2P、加密通道等），CLI/GUI 均可用同一注入点指定。
- **下载校验**：每个 chunk 下载完按扩展清单 hash 校验（spec index-format 下载后校验；pak 解压后按未压缩数据 SHA1，见 D3）。
- **为什么**：FHttpModule 满足默认场景且零额外依赖；抽象层是用户明确要求的扩展位——本工具定位"框架 + 默认实现"，业务层（如自建 P2P/CDN）直接替换后端而更新流程不变。**备选**：直接在各处调用 FHttpModule——不可扩展，否。

### D10. 写策略：原地随机写 vs 顺序重写回退

机械硬盘上，分散的小随机写（seek）可能比顺序写更慢。**智能策略**（可配置，默认开启）：
- 差异总字节小（默认 < 256MB）→ **原地写**（diff chunk 写入，正是本方案的价值点）。
- 差异块数量大且分散（默认 > 2000 块）→ 提示可选用**顺序重写**受影响区域（读-改-写连续区）或全量重写，交由用户/启动器决策。
- 追加永远是顺序写，无此问题。

这是对"机械硬盘体验"目标的直接保障，同时保留极端场景的逃生通道。

### D11. GUI

Slate 独立 Program：主线程跑 UI，更新逻辑跑 worker 线程，通过线程安全进度结构（阶段/百分比/速度/错误）轮询刷新。界面：进度条 + 阶段文本 + 错误/重试 + "完成后启动游戏"。CLI 与 GUI 共用 Core 的 orchestrator（`FUpdateOrchestrator`）。

### D12. 普通文件全量更新

非 pak/ucas 文件（dll / exe / 配置文件等）走全量更新，补齐完整 game patcher 的能力：

- **检测**：底座官方 manifest 的文件级 hash（MD5/SHA1）对比本地 → 变化 / 新增 / 删除（removed 文件按策略删除或保留）。
- **下载**：全量下载到临时区，按 manifest hash 校验。
- **替换**：原子替换（临时文件 + 重命名）；Windows 上被运行进程占用（正在运行的游戏 exe/dll）→ 先要求退出游戏，占用仍存在时用 `MoveFileEx` 延迟替换并标记待处理——更新器约定在游戏未运行时执行。
- **事务**：普通文件替换同样纳入 sidecar 事务日志（备份被替换文件到临时区，记录原 hash），与 pak/ucas 更新共用同一 commit 点：全部写入 + 校验成功才提交，任一失败整体回滚（spec client-patcher 事务性要求覆盖普通文件）。

## Risks / Trade-offs

- **pak/utoc 写回格式兼容性**（自研写器风险）→ 交叉验证：用 repak/UAssetAPI 打开 patcher 写出的文件；自动化测试：打包→更新→引擎 `-game` 加载验证；按 pak/container 版本号做 golden 测试。
- **机械盘随机写**（原地覆盖是随机写）→ D10 智能策略 + 可配置回退；默认差异小时原地写（写量远小于全量，净收益仍大）。
- **空洞膨胀**（追加产生尾部空洞）→ sidecar 记录 + compact 触发策略（D7）；膨胀率可观测（check 命令报告）。
- **IoStore 更新闭包放大下载量**（引用方必须同步更新）→ 构建侧闭包计算尽量精确（最小闭包）；index 暴露预计下载量，客户端展示；对 pak（无闭包问题）不受影响。
- **引擎版本升级改格式** → 格式版本分发（footer/container version）+ 每版本 golden 测试；版本不匹配时明确报错。
- **事务/崩溃恢复复杂性** → sidecar 日志严格两阶段（D5），测试覆盖中断场景（kill -9 模拟）。
- **非加密 hash（xxhash64）无防篡改** → 明确首版范围；后续签名版本在 index 增加签名层，兼容演进（spec index-format 格式兼容演进）。

## Migration Plan

无既有系统迁移；全新项目分阶段交付：

1. **P0**：工程脚手架（Program targets + 模块）+ Core 格式层（pak 读/写 + 空洞）。
2. **P1**：index 格式 + 构建侧 ingest/diff（pak 路径）+ CLI `update`（本地目录模拟远端，无网络）。
3. **P2**：下载（FHttpModule/断点续传）+ 事务/回滚 + sidecar 状态 + CLI `check/compact/repair`。
4. **P3**：IoStore 路径（utoc/ucas 读/写 + chunk 整体重写 + 闭包构建侧）。
5. **P4**：GUI。
6. **P5**：打包管线集成（构建脚本插入 ingest/diff 步骤）+ 端到端验证（打包→发布→更新→引擎加载）。

**回滚**：客户端 sidecar 保留上一版本 index 与备份块，可手动回滚上一版本；服务端保留历史 contentVersion 产物，可随时指向旧版本。

## Open Questions

- 目标引擎小版本与打包格式（pak/IoStore version）以哪个为准——实现时以本机 `D:\Code\UE5` 源码版为准，golden 测试固定。
- CDN 提供方（OSS/S3/自建）与上传方式——不影响 patcher 设计，接入时定。
- compact 与顺序重写回退的默认阈值——实现后以真实数据校准，先取上文默认值。
- IoStore 闭包的最小化精度——首版用保守闭包（变化 package 的依赖全含），后续用引擎引用链优化。
