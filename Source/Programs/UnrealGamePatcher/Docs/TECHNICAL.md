# UnrealGamePatcher 技术方案（实现版）

> 本文档描述**当前实现的真实流程与格式细节**（对照代码与测试）。
> README.md 是概念介绍；本文是技术细节。所有命令与路径以本仓库实测为准。

---

## 1. 总体架构与分层

```
打包/发布侧（构建机器）                   客户端（玩家机器）
+----------------------+              +---------------------------+
| UE Cook / UnrealPak  |              | UnrealGamePatcher.exe     |
|   -> staged build    |              |  Core  格式层/事务/状态     |
|                      |              |  Download 传输原语/执行器    |
|   (pak/ucas/utoc)    |              |  CLI   update/check/...   |
| UGP build (ingest):  |              |  GUI   Slate + worker 线程 |
|   CDC chunk + index  |             +---------------------------+
+----------+-----------+                          |
           | 扩展清单 ingest/diff                  |
           v                                     v
+----------------------+                  +----------------------+
| CDN 静态文件服务器     | --HTTP(Range)--> | <install>/.ugp/      |
| (python cdn_server.py|                  |  index.json 内容索引  |
|  或任意静态托管)       |                  |  chunkcache 下载缓存  |
+----------------------+                  |  backup    回滚备份    |
                                         | UnrealGamePatcher.state.json（sidecar 事务/空洞）
                                         +----------------------+
```

- **单 Program target**（`Source/Programs/UnrealGamePatcher/`），`bCompileAgainstEngine=false`，
  模块：`UnrealGamePatcher`（前端：WinMain + CLI 分发与测试 + Slate GUI）、
  `Core`（格式/下载/事务/状态，无 UI）、`Build`（ingest/diff，发布侧）、`Download`（传输原语 + 执行器）。
- **自研格式层**，不依赖引擎 PakFile / BuildPatchServices / IoStore 模块（仅用 FCompression/FPaths 等底层能力）。
- **工程内不含 HTTP 服务器**：CDN 服务器用 `Scripts/cdn_server.py`（Python 标准库）提供，测试拉起它打真实网络。

---

## 2. 构建侧（发布）：ingest / diff

### 2.1 输入：标准 UAT 输出（staged build）
`UAT BuildCookRun -build -cook -stage -pak`（`Scripts/package_update.bat`）的 staged 输出目录直接喂给
`UGP build`。**没有官方 manifest / BPT chunk**——不跑 `-createchunkinstall`、不跑 BuildPatchTool，
ingest 直接从 staged 文件解析：

- 每个文件按**内容定义切分（FastCDC）**切块（`FBuildIngestor`，格式层 `FChunkDataLoader::Write`），
  chunk **内容寻址**：键 = **未压缩窗口的 SHA1**；CDN 路径 `chunks/<h0><h1>/<h2><h3>/<rest>.chunk`
  （2/2/36 hex 分片，`FChunkDataLoader::MakeChunkRelPath`），跨文件/跨版本自动去重。
  - 机制：Gear 滚动哈希（`h=(h<<1)+G[b]`，256 项定种子表）+ 归一化切点；
    Min 256KB / Avg 1MB / Max 4MB，MaskS=2^20-1（均值前高切率）、MaskL=2^21-1（均值后低切率）。
  - **位移免疫**：Gear 状态仅依赖最近 ~64 字节，插入/删除后自动重同步——起始插 1 字节
    只重切第 1 个窗口（buildtest cdcshift 实证：20 chunk 新增 1），其余边界原样落位。
  - **格式契约**：切分常量 + Gear 表种子是内容地址的一部分，**已发布版本间严禁改动**
    （改 = 全部切点漂移 = 跨版本去重失效）。
  - 客户端零依赖切法：`parts[]` 逐引用记录 `{chunk, offset, size}`，变长透明；
    offset→chunk 定位为顺序走访（`AddWindowsForRange` / `AssembleRangeToBytes`），无除法假设。
