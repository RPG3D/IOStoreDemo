# UnrealGamePatcher 全面 Code Review 报告

> 日期：2026-09-08 · 范围：全部生产代码（Core / Download / Build / 前端）、13+1 套测试、6 份文档
> 方法：6 路并行深度审查（编排层 / 格式层 / 下载层 / 构建与壳层 / 测试 / 文档对齐），
> 所有 critical / major 发现均经**人工二次复核**——包括对照引擎源码（D:\Code\UE5）与
> ElectricDreamsEnv 两版真实清单的实证比对；已排除 2 项误报（见附录 B）。
>
> **修复状态（2026-09-08 当日）**：P0 已全部落地，15/15 套件全绿——
> C1-C7 全部修复 + M1（-check 接线）/ M2（temp 句柄）/ M3（status 校验 + probe
> 重设计）+ M5/M11/M12。回归测试同步新增：H2b（自然 journal 续传）、stableid
> （C1 端到端）、zcomp 新压缩条目（C7）、vd C1/C2 分类、missingutoc（C3 信任
> 模式自愈）、buildtest 空目录/尾分隔符（C6/M5）。清单格式新增可选字段
> `ioChunks[].metaHash`（旧清单回退 id-only 语义，重新 ingest 即迁移）。
> 遗留：P1/P2 按本报告第四、五节排期；加密/签名容器支持仍属覆盖缺口。
>
> **追加（2026-09-12）**：空洞回填已落地——patcher 记录自身产生的死区
> （此前 Holes 无生产者，本报告"覆盖缺口"第 1 条），下一次更新的追加优先
> 回填（pak 32B 对齐 / ucas 块对齐 best-fit，逻辑块序稠密不变），整文件重建
> 清空该文件空洞。专项回归：pakinplace 4c（新条目落在旧 EOF 之下）+
> stableid 回填断言（.ucas 尺寸零增长）。15/15 全绿。
>
> **追加（2026-09-12，2）**：网络模式下载阶段改**乐观预取**——此前任何 Changed
> 容器都会全量拉取 parts（4 字节更新 = 10GB）；现在普通文件取本地没有的
> chunk、pak 取变化条目覆盖窗口、ucas 取远端 utoc + 变化 chunk 物理区间
> （清单新增 `ioChunks[].fileOffset/fileSize`）覆盖窗口，整文件重建回退前
> 懒补（EnsureChunksCached）。ucas patcher 同时去掉"组装完整远端容器"依赖
> （只组装 utoc + 按块区间取数解码）。专项回归：downloadtest netdelta
> （4 字节容器增量 → 恰好 1 个窗口 chunk）。15/15 全绿。
>
> **追加（2026-09-13）**：ucas 匹配从**位置**改为 **id 查找**（多重集语义）——
> 此前容器内插入/删除 chunk 会让其后所有位置错位，同内容 chunk 被误判
> Changed（全量拉取+重写，ROADMAP Phase 2 #3）。现在 patcher 与下载预取均按
> id+meta 匹配；目标删除的 chunk → `FIoStoreWriter::DropChunk` 丢条目（旧块
> 记空洞）；插入 → 追加。专项回归：iostoreinplace chunkremoval（删 1 chunk =
> 零字节写入 + 空洞记录 + 收敛）。
>
> **追加（2026-09-17）**：发布侧切分从定长 1MiB 窗口升级为 **FastCDC 内容定义切分**
> （Gear 滚动哈希 + 归一化切点，Min 256KB / Avg 1MB / Max 4MB）——"定长窗口对插入位移
> 全量重切"遗留项关闭。位移免疫实证：buildtest cdcshift（起始插 1 字节 → 20 chunk 新增 1）。
> 切分常量 + Gear 表种子为格式契约，已发布版本间严禁改动。14 套件全绿。

---

## 一、总体结论

**工程基础扎实，但存在一个设计前提级错误和一批"只在理想路径下正确"的缺陷。**

值得肯定（多路独立审查一致确认，非客套）：

- 事务/回滚框架真实且自洽：Pending 恢复、逆序回滚、先备份后写、RolledBack 先持久化再删备份；
- 哈希仲裁贯穿始终：整文件重建以组装后 SHA1 为唯一成功信号，本地复用失败降级 CDN 而非失败；
- 清单防篡改、严格 schema、拒绝负尺寸；下载层 exactly-once 纪律（CAS 守卫）真实存在；
- updatetest / difftest / downloadtest 的断言纪律（字节数钉死、offset 级断言、无 sleep 同步）是真实的高质量测试；
- 引擎事实核对过硬：本文多项结论经引擎源码定案（MoveFileW 语义、FCriticalSection 递归、
  FileIoDispatcherBackend 的 0-seeds 回退等）。

