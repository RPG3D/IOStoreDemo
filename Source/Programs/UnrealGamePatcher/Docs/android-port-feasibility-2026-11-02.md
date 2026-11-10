# Android 适配可行性分析 + libugp.so Spike（2026-11-02 v2）

> 架构（用户拍板）：**UGP 编译为独立 lib（链 UE 符号）→ PatchActivity 与 lib
> 交互完成版本更新 → 引擎从未启动、容器从未挂载 → 切换 GameActivity 加载
> libunreal.so 后才第一次挂载 pak/ucas**。APK 本体更新不在本轮。
> v1（同日上午）误把"引擎先启动、补丁在引擎内跑"当主方案，本文 v2 以用户
> 架构为准，并加入当日完成的编译 spike 实证。

**结论：方案成立，核心可行性已用实物验证。** `UnrealGamePatcherLib-arm64.so`
（AArch64 DYN 共享库，`Java_..._PatchActivity_nativeRunPatch` + `JNI_OnLoad`
已导出）已在本机产出；UGP 全部核心源码 arm64 编译零错误；引擎-lite 引导
（JNI env → PreInit(-NoPak) → orchestrator → 事件漏斗 → JNI 回调）代码
编译+链接通过。

---

## 一、Spike 实证（2026-11-02 当日，未提交）

### 1.1 工具链：Android Program 目标原生产出 .so（无需手工兜底）

UBT 对 `TargetType.Program` 的 Android 构建直接以 `-shared` 链接：

```
clang++ ... -shared -Wl,-soname,libUnreal.so
  -Wl,--version-script=.../ExportSymbols.ldscript
  -o Binaries/Android/UnrealGamePatcherLib-arm64.so @...rsp
```

- 导出脚本 `{ global: _Znwm*; ...; Java_*; ANativeActivity_onCreate; JNI_OnLoad; local: *; }`
  ——`Java_*` 全导（JNI 按名绑定即插即用）+ 要求两个固定符号存在（见 1.3c）。
- 产物验证（llvm-readelf）：`Type: DYN (Shared object file)`、`Machine: AArch64`、
  JNI 导出符号齐全。
- **打包装箱注意**：输出文件名为 `<Target>-arm64.so`，soname 硬编码 `libUnreal.so`
  ——打包时把文件改名为 `libugp.so` 放 `jniLibs/arm64-v8a/` 即可（独立加载、
  无 DT_NEEDED 依赖方，soname 不参与解析）。

### 1.2 引擎-lite 引导路径成立（PatchActivity 进程内、引擎 boot 之前）

```
PatchActivity(专属 ":patcher" 进程) → System.loadLibrary("ugp")
→ 新建 Java 线程调 nativeRunPatch(cmdline, cdn, version, installDir, listener)
  ├─ AndroidJavaEnv::InitializeJavaEnv()        // 新无参 API（5.7+），缓存 VM/env
  ├─ FCommandLine::Set("-NoPak ...")            // 不挂任何 pak/utoc
  ├─ FTaskTagScope(ETaskTag::EGameThread) + GEngineLoop.PreInit(...)
  ├─ FInstallLock(InstallDir)                   // POSIX flock，跨进程互斥
  ├─ FUpdateOrchestrator::UpdateWithOptions(...) // FUpdateOptions::Events =
  │    TFunction<void(const FUpdateEvent&)> ——现成事件漏斗，JNI 回调直接骑上：
  │    listener.onUpdateEvent(phase, error, isError, current, total, message)
  └─ GLog->Flush / RequestEngineExit / AppPreExit / UnloadModules / AppExit
     （与独立 patcher 的 CLI 收尾逐字相同）
→ Java 收尾 patcher 进程 → 主进程 startActivities(GameActivity)
→ 引擎完整启动 → 从 Saved/Paks 挂载新容器（见 §二）
```

关键点：**patch 全程发生在引擎 boot 之前**——GameActivity 启动时容器已是
新版，一次启动即挂载，无 stale 句柄、无进程重启循环（v1 方案的时序复杂度
整体消失）。patcher 进程只加载 libugp.so（自含引擎子集），libUnreal.so 从不
进入该进程——同一进程内永远只有一份引擎代码，无符号冲突面。

### 1.3 实际踩到的三个坑与解法（全部有案底）

| # | 坑 | 根因 | 解法 |
|---|-----|------|------|
| a | `IMediaTextureSample.h` 的 `GetMediaTextureSampleColorConverter` 在 `#if WITH_ENGINE` 内（:294-334），ApplicationCore 的 Android media TU（WITH_ENGINE=0 的 Program）编译失败 | 引擎从未支持过 Android Program 目标（CrashReportClient 平台表无 Android；Launch 无条件依赖 Engine、靠 UBT 剥离） | **引擎 2 处搬移补丁**：前置声明 + 该虚方法移出守卫（指针返回+nullptr 体，对 WITH_ENGINE=1 零差异）。备份 `IMediaTextureSample.h.bak-ugpandroid` |
| b | 链接期 ~30 个未定义符号（`AndroidThunkCpp_*`、`UE::Jni::FGameActivity::native*`、`FAppEventManager` 等） | Android Program 只把 `LaunchEngineLoop.cpp` 内联进 launch 模块 TU，**Launch 模块其余文件（含全部 Android 胶水）不编译**；而 Android Core 平台文件硬引用这些胶水 | libugp 模块内提供**无 Activity 语义桩**（~30 符号：thunk 返回默认值、JNI natives 空实现、事件管理器单例吞事件）——对无头 patcher 是诚实语义；Core 自带定义的子集不得重复（linker 逐成员拉取会报 duplicate） |
| c | 产物偏大（104MB，Development 含符号） | ApplicationCore 的 media TU 引用 RenderCore/RHI 符号 → 两模块进链接 | Ship 期预期大幅缩小（strip+非 Development）；根治=上游化 1.3a（让该 TU 对 Program 可选） |

