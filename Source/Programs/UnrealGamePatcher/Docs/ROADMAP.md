# UnrealGamePatcher 改进计划（Roadmap）

> 本文档是**后续开发的执行计划**：README / TECHNICAL 描述"现在是什么"，
> 本文描述"接下来做什么、为什么这么做"。
> 来源：2026-08-30 全面 review（5 路并行审查，96 项发现，42 项已修复）+
> 下载器专项设计讨论（统一分段模型、元数据上限、模块独立、开源库取舍）。
> 状态：**Phase 0 已实施（2026-08-30）；Phase 1 unified-downloader 已实施（2026-09-02）；Phase 2 未实施**。
> **2026-09-08 全面 review（见 review-2026-10-09-0206.md）**：发现 7 critical（含 C1 ucas 判据前提错误、
> C5 journal 续传主路径失效），P0（C1-C7 + M1/M2/M3 + M5/M11/M12）已当日修复，
> 15/15 套件全绿；P1/P2 待排期。
> **2026-10-09 盲评（review-2026-10-09-0206.md §二）**：H1（pak CoW 在真实规模 pak 的
> 网络模式下因"先取数后判 Same"整体静默退化为全容器重下+重写）+ H2（下载执行器
> 丢弃取消句柄、协作取消为空操作）已修复；新增全局 I/O 统计系统（FIOStats：磁盘
> 读/写、网络下行、压缩/解压、chunk 库加载、哈希量——orchestrator 与原地 patcher
> 均打印，FUpdateStats::IOStats 可断言）与 downloadtest bigpak 多窗口回归
> （断言 CoW 只写变化条目 + 只读其覆盖窗口）；13/13 套件全绿。
> 实施按 Phase 顺序，每项带验收标准。

---

## 0. 总览

| 阶段 | 主题 | 规模 | 触发条件 |
|---|---|---|---|
| **Phase 0** | 正确性快修（传输原语 / 内存上限 / 格式瘦身 / 全新安装体验 / 错误码） | 中小（2-3 天） | 随时可做 |
| **Phase 1** | 下载器通用化 + 独立模块 `UnrealGamePatcherDownload` | 大（核心重构） | 需要整文件/大文件下载能力之前 |
| **Phase 2** | review 遗留的独立问题 | 每项独立 | 逐项立项 |

---

## 1. Phase 0 —— 正确性快修

> 2026-08-30 第二轮补充：§1.4/1.5/1.6 来自 review 收尾 + 真实 debug 实测
> （`-install` 不存在的目录 → code 11 + 乱码消息；`updateStrategy` 只写字段被质疑并确认）。

### 1.1 Range 构造三分支（`FHttpChunkDownloader` L41）

**问题**：`if (Offset > 0)` 导致两个死象限——
- `Offset=0, Size=N`（取前 N 字节）：不发 Range → 服务器回全量 → 长度校验失败（修复前是静默错误数据）
- `Offset>0, Size=-1`（开放结尾）：构造出倒挂区间 `bytes=100-98` → 416

**方案**（实施修正：`Offset=0, Size=N` 在所有现有调用方语义里就是"整个文件（大小 N）"，与"取更大文件前 N 字节"参数不可区分——若对全量 GET 附带 Range 会污染 CDN 缓存/统计（实测 downloadtest B 抓住），故保留"Offset=0 = 整文件、不发 Range"）：
```
Offset > 0 && Size >= 0  →  "bytes=Offset-(Offset+Size-1)"   （精确区间）
Offset > 0 && Size < 0   →  "bytes=Offset-"                  （开放结尾，修复倒挂区间）
Offset == 0              →  不发 Range                        （整个文件）
Size == 0                →  显式拒绝
```
配套：**只要发了 Range 头就要求 206**。"取某更大文件前 N 字节"不在现 API 表达范围，由 Phase 1 的 `FHttpRangeRequest` 显式方法承接。

