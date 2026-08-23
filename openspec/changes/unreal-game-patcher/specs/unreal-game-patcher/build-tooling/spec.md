## Purpose

构建侧后处理工具：在 UnrealPak / IoStore 打包输出之上生成 CDN index 与按 chunk 切分的下载产物，并生成任意两版本间的差异产物，支持版本共存与跳版本更新。

## ADDED Requirements

### Requirement: 全量 index 生成
构建工具 SHALL 能从一份打包输出（pak 与/或 IoStore 产物）生成对应的 index 清单（见 index-format 能力），包含全部目标文件的 chunk 清单与内容 hash。

#### Scenario: 首次发布生成 index
- **WHEN** 构建工具处理一份新打包输出
- **THEN** 生成完整 index 与全量 chunk 产物，供 CDN 发布

### Requirement: chunk 下载产物切分
构建工具 SHALL 按 chunk 粒度组织下载产物：pak 目标按文件数据块、IoStore 目标按 chunk，使客户端仅需下载差异 chunk。

#### Scenario: chunk 级文件产出
- **WHEN** 构建工具完成产物生成
- **THEN** CDN 上每个 chunk 可作为独立文件下载，客户端能按 id 精确获取单个 chunk

### Requirement: 差异产物生成
构建工具 SHALL 支持给定新旧两个版本，输出两者间的差异 chunk 集合（changed / added / removed），供客户端跳版本更新。

#### Scenario: 生成增量包
- **WHEN** 构建工具以 v1 与 v3 作为输入
- **THEN** 输出 v1→v3 的差异 chunk 清单与对应 chunk 文件

#### Scenario: 空差异
- **WHEN** 两个输入版本内容一致
- **THEN** 差异产物为空集，不产生任何 chunk 文件

### Requirement: 版本共存与跳版本
CDN 产物布局 SHALL 支持多版本共存，任一受支持的历史版本均可作为跳版本更新的起点；过期版本产物 SHALL 可按策略清理且不影响其它版本。

#### Scenario: 多版本共存
- **WHEN** CDN 同时存在 v1、v2、v3 三套产物
- **THEN** 持有任一版本的客户端都能得到到目标版本的差异 chunk

### Requirement: 确定性构建
相同输入与相同配置 SHALL 产生字节级相同的产物（index 与 chunk 文件），以保证缓存与去重有效。

#### Scenario: 重复构建一致
- **WHEN** 同一打包输出被处理两次
- **THEN** 两次产出的 index 与 chunk 文件字节一致

### Requirement: 产物完整性
构建工具 SHALL 为每个生成的 chunk 文件与 index 文件附上校验信息（内容 hash），并对齐 index 中的记录，保证客户端可独立校验。

#### Scenario: 产物可校验
- **WHEN** 客户端下载任一产物
- **THEN** 客户端可依据产物自带校验信息独立验证其完整性

### Requirement: 复用官方 manifest
构建工具 SHALL 基于 UE 打包产出的官方 manifest 生成 CDN 清单：文件级清单与文件 hash 直接复用官方 manifest（不另行计算或定义），仅在其上附加更新策略与 pak/ucas 的 chunk 差异信息。

#### Scenario: manifest 优先
- **WHEN** 构建工具处理一份含官方 manifest 的打包输出
- **THEN** 产出的 CDN 清单以官方 manifest 的文件清单与 hash 为准，仅在其上附加更新策略与 chunk 差异信息

#### Scenario: 无 manifest 拒绝产出
- **WHEN** 打包输出不含官方 manifest
- **THEN** 构建工具以非零退出码失败并提示启用 manifest 打包，不产出 CDN 清单

### Requirement: 与打包管线集成
构建工具 SHALL 作为打包管线的后处理步骤运行：输入为 UnrealPak / IoStore 输出目录（含官方 manifest），输出为 CDN 发布目录，退出码区分成功 / 失败 / 参数错误。

#### Scenario: 管线失败传播
- **WHEN** 输入打包产物缺失或损坏
- **THEN** 构建工具以非零退出码失败并给出明确错误信息，不产出部分发布物