- 容器**直接解析**（无需重装）：
  - `.pak` → `FPakReader` 逐条目（mount 相对路径 / payload 偏移 / 存储大小 / 未压缩大小 / 内置 SHA1）→ `entries[]`。
    payload 偏移 = `Entry.Offset + GetSerializedEntrySize`（V12 未压缩条目头=53B：8+8+8+4+20+1+4）。
  - `.ucas` + sibling `.utoc` → `FIoStoreReader` 逐 chunk（id / 逻辑 RawOffset / RawSize）→ `ioChunks[]`。
- 普通文件（dll/exe/...）整文件 SHA1。

**自研 chunk 文件（.chunk）格式**：`[8B "UGPCHNK1"][8B usz][8B csz][1B StoredAs][20B SHA1]`
- 头 = 45B，无版本分支；`StoredAs`：0 = raw / 1 = Oodle；`SHA1` 覆盖**未压缩**数据（= 内容地址，加载时校验）。
- 写时先试 Oodle，**节省 ≥10%（压缩后 < 90%）才存 Oodle**，否则存 raw——90%~100% 带
  基本是不可压内容（已压缩纹理/音频、ucas 内已压缩 chunk），压了白压还多一次解压；
  两种 flag 加载器都能读。阈值常量 `FChunkDataLoader::MinOodleSavePercent`（=10）。

### 2.2 ingest（CLI `build -staged= -app= -version= -out=`）
把 staged 目录转成 CDN 扩展清单布局——**元数据 / 实体分离**：

```
<cdn>/
  index/
    latest.json                     # 版本公告（最后写；无 -version= 时客户端解析它）
    <contentVersion>/UnrealGamePatcher.index.json   # 扩展清单（自校验 XXH3-64）
  chunks/<h0><h1>/<h2><h3>/<rest>.chunk             # 全局共享的 CDC 变长 chunk
```

- 文件条目：whole-file SHA1；`Size`=parts 之和；`parts[]`=连续窗口引用 `{chunk, offset, size}`（客户端据此重建）。
- chunk 条目：内容地址（SHA1）→ CDN 相对路径 + chunk 文件大小；chunk 在**全局 `chunks/`**，
  跨版本相同窗口 = 同一对象（不重复上传/存储），客户端 chunk 源 = CDN 根（本地模式 `<cdn>/chunks/`，
  网络模式 `<base>/chunks/`，均无版本目录）。
- pak 条目元数据（`entries[]`）与 IoStore 元数据（`ioChunks[]`）见 §2.1，供客户端原地写路径使用。
- 确定性：键排序、无时间戳、`selfCheck`=XXH3-64(规范化正文)，两次 ingest 字节一致（`buildtest` 验证）。
- 发布顺序：先传 chunks（幂等）→ 再传 index → **最后写 latest.json**——客户端轮询到的版本恒完整。

### 2.3 diff（CLI `diff -base= -target= -out= [-targetcdn=]`）
两个扩展清单 → `FDeltaManifest`（格式 `UnrealGamePatcher.delta`，同样 canonical JSON + XXH3-64 自校验）：
- `files`（目标侧：added/changed 的文件及其目标内容）、`removedFiles`、`chunks`（需要下载的 chunk 及其 CDN 路径）。
- 差异规则（D3）：同 id + 同 hash ⇒ Same；同 id 异 hash ⇒ Changed；远端独有 ⇒ Added；本地独有 ⇒ Removed。
- 支持任意两版本 diff（含 v1→v3 跳版本，`diffbuild` 验证 delta == 各步 delta 并集）。
- 拒绝 AppId 不匹配。

### 2.4 发布管线（`Scripts/publish_update.bat`）
`-Staged <staged dir> -App <name> -Version <v> -CdnRoot <cdn> [-BaseIndex <旧index> -NewIndex <新index> -DeltaOut <dir>]`。
cmd 批处理 `%1` 会在 `=` 处截断 → **参数用空格分隔**。失败传播退出码（CI 可接）。

---

## 3. 客户端更新流水线（CLI `update -cdn= -install= [-version=] [-check]`）
`-cdn` 是唯一数据源：`http(s)://`（latest.json/index/chunks 全走 HTTP）或本地目录（裸路径 / `file://`）。
`-version` 缺省时从 `index/latest.json` 解析（HTTP 下载或本地读）。

