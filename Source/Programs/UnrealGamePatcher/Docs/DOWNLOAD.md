# 通用下载器（UnrealGamePatcher Download）

> **定位**：一套可独立复用的 UE 下载组件——从"任意大小文件的 HTTP(S) 断点续传"到
> "海量小文件的并行拉取"，业务层只派发任务，不感知线程/分段/续传细节。
> 已拆为独立模块 `UnrealGamePatcherDownload`（unified-downloader，见 ROADMAP.md §2），
> 本文件即该模块的组件规格书。状态标记：✅ = 已实现。

**基准实测**（`-cli benchtest`，本机 loopback + CDN 服务器每连接限速，2026-09-01）：

| 项目 | 结果 |
|---|---|
| 吞吐·单连接（1MB/s 限速 × 16MB） | 16.1s（**0.99 MB/s**，贴限速上限） |
| 吞吐·窗口执行器（4 段并发） | 4.0s（**3.99 MB/s**） |
| **并发加速比** | **×4.02 ≈ 段数线性扩展**（窗口是有效旋钮的直接实证） |
| 内存·窗口 24 × 4MB 数据预算 | 96MB；进程峰值增量实测 ~192MB（引擎 curl response TArray ×1.5 增长 + realloc 瞬时双份，常量系数 ~2×，窗口仍是线性旋钮） |

---

## 1. 能力总览

| 能力 | 状态 | 说明 |
|---|---|---|
| HTTP(S) Range 精确区间 / 开放结尾 / 整文件 GET | ✅ | 统一原语，详见 §3 |
| 传输层内存硬上限（Header 阶段取消） | ✅ | 默认 256MB，永不缓冲超限响应 |
| 进度看门狗（防 Content-Length 撒谎） | ✅ | 已收字节超限即取消 |
| 断点续传（journal 位图，段粒度） | ✅ | 见 §6 |
| 窗口驱动执行器（回调驱动单锁状态机，零专用线程） | ✅ | §5 |
| 分段并行下载大文件（4MB 量子） | ✅ | §5 |
| 校验后落盘（损坏数据绝不落盘） | ✅ | 尺寸 + 可选 SHA1 |
| 传输重试 / 超时取消 | ✅ | 引擎 curl 连接/活动超时；执行器有界重试 |
| 可插拔后端 | ✅ | `IChunkDownloader` 接缝（自定义 CDN/P2P/文件映射测试后端） |
| job 级协作式取消 | ✅ | 执行器 `Cancel(Handle)`，单次 OnDone 语义不变 |

## 2. 架构分层

```
业务层        派发任务（Url/Dest/ExpectedSize/ExpectedSha1），只等完成
                │
业务壳        FDownloadManager（✅）：cache 检查 / SHA1 终验 / 落位
                │                    （传输与线程细节对业务不可见）
执行器        FDownloadExecutor（✅）：窗口调度，回调驱动单锁状态机（§5）
                │
传输原语      FHttpRangeRequest（✅）：单请求原语，见 §3
                │
引擎 IO 线程  UE HttpThread（✅ 已在复用）：单线程 libcurl multi 事件循环，
              所有 in-flight 连接在这条线程上非阻塞复用——回调/收发都发生在它
              上面，它是"并发连接数"的真实载体（与线程数无关）
```

**线程职责划分（已拍板，实施两次简化后收敛）**：HTTP 链路（连接复用/超时/回调
串行化）**完全交给引擎**；执行器**不设任何专用线程**——全部 job 状态在单一状态
对象里，由一把锁串行化"调用线程 Submit/Cancel"与"引擎 IO 线程 settle 回调"两个
入口，回调内联完成写盘/journal/终验/落位并补窗口。传输并发由**窗口大小**决定。

## 3. 传输原语规格（✅ 已实现，语义以此为准）

`IChunkDownloader::DownloadRange(Url, Offset, Size, OutData)`：

| 参数组合 | 行为 |
|---|---|
| `Size >= 0`（任意 Offset，含 0） | 精确区间：`Range: bytes=Offset-(Offset+Size-1)`，**必须 206**。多段下载的段 0 也走此路（显式 `bytes=0-(n-1)`，绝不整文件误取） |
| `Offset > 0, Size < 0` | 开放结尾：`Range: bytes=Offset-`，**必须 206** |
| `Offset == 0, Size < 0` | **整文件 GET（不发 Range 头）**——请求整个资源时走此路：单段整文件下载与探测回退（无 Range 服务器）；执行器规则 = "整个资源 = 普通 GET，真子区间 = 精确 Range" |
| `Size == 0` | 拒绝（调用方 bug） |

**响应校验链**（任一失败即拒，坏数据绝不交给调用方）：
1. **传输层上限**：`OnHeaderReceived` 解析 `Content-Length`，超过 `MaxResponseBytes`
   （默认 **256MB**，public static 可调）→ 立即 `CancelRequest()`——body 一个字节不下；
   `OnRequestProgress64` 看门狗兜底（防 Content-Length 撒谎 / chunked 无长度）
