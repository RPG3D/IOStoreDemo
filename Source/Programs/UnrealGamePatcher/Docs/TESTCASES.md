# UnrealGamePatcher 测试说明（TESTCASES）

> 本文档是 UGP 全部测试套件的**权威刻度尺**：每一套解决什么问题、怎么做、
> 预期效果（关键断言）与依赖。AI 开发 UGP 时以此为准——改动后必须保证
> 所有套件通过（`UnrealGamePatcher.exe -cli <suite>` 退出码 0）。
> 与 `TECHNICAL.md`（实现细节）互补；README.md 是概念介绍。

---

## 运行方式

```bat
UnrealGamePatcher.exe -cli <suite>
```

| 套件 | 一句话 | 覆盖 |
|------|--------|------|
| `selftest` | 哈希封装自测 | 哈希工具层 |
| `statetest` | sidecar 状态文件 读写/损坏检测 | 事务/状态层 |
| `indextest` | 扩展清单 读写/确定性/防篡改 | 清单格式层 |
| `difftest` | 差异分析分类 + 本地真相提取 + 容器虚拟磁盘分类 | 差异层 |
| `buildtest` | 构建侧 ingest 确定性/一致性 | 发布侧 |
| `diffbuild` | 构建侧版本 diff/跳版本/防篡改 | 发布侧 |
| `updatetest` | 客户端更新主流程（下载/复用/修复/信任/验证） | 更新管线 |
| `maintest` | check / compact / repair 维护命令 | 维护层 |
| `downloadtest` | HTTP 下载层（range/续传/并发/重试/占用文件） | 下载层 |
| `benchtest` | 吞吐/内存基准（窗口 vs 单连接） | 性能基准 |
| `iostoretest` | IoStore 读取器（utoc 解析/chunk 解码） | 格式层 |
| `e2etest` | 端到端管线（ingest→update→引擎可加载） | 全链路 |
| `perftest` | 增量 vs 全量 下载/写入 比率测量 | 性能 |
| `pakinplace` | pak 条目级原地写（只写变化条目） | 原地写 |
| `iostoreinplace` | ucas chunk 级原地写（追加+utoc 原子重写） | 原地写 |

**核心语义（自 2026-08 引入）**：
- **虚拟磁盘模型**：pak/ucas 容器的真值 = **内部子文件 hash**（pak 条目内置
  SHA1 / ucas 的 utoc chunk id），**不是容器整文件 hash**——原地写导致每台
  机器物理布局（字节顺序）不同，整文件 hash 不是有效真值。
- **信任/验证双模式**：默认更新是**信任模式**（信任上次提交的本地 index，
  不重读容器、不 hash 普通文件）；`repair`/`check`/`verifyOnUpdate` 标记/
  超过验证间隔（默认 10 天）触发**验证模式**（重读容器结构、重算条目
  payload hash、逐 chunk 数据校验、hash 全部普通文件）。
- **同尺寸损坏检测只发生在验证模式**——信任模式故意不检测（可接受代价，
  定期验证/手动 repair 兜底）。

---

## 1. selftest — 哈希封装自测

**解决什么问题**：`FPatcherHash` 是全部校验/命名的地基（XXH3-64 自校验、
FIoHash/chunk id 的 hex 转换），一处错全盘错。

**怎么做**：对 `FPatcherHash` 的每个封装跑已知向量 + 往返测试（hex 转字节再
转回、大小端一致性）。

**预期效果**：全部哈希 API 与引擎/外部工具（xxhsum -H2、LexToString）一致。

---

## 2. statetest — sidecar 状态文件

**解决什么问题**：`UnrealGamePatcher.state.json`（事务/空洞）是回滚与崩溃恢复
的唯一依据，文件损坏必须被**拒收**而非静默接受。

**怎么做**：临时目录下（1）写/读完整状态（空洞 + pending 事务）往返；
（2）空状态往返；（3）7 种损坏注入：非 JSON、缺 format、错 format、
错 version、截断 JSON、文件缺失、事务 target 类型错误。

**预期效果**：合法状态往返逐字段一致；7 种损坏全部 `Load` 返回 false。

---

## 3. indextest — 扩展清单

**解决什么问题**：`UnrealGamePatcher.index.json` 是 CDN 侧唯一真相，
带 XXH3-64 自校验——任何篡改/损坏必须被检测并拒收。

**怎么做**：（1）完整清单往返（files/chunks/baseVersion + parts/entries/
ioChunks 容器元数据 + verifyOnUpdate）；（2）空清单往返；（3）**确定性**：同清单
存两次字节一致（自校验是内容的函数）；（4）**篡改检测**：改文件 sha1 一位、改
contentVersion、改 chunk size、删除 selfCheck 字段——全部拒收；（5）**损坏/schema**：
非 JSON、错 format、错 version、缺 appId、非 hex sha1、坏容器元数据、文件缺失——全部拒收。

