## Purpose

客户端 patcher：下载并校验 index、分析差异、下载差异 chunk（断点续传）、事务性应用（失败回滚）、完整性校验与刷新 index，提供 CLI 与 GUI 两种入口。

## ADDED Requirements

### Requirement: 更新流程编排
patcher SHALL 以确定顺序执行：获取 index → 校验 index → 差异分析 → 下载差异 chunk → 事务性应用 → 完整性校验 → 刷新本地 index；任一环节失败 SHALL 明确报告，不进入下一环节。

#### Scenario: 完整更新成功
- **WHEN** 上述全流程执行且各环节成功
- **THEN** patcher 报告更新成功并刷新本地 index，本地文件与目标版本一致

#### Scenario: 环节失败中止
- **WHEN** 任一环节失败（如下载失败、校验失败）
- **THEN** patcher 中止后续环节并报告具体失败原因与可恢复步骤

### Requirement: 差异分析报告
在下载前 patcher SHALL 基于本地状态与远端 index 输出差异报告：changed / added / removed 数量、预计下载字节数与比例；支持只检查模式（--check）不执行任何下载与写入。

#### Scenario: 只检查模式
- **WHEN** 用户以只检查模式运行 patcher
- **THEN** patcher 仅输出差异报告与预计下载量，不产生任何网络下载与本地写入

### Requirement: chunk 下载
patcher SHALL 支持按 chunk 下载：并发下载、失败重试、断点续传；下载完成的 chunk SHALL 通过内容 hash 校验后才进入应用阶段。

#### Scenario: 断点续传
- **WHEN** 下载在中间中断后重新开始
- **THEN** patcher 仅重下未完成或未校验通过的 chunk，已完成部分不重复下载

#### Scenario: 下载损坏重试
- **WHEN** 某 chunk 下载完成但 hash 校验失败
- **THEN** patcher 重试该 chunk，重试超过上限后整体失败并报告

### Requirement: 可插拔下载后端
patcher SHALL 通过抽象下载接口获取内容（支持按 URL + Range 请求字节区间），默认提供基于引擎内置 HTTP 的实现，并允许业务层注册自定义下载器（如私有协议、P2P、定制 CDN 客户端）；更新流程的下载调度、断点续传、并发与校验 SHALL 与具体下载后端解耦。

#### Scenario: 默认下载器
- **WHEN** 未注册自定义下载器
- **THEN** patcher 使用引擎内置 HTTP 实现下载，支持 Range 请求

#### Scenario: 自定义下载器
- **WHEN** 业务层注册了自定义下载器
- **THEN** patcher 通过自定义下载器获取内容，其余更新环节不变

#### Scenario: Range 请求
- **WHEN** 需要下载文件的某个字节区间（断点续传或 chunk 请求）
- **THEN** 下载接口以 Range 语义获取该区间

### Requirement: 事务性应用与回滚
对目标文件的写入 SHALL 具备事务性：先记录更新前状态（原块备份或日志），全部写入完成并通过校验后才提交；任一块写入或校验失败 SHALL 将目标文件恢复至更新前状态（或标记为待修复并拒绝继续）。

#### Scenario: 应用中途失败回滚
- **WHEN** 应用过程中某块写入失败
- **THEN** 目标文件被恢复至更新前状态，本地 index 不变，客户端状态一致

#### Scenario: 全部成功提交
- **WHEN** 全部差异块写入且校验通过
- **THEN** patcher 提交本次更新，刷新本地 index 并标记完成

### Requirement: 原地应用
patcher SHALL 通过 pak / IoStore 应用引擎写入差异块，未变化的文件数据不得被重写或搬动（空洞管理与 compact 除外）。

#### Scenario: 未变化数据零写入
- **WHEN** 一次更新只有少量差异块
- **THEN** 写入总量仅覆盖差异块本身，未变化数据所在区域不被写入

### Requirement: 普通文件全量更新
非 pak/ucas 目标（如 dll、exe、配置文件等普通文件）SHALL 采用全量更新：依据清单的文件级 hash 检测变化（hash 不同或本地缺失），全量下载该文件，校验 hash 后原子替换本地文件；未变化的普通文件 SHALL 不下载、不写入。

#### Scenario: 检测到普通文件变化
- **WHEN** 清单中某普通文件（如 dll）的文件级 hash 与本地不同
- **THEN** patcher 全量下载该文件，校验 hash 后原子替换（临时文件 + 重命名），未变化文件不下载

#### Scenario: 新增普通文件
- **WHEN** 清单包含本地缺失的普通文件
- **THEN** patcher 全量下载并安装该文件

#### Scenario: 普通文件校验失败
- **WHEN** 下载的普通文件 hash 与清单不符
- **THEN** patcher 重试或整体失败，不写入未通过校验的文件

### Requirement: CLI 入口
patcher SHALL 提供命令行入口：可指定 CDN 地址 / 本地安装目录 / 操作模式（更新、只检查、compact、修复），通过退出码区分成功 / 需更新未执行 / 失败 / 参数错误，并以结构化日志输出进度。

#### Scenario: 命令行更新
- **WHEN** 用户以命令行参数运行更新
- **THEN** patcher 输出可解析的进度日志，并以约定退出码结束

#### Scenario: 参数错误
- **WHEN** 用户提供缺失或非法的命令行参数
- **THEN** patcher 打印用法说明并以参数错误退出码结束

### Requirement: GUI 入口
patcher SHALL 提供 GUI（Slate）入口：展示更新进度（当前阶段 / 百分比 / 下载速度）、错误与提示，并提供"开始更新"与"完成更新后启动游戏"操作。

#### Scenario: GUI 更新流程
- **WHEN** 用户在 GUI 中点击开始更新
- **THEN** GUI 实时展示各阶段进度，完成后展示结果并可启动游戏

#### Scenario: GUI 错误提示
- **WHEN** 更新过程发生可恢复错误
- **THEN** GUI 展示错误信息与重试入口，不崩溃、不静默

### Requirement: 断点与恢复
patcher SHALL 持久化更新进度（已下载 chunk、应用阶段标记），中断后重新运行 SHALL 从断点继续而非从头开始。

#### Scenario: 中断恢复
- **WHEN** 更新中断后用户重新运行 patcher
- **THEN** patcher 复用已下载且校验通过的 chunk，从应用断点继续