但 review 发现了**不能回避的问题**，按影响排序：

1. **ucas 增量判据建立在错误前提上**（C1）：`chunk id = 内容 hash` 的假设对真实引擎容器不成立——
   引擎 chunk id 由**包名哈希**派生（`FPackageId::FromName`），改资产不改 id。diff 只比 id 多重集，
   连验证模式都无法覆盖（远端清单根本没记 meta hash）。**真实世界里"改一个既有资产"的 ucas 更新永远不会下发。**
2. **一条宣传的核心特性实际不工作**（C5）：journal 断点续传在主路径上失效——全新下载从不写 journal 头，
   崩溃后重放永远拒收、全部重下。三个续传测试全部手工构造带头的 journal 文件，恰好掩盖了这个 bug。
3. **一批"静默错误状态"路径**（C2/C3/C4/C7）：pak 删除条目、丢失的 utoc、崩溃窗口中的 utoc 备份、
   追加的压缩条目——共同模式是**所有自检都通过、安装实际已坏/已过时，且多数连 repair 都不修**。
4. **测试体系的系统性弱点**：多处测试"手工构造了生产路径本应产生的状态"（journal 头、空洞清理），
   fixture 与真实引擎行为脱节（id 内容寻址 vs 包名派生），EDS"真实工程"两版之间容器字节全同——
   **端到端从未真正验证过"容器内容变化"这一核心场景**（36KB 增量全部来自 2 个 manifest 文本文件）。

**建议：在修复 C1-C7 之前，不要用于真实玩家分发。** 其余按 P1/P2 排期即可，多数是小改动。

---

## 二、发现汇总

| 级别 | 数量 | 说明 |
|---|---|---|
| Critical | 7 | 错误安装 / 数据损坏 / 核心特性失效 / 发布管线危险 |
| Major | 16 | 功能缺陷（发布路径有兜底）或潜伏缺陷（可复现但当前路径未触达）+ 测试断言级 |
| Minor / Nit | 30+ | 健壮性、统计、文档漂移、命名（正文择要） |

---

## 三、Critical（7 项）

### C1. ucas 内容变更盲区——设计前提错误
- **位置**：`FIndexManifest.h:34-42`（注释"chunk id = content hash"）· `FDiffAnalyzer.cpp:50-68` · `FLocalTruthExtractor.cpp:326`
- **证据链**（全部人工核实）：
  1. 引擎源码 `IoStoreUtilities.cpp:1701/1739`：`GlobalPackageId = FPackageId::FromName(PackageName)`——**包名派生**；
     `:2018`：export bundle chunk id 由它构造。内容变化不改 id。
  2. 远端清单 `ioChunks[]` 只有 `{id, offset, size}`，**无 meta hash** → 远端无内容信号。
  3. 本地真值（信任与验证模式同）只记录引擎 id（`FLocalTruthExtractor.cpp:326`）。
  4. `UcasChunkSetsEqual` 只比 id 多重集；验证模式的 meta-hash 校验是**本地自比**（数据 vs 本地 utoc），
     不存在跨版本内容比较。
- **后果**：真实引擎容器"改既有资产"（最常见更新形态）→ 判 Same → 永不更新，`check`/`repair` 同样不报。
  pak 路径不受影响（条目 SHA1 是内容哈希）。
- **实证**：EDS 两版真实清单比对——61 文件中只有 2 个 manifest 文本变了，**全部容器字节一致**：
  所谓"真实工程增量验证"从未覆盖容器内容变化。
- **修复方向**：manifest `ioChunks[]` 增加 `MetaHash`（FIoHash，ingest 侧 `FIoStoreReader` 已有 `C.Hash`，
  一行序列化）；diff 比较 (Id, MetaHash)；in-place patcher 的 Same 判据同步升级；测试 fixture 改为
  "固定 id + 变 meta hash"以模拟真实引擎容器。

### C2. pak 删除条目不可见
- **位置**：`FDiffAnalyzer.cpp:30-46`（`PaksLogicallyEqual`）
- **问题**：只遍历 `Remote.PakEntries`（remote ⊆ local 即判 Same），本地独有条目永不检查。
  对照：ucas 对称函数先比 Num——不对称性证明这是疏漏而非策略。
