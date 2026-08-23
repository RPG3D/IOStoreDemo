## Context

下载层现状（Phase 0 已硬化）：`FHttpChunkDownloader`（同步单请求原语，Range 三态/
206 校验/256MB 传输上限/Header 阶段取消/进度看门狗）+ `FDownloadManager`（8 阻塞
worker、`.part` 追加续传、校验后落盘）。两个结构性问题：①"传输并发 = 线程数"
（8 worker 大半时间阻塞在等 IO，而 UE HttpThread 本就是单线程 curl_multi 复用、
并发连接数的真实载体）；②单响应整缓冲 + 无分段，大文件（>256MB 单响应）被传输
上限直接拒绝。设计已评审拍板并完整记录于
`Source/Programs/UnrealGamePatcher/Docs/DOWNLOAD.md`（组件规格书）与 `ROADMAP.md` §2，
本设计不重复其内容，只记录落地细节与差异。

约束：
- 传输继续走引擎 `FHttpModule`（不引入第三方下载库，决策记录见 DOWNLOAD.md §8）
- 现有调用方（`FDownloadManager::DownloadAll` 的全部 14 套测试、orchestrator 网络模式）
  对外行为不变——重构对业务层透明
- 新模块不得依赖 UI/业务（`bCompileAgainstEngine=false` 同级 Program 模块）

## Goals / Non-Goals

**Goals**
- 新模块 `UnrealGamePatcherDownload`：原语迁移 + 窗口驱动执行器 + journal 续传
- 传输并发与线程数解耦（窗口 = 并发旋钮），大文件可用
- 全部现有测试无回归 + 新增用例覆盖分段/续传/公平性/上限/探测

**Non-Goals**
- P2P/多协议（`IChunkDownloader` 接缝留给未来后端）
- HTTP/2 多路复用调优、代理认证、限速策略
- orchestrator/事务/回滚的任何行为改动

## Decisions

### D1. 回调驱动单锁状态机（最终模型；替代"N 个阻塞 worker"，实施中两次简化后收敛）

**HTTP 链路完全交给引擎**：连接池/连接复用、活动超时（引擎 curl 层默认 30s 无进度
即断）、完成回调串行化，引擎 `FHttpModule` + HttpThread 已全部提供（源码核实
`CurlHttp.cpp` 的 `CURLOPT_CONNECTTIMEOUT` + activity timeout timer）。执行器
**不设任何专用线程**（实施先从计算池简化为单工作线程、再简化为零线程），全部
job 状态收敛为一个 `FExecutorState`，唯一同步原语是一把锁：

```
Submit/Cancel（调用线程）     lock → 建/标记 FJob → Refill()
HTTP 回调（引擎 HttpThread）  lock → settle → 写段/journal/verify/promote → Refill()
```

传输并发 = in-flight 窗口（默认 24 段 ≈ 96MB 峰值）。回调内联做段写（4MB 毫秒级）
与整文件 SHA1（大文件秒级停顿，期间其他连接暂停但无正确性问题；5.2 基准如有影响
再把 verify 单独派发引擎线程池）。收益：删线程/队列/轮询 ~200 行，回调直接写段
省一次 4MB 拷贝；模型与引擎贴合，竞态面收敛为"一把锁内重入即可"。

### D2. 原语迁移与双入口

`FHttpChunkDownloader` 原样迁移为模块内 `FHttpRangeRequest`（对外名可保留别名，
Phase 0 语义与测试零改动）。执行器**不走同步阻塞接口**，直接绑定原语的回调形态
（`OnHeaderReceived`/`OnRequestProgress64`/`OnProcessRequestComplete`）——同步
`DownloadRange` 保留给简单调用方与测试。

### D3. 分段状态机（per-job）

```
Pending → Probing(仅 size 未知) → Segments{Planned, InFlight, Done} → Verifying
        → Promoting(原子落位) → Completed | Failed | Canceled
```
- 段重试：单段传输失败重试 N 次；内容校验失败（长度不符）同段重取
- 尾段对齐：`last = total - (segments-1)*SegmentSize`
- journal：目标文件旁 `<dest>.journal`（位图 + 魔数 + 段大小 + 总大小），每次段
  完成追加记录（非覆写位图，避免 journal 自身半写）；启动时重放校验（魔数/段大小
  /总大小不匹配 → 丢弃重来）
- 预分配：`Seek(total-1)` 写 1 字节（IFileHandle 无 SetSize，这是平台无关写法）

### D4. 三规则调度的数据结构

每个 job：`EnqueueOrder`（原子递增）、`SegmentsRemaining`、`InFlightCount`、
`SegmentsLeft≤2 优先`。调度器在"窗口有空位"事件点运行（任何 settle/Submit 后的
`Refill()`，单调度点），选择规则：① 最早入队且剩余 ≤ 2 → 补；② 否则按入队序轮转
且该 job in-flight < 4 → 补下一段；③ 无可补则等待。所有调度决策与状态迁移都在
同一把锁内串行执行（D1 单锁状态机）。

### D5. 业务壳边界

`FDownloadManager` 签名不变（`DownloadAll(Requests, Stats, Failed)`），内部改为：
cache/预检 → 组装 `FDownloadJob` 提交执行器 → 等全部完成回调（一个聚合原语）→
终验/落位统计。失败语义不变（整批失败 + 失败 id）。`IChunkDownloader` 接缝保留：
自定义后端需提供"回调式单请求"能力（文件映射测试后端同步实现即可，回调同步触发
同样成立）。

### D6. 配置面

`FDownloadExecutorConfig`：SegmentSize（4MB）、Window（24）、MaxRetries（3）。
超时交给引擎（连接 30s + 活动 30s，见 D1），不自建计时器。均静态常量默认 +
构造可覆盖；不做 ini 绑定（Program 无配置体系，保持显式）。

## Risks / Trade-offs

- **状态机复杂度**：回调驱动把线性流程切成状态机（D3），以"每段状态原子量 + 单调
  度点"控制；测试用确定性注入后端覆盖全部迁移边
- **引擎回调线程语义**：依赖 `CompleteOnHttpThread` 的既有行为（Phase 0 已实测验证
  Header/Progress/Complete 三委派线程）；若引擎配置变化（如禁用 HttpThread 改游戏
  线程 tick），原语层已有 `NeedsSingleThreadTick` 兼容路径可查
- **journal 与预分配的磁盘开销**：预分配写 1 字节 + journal 小文件，可忽略
- **内存峰值**：窗口 × 4MB ≈ 96MB（默认），配置显式可调

## Migration Plan

1. 建模块骨架 + 原语迁移（纯移动，14/14 全绿）
2. 执行器 + 三规则调度（downloadtest 切换到执行器，14/14 全绿）
3. journal 续传替换 `.part`（downloadtest B 升级，14/14 全绿）
4. `FDownloadManager` 切薄壳；大文件/公平性/探测新用例
每步独立可提交；真实包端到端（rustfs）在步骤 2/4 后各跑一次。

## Open Questions

- 执行器单例还是 per-batch 实例？（倾向 per-batch：与现 DownloadAll 语义对齐，
  无跨批状态；Phase 1 先 per-batch，真实需求出现再引入共享单例）
- 公平性测试的"先入先完成"断言精度（小文件先完成即可，不苛求严格顺序）