### 3.1 阶段总览
```
1 加载远端 index        <cdn>/<version>/UnrealGamePatcher.index.json（自校验，篡改即拒）
2 提取本地真相          信任模式：**零磁盘内容读取**——本地 index（.ugp/index.json，上次
                       提交的远端 manifest 副本）直接作为本地真值，diff = 清单对清单；
                       仅做一次**名字级**磁盘巡检修复漂移（缺文件→补拉、多余→删除、
                       普通文件 size 变化→重拉；容器 size 因原地写合法漂移，不比对）。
                       同尺寸损坏/容器损伤信任模式不可见，由验证模式兜底。
                       验证模式：重读容器结构 + 重算条目 payload hash + 逐 chunk 数据
                       校验 + hash 全部普通文件（sidecar 丢失/损坏也走此路径）
3 差异分析              容器=虚拟磁盘：pak 按条目内置 hash 集合、ucas 按 utoc chunk-id
                       多重集（与字节布局/顺序无关）；普通文件按整文件 SHA1
4 下载（网络模式）       **乐观预取**：按目标类型取 apply 真正需要的窗口——
                         普通文件取本地状态里没有的 chunk；pak 取"变化条目"
                         覆盖的窗口（条目元数据逐条比对）；ucas 取远端 utoc
                         （小）+ 变化 chunk 的物理块区间覆盖窗口（清单记录
                         fileOffset/fileSize）。整文件重建回退前懒补缺失部分
                         （EnsureChunksCached）——小delta不再拉整容器
5 应用                  整文件重建（默认回退） / pak 条目级原地 / ucas chunk 级原地
6 校验                  重建/修补后按容器内部 hash（逐条目/逐 chunk）验证；不通过回退下一策略
7 提交                  刷新本地 index、清备份、事务置 committed；验证模式写回 LastVerifiedAt
```

验证模式触发：`repair`/`check` 命令、远端 index 的 `verifyOnUpdate` 标记、
距上次验证超过 `VerifyEveryDays`（默认 10 天）。信任模式不检测同尺寸损坏
（可接受代价，由验证/repair 兜底）。

### 3.2 差异规则（D3 落地 + 虚拟磁盘模型）
- **容器（.pak/.ucas）= 虚拟磁盘**：真值 = **内部子文件 hash**，不是容器整文件
  hash（原地写使每台机器物理布局不同，整文件 hash 不是有效真值）。
  - pak：逐条目内容 hash（验证模式 `ComputePayloadHash` 重算 payload；
    信任模式读内置 hash——每次 update 直接解析本地 pak index，不依赖缓存副本）。
  - ucas：**（chunk id，meta hash）多重集**（与顺序无关）。真实引擎容器的
    chunk id 由**包名**派生（引擎源码 `FPackageId::FromName`，内容修改不改 id），
    内容信号是 utoc 的 **meta hash**（chunk 原始数据的 FIoHash）——2026-09-08
    review 修复 C1：此前 id-only 判据会漏掉"改既有资产"类更新，验证模式也覆盖
    不了（清单未记 meta hash）。旧格式清单（无 metaHash）回退 id-only；验证
    模式再逐 chunk 数据对照 meta hash——损坏容器翻 Changed 走整文件重建修复。
  - utoc：容器元数据，状态镜像其 ucas（Same/Changed/Removed），不独立参与整
    文件 hash 比较。
- 普通文件（dll/exe/...）：whole-file SHA1。

### 3.3 应用策略（按目标自动选择）
1. **整文件重建**（默认/回退）：按 parts 从 chunk 组装（同 chunk 优先从本地文件切片复用）→ 临时文件 → whole-file hash 校验 → 原子替换。
   网络模式：先对全部目标需要的 chunk 做下载阶段（缺失缓存自动补下），下载失败整体中止不落盘。
2. **pak 条目级原地**（`FInPlacePakPatcher`）：仅当该 pak 有 `entries[]` 元数据且本地可读。
   同条目零搬动；变化条目**重追加**（CoW v2：永不原地覆写 payload，优先回填由已提交
   index 补集重算出的空洞），目录/footer 随新 payload 在尾部重写（512 扇区对齐单写 =
   提交点）。
   修补后按**逐条目内容 hash** 验证（虚拟磁盘模型，不要求整文件字节一致），否则回退整文件重建。
   验证模式（`bVerifyLocalData`）重算本地条目 payload hash——同尺寸损坏的存储 hash 头没变，
   必须重算才抓得到。
