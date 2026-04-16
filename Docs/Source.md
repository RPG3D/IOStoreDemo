# 源码实现说明

本文档说明 OnDemandSubsystem 的实现细节。

## OnDemandSubsystem 概述

`OnDemandSubsystem` 是一个 `UGameInstanceSubsystem`，封装了 IoStoreOnDemand 运行时 API，提供蓝图可调用的接口和控制台命令。

### 文件位置

- `Source/IOStoreDemo/OnDemandSubsystem.h`
- `Source/IOStoreDemo/OnDemandSubsystem.cpp`

### 职责

1. 管理 OnDemand 内容的挂载和安装
2. 提供蓝图可调用的 API
3. 注册控制台命令
4. 管理 DLC 下载请求生命周期

### 生命周期

```
GameInstance 初始化
    └─▶ OnDemandSubsystem::Initialize()
         └─▶ RegisterConsoleCommands()

GameInstance 销毁
    └─▶ OnDemandSubsystem::Deinitialize()
         └─▶ Cancel 当前请求
         └─▶ Reset ContentHandle
         └─▶ UnregisterConsoleCommands()
```

---

## 控制台命令

### 命令列表

| 命令 | 说明 | 示例 |
|------|------|------|
| `installdlc <tagset>` | 下载并安装指定标签的 DLC | `installdlc NewMap` |
| `ondemandstatus` | 显示 OnDemand 缓存状态 | `ondemandstatus` |
| `getinstallsize <tagset>` | 获取指定标签内容的安装大小 | `getinstallsize NewMap` |

### 实现原理

控制台命令通过 `IConsoleManager` 注册，在子系统初始化时绑定到子系统方法：

```cpp
// OnDemandSubsystem.cpp:31-85
void UOnDemandSubsystem::RegisterConsoleCommands()
{
    IConsoleManager& ConsoleManager = IConsoleManager::Get();
    
    RegisteredConsoleCommands.Add(
        ConsoleManager.RegisterConsoleCommand(
            TEXT("installdlc"),
            TEXT("Install DLC by tagset"),
            FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) {
                // 获取子系统并调用方法
                if (UOnDemandSubsystem* Subsystem = GI->GetSubsystem<UOnDemandSubsystem>()) {
                    Subsystem->InstallDLC(Args[0]);
                }
            })
        )
    );
}
```

---

## InstallDLC 流程

### 完整流程

```
InstallDLC(TagSet)
    │
    ├─▶ TryGetOnDemandIoStore()
    │       获取 IOnDemandIoStore 实例
    │
    ├─▶ MountDLCToc(TagSet)
    │       │
    │       ├─▶ 设置 MountArgs
    │       │       MountId = "DLC_InstallOnDemand"
    │       │       TocRelativeUrl = "ias-content/dlc-install.iochunktoc"
    │       │       Options = InstallOnDemand
    │       │
    │       └─▶ OnDemandStore->Mount(MountArgs, Callback)
    │               │
    │               └─▶ OnMountComplete(Result, TagSet)
    │
    └─▶ (异步回调) OnMountComplete()
            │
            ├─▶ 检查 Result.Status.IsOk()
            │
            ├─▶ 创建 ContentHandle
            │       FOnDemandContentHandle::Create("DLC_Install")
            │
            └─▶ OnDemandStore->Install(InstallArgs, CompleteCallback, ProgressCallback)
                    │
                    ├─▶ OnInstallProgress(Progress)
                    │       打印进度百分比
                    │
                    └─▶ OnInstallComplete(Result)
                            打印下载字节数
                            Reset CurrentInstallRequest
```

### 关键代码

```cpp
// OnDemandSubsystem.cpp:111-134
void UOnDemandSubsystem::MountDLCToc(const FString& TagSet)
{
    UE::IoStore::FOnDemandMountArgs MountArgs;
    MountArgs.MountId = DLCMountId;
    MountArgs.TocRelativeUrl = TEXT("ias-content/dlc-install.iochunktoc");
    MountArgs.Options = UE::IoStore::EOnDemandMountOptions::InstallOnDemand;
    
    OnDemandStore->Mount(MoveTemp(MountArgs),
        [this, TagSet](UE::IoStore::FOnDemandMountResult Result) {
            OnMountComplete(MoveTemp(Result), TagSet);
        });
}

// OnDemandSubsystem.cpp:136-176
void UOnDemandSubsystem::OnMountComplete(...)
{
    // 创建安装请求
    UE::IoStore::FOnDemandInstallArgs InstallArgs;
    InstallArgs.MountId = DLCMountId;
    InstallArgs.TagSets = { TagSet };
    InstallArgs.ContentHandle = DLCContentHandle;
    InstallArgs.Options = InstallSoftReferences | CallbackOnGameThread;
    
    CurrentInstallRequest = MakeShared<FOnDemandInstallRequest>(
        OnDemandStore->Install(MoveTemp(InstallArgs), ...)
    );
}
```

---

## 缓存管理方法

### OnDemandStatus

显示当前缓存使用情况。

```cpp
// OnDemandSubsystem.cpp:231-249
void UOnDemandSubsystem::OnDemandStatus()
{
    UE::IoStore::FOnDemandCacheUsage Usage = OnDemandStore->GetCacheUsage({});
    
    // 输出:
    // - Install Cache: 已用/最大
    // - Streaming Cache: 已用/最大
    // - Referenced Size: 被引用的大小
    // - Fragmented Size: 碎片大小
}
```

### GetInstallSize

获取指定标签内容的安装大小（不实际下载）。

```cpp
// OnDemandSubsystem.cpp:205-229
void UOnDemandSubsystem::GetInstallSize(const FString& TagSet)
{
    UE::IoStore::FOnDemandGetInstallSizeArgs Args;
    Args.MountId = DLCMountId;
    Args.TagSets = { TagSet };
    Args.ContentHandle = DLCContentHandle;
    
    TIoStatusOr<FOnDemandInstallSizeResult> Result = OnDemandStore->GetInstallSize(Args);
    if (Result.IsOk()) {
        UE_LOG(LogOnDemand, Log, TEXT("Install size: %llu bytes"), 
            Result.ValueOrDie().InstallSize);
    }
}
```

### UninstallDLC

卸载 DLC（未实现）。

```cpp
void UOnDemandSubsystem::UninstallDLC(const FString& TagSet)
{
    UE_LOG(LogOnDemand, Warning, TEXT("UninstallDLC not yet implemented"));
}
```

---

## ContentHandle 管理

`FOnDemandContentHandle` 用于 pin 住缓存内容，防止被清理。

```cpp
// 创建句柄
DLCContentHandle = UE::IoStore::FOnDemandContentHandle::Create(TEXT("DLC_Install"));

// 安装时关联
InstallArgs.ContentHandle = DLCContentHandle;

// 释放句柄（允许缓存清理）
DLCContentHandle.Reset();
```

---

## IOnDemandIoStore API 参考

核心 API 通过 `UE::IoStore::TryGetOnDemandIoStore()` 获取：

| 方法 | 说明 |
|------|------|
| `Mount()` | 挂载 OnDemand 容器 |
| `Unmount()` | 卸载容器 |
| `Install()` | 安装内容到本地缓存 |
| `GetInstallSize()` | 获取安装大小 |
| `GetCacheUsage()` | 获取缓存使用情况 |
| `Purge()` | 清理缓存 |
| `Defrag()` | 碎片整理 |
| `Verify()` | 验证缓存 |

详细 API 文档参见 [Docs/IoStoreOnDemand.md](IoStoreOnDemand.md)。