2. HTTP 状态 200/206；**发了 Range 头就必须 206**（防剥 Range 的代理回全量）
3. 长度精确匹配（`Size >= 0` 时响应字节数必须等于 Size）
4. 非空响应

**并发与取消安全**：
- 原子 Done 标志保证**每次等待周期恰好一次事件触发**——Header 取消 / 进度取消 /
  正常完成 / 超时四条路径互不产生二次触发（二次触发会打到已还池的事件 = 跨请求污染）
- 60s 单请求超时：`CancelRequest()` + 事件**不还池**（宁可泄漏一个事件，不冒池污染风险）
- 请求创建串行化（curl easy-handle 在调用线程配置）

**已知边界**（已解决）：原 v1 把 `Offset=0, Size=N` 固定为"整文件"，导致"取更大
文件的前 N 字节"不可表达、多段下载的段 0 会误取整个 body——unified-downloader
已改为 **`Size >= 0` 一律精确区间（含 Offset=0）**，整文件 GET 仅剩
`Offset=0, Size<0` 一种形态（探测回退专用）。

## 4. 内存与安全边界（✅）

| 防线 | 值 | 防什么 |
|---|---|---|
| 传输层 `MaxResponseBytes` | 256MB | CDN 返回 100GB 响应 → Header 阶段取消 |
| 分段窗口内存预算 | 窗口 24 × 4MB = 96MB 数据（实测进程峰值 ~2×，见 §1 基准） | 内存上界静态可算，窗口是线性旋钮 |
| 业务层 `MaxManifestBytes` | 64MB | 元数据（json/xml）超限拒收（10 万 chunk 的 index ≈ 30MB，定容见 ROADMAP §1.3） |
| 任务 `Size` 预检 | ≤ 传输上限 | manifest 被篡改声明巨量 chunk → 下载前拒绝 |
| 本地元数据预检 | 同 64MB | 本地 CDN 目录里的超大 index/latest.json 读前拒绝 |

## 5. 分段模型与执行器运行模型（蓝图，Phase 1）

### a) 统一分段模型

没有两条下载路径，只有一条：**一切下载 = 分段任务序列**。1MB 小文件 = 单段特例；
100GB = 25,600 段。

**段大小 = 4MB（默认常量，可配）**——内存/调度/续传的统一量子。定容依据：
- 内存量子：段大小 = 单请求峰值。窗口 24 并发 × 4MB = **~96MB 峰值**
- 请求开销：100GB / 4MB = 25,600 请求，keep-alive 下可接受
- 断点粒度：中断后最小重下 4MB

### b) 执行器运行模型：回调驱动单锁状态机（零专用线程，已实施）

**原则：HTTP 链路完全交给引擎**——连接复用、活动超时（引擎 curl 层默认 30s 无进度
即断）、回调串行化，`FHttpModule` + HttpThread 全部现成；执行器**零专用线程**：

```
Submit/Cancel（调用线程）        lock → 建/标记 FJob → Refill()
HTTP settle 回调（引擎 HttpThread）lock → 写段/journal/verify/promote → Refill()
```

- **并发旋钮 = 窗口大小**——想 32 路并发不必开 32 线程（实际上一条线程都不开），
  内存代价 = 窗口 × 4MB（明确可算）
- **单一状态对象 + 一把锁**：全部 job 状态（段/重试/journal/调度）在一个
  `FExecutorState` 里，两个入口（调用线程、引擎回调线程）都先拿锁再动状态；
  竞态面收敛为"锁内重入"，调度决策天然串行（单调度点）
- **超时交给引擎**：curl `CURLOPT_CONNECTTIMEOUT`（默认 30s）+ 活动超时（30s 无
  进度即断并回调失败），执行器不另建计时器；段失败走有界重试（默认 3 次）
- **权衡**：段写（4MB 毫秒级）与整文件 SHA1（大文件秒级）内联在 HttpThread 上，
  期间其他连接暂停但无正确性问题；吞吐基准（见文首"基准实测"表，`-cli benchtest`
  可复跑）如有影响，再把 verify 单独
  派发引擎线程池——那是唯一值得考虑的"任务分发"

### c) 两规则调度（窗口补位时生效）