**预期效果**：合法往返一致、确定性成立、7+ 种篡改/损坏全部 `Load` false。

---

## 4. difftest — 差异分析 + 本地真相 + 容器虚拟磁盘分类

**解决什么问题**：差异分类（Same/Changed/Added/Removed）是更新的判定基础；
本地真相提取决定"客户端看到什么"。

**怎么做**：
1. 手搓 manifests 四分类（same/changed/added/removed）；
2. 边界（identical/empty/单边缺失）；
3. chunk 级 diff（同 id 跳过/新 id/缺 id）；
4. 从**真实 pak**（.manifesttest 打包产物）提取本地真相（逐条目内置 hash）；
5b. **C1/C2 回归**：同 id 不同 meta hash → ucas Changed（id 是包名派生的，
   内容信号是 meta hash）；无 metaHash 的旧清单回退 id-only 语义；pak 端
   本地独有条目（被移除的资产）→ Changed（旧的 remote⊆local 比较漏掉它）；
5. 端到端：本地真相 vs 手搓远端 + 反向 diff 对称性；
6. **容器虚拟磁盘分类**（新增）：同条目不同物理顺序的两个 pak → Same（忽略
   整文件 hash）；改一条目 → Changed；同 chunk-id 多重集不同顺序的两个 ucas
   → Same；改一个 chunk id → Changed；utoc 镜像其 ucas（Same/Changed）。

**预期效果**：全部分类精确；**"不同追加顺序的两容器判为 Same"**是虚拟磁盘
模型的直接验证——原地写不会误判 Changed。

---

## 5. buildtest — 构建侧 ingest

**解决什么问题**：`FBuildIngestor` 把标准 UAT 输出（staged 目录）切成自研 1MiB
窗口 chunk 并导出扩展清单，必须确定、一致、幂等。

**怎么做**：同一 staged fixture（真实 pak/ucas/utoc + 一个普通文件）两次 ingest
到不同 CDN 根；比较输出（1）两次 ingest 的 index **字节一致**（确定性）；
（2）清单文件哈希与 staged 磁盘文件一致、容器条目元数据直接从容器解析（pak
逐条目 / ucas 逐 chunk）；（3）chunk 文件在 CDN 布局且大小与清单一致；
（4）同一 CDN 根幂等重入。

**预期效果**：两次 ingest 字节级一致；清单与 staged 一致；重复 ingest 不破坏。
另含 **C6/M5 回归**：空 staged 目录拒收（发布空清单 = 客户端删光安装）、
`-staged=` 尾部分隔符不破坏文件 id。

---

## 6. diffbuild — 构建侧版本 diff

**解决什么问题**：`FBuildDiffer` 产出 `UnrealGamePatcher.delta`（发布侧增量），
必须支持任意两版本 diff 与防篡改。

**怎么做**：（1）手搓 v1→v2 全部分类；（2）相同清单 diff 为空；（3）**版本共存 +
跳版本**：v1→v3 的 delta == v1→v2 与 v2→v3 的并集；（4）delta 篡改检测；
（5）AppId 不匹配拒收。

**预期效果**：分类正确、空 diff、跳版本并集成立、篡改拒收、App 校验。

---

## 7. updatetest — 客户端更新主流程（最大套件）

**解决什么问题**：更新管线的每一环——全量安装、幂等、只检查不写、删除、
损坏修复、chunk 复用、信任/验证双模式、崩溃恢复。

**怎么做**（真实 IOStoreDemo 打包产物 ingest 进本地 CDN）：
1. **空目录全量安装**：4 文件（pak/ucas/utoc/boot.txt）、13 chunk 全取、逐文件 hash==清单、写入字节数
   精确；
2. **pak 完整性交叉验证**：安装后 pak 每条目内置 hash 匹配 payload；
3. **no-op**：再次更新 0 取 0 写；
4. **check-only 纯读**：报 stray 为 removed 但不删、状态文件不动；
5. **删除**：stray 文件被删；
6. **损坏修复**（**验证/repair 模式**）：翻转 ucas 中间字节 → 修复 ucas+utoc，
   逐 chunk 数据校验抓出损坏（信任模式不检测——见 §9）；
7. **纯复用新增文件**：1.0.1 加 readme（用本地已有 chunk 切片）→ 0 取 1 复用（机制见 `TECHNICAL.md` §3.6）；
8. **部分复用+部分下载**：1.0.2 readme 增补 → 1 复用 1 取；
9. **信任模式跳过 hash**（新增）：损坏 ucas 后普通更新 → **不检测**（Same），
   并证明损坏仍在盘上（chunk 数据不匹配 utoc meta hash）；
