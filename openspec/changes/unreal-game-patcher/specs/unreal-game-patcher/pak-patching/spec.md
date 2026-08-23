## Purpose

对 UE5 pak 文件做增量差异更新：解析 pak 格式、对比索引得出差异文件块、原地覆盖或末尾追加应用、空洞管理与紧凑回收，未变化数据零重写。

## ADDED Requirements

### Requirement: pak 格式解析
patcher SHALL 能解析 pak 的 footer、版本信息、目录索引与文件条目（路径、偏移、大小、压缩块信息），并能从 pak 中读取任意文件数据块。

#### Scenario: 读取目录索引
- **WHEN** patcher 打开一个合法 pak 文件
- **THEN** patcher 能列出其中全部文件条目及其偏移、大小、压缩信息

#### Scenario: 非法 pak
- **WHEN** patcher 打开一个 magic 或结构非法的文件
- **THEN** patcher 明确报错并终止该目标的处理，不产生部分结果

### Requirement: 差异分析
patcher SHALL 对比本地 pak 目录与远端 index，得出 changed / added / removed 三类的文件块集合，并以内容 hash 确认变化（hash 相同视为未变化）。

#### Scenario: 仅部分文件变化
- **WHEN** 本地 pak 与远端 index 相比只有一个文件块内容变化
- **THEN** 差异结果只包含该 changed 块，其余块不进入下载与应用集合

#### Scenario: 新增与删除文件
- **WHEN** 远端 index 相比本地包含新增文件块且本地包含远端已移除的文件块
- **THEN** 差异结果分别列出 added 与 removed 集合

### Requirement: 原地覆盖应用
对于尺寸不变（或新数据可装入原位置）的差异块，patcher SHALL 原地写入该块，不移动或重写 pak 内其它任何数据。

#### Scenario: 等尺寸覆盖
- **WHEN** 一个 changed 块的压缩后尺寸与本地原块相同
- **THEN** patcher 在原偏移处直接覆盖该块，pak 中其它文件数据字节不变

### Requirement: 末尾追加与索引重写
对于原位置无法容纳的差异块，patcher SHALL 将新块追加到 pak 末尾，并重写目录索引与 footer 使新偏移生效；追加写入 SHALL 遵守引擎要求的对齐规则。

#### Scenario: 尺寸变大追加
- **WHEN** 一个 changed 块的压缩后尺寸大于本地原块
- **THEN** patcher 将新块按对齐要求追加到文件末尾，重写索引使该文件指向新偏移，原位置标记为空洞

#### Scenario: 追加后索引有效
- **WHEN** patcher 完成追加并重写索引
- **THEN** 引擎以标准方式打开该 pak 能读到全部文件的正确数据

### Requirement: 空洞管理
patcher SHALL 维护空洞记录（因追加而失效的原数据区，含偏移与大小），空洞信息 SHALL 持久化，供 compact 或后续追加复用。

#### Scenario: 记录空洞
- **WHEN** 一次更新产生若干空洞
- **THEN** patcher 将空洞的偏移与大小记录到持久化状态中

### Requirement: 紧凑回收
patcher SHALL 支持 compact 操作：将 pak 重新打包为无空洞的紧凑布局，仅保留当前有效数据；compact 后目录索引与 footer 同步更新，文件仍可被引擎正常加载。

#### Scenario: 执行 compact
- **WHEN** patcher 执行 compact
- **THEN** 生成的新 pak 不再包含空洞，且内容与 compact 前逻辑等价（全部文件数据与索引一致）

### Requirement: 压缩块支持
首版 SHALL 支持 UE5 标准压缩方法（Oodle、zlib、LZ4）的压缩块读写；加密 pak 与签名 pak 不在首版范围，遇到时 SHALL 明确报错。

#### Scenario: 压缩 pak 的读写
- **WHEN** pak 使用 Oodle / zlib / LZ4 压缩
- **THEN** patcher 能解压读取并正确写入更新后的压缩块

#### Scenario: 不支持的加密 pak
- **WHEN** patcher 遇到首版不支持的加密或签名 pak
- **THEN** patcher 明确报告不支持，不执行任何写入

### Requirement: 应用后完整性校验
应用流程 SHALL 支持对已写区块的内容 hash 校验，校验失败 SHALL 触发该次更新的回滚（详见 client-patcher 事务性要求）。

#### Scenario: 校验失败回滚
- **WHEN** 应用后某个写入块 hash 校验不通过
- **THEN** patcher 将 pak 恢复至更新前状态或标记为可修复，并报告失败