- **后果**：版本 N+1 从 pak 移除资产 → 判 Same → 玩家侧资产永久残留且挂载，update/check/repair 全盲
  （verify 模式本地真值提取后走同一比较器）。
- **修复**：加 `LocalByPath.Num() == Remote.PakEntries.Num()`（或双向遍历）。

### C3. utoc 丢失永不自愈，且报成功
- **位置**：`FDiffAnalyzer.cpp:198-221`
- **问题**：utoc 镜像把自身 `Added`（本地缺失）覆盖为兄弟 ucas 的 `Same`——与该函数自己的注释
  （"A Changed/Added utoc stays a target"）直接矛盾。提交后 sidecar=Remote verbatim，声称 utoc 已装 →
  此后每一轮（信任/验证/repair）都判 Same。镜像同样吞掉 stray-utoc（多余文件不删）。
- **后果**：容器无法挂载，patcher 永远报成功；唯一出路是手删 ucas 或重装。
- **修复**：镜像仅对 `Changed` 生效；`Added`/`Removed` 保持自身分类。

### C4. utoc 备份持久化时序窗口 → 回滚产生失配对
- **位置**：`FUpdateOrchestrator.cpp:1112-1156`（+ 回滚分支 `:583-584`）
- **问题**：兄弟 utoc 的 BackupPath 仅在内存赋值（:1123），`SaveTransactionState` 在 Patch（:1139）
  **之后**（:1145）。崩溃落在这个窗口（含整个 patch 时长）→ 重跑回滚：ucas 从备份还原，utoc 因
  BackupPath 为空走"无备份即原样"分支（:583-584 假设"原始字节未动"——已被违反）→ utoc 留 patched 版。
  随后 in-place patcher 打开（新 utoc + 旧 ucas）：id 全 Same、零写入、id 多重集校验通过 → **对损坏容器报成功**。
- **兜底**：仅 verify 模式的逐 chunk 重哈希能抓（默认 ≤10 天窗口）。
- **修复**：备份完成立即 Save（把 :1145 的 save 提到 :1139 之前）；顺带给 `FIoStoreReader::Open` 加
  块范围 vs ucas 实际尺寸校验（一行），把此类失配转为安全回退。

### C5. journal 断点续传主路径失效（宣传特性不工作）
- **位置**：`FDownloadExecutor.cpp:478-516`（`Preallocate`）
- **问题**：全新下载在 `Preallocate` 中因 `SegmentsDone == 0` **跳过** `JournalReset`（:497），
  此后只有 `JournalAppendSegment` 追加 `SEG n` 行——journal 永远没有 `UGPJRN1` 头。崩溃重启后
  `JournalReplay` 因 `Lines[0] = "SEG 0"` 头校验失败而拒收 → **全部重下**。唯一写头的路径是
  verify-retry（:537），鸡生蛋。正确性无损（宁可重下），但 README/DOWNLOAD 宣称的
  "段粒度续传，中断损失 ≤4MB"实际不存在。
- **测试掩盖**：三个续传用例（`UnrealGamePatcherCLI.cpp:3228/3687/4093`）全部**手工构造**带头 journal——
  测的是重放器，不是生产路径。
- **修复**：fresh 下载（OnDisk≠Total 且无有效 journal）也走 `JournalReset`；补一个真实中断测试
  （下载中途杀进程重跑，断言只补缺段）。

### C6. 空 / 错位 staged 目录发布 → 客户端全删（实机复现）
- **位置**：`FBuildIngestor.cpp:50-204`（无 sanity check）+ `UnrealGamePatcherCLI.cpp:1116-1131`（无条件写 latest.json）
- **问题**：`-staged=` 指向空目录或上层目录（`Saved/StagedBuilds` 而非 `.../Windows`）→ ingest 0 文件成功 →
  latest.json 翻转 → 客户端 diff：全部本地文件 = 本地独有 → Removed → **删光安装目录**（复现时只剩 .ugp 与 state 文件）。
- **修复**：ingest 校验文件数下限（如 >0 且容器 ≥1），或与上一版本比对突变阈值；`RunBuild` 发布前再拦一道。

