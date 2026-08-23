## Purpose

对 UE5 IoStore（.ucas/.utoc）做 chunk 级增量更新：以 utoc 为索引，利用 chunk id（内容 hash）判定差异、下载差异 chunk 并应用，处理压缩 block 重排与更新闭包，未变化数据零重写。

## ADDED Requirements

### Requirement: utoc 解析
patcher SHALL 能解析 utoc 的 container header、block entries（压缩块偏移与大小）、chunk entries（chunk id、chunk 内偏移、大小、原始大小）与压缩方法信息，并能从 ucas 中读取任意 chunk 与 block。

#### Scenario: 读取 chunk 清单
- **WHEN** patcher 打开一个合法的 utoc/ucas 对
- **THEN** patcher 能列出全部 chunk（id、偏移、大小、原始大小）与 block 偏移表

#### Scenario: 非法 container
- **WHEN** patcher 打开结构非法的 utoc/ucas
- **THEN** patcher 明确报错并终止该目标处理

### Requirement: chunk 级差异
patcher SHALL 以 chunk id 为差异判据（chunk id 为内容 hash）：本地与远端同 id 视为内容一致，无需更新；id 不同或缺失视为需要更新。

#### Scenario: 同 id 跳过
- **WHEN** 本地 ucas 中某 chunk 的 id 与远端 index 相同
- **THEN** patcher 不下载、不写入该 chunk

#### Scenario: 异 id 更新
- **WHEN** 本地 chunk 缺失或 id 与远端不同
- **THEN** patcher 将该 chunk 纳入下载与应用集合

### Requirement: 差异应用与连续重写
由于 IoStore 的 chunk block 序列必须自 chunk 的 OffsetInContainer 起连续排列，patcher SHALL 按 chunk 整体应用差异：压缩数据尺寸不变时原地覆盖；尺寸变化时将该 chunk 的 block 序列作为一个整体重新写入 ucas 末尾的连续区域，并重写 utoc 中该 chunk 的条目（offset / size / block 偏移），未变化 chunk 的数据不搬动。更新后 container SHALL 仍可被引擎 FIoDispatcher 正常加载。

#### Scenario: chunk 等尺寸覆盖
- **WHEN** 差异 chunk 的压缩数据与原 chunk 尺寸相同
- **THEN** patcher 原地覆盖，utoc 中该 chunk 条目不变，其它 chunk 数据零搬动

#### Scenario: chunk 尺寸变化
- **WHEN** 差异 chunk 压缩后尺寸与原 chunk 不同
- **THEN** patcher 将该 chunk 的 block 序列整体追加到 ucas 末尾连续区域并重写 utoc 中该 chunk 条目，其余 chunk 数据保持原位

#### Scenario: 更新后引擎可加载
- **WHEN** patcher 完成一次 IoStore 更新
- **THEN** 引擎通过 FIoDispatcher 按新 utoc 能读到全部更新后 chunk 的正确内容

### Requirement: 更新闭包
由于 chunk id 为内容 hash，任何被更新 chunk 引用方（如依赖其数据的其它 chunk）SHALL 一并纳入本次更新集合，保证更新后整个 container 引用一致、无悬垂引用。

#### Scenario: 引用链同步更新
- **WHEN** chunk A 被更新导致其 id 变化，而 chunk B 引用 A
- **THEN** B 也被纳入更新集合，更新完成后 B 引用的是 A 的新 id，container 内无悬垂引用

### Requirement: 空洞记录
因 block 追加而失效的原数据区 SHALL 被记录为空洞并持久化，供 compact 回收。

#### Scenario: 记录追加空洞
- **WHEN** 一次 IoStore 更新产生追加导致的失效数据区
- **THEN** patcher 将空洞偏移与大小持久化记录

### Requirement: 压缩支持与完整性
首版 SHALL 支持 IoStore 标准压缩方法；不加密。应用后 SHALL 支持对写入 chunk 的 hash 校验，校验失败 SHALL 触发回滚（见 client-patcher 事务性要求）。

#### Scenario: 写入校验失败
- **WHEN** 应用后某 chunk 的 hash 校验不通过
- **THEN** patcher 恢复 container 至更新前状态或标记为可修复，并报告失败