**验收**：现有调用方（全量/续传/探测）回归 + 续传统计不被全量 Range 请求污染（downloadtest B 实测抓住过）。

### 1.2 传输层硬上限（防 100GB 响应）

**问题**：UE HTTP 整响应缓冲进内存——CDN 返回 100GB 时 OOM/挂死；且发生在每次更新的第一步（latest.json → index）。review 加的长度校验在 `Size=-1` 时失效。

**方案**（两层，传输层是根本）：
- `FHttpChunkDownloader` 增加 `MaxResponseBytes`（默认 **256MB**，传输层不变式："永不缓冲超过 X"）：
  - **`OnHeaderReceived` 查 Content-Length，超限立即 `CancelRequest()`**——body 一个字节都不下
  - `OnRequestProgress` 兜底（服务器谎报 / chunked 无长度时按已收字节取消）
- `FDownloadManager` 对 `Request.Size` 预检（manifest 篡改声明 100GB chunk → 直接判失败）

**验收**：单测——超长 Content-Length 的响应被取消且不落盘；正常大小响应无回归。

### 1.3 元数据语义上限 `MaxManifestBytes`

**问题**：`FetchCdnResource` 以 `Size=-1` 拉任意 JSON；恶意/配错的 CDN 用 100GB "json" 拖死版本检查；file:// 分支的 `LoadFileToString` 同样无预检；顺带 `(int32)Bytes.Num()` 截断点。

**方案**：
- `FUpdateOptions.MaxManifestBytes` 默认 **64MB**（可配）
- `FetchCdnResource` 传 cap；file:// 分支读前 `FileSize` 预检；超限报 `ManifestTampered` 类错误
- 定容依据：每 chunk ~110B + 每 part ~110B JSON → **10 万 chunk（百 GB 游戏）≈ 30MB**，64MB 留一倍余量（32MB 会误杀超大工程）

**验收**：超限 index 拒收且错误清晰；正常 index（~1.7MB）无回归。

### 1.4 格式瘦身：删除 `updateStrategy` / `EUpdateStrategy`

**问题**：只写的遗留字段——全库 grep 确认客户端 update 流程**零处**业务分支读它（仅序列化 + 日志打印）。实际 per-file 分流依据是**扩展名 + 元数据存在性**（`FUpdateOrchestrator.cpp:1001/1053`：`.ucas`+IoStoreChunks → ucas 原地；`.pak`+PakEntries → pak 原地；其余 → 整文件重建）。且真实安装天然 Mixed（pak + ucas + 几十个普通文件并存），顶层枚举本就不可能驱动逐文件决策。误导读者以为顶层策略驱动行为（2026-08-30 被 review 现场质疑并证实）。

**方案**：
- 删 `EUpdateStrategy` 枚举、`FIndexManifest::Strategy`、`FDeltaManifest::Strategy`
- index/delta 序列化去掉 `updateStrategy` 字段；ingest 的文件混合度计算（`bHasIoStore/bHasPak/bHasPlain`）一并删除
- **兼容性**：Load 对该字段是容忍式可选读——旧 index/delta 照常加载（未知字段被跳过），**无需 bump 格式版本**
- 测试：indextest 的"missing updateStrategy → 拒收"断言**反转为"缺失可加载"**；`Sample/Empty` 样例与 diffbuild 相关断言清理
- 文档：TECHNICAL 清单字段表去掉 `updateStrategy`

**验收**：14/14 全绿。**兼容性说明（实施修正）**：自校验绑定精确正文——旧写入器产生的文件（正文含 updateStrategy、内嵌旧 selfCheck）会被新 Load 的自校验**正确拒绝**，无"透明加载旧文件"可言；v2 格式未对外发布过，真实迁移 = 重新发布。indextest 以注释记录该结论，不设伪兼容用例。

### 1.5 全新安装体验：安装目录自动创建