10. **verifyOnUpdate 强制验证**（新增）：craft 1.0.7 带 `verifyOnUpdate` →
    更新自动进验证模式 → 修复 ucas+utoc，且 `LastVerifiedAt` 被记录；
11. **崩溃恢复**（1.0.3-1.0.6）：注入失败/崩溃 → 回滚或留 Pending → 重跑收敛。

**预期效果**：全量=精确字节；no-op=零读写；check=纯读；复用/取数精确计数；
损坏修复只在验证模式；信任模式故意不检测同尺寸损坏；verifyOnUpdate 是发布侧
强制验证触发器；崩溃后重跑收敛到目标版本。**§13 missingutoc（C3 回归）**：
信任模式下删除 utoc → update → utoc 以 Added 身份触发（不被镜像盖成 Same）
+ 兄弟 ucas 被强制重建 → ucas/utoc 对恢复一致 → 重跑 no-op——旧的镜像把
Added 盖成 Same，容器无法挂载却永远报成功。

---

## 8. maintest — check / compact / repair

**解决什么问题**：三个维护命令的正确性——check 纯读报告、compact 回收空洞、
repair 只修损坏。

**怎么做**：安装 1.0.0 后（1）**check**：no-op 报告 0 写 0 取、状态无空洞；
（2）**compact**：向 sidecar 注入 500KB 空洞 → 报告膨胀率 ~4.8% → compact
重建 ucas（内容等价）→ 空洞清除 → 空列表 no-op；（3）**repair**：翻转 pak
**条目 payload 内字节**（verify 模式重算 payload hash 才抓得到）→ repair 只
重建该 pak（逻辑等价：逐条目 hash 匹配远端）→ 不动 stray、不增删 → 幂等。

**预期效果**：check 纯读；compact 精准回收；repair 只修损坏且幂等；
**pak 修复后的断言是"逐条目逻辑等价"而非整文件字节一致**（虚拟磁盘模型）。

---

## 9. downloadtest — HTTP 下载层 + 网络更新

**解决什么问题**：真实 HTTP 传输（外部 python CDN 服务器）下的 range/
续传/并发/重试/自定义后端/占用文件回滚。

**怎么做**（起 `../Scripts/cdn_server.py`，故障注入经 `__ctl` 控制端点）：
- A/B/D-H（C 已并入 A 的并发断言）：range 下载、journal 位图续传（缺 N 段只重下
  N 段，含非连续位图）、并发（Peak ≥ 2）、重试、`__ctl` 故障注入（500/404/截断）、
  执行器级 H 组（分段/公平/取消/SHA1 终验/上限/探测回退/真 HTTP 段 0 的 206）；
- E：**端到端网络更新**——全量装（每 chunk 走 HTTP）→ no-op（命中缓存）→
  删一个缓存 chunk + 损坏 ucas → **强制验证更新**（`bForceVerify`）→ 缺失
  chunk 重新拉取（服务端请求数增加）、修复 ucas+utoc；
- F：自定义后端（文件映射）驱动全流程，证明传输与管线解耦；
- G：**占用文件**——翻转 pak 条目 payload 并锁文件（只读共享）→ 强制验证
  更新写失败 → 事务回滚（文件保持原样）→ 解锁后同更新成功（逻辑等价）；
- H：**执行器级用例**——大文件 4MB 分段（9MB=3 段）/ journal 只补缺段
  （offset 断言）/ **H2b 自然 journal 续传（C5 回归）**：让执行器自己产生
  journal（段 0 落盘 + 执行器中途销毁 = 真实崩溃现场），断言 journal 带
  UGPJRN1 头且重启只补缺段——此前三个续传用例全是手工造 journal，恰好掩盖了
  "全新下载从不写头"的生产 bug / 公平性（小文件先于大文件完成）/ 传输上限（谎报尺寸 →
  Header 阶段取消、磁盘零残留）/ 探测回退（无 Range 服务器 → 单次整文件 GET）。

**预期效果**：range/续传/并发/重试精确；网络更新与本地流一致；占用文件写
失败完整回滚；解锁后修复成功；分段/公平/上限/探测回退行为与 DOWNLOAD.md §5 一致。

---

## 10. iostoretest — IoStore 读取器

**解决什么问题**：自研 `FIoStoreReader` 能正确解析真实 utoc/ucas（引擎格式）。

**怎么做**：解析真实 `pakchunk1-Windows.utoc/ucas`：头/版本/块方法；**结构
完整性**（chunk 块区间覆盖、块偏移合法性）；解码一批 chunk（raw + 压缩），
逐 chunk 数据 hash 对照 utoc meta hash。