3. **ucas chunk 级原地**（`FInPlaceIoStorePatcher`）：仅当有 `ioChunks[]`。
   按 **id 查找**（多重集语义，与位置/顺序无关——cook 可自由插入/删除/重排 chunk）+
   meta hash 对比：同 id 同 meta 零搬动；变化 chunk 从远端 utoc 块表解码（**不组装完整
   远端容器**——只组装小的 utoc + 按物理块区间取数解码）；目标删除的 chunk → `DropChunk`
   丢条目（旧块成死区记空洞）；新 chunk 追加 → **utoc 原子重写**。
   sibling `.utoc` 目标自动跳过（已由修补重写）并在修补前备份（回滚对称）。
   结果**逻辑等价非字节一致**（raw 追加 + 本地顺序可异），以 chunk-id 多重集校验；验证模式前置逐 chunk
   meta-hash 校验，损坏容器 → 回退整文件重建（即修复）。

### 3.4 事务 / 回滚 / 崩溃恢复（sidecar `UnrealGamePatcher.state.json`）
- 每次 apply 前把全部目标写入 sidecar（`status=pending`，目标含 file/op/offset/size/originalHash/newHash/backupPath）。
- 逐目标**先备份原文件**到 `.ugp/backup/` 再操作；每步写日志（崩溃恢复可定位进度）。
- 任一失败 → **回滚**：按目标逆序恢复备份（Added 无备份则删除），备份目录清除，状态 `rolled-back`。
- 下次启动读到 `pending` → **先回滚恢复**，再应用新版本（模拟 kill 后 rerun 结果正确，`updatetest` 验证）。
- **空洞回填（CoW v2，2026-09-21 起）**：pak 死区不再记入 sidecar——由**已提交 index 的
  补集**实时重算（`ComputePlacementHoles`）；**下一次更新的追加优先回填**这些空洞
  （pak 32B 对齐 / ucas 块对齐，best-fit；逻辑块序保持稠密，物理写进死区，
  引擎按块表 FileOffset 读取不受影响）。ucas 侧被替换 chunk 的旧块区间同理。
  整文件重建（CDN 布局）天然无死区；`compact` 仍是手动兜底（临时文件 + 原子替换，回收全部死重）。
  `check` 报告的膨胀率基于真实空洞记录（阈值：>10% 或 >512MB）。

### 3.5 本地状态布局
```
<install>/
  pakchunk1-Windows.pak / .ucas / .utoc ...   # 内容文件（manifest 的同名命名空间）
  UnrealGamePatcher.state.json                 # sidecar：空洞列表 + 事务日志
  .ugp/
    index.json       # 本地内容索引 = 上次提交版本的远端 index（chunk 级真相）
    chunkcache/      # 下载的 chunk 文件缓存（网络模式，更新成功后清理——临时存储，不留双份数据）
    backup/          # 事务回滚备份
```

### 3.6 本地 chunk 复用（未变化窗口零下载）

整文件重建回退路径（新装/修复/in-place 失败时按 parts 从 CDN 重装文件）中，
**未变化的 chunk（CDC 变长窗口）直接从本地容器拷贝，不下载**。虚拟磁盘模型下这正是关键：
容器字节排序每台机器不同，但**窗口是内容寻址的（SHA1）**，匹配完全不看顺序。

> 注意区分两个"复用"：本节的**容器内窗口复用**（从已安装 pak/ucas 切片拷贝）；
> `FDownloadManager` 的"复用已下载 chunk"指**下载断点续传**（journal 位图段粒度续传，另一概念，见 §4）。

**算法**（`RebuildFileFromChunks`，~90 行）：

1. **`SameChunks`**：两个版本间内容未变的窗口集合（按窗口 SHA1 比较，即跨版本去重）。
2. **`LocalSources`**：反向索引 `窗口SHA1 → [(本地文件, 文件内偏移, 窗口内偏移, 窗口大小)]`，
   由本地 `.ugp/index.json` 的 parts 一次性建表。