**问题**：要求 `-install=` 指向的目录**预先存在**（`ExtractInstall` 的 `DirectoryExists` 检查）——但全新安装恰是最常见的真实场景（patcher 引导到新目录/新玩家机器）。2026-08-30 debug 实测踩中：`D:\UGPDebug\install` 不存在 → 更新直接失败。

**方案**：`UpdateWithOptions` 进入时对 `InstallDir` 执行 `CreateDirectoryTree`（幂等，已存在则无操作）。
**权衡**：路径打错会创建一个错误目录并白下载一次全量——可接受，错误与日志都含完整路径，可发现可清理。

**验收**：全新不存在的路径一键安装成功；既有安装无回归。

### 1.6 错误码与消息清理

**问题**（同次实测）：
- "install directory not found" 被归入 **`Internal=11`**（catch-all）——调用方（CI/GUI）无法区分"路径没建"和"内部错误"
- 中文事件消息（如 `????????`）在 GBK 控制台/Git Bash 显示为问号，排障信息丢失

**方案**（已拍板）：
- 失败路径逐一核对错误码：环境类失败（目录不存在/无写权限）与内部错误分离；与 Phase 2 #5 的退出码空间重排**合并拍板**（一次改完，避免二次破坏退出码契约）
- **消息语言 = 英文**（脚本/控制台最稳，GBK 控制台无乱码；GUI 层如需中文由 UI 自行本地化，事件消息保持英文）

**验收**：每个失败路径有可区分的退出码；Git Bash/cmd 下错误消息可读。

### 1.7 review 遗留顺手项

- `check`/`compact` 忽略 `FPatcherStateFile::Load` / `ExtractInstall` 返回值（损坏 sidecar 静默当干净）→ 检查并报错
- `pakpatch` 未识别的 op 静默跳过 → 报错退出
- GUI 恒返回 0（与头文件契约矛盾）→ 传播 worker 结果

---

## 2. Phase 1 —— 下载器通用化 + 模块独立

### 2.1 目标 / 非目标

**目标**：一个下载器同时服务 1MB chunk 小文件与 100GB 级大文件；业务层只派发任务，不感知线程/分段；断点续传精确到段；内存峰值有界且可配。

**非目标**：P2P/多协议（留接缝）；HTTP/2 多路复用调优；代理认证管理。

### 2.2 模块划分

```
UnrealGamePatcherDownload（新模块，无 UI / 无业务依赖，可独立测试）
├── FHttpRangeRequest      同步原语：Phase 0 修好的单请求（Range 构造/206/长度/上限/探测）
├── FDownloadExecutor      窗口驱动执行器（回调驱动单锁状态机，零专用线程，见 §2.3b）
│                           + 分段 + 公平调度 + journal 续传 + 协作取消
└── FDownloadJob / Stats   任务描述 / 字节级统计

UnrealGamePatcherCore（业务侧，改动小）
├── FDownloadManager       退化薄壳：cache 检查 / SHA1 终验 / 落位（业务职责保留）
└── IChunkDownloader       可插拔接缝保留（后端替换点；downloadtest F 已验证其价值）
```

### 2.3 核心设计

#### a) 统一分段模型——"小文件是特例"

没有两条下载路径，只有一条：**一切下载 = 分段任务序列**。1MB chunk = 恰好一段且小于粒度的退化情形；100GB = 25,600 段。

**段大小 = 4MB（默认常量，可配）**，是内存/调度/续传的统一量子。定容依据：
- 内存量子：UE HTTP 整响应缓冲，段大小 = 单请求峰值。8-16 并发 × 4MB = **32~64MB 峰值**（8MB 则 64~128MB，偏胖）
- 请求开销：100GB / 4MB = 25,600 请求，keep-alive 下可接受
- 断点粒度：中断后最小重下单位 4MB，足够细

#### b) 执行器运行模型：回调驱动单锁状态机（零专用线程，已实施）