### C7. 追加压缩 pak 条目按 method-0 落盘 → 引擎读垃圾、自检全过
- **位置**：`FInPlacePakPatcher.cpp:232-247` + `FPakWriter.cpp:291-299`
- **问题**：压缩 entry bail-out 预检（:150-164）只看**本地存在**的压缩条目。remote-only 的新压缩条目
  （真实 staged pak 普遍 Oodle 压缩）走 append：`W.CompressionMethodIndex = 0`，而 `RemoteBytes` 取的是
  `StoredSize` 的**压缩字节**；条目头声明 `UncompressedSize` = 原始尺寸。引擎按未压缩读出压缩流 = 垃圾；
  UGP 逐条目校验哈希的是存储字节 → **通过**。变尺寸的本地未压缩 / 远端压缩条目同样命中。
- **修复**（保守且一行级）：append/overwrite 前检查 `RemoteEntry.StoredSize != RemoteEntry.UncompressedSize`
  → bail 整文件重建。根本修：清单记录压缩方法与块布局。

---

## 四、Major（16 项）

**功能 / 正确性**

| # | 位置 | 问题 | 备注 |
|---|---|---|---|
| M1 | `UnrealGamePatcherCLI.cpp:1777,1838` + README:51 | `update -check` 幽灵参数：文档与 usage 均写，从未解析——传了它照样**全量写盘** | 用户以为只读检查 |
| M2 | `FInPlaceIoStorePatcher.cpp:161-162` | `.ugp-remote.ucas` 在 reader 仍持句柄时 Delete → Windows 共享冲突静默失败 → 每次原地 patch 后在 Paks 目录残留**整个新容器的副本**（GB 级），下次更新的 stray 清理才删 | 修：作用域先关 reader |
| M3 | `FHttpChunkDownloader.h:34` + `FHttpRangeRequest.cpp:299-320` | 异步原语与 probe 全链 **status-blind**：非 2xx 带 body 的响应当内容交付（契约明文禁止）；probe 的 Content-Length 分支无状态检查且首头胜出 → 依赖服务器头序 | 发布路径有长度+SHA1 兜底 |
| M4 | `FBuildIngestor.cpp:170-188` + `FChunkDataLoader.cpp:104` | ingest 每次运行**非原子重写全部 chunk 文件**（含已发布版本共享的）——对在线 CDN 跑 ingest 期间玩家可读到半截 chunk（mtime 实证） | 修：存在即跳过或 temp+rename |
| M5 | `FBuildIngestor.cpp:54` | `-staged=` 尾部带分隔符 → `RightChop(Dir.Len()+1)` 吃掉文件名首字符 → 发布**自洽但全错**的清单（selfCheck 通过）（实机复现） | |
| M6 | `FPakWriter.cpp:347-350,190-198` | append 不去重目录记录 → `[old, new]` 并存；后续 Overwrite **first-match** 打在陈旧记录上，引擎解析 **last-match**（TMap::Add 覆盖）→ 引擎可能继续供给旧内容 | 陈旧副本只增不减 |
| M7 | `FPakWriter.cpp:434-438` | FullDirectoryIndex 文件名恒按 FUtf8String（无 NUL 计长）写——pak **v10/v11**（UE4.26-5.1 内容）引擎按 FString 读 → 每个文件名截掉末字符、容器不可用 | UGP 自研 reader 两种都读 → 测不出 |
| M8 | `FIoStoreWriter.cpp:196` | 方法名解析在槽位外找 NUL——reader 已修的同一越界读，writer 没修（损坏 utoc → UB） | |
| M9 | `FPatcherHash.cpp:68-76` + `FIndexManifest.cpp:342` | `HexToBytes` 奇数长度输入 1 字节堆溢出；`entries[].sha1` 是唯一没做 IsHex40 校验的哈希字段 | 需恶意/构造清单触达 |
| M10 | `FUpdateOrchestrator.cpp:1112-1213` + diff 镜像 | 本地缺 `.ucas`、远端内容未变：ucas 整文件重建（CDN 布局），utoc 判 Same 不动 → 旧 utoc 指向新 ucas 之外的偏移，信任模式永不发现 | verify 模式偶然正确 |
| M11 | `FDownloadExecutor.cpp:348-356,529-548` | 零尺寸任务：0 段 → 验证失败 → retry 分支 return 后无人再触发 → **OnDone 永不回调，整批挂死**（manager 预检放行 Size==0） | 发布路径不可达（chunk 恒非空） |
| M12 | `FDownloadExecutor.cpp:451-452` | `Total==0 && !bRangeOk` → `Seg=0` 除零崩溃（probe 路径） | 同上潜伏 |
| M13 | `FDownloadExecutor.cpp:401` | `J.InFlight` 只增不减 → 发过段的任务 `Cancel` 走 `InFlight==0` 分支永不完成（OnDone 违约）；公平调度 Rule-0 逐步失效 | 现网 cancel 仅测试使用 |
| M14 | `FDownloadExecutor.cpp:352→549→586` | 计划期 `VerifyAndPromote→Finish→OnDone` 在 `Refill` 迭代 Jobs 数组**内部**触发——回调里再 Submit = 迭代器失效 UB（当前 manager 回调安全） | API 契约未禁止重入 |
| M15 | `FDownloadManager.cpp:81` | 预检把 256MB **响应**上限当**整文件**上限 → >256MB 单文件被拒，与模块"任意大小文件"定位直接矛盾（发布路径只下 chunk 不受影响） | |
| M16 | `FDownloadExecutor.cpp`（probe）| probe 不占窗口槽、析构无法取消 → N 个未知尺寸任务单轮全探测（无界扇出）；析构后 settle 仍可能触发 OnDone | |

