## Why

当前项目文档分散且部分内容与代码不同步。AGENTS.md 承载过多细节，新工具（download_minio.ps1）未记录，遗留脚本（Build/BatchFiles/）造成混淆。需要重新规划文档结构，确保文档与代码对齐。

## What Changes

- 新增 `Docs/Tools.md`：记录 FindUE5.ps1、download_minio.ps1、OnDemandCacheAnalyzer.py、mc.exe 使用说明
- 新增 `Docs/Source.md`：记录 OnDemandSubsystem 实现细节和控制台命令原理
- 更新 `AGENTS.md`：简化为工程简介，移除冗余细节，指向详细文档
- 删除 `Build/BatchFiles/BuildClient_Win64.bat`：遗留脚本，硬编码路径且功能已被 `IoStoreServer/build_and_upload.bat` 替代
- 清理 `Build/Windows/` 目录：旧打包产物，应删除

## Capabilities

### New Capabilities

- `tools-documentation`: 工具使用文档，涵盖 FindUE5.ps1、download_minio.ps1、OnDemandCacheAnalyzer.py
- `source-documentation`: 源码实现文档，涵盖 OnDemandSubsystem 及控制台命令

### Modified Capabilities

无（此次变更为文档更新，不影响功能需求）

## Impact

- 文档文件：新增 2 个，修改 1 个
- 遗留代码：删除 1 个脚本，清理 1 个目录
- 无功能代码变更
- 无 API 变更