**HTTP 链路完全交给引擎**：连接复用、活动超时（引擎 curl 层默认 30s 无进度即断）、
回调串行化，`FHttpModule` + HttpThread 全部现成（源码核实 CurlHttp.cpp）；执行器
**不设任何专用线程**（实施先从计算池简化为单工作线程、再简化为零线程）：

```
Submit/Cancel（调用线程）        lock → 建/标记 FJob → Refill()
HTTP settle 回调（引擎 HttpThread）lock → 写段/journal/verify/promote → Refill()
```

- **并发旋钮 = 窗口大小**；内存代价 = 窗口 × 4MB（~96MB，明确可算）
- **单一状态对象 + 一把锁**：全部 job 状态在一个 FExecutorState 里，两个入口都先
  拿锁再动状态；调度决策天然串行（单调度点）
- **超时交给引擎**（连接 30s + 活动 30s），执行器不另建计时器；段失败走有界重试
- 替代原"N 个阻塞 worker"设计：原方案线程数既当并发旋钮又当后处理池，
  新方案两者都消失——只剩窗口管并发、引擎管传输

#### c) 探测（size 未知 / 续传校验）

`GET Range: bytes=0-0` 一次拿到 `Content-Range: .../total` **和** Range 支持验证——比 HEAD 可靠（HEAD 不支持/不一致的服务器也能工作）。内容寻址 chunk 不可变，无 TOCTOU。已知 size 的请求不探测（manifest 有 `Chunks[].Size`）。

#### d) journal 位图续传（替换 `.part` 追加模型）

- 下载开始：`Seek(total-1)` 写 1 字节预分配目标文件
- 每段完成：直接 `Seek(offset)+Write` 落进目标文件（无合并步骤）+ 在 `.journal` 记一位
- 崩溃重启：读 journal，只补缺段
- 全部完成 → SHA1 终验 → 删 journal → 落位

#### e) 两层上限（Phase 0 已就位，Phase 1 继承）

传输层 `MaxResponseBytes` 硬上限（Header 阶段取消）+ 业务层 `MaxManifestBytes` 语义上限。分段下载自身保证单请求 ≤ 4MB。

#### f) 协作式取消

段任务检查取消标志；超时取消沿用已修模式（CancelRequest + 事件不还池）。下载器对外暴露 job 级 Cancel。

#### g) 事件 / 统计

`FDownloadStats` 增加**字节级进度**（现有 `FUpdateEvent` 通道已就绪，加粒度）；分段完成事件可选。

### 2.4 关键接口草案（已实施；最终签名以 FDownloadExecutor.h 为准——Submit 返回 int64 句柄而非 FDelegateHandle，PriorityWeight 未引入，收尾优先规则已内置）

```cpp
// 任务描述（业务层唯一可见的东西）
struct FDownloadJob
{
    FString Url;
    FString DestPath;          // 最终文件（执行器负责 journal/预分配/落位）
    int64 ExpectedSize;        // -1 = 未知（执行器探测）
    FString ExpectedSha1;      // 可空；终验用
    int32 PriorityWeight;      // 预留（收尾优先已内置）
};

// 执行器
struct FDownloadExecutor
{
    static constexpr int64 DefaultSegmentSize = 4 << 20;   // 4MB
    static constexpr int32 DefaultWindow = 24;             // in-flight 段
    // 内部为回调驱动单锁状态机：引擎 IO 线程回调 + 窗口（§2.3b）。
    // Submit 后业务层只等完成事件；不感知线程/分段/回调
    FDelegateHandle Submit(FDownloadJob&& Job, TFunction<void(const FDownloadResult&)> OnDone);
    void Cancel(FDelegateHandle Handle);
};
```

### 2.5 测试计划

