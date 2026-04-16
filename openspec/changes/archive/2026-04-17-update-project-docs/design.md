## Context

当前项目文档存在以下问题：
- AGENTS.md 承载过多细节，定位模糊
- 新增工具未记录（download_minio.ps1）
- 遗留脚本和产物目录造成混淆
- 文档分散在多个位置，缺乏清晰索引

## Goals / Non-Goals

**Goals:**
- 建立清晰的文档层级：AGENTS.md（简介）→ Docs/（细节）
- 确保所有工具和核心实现有对应文档
- 清理无用的遗留文件

**Non-Goals:**
- 不修改功能代码
- 不改变 IoStoreOnDemand 的技术实现
- 不修改 OpenSpec 配置

## Decisions

### D1: 文档分层结构

```
AGENTS.md                    ← 工程简介、索引、快速开始
Docs/IoStoreOnDemand.md      ← 技术细节（已有）
Docs/Tools.md                ← 工具使用说明（新增）
Docs/Source.md               ← 源码实现说明（新增）
```

**理由**: 符合用户定义的 AGENTS.md 定位（工程简介），细节归入 Docs/。

### D2: 遗留文件处理

| 文件 | 处理 | 理由 |
|------|------|------|
| `Build/BatchFiles/BuildClient_Win64.bat` | 删除 | 硬编码路径、无 OnDemand 支持、功能重复 |
| `Build/Windows/` | 删除 | 旧打包产物，不属于源码 |

**替代方案**: 保留并更新 `BuildClient_Win64.bat` — 否决，因为 `IoStoreServer/build_and_upload.bat` 已是正确实现。

### D3: AGENTS.md 简化策略

移除以下冗余内容：
- FindUE5.ps1 完整参数表 → 改为指向 Docs/Tools.md
- UAT 命令详细参数 → 保留简化版，详细版在 Docs/IoStoreOnDemand.md
- OnDemand 文件大小详情 → 移至 Docs/Source.md

保留以下核心内容：
- 项目概述
- 核心技术简介
- 关键文档索引
- 命令/脚本索引表
- 快速开始

## Risks / Trade-offs

- **风险**: 文档分散可能导致查找困难 → **缓解**: AGENTS.md 提供清晰索引
- **风险**: 文档更新滞后 → **缓解**: 将文档位置记录在 AGENTS.md，便于维护