3. **匹配 + 拷贝**：重建文件时，对每个 `SameChunks` 的 part，在 `LocalSources` 里找
   **切片匹配**的本地文件（`窗口内偏移 == part.offset && 窗口大小 == part.size`），
   从该文件对应偏移直接 `Memcpy` 窗口字节。新格式下每个 part 都是整窗口
   `(sha1, 0, chunkSize)`（chunk 为 CDC 变长，256KB..4MB），匹配退化为"本地任何文件是否携带此窗口"——一次查表。
4. **校验兜底**：组装完整文件后 whole-file SHA1 校验；不匹配（本地字节损坏）
   → attempt 2 全部重新下载。

**为什么排序不一致不影响**：匹配按内容 SHA1 + 切片，与窗口在容器里的**位置无关**；
原地写保留未变窗口的位置，整文件重建按同一 parts 顺序组装 → 切片总能对上。
真遇到不同顺序组装的容器（对不上）→ 该源被拒 → 回退下载（优雅降级，不崩）。

**成本**：建表 O(全部 parts) 一次；每个 part 查表 O(1)；整文件哈希兜底。
典型效果：修复 251MB 的 ucas（2 个 chunk 损坏）→ 其余全部本地拷贝，只下载 2 个。

---

## 4. 下载层

- **`IChunkDownloader`**：`DownloadRange(Url, Offset, Size, Out)` —— 可插拔后端（自定义 CDN/P2P/加密）。
- **`FHttpRangeRequest`**（默认传输原语，`UnrealGamePatcherDownload`）：FHttpModule + `Range` 头 +
  `SetDelegateThreadPolicy(CompleteOnHttpThread)`（HTTP 事件循环线程自行驱动，无需游戏线程 tick）；
  `FHttpChunkDownloader`（Core 内）只是为旧调用方保留的同名适配器。
- **`FDownloadExecutor`**：窗口驱动（4MB 分段 / 默认窗口 24）、回调驱动单锁状态机
  （零专用线程，HTTP 链路/超时全交给引擎）、journal 位图续传（只补缺段）、
  **校验通过才改名落盘**（损坏绝不落盘）、任一失败整体失败并报告失败 id。
- **`FDownloadManager`**：业务薄壳（cache 检查 → 组装 Job 提交执行器 → 聚合等待 → 统计）。
- **外部 CDN 服务器**（`Scripts/cdn_server.py`，纯标准库）：Range 支持、运行时故障注入
  （`__ctl/fail_next`、`__ctl/truncate_next?bytes=N`）、可观测（`__ctl/stats`：file/ranges/errors/peak/maxrange）。

---

## 5. 格式细节速查（自研层）

### 5.1 扩展清单 `UnrealGamePatcher.index.json`
```
{ format, version, appId, contentVersion, baseVersion, verifyOnUpdate?,
  files: { <id>: { sha1, size, parts:[{chunk,offset,size}], entries?:[{path,offset,storedSize,uncompressedSize,sha1}],
                  ioChunks?:[{id,offset,size,metaHash?}] } },
  chunks: { <sha1>: { sha1, size, file } },
  selfCheck: <XXH3-64 16hex> }
```
（`updateStrategy` 已移除——只写的遗留字段；应用策略由客户端逐文件按扩展名 + 元数据存在性决定。）
- `selfCheck` = XXH3-64(规范化正文，除 selfCheck 外)；Save/Load 同一 `SerializeBody` 保证确定性。
- 加载时严格校验（缺失字段/错误类型/自校验不符 → 拒收）。

### 5.2 pak（`FPakReader` / `FPakWriter`）
- footer/目录解析（多版本分发）；V10+ 需要 PathHashIndex 或 FullDirectoryIndex 才能挂载。
- **内置每文件 SHA1 在 payload header（53B）**，不在编码索引里；未压缩条目 = 原始数据 SHA1。
- 写：CoW 重追加（目录+footer 尾部重写，footer 512 扇区对齐单写 = 提交点）。
- 坑：`FString` 写盘长度含 null 终止符，必须显式补 0（曾修 OOB 导致输出不确定）。