- 现有 `downloadtest` 全部迁移（range/续传/并发/重试/占用回滚/自定义后端/网络更新）
- 新增：
  - 大文件分段（构造 >4MB 资源，验证段数、并发上限、journal 位图）
  - 中断恢复（kill 模拟 → 重启只补缺段，字节级比对）
  - 公平性（大文件先入队 + 小文件后入队 → 小文件先完成）
  - 上限取消（超长 Content-Length → 不落盘 + 明确错误码）
  - 探测（无 Range 服务器回退行为）

### 2.6 为什么不引入开源下载库（决策记录）

| 候选 | 结论 |
|---|---|
| UE `FHttpModule` | ✅ 它就是选好的"开源传输库"（libcurl + 全部平台胶水），已链接、零许可/发布成本 |
| libcurl 直连 | 绕开 FHttpModule = 自做证书/代理/平台配置 + 对齐引擎内置版本，得到的只是已有的 10% |
| aria2 | GPLv2 许可不可入；daemon 形态；维护不活跃 |
| BITS | 依赖系统服务、段级并行不可控、破坏可插拔测试、跨平台死路 |
| cpp-httplib / Poco | 太薄，90% 还是自研 |

**核心判断**：难的不是 HTTP 协议（10%），是调度/续传/校验/UGP 集成（90%）——开源库不以我们需要的形状提供这 90%。将来 P2P（libtorrent，BSD）或主机平台下载器走 `IChunkDownloader` 接缝挂新后端，是局部改动。

---

## 3. Phase 2 —— 遗留独立问题

| # | 问题 | 影响 | 方案思路 | 依赖 |
|---|---|---|---|---|
| 1 | **复用偏移错位**：in-place 修补过的容器本地物理布局 ≠ 提交的远端 index → 复用切片拷错字节 → 哈希门拦截全量重取 | 性能（"零下载"承诺失效）；正确性无损 | 提交时记录修补后的真实布局（本地 index 存实际 entry/chunk 偏移） | 无 |
| 2 | **ChunkCache 无上限**：apply 期间全部已取 chunk 常驻 | 多 GB 更新 OOM 风险 | LRU / 逐文件释放；与 4MB 段量子合并做内存预算 | Phase 1 |
| 3 | ~~**ucas 原地补丁按位置匹配**~~ **已完成（2026-09-13）**：patcher/下载预取均按 id+meta 查找（多重集语义），`DropChunk` 支持原地删除 | 已闭环 | — | 无 |
| 4 | **Compact 绕过事务机制**：无备份无 pending | 崩溃窗口 | 纳入事务框架 | 无 |
| 5 | **退出码碰撞**：`EPatcherError::ManifestMissing=2` 与 CLI `Usage=2` 冲突 | CI 无法区分"清单缺失"和"参数错误" | 错误码空间重排（需拍板） | 设计决策 |
| 6 | ~~**FIoHash 未验证**~~ **已完成（2026-09-08 前落地，review 确认）**：Reader 对每个解码 chunk 比对 TOC meta hash（`FIoStoreReader.cpp` ReadChunk），损坏 chunk 即读失败 | 已闭环 | — | 无 |
| 7 | **XXH3 自校验可伪造**：防损坏不防恶意改写 | 安全边界 | 发布侧签名（Ed25519 对 index 正文） | 无 |
| 8 | ~~**openspec 失真**~~ **已处置（2026-10-09）**：design.md D3"官方 manifest 底座"等与自研格式实现矛盾——openspec 目录（含 archive）整体删除，避免历史设计文档干扰后续开发 | 已闭环 | — | 无 |
| 9 | ~~**发布侧定长 1MiB 窗口位移全量重切**~~ **已完成（2026-09-17）**：ingest 升级 FastCDC 内容定义切分（Gear + 归一化，256KB/1MB/4MB）；起始插 1 字节 = 20 chunk 新增 1（buildtest cdcshift）；切分常量+Gear 种子为格式契约 | 已闭环 | — | 无 |

---

## 4. 实施约定