**测试断言级 Major**（详见第六节）

- maintest 的 compact"空洞已清"自证（测试自己清空洞，CLI 层清理逻辑零覆盖）；
- updatetest §12 verifyOnUpdate 无写量断言（回退整文件重建也全绿）；
- e2etest `bHashOk` 死变量 + 同一 pak 循环打开 4 次（TESTCASES §11 宣称的"每文件 hash 匹配"在该套件不存在）。

---

## 五、Minor / Nit（择要）

**生产代码**：Rollback 全程忽略文件系统返回值（`EPatcherError::RollbackFailed` 全库零使用）；压缩 bail-out
两洞（本地未压缩/远端压缩同尺寸；信任模式 hash=0 走 overwrite）；无加密 pak 拒绝（明文写进带加密标志的条目）；
manifest id 无路径穿越校验（HTTP+恶意 CDN 可写/删安装目录外文件——自校验防损坏不防改写）；`ExpectedSha1`
从不设置（下载层端到端 SHA1 校验为死代码，chunk guid≠文件哈希，apply 期内嵌哈希兜底）；journal 无身份绑定；
`Completed` 统计把预检拒绝算进分母；GUI 版本解析静默回退硬编码 "1.0.0"；Core 公有头 include Download 模块头但依赖
为 private（分层倒置）；FBuildDiffer delta 不带 parts/entries（与头文件契约矛盾）；ingest 不校验 staged 容器内嵌
哈希；FPakReader 校验前分配（IndexOffset/Size 有符号溢出 → 4EiB 分配崩溃而非报错）；TOC 版本 <8 与签名容器不支持
（文档却称"直接报错"，实际是错误解析）；FIoStoreWriter 硬编码 ContainerFlags=0x09。

**文档**：DEBUG.md 最失同步（§4 称 check 走信任模式——实际强制 verify；日志路径错；"executor VeryVerbose 逐段日志"
不存在——执行器只有 2 条 Warning）；窗口默认实为 **8** 非 24（`FUpdateOrchestrator.h:26`；24 只是 executor 裸默认）；
TECHNICAL §3.1/§3.2 信任模式两代描述并存自相矛盾；ROADMAP Phase 2 #6（FIoHash）已修未标；套件计数 13/14/15 三个
版本打架且 `benchtest` 三份文档漏记；check/repair 实际支持 HTTP（仅 compact 本地限定）；`FHttpChunkDownloader`
描述为旧设计（"并发池"不存在）；fixture 体积 4 个数字打架；downloadtest 实为 87 项非"50 项"；pakpatch 参数与
`--version` 未入文档；TESTCASES 低估 pakinplace（其实还断言了字节一致）。

---

## 六、测试体系评估

**真实强度**（值得保持）：updatetest 的字节钉死（`BytesWritten == ExpectedBytes`、0 取 1 复用精确计数）、
回滚快照逐字节比对；downloadtest 的 offset 级 journal 断言、控制端点同步（无 sleep）；difftest §6 直接验证
虚拟磁盘模型；pakinplace 的 `BytesWritten == 91` 与 zcomp 压缩多块全链路。

**系统性弱点**：

1. **手工构造生产状态**（最危险的反模式）：journal 头（C5）、compact 空洞（测试自己清）——生产路径的
   产出物从未被验证可被消费。