### 5.3 IoStore（`FIoStoreReader` / `FIoStoreWriter`）
- utoc 头 144B（TocMagic "-==--==--==--==-"、版本 8、块大小 65536、方法名（Oodle）等）。
- 布局：头 | chunk ids(12B) | offset-lengths(10B=5+5，MSB-first) | perfect-hash seeds | blocks(12B: offset 5+csize 3+usz 3+method 1) | 方法名 | 目录索引 | meta(24B=FIoHash 20+flags 1+pad 3)。
- chunk 数据 = 其块区间内各块的物理数据（Oodle/zlib 解压）拼接；RawOffset 是逻辑偏移。
- chunk id = 内容 hash 前 11 字节 + 类型字节；**完整性用 meta hash**。
- 写：`ReplaceChunk`（追加新数据 + 重定向条目）/ `AppendChunk`（新条目），`Commit` 整体重写 utoc（原子）。

---

## 6. 维护命令

| 命令 | 作用 |
|------|------|
| `check -cdn= -version= -install=` | 差异 + 空洞/膨胀率报告（>10% 或 >512MB 提示 compact），不写盘 |
| `compact -cdn= -version= -install=` | 重建带空洞文件（临时文件 + 原子替换），清空洞记录 |
| `repair -cdn= -version= -install=` | 只重建损坏（hash 不匹配）文件，不做增删 |
| `pakdump / pakpatch` | pak 解析 / 增量编辑测试工具 |
| `iostoredump -utoc= -ucas=` | 转储 utoc chunk 清单 |

---

## 7. 验证与测试

自测入口 15 个 = 13 套功能套件 + `benchtest`（性能基准）+ `perftest`（`e2etest` 的别名）（`UnrealGamePatcher.exe -cli <suite>`，全过 = 退出码 0）：

`selftest / statetest / indextest / difftest / buildtest / diffbuild / updatetest / maintest / downloadtest / benchtest / iostoretest / e2etest / perftest / pakinplace / iostoreinplace`

每套的**解决什么问题/怎么做/预期效果**见 `TESTCASES.md`（测试刻度尺文档）。

**I/O 统计（FIOStats，2026-10-09）**：进程级 I/O 记账——磁盘读/写（字节+次数）、网络
下行/上行、压缩/解压（入/出+次数）、chunk 库加载次数、哈希字节数。update / compact 结束
打印全量摘要（`FUpdateOrchestrator: update I/O update <v>: disk read ... | chunk loads ...`）
并写入 `FUpdateStats::IOStats`（测试可断言）；pak/ucas 原地 patcher 各自打印本次 patch 的
增量。用途：**让静默回退可见**——一次小增量若出现容器级读/下/写体积，说明命中
了回退路径（H1 案例：修复前 pak CoW 在网络模式静默退化为全容器重下+重写，写量统计
完全看不出来）。downloadtest 的 bigpak 场景以断言钉死该契约（CoW 只写变化条目 +
只读其覆盖窗口）。

实测性能（真实 IOStoreDemo 打包产物）：
- fixture 全量安装 10.4MB（9 chunk）。单文件变更：下载 9.4KB（0.1%）、写入 20KB（0.2%）；
  窗口内容寻址 + 本地容器复用（§3.6）后，未变窗口零下载。
- pak 原地：939B 包改 1 条目 + 增 1 条目 → 只写 91B（5.4%）。
- ucas 原地：10.3MB 容器换 1 chunk + 增 1 chunk → 只追加 12KB（0.12%），重跑收敛为 no-op。

---

## 8. 已知限制与后续

- ucas 原地结果是"逻辑等价非字节一致"；若要字节一致需按源压缩参数重编码（脆弱，暂不）。
- `-cdn` 为 `http(s)://` 时 latest.json / index / chunks 全走 HTTP（同一数据源）；
  `check`/`compact`/`repair` 是本地维护命令，`-cdn` 用本地目录（裸路径或 `file://`）。
- 无 AES/.sig 加密容器支持（IoStore encrypted/signed 直接报错）。

- 真实"引擎加载更新后容器"的 e2e 未自动化（目前用自研 reader 交叉验证代替）。