```
1. 收尾优先：最早入队、剩余段数 ≤ 2 的下载 → 优先补位（每 job in-flight ≤ 4）
   （小文件几乎入队即完成 → 完成顺序贴近入队顺序）
2. 任意补位：其余 job 不限量补位（单文件任务要吃满窗口；per-job 公平上限是
   有意的吞吐取舍，不实现——将来需要公平性时用调度加权，而非硬 cap）
   ⚠️ 历史注记（2026-10-09 review M23 → 2026-10-24 修复）：`J.InFlight` 曾只增
   不减（Cancel 的 InFlight==0 判定随之失效），已改为 settle 对称递减；本节原
   "三规则"描述与代码的两规则不符，已按代码回正（详见 fixplan-2026-10-24.md）。
3. 尾段自然补位
```
纯段级 FIFO 会饿死小文件（大文件入队瞬间灌满窗口）；纯按下载并行又无法保证
先入先完成。收尾优先 + 不限量补位是两者的工程折中。

**探测**：size 未知时发 `GET Range: bytes=0-0`，一个请求同时拿到
`Content-Range: .../total` 与 Range 支持验证（比 HEAD 可靠）。

## 6. 断点续传（✅ journal 位图，段粒度）

**已实施（✅）**：journal 位图——预分配目标文件（`Seek(total-1)` 写 1 字节），每段
完成直接 `Seek(offset)+Write` 落位（`FILEWRITE_Append` 非截断打开，避免多段文件
互相清零）并在 `.journal` 记位；崩溃重启读 journal 只补缺段（staging 尺寸与 journal
不符则丢弃续传重来）；全部完成 → SHA1 终验 → 删 journal。
段粒度续传，中断损失 ≤ 4MB。

**历史**：Phase 0 的 `.part` 追加式（`ResumeFrom = .part` 现有字节数，整文件粒度）
已被上述段粒度 journal 模型替换。

## 7. 业务壳职责与使用方式（✅）

`FDownloadManager` 保留业务职责，传输可整体替换：

```cpp
// 1. 组装任务（id/url/期望尺寸/落点；ExpectedSha1 可选做终验）
TArray<FDownloadRequest> Requests;
for (...) { FDownloadRequest R; R.Id=...; R.Url=...; R.Size=...; R.DestFile=...; Requests.Add(R); }

// 2. 派发（MaxConcurrent 映射为执行器窗口，默认 24；可插拔 Downloader 传入）
FHttpRangeRequest Http;                  // 或自定义 IChunkDownloader 后端
FDownloadManager Manager(Http, 24, 3);
FDownloadStats Stats; TArray<FString> Failed;
if (Manager.DownloadAll(Requests, Stats, Failed)) { /* 全部就位 */ }
```

语义保证：
- **cache 命中跳过**（尺寸+哈希校验通过的目标不重下）
- **校验后落盘**：staging 文件完成尺寸+SHA1 校验通过才原子换名成 DestFile
- **失败语义**：任一目标超过重试上限 → 整批失败，`Failed` 带回失败 id 列表
- **线程**：执行器零专用线程（§5b）；UE HTTP 委派在 HTTP 线程派发
  （`CompleteOnHttpThread`），调用线程阻塞等待聚合事件、无 UI 依赖

## 8. 设计决策记录

| 决策 | 结论 | 依据 |
|---|---|---|
| 不引入开源下载库 | 自研执行器 + 引擎 FHttpModule | 难的是调度/续传/校验/集成（90%），不是 HTTP 协议（10%）；aria2 GPL、BITS 依赖系统服务、cpp-httplib 太薄（ROADMAP §2.6） |
| `Size>=0` = 精确区间（含 Offset=0） | v2 修订 | 原"`Offset=0` = 整文件"使段 0 误取整个 body（实测：9MB body ≠ 4MB 段，重试耗尽）；整文件 GET 收敛为 `Offset=0,Size<0` 单一形态，探测回退专用 |
| 段 4MB / 收尾阈值 2 / 每下载并发 4 | 已拍板 | 内存量子与请求开销的平衡；先入先完成 vs 队头阻塞的工程折中 |
| 两层上限 | 传输 256MB / 元数据 64MB | 传输层保内存安全；业务层保语义正确 |
| 事件单次触发（原子 Done） | 已实施 | 早取消与正常完成竞争触发 = 跨请求事件污染（实测修复） |
| **回调驱动单锁状态机（零专用线程）** | 已实施 | HTTP 链路（复用/超时/回调串行化）引擎全包；自建线程/队列/计时器皆为重复机制（源码核实 CurlHttp.cpp）；竞态面收敛为一把锁；并发旋钮 = 窗口大小 |

## 9. Phase 1 落地清单（指向 ROADMAP.md §2）

1. 新模块 `UnrealGamePatcherDownload`：`FHttpRangeRequest`（原语迁移）+
   `FDownloadExecutor`（窗口调度，回调驱动单锁状态机 + journal，运行模型见 §5b）+ `FDownloadJob/Stats`
2. `FDownloadManager` 退化为业务薄壳；`IChunkDownloader` 接缝保留
3. 测试：现有 downloadtest 全迁移 + 大文件分段/中断恢复/公平性/上限取消/探测回退新用例
4. 验收：14/14 全绿 + 真实包端到端 + 大文件分段吞吐基准