**预期效果**：真实容器解析成功、结构完整、chunk 解码正确且 meta hash 匹配。

---

## 11. e2etest — 端到端管线

**解决什么问题**：全链路（ingest → update → 引擎可加载）一次跑通。

**怎么做**：真实打包产物 ingest → 空目录安装 → 验证（1）每文件 hash 匹配
清单；（2）pak 打开且每条目内置 hash 匹配 payload（**引擎格式可加载**）；
（3）utoc/ucas 解析通过；（4）增量（1.0.1/1.0.2 readme）只取差量。

**预期效果**：安装结果可被引擎加载（自研 reader 交叉验证代替真实引擎 e2e）。

---

## 12. perftest — 增量性能测量

**解决什么问题**：量化"只下差量、原地写"相对全量重写的收益。

**怎么做**：全量安装（fetch/write 基线）→ 1.0.1/1.0.2 增量 → 计算
`delta/full` 比率。

**预期效果**：单文件变更的下载/写入远低于全量（实测 ~0.1%/~0.2%，窗口复用生效），并打印比率。

---

## 13. pakinplace — pak 条目级原地写

**解决什么问题**：**不同点**——只写变化条目，避免整 pak 重写（Steam/Wegame
弊端）。

**怎么做**：构造两个 pak 版本（一条目原地改、一条目追加），走真实管线 ingest
（带条目元数据）→ 装 v1 → 对 v2 做 `FInPlacePakPatcher::Patch` → 断言只写
了变化条目（统计：same 零搬动 / overwrite 原地 / append 追加）→ **逐条目
逻辑等价**（每远端条目本地存在且 hash 匹配；不要求整文件字节一致）。
压缩多块小节另含 **C7 回归**：v2 追加一个**新的 Oodle 压缩条目** → 原地
patcher 必须 bail 整文件重建（method-0 追加压缩字节 = 引擎读垃圾，且逐条目
hash 校验抓不住）→ 断言走重建路径且新条目在盘上保持压缩形态、解码正确。

**预期效果**：939B pak 改 1 条目+增 1 条目只写 ~91B（5.4%）；结果逻辑等价。
**4c 空洞回填回归**：4b 的原地 patch 把死区记入 sidecar → 1.2 再追加一个
新条目 → 断言新条目被放置在**旧 EOF 之下**（物理证据：落进了记录的空洞）
且解码正确、重跑 no-op。

---

## 14. iostoreinplace — ucas chunk 级原地写

**解决什么问题**：**不同点**——只追加变化 chunk + 原子重写 utoc，不动整个
10MB+ 容器。

**怎么做**：对真实 10.3MB 容器换 1 chunk + 增 1 chunk → `FInPlaceIoStorePatcher`
（替换/追加 raw 块 → utoc 原子重写）→ **chunk-id 多重集等价**验证 → 重跑
收敛为 no-op。**stableid 小节（C1 回归，端到端）**：模拟真实引擎容器——
chunk id 由包名派生（内容修改不改 id），v3 用显式目标 id（= 旧 id）替换
chunk 0 数据、meta hash 换新 → 断言 (Id, MetaHash) diff 判 Changed（旧
id-only 判据说 Same、更新永不下发）→ 原地 patch 保 id 换数据 → 解码 ==
编辑后内容 → 重跑 no-op。**回填断言**：1.2 的替换 chunk（4KB → 一个 64KB
块）必须塞进 1.1 替换留下的 64KB 死区——.ucas 文件尺寸零增长。

**预期效果**：只写 ~12KB（0.12%）；结果逻辑等价非字节一致；幂等收敛。

---

## 依赖与数据

- **真实打包产物 fixture**：`.manifesttest/chunkinstall/Windows/1.0.0/pakchunk1`
  （真实小容器 pak/ucas/utoc，作 staged 输入）——buildtest/difftest/updatetest/
  maintest/downloadtest/e2etest/perftest/pakinplace/iostoreinplace 共用；
- **外部 CDN 服务器**：`../Scripts/cdn_server.py`（downloadtest）；
- **测试工作目录**：`Saved/<suite>`（Program 的 ProjectDir 下），测完清理。

## 新增测试时

1. 在 CLI 加 `static int32 RunXxx()` + 分发表一条目（`{ TEXT("xxx"), RunXxx }`）；
2. 用共享脚手架（`MakeCheck`/`GetTestWorkDir`/`ResetWorkDir`/`FinishTest`/
   `Sha1OfBytes`/`CheckContainerLogicallyEqual`/`FRecordingBackend`）；
3. 每套套件结尾 `return FinishTest(bAllPassed, TEXT("xxx"));`；
4. 更新本文档（新增行的"解决什么问题/怎么做/预期效果"）。