- **顺序**：Phase 0 全部 → Phase 1（先模块骨架+原语迁移，再执行器，再调度/journal，每步 14/14 全绿）→ Phase 2 逐项
- **Phase 0 各项相互独立**（§1.1-1.7 无依赖），可单独落地单独提交；唯一交叉：§1.6 的错误码重排与 Phase 2 #5 合并拍板、§1.3 与 §1.2 的上限常量同处 `FHttpChunkDownloader`
- **每个 Phase 开工前**：以 `review-2026-10-09-0206.md` 的开放项清单为准做影响面评估（历史 openspec specs 已于 2026-10-09 删除）
- **回归底线**：14 套测试全绿 + 真实包（54 files / 3256 chunks / 3.37GB）端到端无回归
- **性能基线**：perftest 现值 0.1%/0.2%（下载 9.4KB / 写 20KB）不得劣化；Phase 1 后补大文件分段的吞吐基准

---

## 5. Phase 3 —— pak 全 CoW v2：空洞复用 + footer 提交点（方向已确认，2026-09-19）

> 来源：index/数据一致性专项（review-2026-10-09-0206.md §五时间线）+ 设计讨论。
> 动机：现 in-place 方案为"同尺寸覆写"配了全量容器备份，备份拷贝使本地写放大
> ≈ 1x 容器——in-place 相对 FastCDC 整文件重建的成本优势被抵消，且引入 C8/C9/C10
> 一类回滚破口。**回退的本质不是"撤销改动"，而是"改动从未破坏旧版本"。**

### 5.1 核心不变式（唯一安全条件）

> **任何时刻，只允许写"当前已提交 footer 的 index 不引用的字节"——洞里的或文件尾的。**

- "标记空洞"与"复用空洞"必须间隔一次 index commit：死区只有被新 footer 提交后，
  才对下一代更新成为安全写入空间（v2 追加的 a.lua(v1) 区域，v3 才能覆写）。
- footer 是 pak 天然的**提交记录点**：最后写、单扇区原子写。注意 FPakInfo 布局
  引擎要解析，**不能加 CRC 字段**——完整性靠 magic + version + IndexOffset 边界
  检查 + **IndexHash（index 的 SHA1）** 校验；扫描器必须**验证候选 footer 而不能
  只匹配 magic**（payload 中可能出现 magic 字节型，GB 级尾部扫描必有假命中，
  全靠后续校验过滤）。
  任意断电时刻，盘上存在一个完整自洽的版本，由"最后一个合法 footer"选出。

### 5.2 写序与崩溃语义（三级写，级间 flush）

```
payload(追加,优先回填洞) → flush → 新 index blob(追加) → flush → footer(单扇区原子写)→ flush
```

| 断电时刻 | 盘上状态 | 恢复动作 |
|---|---|---|
| payload/index 写毕前 | 尾部多出半截垃圾，F1 完好 | 无（F1 视角即完整旧版） |
| footer 写花 | F2 CRC/magic 不过 | 从文件尾向前按扇区扫描，找回 F1 → 截断到 F1 |
| footer 落盘后 | F2 有效，其引用的一切已先于它持久化 | 无（新版完整） |

恢复 = WAL 式向后扫描合法 footer + 截断，**无需备份目录、无需回滚拷贝**。

> 引擎侧行为（已对照 UE 源码定案，`PakFile.cpp:271-308` `FPakFile::Initialize`）：
> 引擎按 `FileSize - GetSerializedSize(版本)` 从最新版本起**定点探测** footer
> （magic 不匹配就降一个版本尺寸再试，只覆盖历代已知 footer 尺寸），**不做扫描**；
> footer 不在物理末尾 = `Fatal "Trailing magic number ... Verify your installation"`。
> 即写入中途的瞬态对引擎是 **fail-closed 拒绝挂载**（非静默错数据）——瞬态不可读
> 可接受的依据；更新器职责 = 启动后、做任何事前先跑恢复扫描。

