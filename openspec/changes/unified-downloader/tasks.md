## 1. 模块骨架与原语迁移

- [x] 1.1 新建 `UnrealGamePatcherDownload` 模块（Build.cs：仅依赖 Core/HTTP；加入 Target 与 launcher 模块依赖）+ `.gitignore` 无需项确认
- [x] 1.2 `FHttpRangeRequest` 迁移：`FHttpChunkDownloader` 语义原样迁入（Range 三态/206 校验/长度校验/Size==0 拒绝/MaxResponseBytes Header 取消/进度看门狗/原子 Done/超时事件不还池），原类名保留别名指向新实现
- [x] 1.3 `IChunkDownloader` 接缝与 `FDownloadManager`/`FUpdateOrchestrator` 的 include 路径切换到新模块（纯移动，无行为改动）
- [x] 1.4 验证：构建 + 14/14 全绿（此步无任何行为变化）

## 2. 执行器与分段模型

- [x] 2.1 `FDownloadJob`/`FDownloadResult`/`FDownloadExecutorConfig`（SegmentSize=4MB、Window=24、ComputePoolThreads=4、MaxRetries=3、Timeout=60s；design D6，显式构造可覆盖）
- [x] 2.2 段状态机（design D3）：Pending→Probing→Segments→Verifying→Promoting→Completed/Failed/Canceled；原子段计数 + 单调度点
- [x] 2.3 分段规划：按 SegmentSize 切分、尾段对齐；size 未知时先探测（`Range: bytes=0-0` 解析 Content-Range 总大小 + Range 支持验证；探测失败回退整文件单段）
- [x] 2.4 窗口提交：执行器经原语回调形态（非阻塞 `DownloadRange`）挂请求；settle 回调在引擎 HttpThread 锁内内联处理；请求超时依赖引擎 curl 连接/活动超时（不自建计时器）
- [x] 2.5 段后处理（回调内联，零专用线程）：段数据落盘（`FILEWRITE_Append` 非截断打开 + 绝对 `Seek(offset)+Write`）、整文件 SHA1 终验、原子落位
- [x] 2.6 三规则调度器（design D4）：收尾优先 ≤2 / 公平轮转 in-flight≤4 / 尾段补位；单调度点（每次 Submit/settle 后 `Refill()`，单锁内串行）
- [x] 2.7 downloadtest 切换执行器（`FDownloadManager` 内部替换，签名不变）：14/14 全绿 + rustfs 真实包端到端一次

## 3. journal 位图续传

- [x] 3.1 journal 格式与读写（`<dest>.journal`：魔数 + 段大小 + 总大小 + 追加式完成记录；启动重放校验，不匹配即丢弃重来）
- [x] 3.2 分段续传集成：预分配 → 回调记位 → 重启只补缺段 → 全段完成终验后清理 journal
- [x] 3.3 downloadtest B 升级：断点续传用例改为"缺 N 段只重下 N 段"（服务器统计断言从"仅尾部"改为"缺段字节数"）

## 4. 业务壳边界与新用例

- [x] 4.1 `FDownloadManager` 切薄壳：cache/预检 → 组装 Job 提交执行器 → 聚合等待 → 终验/落位统计；失败语义不变（整批失败 + 失败 id）
- [x] 4.2 新用例：大文件分段（>4MB 资源，验证段数/并发上限/journal 位图）
- [x] 4.3 新用例：中断恢复（kill 模拟 → 重启只补缺段，字节级比对）
- [x] 4.4 新用例：公平性（大文件先入队 + 小文件后入队 → 小文件先完成）
- [x] 4.5 新用例：上限取消（超长 Content-Length → 不落盘 + 明确错误码）与探测回退（无 Range 服务器）
- [x] 4.6 文档：DOWNLOAD.md 状态标记翻转（✅ 蓝图项）、README 下载层章节、ROADMAP Phase 1 状态

## 5. 收尾验证

- [x] 5.1 全量回归：14/14 全绿 + rustfs 全新安装/复跑端到端
- [x] 5.2 吞吐基准：大文件分段 vs 单连接的下载速率对比记录（写回 DOWNLOAD.md §1）
- [x] 5.3 内存预算复核：窗口 × 4MB 实测峰值确认（默认 24 ≈ 96MB）
