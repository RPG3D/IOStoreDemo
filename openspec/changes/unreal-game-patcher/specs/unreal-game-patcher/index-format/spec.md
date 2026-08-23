## Purpose

统一 CDN 清单体系：以 UE 打包产出的官方 manifest（文件级清单与 hash）为底座，在其上扩展描述 pak 与 IoStore（.ucas/.utoc）的 chunk 差异、更新策略与版本信息，供 patcher 做差异对比、下载与应用，最大化与 UE 生态的兼容性。

## ADDED Requirements

### Requirement: 复用 UE 官方 manifest
CDN 清单 SHALL 以 UE 打包产出的官方 manifest 为文件级清单与文件 hash 的来源，不另行定义文件清单或重新计算文件 hash；官方 manifest 未覆盖的信息（pak/ucas 的 chunk 差异、更新策略、版本关联）由清单的扩展部分补充。

#### Scenario: manifest 为准
- **WHEN** 打包输出含官方 manifest（含文件清单与文件级 hash）
- **THEN** CDN 清单直接采用其文件级清单与 hash，仅附加扩展信息，文件 hash 与官方 manifest 一致

#### Scenario: manifest 缺失
- **WHEN** 打包输出不含官方 manifest
- **THEN** 构建工具以非零退出码失败并提示启用 manifest 打包，不产出 CDN 清单

### Requirement: 更新策略标记
清单 SHALL 为每个目标文件标记更新策略：pak/ucas 目标走 chunk 增量，其余普通文件（dll、exe、配置文件等）走全量。

#### Scenario: 策略区分
- **WHEN** 清单同时含 pak/ucas 目标与普通文件
- **THEN** 客户端对 pak/ucas 执行 chunk 增量流程，对普通文件执行全量下载替换流程

### Requirement: 版本化清单
清单 SHALL 包含格式版本号、目标内容版本号、生成时间与生成工具信息。patcher SHALL 能依据目标内容版本号判断本地版本是否过期。

#### Scenario: 识别本地版本过期
- **WHEN** patcher 下载的清单目标内容版本号与本地不一致
- **THEN** patcher 判定本地版本过期，进入差异分析流程

#### Scenario: 本地已是最新
- **WHEN** 清单目标内容版本号与本地一致
- **THEN** patcher 报告无需更新，不下载任何内容

### Requirement: 双目标支持
同一清单 SHALL 能同时描述 pak 与 ucas/utoc 两类目标，每类目标各自独立列出清单，互不混淆；两类目标均以文件级条目存在于官方 manifest 中。

#### Scenario: 混合部署
- **WHEN** 一个更新同时包含 pak 目标与 ucas/utoc 目标
- **THEN** patcher 分别对两类目标执行各自的差异分析与应用流程

### Requirement: chunk 粒度描述
清单的扩展部分 SHALL 以 chunk 列表描述 pak/ucas 目标的内容，每个 chunk 条目包含：标识（id）、内容 hash、chunk 内偏移与大小、未压缩原始大小。pak 目标的 chunk 为文件数据块；IoStore 目标的 chunk 直接对应 utoc chunk。

#### Scenario: 描述 pak 文件块
- **WHEN** 清单扩展部分描述一个 pak 目标
- **THEN** 其 chunk 列表中的每个条目对应 pak 内一个文件数据块，并给出其在 pak 中的偏移与大小

#### Scenario: 描述 IoStore chunk
- **WHEN** 清单扩展部分描述一个 ucas/utoc 目标
- **THEN** 其 chunk 列表直接来源于对应 utoc 的 chunk entries

### Requirement: 内容 hash 校验
文件级完整性 SHALL 以官方 manifest 提供的文件 hash（MD5/SHA1）为准。pak 目标的 chunk（文件数据块）差异判断与完整性校验 SHALL 复用 pak 条目内置的每文件 hash（`FPakEntry::Hash`，打包时对未压缩数据计算，粒度即文件块），不另行定义或重复计算内容 hash；IoStore 目标 SHALL 复用 utoc 的 chunk id（内容 hash）。仅在目标条目缺失可用内置 hash 时，才允许使用清单扩展部分记录的 hash 兜底。

#### Scenario: 判断 chunk 是否变化（复用内置 hash）
- **WHEN** patcher 对比本地 pak 条目内置 hash 与远端清单扩展中对应条目 hash
- **THEN** hash 相同时判定无需更新该 chunk，hash 不同时判定需要更新

#### Scenario: 下载后校验
- **WHEN** patcher 下载一个 chunk 完成
- **THEN** patcher 以对应内置/清单 hash 校验（pak 解压后按未压缩数据 hash 校验；IoStore 按 chunk id 对应 hash），不一致则重试或判定失败

### Requirement: 版本关联与跳版本
清单 SHALL 记录其基础版本信息，允许仅持有早期版本的客户端确定本地与目标之间的差异 chunk 集合，而无需中间版本的清单。CDN SHALL 保留多个历史版本产物。

#### Scenario: 跳版本更新
- **WHEN** 本地为 v1、目标为 v3，清单记录 v3 基于 v1 的差异信息
- **THEN** patcher 仅下载 v1→v3 的差异 chunk，不下载 v2 内容

### Requirement: 清单完整性
清单 SHALL 内置自身校验信息，patcher 下载后 SHALL 先校验清单完整性再使用。

#### Scenario: 清单损坏
- **WHEN** patcher 下载的清单未通过自身校验
- **THEN** patcher 判定清单下载失败并重试，不基于损坏清单执行任何分析

### Requirement: 格式兼容演进
清单格式版本不兼容变化 SHALL 通过格式版本号显式标识，旧格式版本在支持的范围内 SHALL 仍可被解析（向前兼容策略），无法解析时 SHALL 明确报错而非静默误解。

#### Scenario: 未知格式版本
- **WHEN** patcher 遇到高于其支持的清单格式版本
- **THEN** patcher 明确报告"清单格式版本过高，需要升级 patcher"，不继续执行

### Requirement: 元数据与诊断
清单 SHALL 携带目标文件清单级元数据（文件总大小、chunk 总数），供 patcher 估算下载量与展示进度。

#### Scenario: 估算更新体积
- **WHEN** patcher 完成差异分析
- **THEN** patcher 依据差异内容的元数据给出需要下载的总字节数与比例