### 5.2b 跨容器一致性模型（2026-09-19 review 修订 #1，原方案缺口）

单容器原子性只保证**每个 pak 各自**完整；一次更新动多个文件，v2 把 pak 摘出
事务后必然出现"pakchunk0 已 v3、pakchunk1 仍 v2"的混合版本中间态。模型显式化：

- **sidecar（版本完成标记）永远最后写**：更新完成 = sidecar 翻转，此前任何
  容器先提交都不算数；
- **恢复 = 扫描器（无条件、O(1)/pak）+ 幂等重放**：无 pending 标记；下轮运行
  对 sidecar(old) vs remote(new) 重放 diff，已提交的容器因条目全 Same 零写收敛。
  重放幂等性：CoW patch / WriteAtomic / 删除均天然幂等；
- **接受的风险**：更新被放弃时安装目录停留混合版本，引擎无法察觉——由
  "更新器是唯一启动门"把守（与 §5.2 引擎 fail-closed 同一道门）；
- 失败（非崩溃，如网络错误）路径：不回滚容器，报失败留待下轮重放。
- check/repair/compact 入口同样先跑扫描器（"尾部不可解析"交 repair 判定）。

### 5.3 方案要点

- **永不覆写 payload**：同尺寸变更条目与变尺寸一律"追加优先回填洞、EOF 兜底"，
  删除现有 `Overwrite` 路径。回退依赖从"全量备份"变为"旧版本在带内天然完整"。
- **洞清单从容器重算，sidecar holes 直接删除**（2026-09-19 review 修订 #2）：
  权威来源 = 当前 footer index 未引用区间的补集（解析 index + 区间排序即可，
  每次更新本来就要解析 index）；不再有累计簿记，重复记账无从发生。
  → **C10 与 C12 双双结构性根除**（原 5.4 把 C12 列为"仍保留"系旧"簿记保留"
  假设的残留，已纠正）；事后校验对本轮写入条目一律重算 payload 哈希。
- **compact 保留**（= LFS cleaning），语义不变。
- **已知限制（同尺寸大条目）**：pak 条目是连续存储 blob，2 GB 条目改 1 字节 =
  追加 2 GB（下载亦然）。未来优化方向：大条目走 NTFS 式数据 journal（覆写前
  journal 旧字节，∝ 条目大小，仍无需全量备份）——优化项，不是本期必需。
- **加密 index / 签名 pak 门控**：CoW 重写 index+footer 必然破坏签名/加密，
  检测到即拒绝 CoW → 走整文件重建或维持不支持，写入验收标准。
- FastCDC 整文件重建继续作为终极兜底路径（现成回退路径，零额外工作）。

### 5.4 被根除 / 仍保留的存量问题

- **根除**：C8 类（备份失败 → utoc 失配，备份整个机制消失）、C9 大半（无回滚拷贝）、
  **C10 + C12**（洞改从容器重算 + 写入条目重算哈希，记账失配与校验盲区双双消失，
  见 5.3 修订 #2）；全量备份的 I/O 与磁盘峰值开销。
- **仍保留**：M20（V10/11 文件名编码）等格式层问题不受影响，照旧排期。
- **ucas 不在本期**：同构但更简单（Commit 本就只追加 + utoc 原子替换），pak 落地后照搬。

### 5.5 实现任务（2026-09-19 review 修订 #6：扫描器提前，测试补幂等重放）

