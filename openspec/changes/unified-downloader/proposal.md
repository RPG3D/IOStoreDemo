## Why

UnrealGamePatcher 的下载层已成长为可独立复用的通用组件（Phase 0 完成传输原语硬化：
内存硬上限、Header 阶段取消、Range 语义修正），但当前"8 个阻塞 worker"模型把
**传输并发**与**线程数**耦合，且无法服务大文件（单响应整缓冲）。UE 的 HttpThread
本就是单线程 curl_multi 复用（并发连接数的真实载体），多线程只该承担哈希/写盘等
重活——需要把下载器重构为"回调单线程 + 窗口 + 计算池"的执行器模型，并拆为独立
模块（设计已评审拍板，见 `Source/Programs/UnrealGamePatcher/Docs/DOWNLOAD.md` 与
`ROADMAP.md` §2）。

## What Changes

- **新模块 `UnrealGamePatcherDownload`**（无 UI/无业务依赖，可独立测试）：
  - `FHttpRangeRequest`：传输原语迁移（Phase 0 已硬化的单请求语义原样保留）
  - `FDownloadExecutor`：窗口驱动执行器——in-flight 窗口（默认 24）挂引擎 IO 线程，
    回调只做轻量簿记；计算池（2-4 线程）承担 SHA1/写盘/落位；三规则公平调度在
    窗口补位时生效
  - `FDownloadJob` / `FDownloadStats`：任务描述与字节级统计
- **分段模型**：一切下载 = 分段任务序列（段 = 4MB 统一量子）；大文件按段并行、
  小文件退化为单段
- **断点续传升级**：`.part` 追加式 → journal 位图（预分配 + 段粒度续传，中断损失 ≤ 4MB）
- **探测**：size 未知的下载先 `Range: bytes=0-0` 探测总大小并验证 Range 支持
- **`FDownloadManager` 退化为业务薄壳**：保留 cache 检查 / SHA1 终验 / 落位；
  传输与线程细节全部下沉执行器；`IChunkDownloader` 可插拔接缝保留
- **两层内存上限继承**：传输层 `MaxResponseBytes`（256MB）+ 业务层 `MaxManifestBytes`（64MB）
- 测试：现有 downloadtest 全迁移 + 新增大文件分段/中断恢复/公平性/上限取消/探测回退用例

不改动：pak/ucas 解析与原地写、事务/回滚、index/delta 格式、CLI/GUI 对外命令面。

## Capabilities

### New Capabilities
- `download-executor`: 通用下载执行器——统一分段模型（4MB 量子）、窗口驱动运行模型
  （回调单线程 + 计算池）、三规则公平调度、journal 位图续传、两层内存上限、传输原语
  语义（Range 三态/206 校验/上限取消/探测）、可插拔后端接缝与业务壳职责边界

### Modified Capabilities
<!-- 无：现有 client-patcher 等 capability 的对外行为不变（下载层重构对业务层透明）。 -->
