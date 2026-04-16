## ADDED Requirements

### Requirement: OnDemandSubsystem 概述
Docs/Source.md SHALL 包含 OnDemandSubsystem 的概述，说明其作为 UGameInstanceSubsystem 的定位和职责。

#### Scenario: 理解子系统架构
- **WHEN** 开发者需要理解 OnDemand 运行时架构
- **THEN** 可参考 Docs/Source.md 了解 OnDemandSubsystem 的角色

### Requirement: 控制台命令列表
Docs/Source.md SHALL 列出所有控制台命令（installdlc, ondemandstatus, getinstallsize）及其实现原理。

#### Scenario: 使用控制台命令
- **WHEN** 用户在游戏中使用控制台命令
- **THEN** 可参考 Docs/Source.md 了解命令的实现方式

### Requirement: InstallDLC 流程说明
Docs/Source.md SHALL 说明 InstallDLC 的完整流程：MountDLCToc → OnMountComplete → Install → 回调。

#### Scenario: 理解 DLC 安装过程
- **WHEN** 开发者需要调试 DLC 安装问题
- **THEN** 可参考 Docs/Source.md 理解安装流程和回调机制

### Requirement: 缓存管理方法
Docs/Source.md SHALL 说明 OnDemandStatus、GetInstallSize 等方法的实现原理和 IOnDemandIoStore API 使用。

#### Scenario: 监控缓存状态
- **WHEN** 开发者需要监控 OnDemand 缓存
- **THEN** 可参考 Docs/Source.md 了解相关方法的实现