> **状态（2026-09-21）：1–4 已完成，5 已完成（9 套件全绿：pakinplace 含恢复
> 测试 5a–5f、selftest、statetest、indextest、difftest、buildtest、updatetest、
> maintest、iostoreinplace）。** 实现期修出两个计划外 bug：① `AppendEntry`
> 同路径必须替换旧目录项（否则重复目录记录污染 index）；② 扫描器打开 pak
> 必须 `OpenWrite(Path, true, true)`——引擎默认参数走 CREATE_ALWAYS 打开即
> 截断（已记入 AiAgentKB `ue-openwrite-default-truncates`）。downloadtest /
> benchtest / e2etest 属网络/性能重型，本轮未跑。
>
> **测试审计轮（2026-09-21，12 套件全绿）**：全量测试用例审计（重复/过期/
> 遗漏）后补齐——pakinplace 新增 5b2（>64KB 多窗口扫描）、section 6（失败
> 注入：pak 不备份不回滚、CoW 提交存活、重放零写收敛）、section 7（零字节
> 容器 → Unrecoverable → 整文件重建）、section 8（**删除 pak 条目 → patcher
> 拒绝 CoW → 整重建收敛**，审计发现的真实产品缺口：就地追加路径不表达删
> 除，原实现会留下旧条目且永不收敛，已在 `FInPlacePakPatcher` 加"本地多出
> 条目 → 整文件重建"守卫）；e2etest 去重、footer 512 对齐断言、benchtest
> 清场。加密/签名 gating 测试按用户决定暂缓。过期断言：零。
>
> **JSON 解析层结论（2026-09-21 spike 实测后）**：评估过"USTRUCT 反射 +
> FJsonObjectConverter 重写 Load"（UHT 对 Program 默认生效、JsonUtilities
> 零负担，均实测通过），但**放弃**：① selfCheck 绑定手写 `SerializeBody`
> 的紧凑字节，规范层必须保留；② wire 键与成员名语义错位（`offset` 在
> parts/entries/ioChunks 三个上下文分别映射 Offset/RawOffset/OffsetInFile），
> 反射导入需要按上下文的键重写表 + 递归 walker；③ converter 缺字段静默取
> 默认，严格必填/格式校验层（storedSize 改名拒绝等，indextest 钉死）无论如何
> 要对原始 JSON 手写。实测估算 ~280 行新代码换 ~230 行提取代码，净增。
> 已落地的真实简化：手写 JSON 的公共机械集中到单一头
> `Formats/JsonIo.h`——严格字段读取（ReadInt64Field 等，原三份重复）、紧凑
> 序列化样板（原 6 处）、原子落盘 temp+UTF8+replace（原 4 份逐字重复）、
> selfCheck 的 XXH3-64 计算方案（index/delta 共用）；各格式的契约
> （SerializeBody 键序 + 逐字段校验）留在各自 loader。Load/Save 语义零改动，
> 12 套件全绿。

1. ✅ **恢复扫描器**（新增，最优先）：候选 footer 校验 = magic + version +
   IndexOffset/Size 边界 + IndexHash（不能只匹配 magic）；扫描器是 update /
   check / repair / compact 的公共前置。
2. ✅ `FPakWriter`：删除 `Overwrite`；Commit 三级写序 + 级间 `FlushFileBuffers`；
   footer 尾部扇区对齐、单扇区一次写。
3. ✅ `FInPlacePakPatcher`：全 CoW（同尺寸变更走追加）；洞从当前 index 重算；
   本轮写入条目事后重算 payload 哈希；加密/签名容器显式拒绝。
4. ✅ `FUpdateOrchestrator`：pak 目标不再纳入备份/回滚（容器自回退）；更新启动先跑
   扫描器；sidecar 仍最后写；`TestCrashAfterWrites` 等注入钩子适配。
5. ✅ 测试：footer 写前/写花/写后三态崩溃注入；**容器提交后崩溃 → 重放更新 →
   零写收敛**（幂等重放端到端）；洞复用跨代断言（v2 追加 → v3 覆用 v1 死区）；
   14 套件全绿 + 真实包端到端。

**验收**：断电三态恢复后容器可挂载且版本正确；本地写放大 = 变化条目字节级
（不再含 1x 容器备份写）；perftest 基线不回退；FastCDC 重建回退路径行为不变；
加密/签名 pak 被显式拒绝而非写出损坏容器。