2. **fixture 与真实引擎行为脱节**：所有 IoStore fixture 都是 UGP writer 造的（id 内容寻址）——
   恰好把 C1 的错误前提烤进了测试。修复 C1 时必须同时把 fixture 改成"固定 id + 变 hash"。
3. **4 个 major 断言缺陷**：e2etest 死变量、maintest 自证、verifyOnUpdate 无写量、§2 `bStored` 短路
   （unreadable stored hash 跳过比较而非失败）。
4. **覆盖缺口**（按风险排序）：真实容器内容变化（零覆盖——EDS 实证）；CLI 命令层（update/check/compact/repair
   作为命令从未被任何套件调用——回滚对称性/退出码/latest 解析全裸奔）；CDN 侧坏数据（错字节对尺寸的 chunk、
   缺失/损坏 index）；容器写中途崩溃（现有崩溃注入全在普通文件上）；压缩 bail-out 无"未尝试原地写"断言；
   downloadtest/benchtest 不清理工作目录（~112MB/次）；updatetest 4 处 mid-suite `return` 跳过 FinishTest。

**结论**：13+1 套件作为回归网是真实有效的（多轮真实抓 bug 的历史证明），但它验证的是"UGP 自己的一致性"，
对"与真实引擎世界的接口"（真实容器行为、真实中断、真实 CDN 故障）覆盖不足。

---

## 七、建议行动

**P0（发布阻断，修复前不得用于真实分发）**：C1-C7 + M2（残留文件）+ M3 的 status 检查（一行）。
多数是小改动：C2 一行、C3 三行、C4 一次 save 挪位、C5 一个 reset 调用、C6 一个 sanity gate、
C7 一个条件 bail、M2 一个作用域调整。

**P1**：M4-M16 择要（ingest 原子性、staged 分隔符、重复目录记录、v10/11 名字序列化、HexToBytes 防御）；
测试断言修复（第六节 3/4）；文档同步（DEBUG.md 优先）；`update -check` 解析或从文档删除。

**P2**：其余 minor/nit；测试覆盖缺口按第六节顺序补；`ExpectedSha1 = 文件哈希`（manifest 增加 chunk 文件
SHA1 或复用内嵌）；执行器收尾（InFlight 修复、OnDone 移出迭代、零尺寸拒绝）。

---

## 附录 A：实证方法记录

- 引擎源码定案：`FPackageId::FromName`（IoStoreUtilities.cpp:1701/1739/2018）；`FileIoDispatcherBackend.cpp:233-258`
  （0 perfect-hash seeds → Default 线性表，**解除**对 FIoStoreWriter 0/0 写法的引擎兼容性疑虑）；
  `WindowsPlatformFile` 共享模式（M2 的删除失败定论）；`FParse`/`RemoveExeName` 语义。
- EDS 两版清单比对（61 文件全量 diff）：仅 2 个 manifest 文本变化，容器全部字节一致——C1 盲区与
  "端到端从未测过容器变化"的实证。
- 实机复现：C6（空 staged → 客户端删光）、M5（尾分隔符吃字符）、M4（chunk 文件 mtime 变化）。

## 附录 B：已排除的误报

1. ~~`BytesWritten` 对 MoveTemp 后数组计数为 0~~——`AppendEntry(const FEntryToWrite&)` 是 const 引用，
   不移动，统计正确（格式层 agent 误读）。
2. ~~`FHttpChunkDownloader.h` 是迁移遗留死文件~~——是有意保留的 legacy 适配器（头文件注释明示桥接
   `FHttpRangeRequest`），CLI/GUI 的合法依赖。
3. ~~FUpdateOrchestrator.cpp 623/644 行注释损坏（`\` 开头）~~——grep 显示伪影，原文为正常 `//` 注释。

## 附录 C：值得记录的正面结论

- 引擎兼容性：in-place 重写的 utoc（0 perfect-hash seeds、去掉签名块）在 id 解析层面对引擎合法
  （Default 线性查找路径）；但**带签名的容器**会被引擎拒绝（"Missing signature"），与文档"直接报错"不符，
  归入 P2。
- chunkcache 提交后即删（README 声明属实）；sidecar 为 verbatim 远端清单（entry 元数据保留，
  manifest-diff 依赖成立）；`FIoStoreReader` 的 meta-hash 校验已实装（ROADMAP Phase 2 #6 应标记完成）。
