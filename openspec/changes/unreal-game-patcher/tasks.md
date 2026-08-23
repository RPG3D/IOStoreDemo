# UnrealGamePatcher 实现任务

## 1. 工程脚手架（P0）

- [x] 1.1 在项目 `Source/Programs/UnrealGamePatcher/` 创建**单 Program target**（GUI 默认形态 + `-cli` 参数切换 CLI 模式）与模块（Core/Build/CLI/GUI），模板参考引擎 SlateViewer（`RunSlateViewer` + `IMPLEMENT_APPLICATION` 入口）与 UnrealPak（CLI 选项）；target 设 `Type=Program`、`bCompileAgainstEngine=false`、`SolutionDirectory="Programs/UnrealGamePatcher"`，验证 UBT 构建产出单个 exe、sln 中位于 Programs 分组
- [x] 1.2 Core 模块基础设施：FPaths / FCompression / 日志 / 版本常量与命令行解析骨架，验证 `UnrealGamePatcher --version` 输出版本并退出码 0
- [x] 1.3 引入 xxhash64 与 FIoHash 的封装（含字节序处理），验证与参考实现向量一致的单测

## 2. Pak 格式层（P0）

- [x] 2.1 实现 FPakReader：footer / 目录索引 / 文件条目（含内置每文件 SHA1 hash）/ 压缩块数组解析（多 pak 版本分发），验证用 repak 打包的样本 pak 解析出的文件清单与 `repak list` 一致，且条目内置 hash 与源文件未压缩数据 SHA1 一致
- [x] 2.2 实现 FPakWriter：原地覆盖、末尾追加（32B 对齐）、目录与 footer 重写，验证写出的 pak 能被 repak 打开且逐文件内容与原数据一致
- [x] 2.3 sidecar 状态骨架（UnrealGamePatcher.state.json：空洞列表、事务日志、pending 标记），验证读写 roundtrip 与损坏文件报错

## 3. 清单体系（P1）

- [x] 3.1 **先启用 manifest 打包**（BuildPatchTool / 对应打包配置）并 dump、解析官方 manifest 实际格式（文件 → hash/size、chunk → guid/sha/size）；同时 dump 打包产物 pak 条目，确认内置每文件 SHA1 hash 存在且语义为未压缩数据，验证能解析出文件级清单且字段与实测一致
- [x] 3.2 扩展清单 UnrealGamePatcher.index.json（官方 manifest 底座之上：更新策略标记 + pak/ucas chunk 差异 + 版本化/baseVersion + 自校验）序列化/反序列化/损坏检测，验证 roundtrip 一致 + 篡改/损坏检测单测
- [x] 3.3 本地真相提取：从本地 pak 目录生成 chunk 清单并与远端扩展清单对比（同 id 跳过 / 异 id 与缺失判定 changed/added/removed），验证 diff 结果符合手工构造用例

## 4. 构建侧工具（P1）

- [x] 4.1 build（ingest）：打包输出（含官方 manifest）→ 复用其文件级清单/hash 生成扩展清单 + 按 chunk 切分文件 + manifest（确定性构建、无时间戳嵌入），验证同一输入两次构建产物字节一致且扩展清单的文件 hash 与官方 manifest 一致
- [x] 4.2 diff：两个 index → changed/added/removed 差异集合与增量产物，验证手工构造的两版本用例结果正确（含空差异）
- [x] 4.3 版本共存目录布局与任意两版本 diff 支持，验证 v1/v2/v3 并存且 v1→v3 跳版本差异正确

## 5. 客户端应用与事务（P1/P2）