另有打包期非阻塞项：gradle 编译引擎 GameActivity.java 模板报缺
`com.google.vr.sdk.samples.permission` 包——**与 lib 无关**，集成期由项目
Java/UPL 接管该层时一并处理。

### 1.4 Spike 落下的代码（未提交，工作区）

- `Source/Programs/UnrealGamePatcher/UnrealGamePatcherLib.Target.cs`（Program/Android）
- `Source/Programs/UnrealGamePatcher/UnrealGamePatcherLib/`（Build.cs + JNI 引导）
- 引擎：`IMediaTextureSample.h` 补丁（备份在旁）
- 构建：`Build.bat UnrealGamePatcherLib Android Development -project=...`（本机 NDK r27c）

## 二、不变的事实（承 v1，全部引擎实证）

- **挂载点**：Android 上 `FPaths::ProjectSavedDir()/Paks` =
  `<FilesDir>/UnrealGame/<Project>/Saved/Paks`（AndroidPlatformFile.cpp:261,290），
  是引擎 `GetPakFolders` 三个硬编码扫描目录之一（IPlatformFilePak.cpp）；
  utoc 随 pak 配对挂载（:6123）。**UGP 装这里 = 引擎自动挂载，零引擎改动。**
- **零容器 APK**：`UE_FORCE_USE_PAKS=1`（Build.h:504；ChaosVisualDebugger.Target.cs:60
  先例）——M1（APK 零游戏容器、manifest 零改动）起步，M2（基线+增量、
  FIndexFileEntry 加 bundled 标记）量产再议。
- **UGP 平台性**：Core/Download 全部走 IPlatformFile（唯一 PLATFORM_WINDOWS
  分支=锁名）；解压走 `FCompression::UncompressMemory`（Core 内置 Oodle，
  Android 随引擎）；版本指针 `index/latest.json`（Java 一次 GET）；
  本地版本 `<Paks>/.ugp/index.json`（Java 纯文件读）。
- **FInstallLock**：`UE::FPlatformSystemWideMutex` POSIX=flock（ApplicationSettingsDir），
  跨进程互斥天然可用——patcher 进程与（假想的）第二个实例互斥。
- **崩溃一致性**：patcher 进程被杀 = UGP 事务回滚机制的既定场景（sidecar
  状态机），Android 进程死亡常态被天然覆盖。

## 三、风险与剩余未知

| 级别 | 项 | 说明 |
|------|-----|------|
| R1（中低） | PreInit 在无 Activity 上下文的真机行为 | 路径缓存/物理文件初始化（OBB/APK asset 挂载）在 PreInit 内；UGP 不读 APK asset，预期无碍——真机联调（S1）验证 |
| R2（中低） | `UE::Jni::Env.Initialize()` 无 GameActivity 时的 classloader 获取 | 若需要，PatchActivity 侧补一个最小 Java 桥（setGlobalActivity 桩已就位可升级为真实现） |
| R3（低） | lib 尺寸 | 1.3c；Ship+strip 后复测 |
| R4（低） | 存储 | 外部 filesDir（FUSE）vs 内部（需核 GFilePathBase 选路）；M1 数据量 ~10MB 级影响有限 |
| R5（低） | cleartext HTTP | `usesCleartextTraffic=true` 或 CDN 上 HTTPS |
| R6（信息） | patch 期间 UX | patcher 进程是纯 Java UI，进度条天然可用（事件漏斗直通）——比 v1 引擎内方案更好 |

## 四、实施顺序（Spike S0 已完成）

- ~~S0 编译 spike~~ ✅（本文 §一）
- **S1（1-2d）真机全链**：PatchActivity Java（版本检查+进度 UI）+ UPL manifest
  （LAUNCHER 切换、:patcher 进程声明、INTERNET/cleartext）+ jniLibs 装箱
  libugp.so → rustfs CDN 下全量安装 → GameActivity 挂载可玩。
- **S2（1d）套件上设备**：15 套件接成游戏内 console 命令（Tests 编入游戏）+
  `adb reverse tcp:18080` → 设备 15/15 全绿 = 验收线。
- **S3（按需）M2**：manifest bundled 标记 + staging 拆分。
- S0+S1+S2 ≈ **3-4 人日**（S0 已吃掉其中一天的主要不确定性）。