- [x] 5.1 CLI `update`（本地目录模拟远端，无网络）：分析 → 下载差异 → 原地覆盖/追加 → 刷新索引，验证更新后 pak 与远端逻辑等价（repak 交叉验证 + 逐文件 hash）
- [x] 5.2 事务日志 + 原块备份 + 回滚：应用中途失败恢复原状，验证注入写入失败后文件与更新前字节一致
- [x] 5.3 崩溃恢复：pending 事务中断后重跑按日志回滚或重做，验证模拟 kill 后 rerun 结果正确
- [x] 5.4 CLI `check` / `compact` / `repair`：空洞报告、compact 回收（临时文件 + 原子替换）、损坏修复，验证 compact 后无空洞且内容逻辑等价
- [x] 5.5 空洞膨胀触发策略（默认阈值：>10% 或 >512MB），验证 check 报告膨胀率并在超阈值时提示

## 6. 下载（P2）

- [x] 6.1 FHttpModule 下载 + Range 断点续传 + 下载后 hash 校验，验证本地 HTTP 服务器中断恢复场景只重下未完成 chunk
- [x] 6.2 并发下载池（默认 8 连接），验证多 chunk 并发下载全部通过校验
- [x] 6.3 下载损坏重试与上限，验证超限后整体失败并报告、不落损坏数据
- [x] 6.4 普通文件全量更新：下载 → manifest 文件级 hash 校验 → 原子替换（临时文件 + 重命名）→ 占用处理（要求退出游戏 / MoveFileEx 延迟替换）→ 纳入事务回滚，验证更新后与目标一致且失败可回滚
- [x] 6.5 下载器抽象接口（IChunkDownloader：URL + Range 字节区间 + 进度/取消）与默认 FHttpModule 实现（并发 / 重试 / 断点续传 / hash 校验），验证默认下载器 Range 请求与断点续传正确
- [x] 6.6 自定义下载器扩展：注册业务层自定义 IChunkDownloader（可用 mock 实现验证）并驱动完整更新流程，验证更新流程与具体下载后端解耦、其余环节不变

## 7. IoStore 路径（P3）

- [x] 7.1 实现 FIoStoreReader：utoc header / block / chunk entries / 压缩解析（参考 `Runtime/Core` IoStore 与 `IoStoreUtilities`），验证引擎打包输出的 utoc/ucas 解析出的 chunk 清单与构建日志一致
- [x] 7.2 实现 FIoStoreWriter：变化 chunk 的 block 序列整体连续重写（追加 + 对齐）+ utoc 重建（临时文件原子替换），block/chunk 布局与序列化参考引擎 `IoStoreUtilities` 的 `IoStoreWriter`，验证更新后 container 可被引擎 FIoDispatcher 正常加载
- [x] 7.3 IoStore 差异分析（chunk id 判据）与应用（等尺寸原地覆盖 / 尺寸变化整体重写），验证同 id 跳过、异 id 更新、其余 chunk 零搬动（文件偏移不变）
- [x] 7.4 构建侧 IoStore ingest/diff + 更新闭包计算（保守策略：变化 package 依赖全含），验证更新后 container 无悬垂引用（引擎加载 + 引用检查）

## 8. GUI（P4）

- [x] 8.1 `-cli` 与 GUI 分流（主入口 `FParse::Param`）：验证 `-cli` 模式不创建 Slate 窗口、文本输出与退出码符合约定；GUI 模式正常创建窗口
- [x] 8.2 GUI worker 线程 + 进度轮询（阶段/百分比/速度）+ 错误提示/重试 + "完成后启动游戏"，验证交互闭环且与 CLI 共享同一 orchestrator

## 9. 集成与验证（P5）

- [x] 9.1 端到端验证：打包 → ingest/diff 发布 → 客户端 update → 引擎加载更新后内容，验证完整闭环
- [x] 9.2 打包管线脚本集成（构建步骤插入 ingest/diff），验证脚本可重复执行且失败传播退出码
- [x] 9.3 IO/性能测量：机械盘场景差异写入量 vs 全量写入量对比（含原地随机写 vs 顺序重写回退策略），验证写量显著减少并记录数据
- [x] 9.4 文档：README（用法 / CDN 产物布局 / 阈值配置 / 构建接入），验证文档中命令示例可直接运行
