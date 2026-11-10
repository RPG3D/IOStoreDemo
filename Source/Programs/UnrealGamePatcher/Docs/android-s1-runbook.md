# S1 真机全链 runbook（2026-11-02）

> **2026-11-10 真机复测（muyu，Development）**：保留外部存储，沿用同一批容器，
> 按 UAT 布局放在 `<InstallDir>/IOStoreDemo/Content/Paks/`，并使用 UAT 生成的
> `UECommandLine.txt`。单份容器挂载成功，预制 Asset Registry 加载成功，进入
> `/Game/ThirdPerson/Lvl_ThirdPerson`。旧测试 CDN 2.0.2 把容器放在顶层
> `Content/Paks/`，发布新版本时应直接 ingest 标准 Android stage，保留项目目录层级。
> `PatchActivity` 更新成功后保留 staged 命令行，无需生成绝对路径 uproject 或 `-pakdir`。
> 下文 §十二·三 的 FUSE 推测保留为历史记录。
>
> **2026-11-10 最终复测（miro，Development arm64）**：重新编译游戏库与
> `libugp.so`。零容器 APK 从空目录安装 2.0.5，下载 145 个 chunk 后进入地图。
> 2.0.7 → 2.0.8 下载 1 个 chunk，2.0.8 → 2.0.9 回填 1 个 Pak 空洞。
> 注入损坏的新 Pak footer 后，恢复的文件与上一代 Pak 的 SHA-256 一致。
> 另在文件写入阶段强停，Pending 事务在首次重启后恢复。
>
> 零容器测试包从 UAT 生成的 Gradle 工程组装。去掉 `assets/main.obb.png`，
> 将 manifest 中 `bPackageDataInsideApk` 和 `bHasOBBFiles` 设为 false，
> 保留 `bUseExternalFilesDir=true`，清理旧 APK 输出后重新执行 `:app:assembleDebug`。
> 测试 CDN 通过 `adb reverse` 接入。已有安装断网时的 Pending 恢复和
> `libugp` 初始化的 ICU 数据目录错误仍需处理。

> 架构：libugp.so（独立库）+ PatchActivity（`:ugppatch` 进程）在引擎 boot
> 前完成更新 → GameActivity 首次启动引擎并从 Saved/Paks 挂载。
> 依据：android-port-feasibility-2026-11-02.md（v2）。
>
> **历史状态（2026-11-06）**：UGP 链路在真机上 **已 100% 验证**（安装 / 增量 / 锁 /
> 失败停驻 / 容器挂载）。**唯一未通过项 = 从 FUSE 外部存储读取 IoStore 包数据**
> （闪退发生在 `MaterialShared.cpp:3167`，前置是 `WaitingForIo` + premade asset
> registry 加载失败）。详见 **§十二·三（当前结论与修正清单）**。
>
> 阅读顺序建议：§十二·三（最新结论）→ §十二·二（根因分析）→ §八～§十一（真机验证历程）。
> 早前章节保留为过程记录，凡与 §十二·三 冲突处以后者为准。

---

## 一、APK 验收现状（2026-11-02 实测）

产物：`Binaries/Android/IOStoreDemo-arm64.apk`（201 MB，自包含）

| 检查项 | 结果 | 验证命令 |
|--------|------|----------|
| libugp.so 装箱 | ✅ `lib/arm64-v8a/libugp.so` 24.6 MB（UBT 已 strip） | `unzip -l ... \| grep libugp` |
| PatchActivity 进 dex | ✅ classes15.dex | `grep -a "unreal/patch/PatchActivity"` |
| LAUNCHER 唯一且是 PatchActivity | ✅ 全 manifest 仅 1 个 LAUNCHER | `aapt2 dump xmltree` |
| `:ugppatch` 独立进程 | ✅ | 同上 |
| 明文策略 | ✅ `android:usesCleartextTraffic=true` | 同上 |
| INTERNET 权限 | ✅（引擎 manifest 已声明） | 同上 |
| 引擎内容可加载 | ✅ `assets/main.obb.png` 76 MB（内容打进 APK） | `unzip -l` |
| 包名 | `com.YourCompany.IOStoreDemo` | `aapt2 dump badging` |

## 二、复现命令（本机已全部跑通）

```bash
# 1) 构建 libugp.so（独立库目标，产出 AArch64 DYN）
Build.bat UnrealGamePatcherLib Android Development -project=D:\UEProject\IOStoreDemo\IOStoreDemo.uproject
#    → Binaries/Android/UnrealGamePatcherLib-arm64.so
#    → 同步到插件：cp → Plugins/UGPAndroidPatch/Source/UGPAndroidPatch/libs/arm64-v8a/libugp.so

# 2) 构建游戏（同时产出 APK；UPL 在此执行）
Build.bat IOStoreDemo Android Development -project=D:\UEProject\IOStoreDemo\IOStoreDemo.uproject

# 3) 打完整包（cook + stage + pak + package；cook 命中缓存时约 45s）
RunUAT.bat BuildCookRun -project=D:\UEProject\IOStoreDemo\IOStoreDemo.uproject -noP4 ^
  -platform=Android -clientconfig=Development -architecture=arm64 ^
  -cook -stage -pak -package -skipbuild
```

## 三、真机步骤

```bash
adb devices                              # 先确认设备在线
adb reverse tcp:18080 tcp:18080          # 设备 127.0.0.1:18080 = PC 的 cdn_server.py
adb install -r D:\UEProject\IOStoreDemo\Binaries\Android\IOStoreDemo-arm64.apk
adb logcat -s UGPPatch UE UnrealGamePatcher
```

CDN 侧（PC）：

```bash
python Source/Programs/UnrealGamePatcher/Scripts/cdn_server.py --root D:/tmp_selfcheck/s1_cdn --port 18080 --log /d/tmp_selfcheck/s1_cdn_server.log
```

> **版本契约**：fixture 是 1.0.0；APK 里没有 UGP 的 `.ugp` 状态 →
> PatchActivity 首启会判定"未安装"→ 走全量安装 4 文件/9 chunk 到
> `<外部专属目录>/UnrealGame/IOStoreDemo/Saved/Paks`（= 引擎挂载点）。

## 四、实现要点（代码位置）

| 件 | 位置 |
|----|------|
| PatchActivity（版本检查/进度 UI/JNI/转场） | `Build/Android/src/com/epicgames/unreal/patch/PatchActivity.java` |
| JNI 引导 + 无 Activity 胶水桩 | `Source/Programs/UnrealGamePatcher/UnrealGamePatcherLib/Private/UnrealGamePatcherLib.cpp` |
| 独立库目标 | `Source/Programs/UnrealGamePatcher/UnrealGamePatcherLib.Target.cs` |
| 打包期 UPL（LAUNCHER/明文/装 .so） | `Plugins/UGPAndroidPatch/Source/UGPAndroidPatch/UGPAndroidPatch_APL.xml` |
| UPL 登记（必须在 Build.cs 里） | `Plugins/UGPAndroidPatch/Source/UGPAndroidPatch/UGPAndroidPatch.Build.cs` |
| libugp.so 装箱位置 | `Plugins/UGPAndroidPatch/Source/UGPAndroidPatch/libs/arm64-v8a/libugp.so` |

## 五、本轮踩掉的坑（全部已解，复现时不必再走）

| # | 症状 | 根因 | 处置 |
|---|------|------|------|
| 1 | cook 报插件缺模块 DLL（GameplayStateTree / RigLogic / Bridge / MetaHumanSDK） | Setup_Minimal 清掉了 `Binaries/Win64/UnrealEditor.modules` 清单 | 全引擎脚本补齐 4 处清单（`{"BuildId": <编辑器BuildId>, "Modules": {...}}`） |
| 2 | cook 报 "game module 'IOStoreDemo' could not be found" | 项目模块 `.modules` 的 BuildId 停在 8 月（49e7c931）vs 编辑器 10 月（1101d180） | 删 `UnrealEditor-IOStoreDemo.dll` + 重链 + 按编辑器 BuildId 重写清单 |
| 3 | 项目/引擎级 `Enabled: false` 都挡不住 MetaHumanSDK | **Bridge**（默认启用）在 .uplugin 里显式依赖它 | `.uproject` 禁 Bridge（连带 MetaHuman 系列） |
| 4 | 自写 UPL 完全不执行 | UBT **不做** APL 自动发现，必须在模块 Build.cs 里 `AdditionalPropertiesForReceipt.Add("AndroidPlugin", ...)`（照 AndroidPermission 插件）；且插件**必须真有模块** | 新增 `UGPAndroidPatch` 模块（仅承载 UPL） |
| 5 | copyDir/copyFile 在根级写了也不生效 | `copyFile` 必须放在 `<resourceCopies>` 段内 | 改用 `$S(AbsPluginDir)/libs/$S(Architecture)` → `$S(BuildDir)/libs/$S(Architecture)` |
| 6 | manifest merger 失败：INTERNET 重复 | 引擎 manifest 已声明 INTERNET | UPL 里删掉重复的 uses-permission |
| 7 | `usesCleartextTraffic` 没写进 manifest | `addAttribute` 必须用 `name=`/`value=` 两个属性（写成 XML 属性会被静默忽略） | 改 `<addAttribute tag="application" name="android:usesCleartextTraffic" value="true"/>` |
| 8 | 内容不在 APK（切成 `Android.obb`） | `bPackageDataInsideApk` 默认 false | `Config/DefaultEngine.ini` 加 `[/Script/AndroidRuntimeSettings.AndroidRuntimeSettings] bPackageDataInsideApk=True` |
| 9 | 引擎编辑器无法编译（ImgMedia 缺 protobuf.lib） | Setup_Minimal 剔除第三方库 | 只编 `-Module=` 单模块，不做完整编辑器重编 |
| 10 | 引擎 GameActivity 模板 import Google VR | 本机引擎无 VR 依赖（任何 Android 打包都会死在这） | 模板 3 处 `PermissionHelper.checkPermission` → `checkSelfPermission`（备份 `.bak-ugpandroid`） |

## 六、真机待验风险（对应可行性文档 §三）

- **R1 PreInit 无 Activity 上下文**：libugp 在 `:ugppatch` 进程 PreInit(-NoPak)。
  若崩，logcat 有 `libugp`/native 栈；预案：PatchActivity 侧补最小
  `setGlobalActivity` 等价桥。
- **R2 `UE::Jni::Env.Initialize()` 的 classloader**：无 GameActivity 时若
  FindClass 失败，同上定位。
- **R3 首次 patch 后**：GameActivity 挂载的新容器在 `Saved/Paks`；APK 内原有
  内容（main.obb.png）仍会先挂载（两者文件名不同，S1 不冲突）。M2 阶段再处理
  "同容器新旧版本共存"。

## 七、环境改动清单（可回滚，均已备份）

| 改动 | 备份/回滚 |
|------|-----------|
| 引擎 `GameActivity.java.template`：PermissionHelper → checkSelfPermission | `.bak-ugpandroid` |
| 引擎 4 个插件目录补 `UnrealEditor.modules` | 新增文件，删除即回滚 |
| 项目 `Binaries/Win64/UnrealEditor.modules` 重写 | 重新完整构建编辑器即重生成 |
| 项目 `.uproject`：禁 Bridge / MetaHuman 系列 | `IOStoreDemo.uproject.bak-mhdisable` |
| 项目 `Config/DefaultEngine.ini`：`bPackageDataInsideApk=True` | `DefaultEngine.ini.bak-s1` |
| 引擎 `IMediaTextureSample.h`：ColorConverter 移出 WITH_ENGINE | `.bak-ugpandroid` |

---

## 八、真机验证结果（2026-11-03，设备 6e4e811c，Android 16）

**S1 全链真机贯通。** 修复链与验证序：

| # | 问题 | 修复 |
|---|------|------|
| 1 | `UE::Jni::FEnv` 构造 `check(GJavaVM)` 空指针崩溃（patcher 进程无人设置 VM） | JNI 入口 `GetJavaVM` 后写 Core 全局 `GJavaVM`（libugp 内） |
| 2 | `AAssetManager_open(nullptr)` SEGV——引擎假定必有 AssetManager | Java 传真 `getAssets()`，native `AAssetManager_fromJava` 持全局句柄，thunk 返回真值 |
| 3 | FInstallLock 失败：引擎 Android 无 `ApplicationSettingsDir()` override → 锁文件落 `/`（应用无权） | `FInstallLock::Acquire` 增加 Android 分支：`Acquire(InstallDir, LockRootDir)`，锁文件 flock 在**内部存储** |
| 4 | 锁文件放外部存储 flock 仍失败 | **`/storage/emulated/0` 是 FUSE，不支持 flock**——锁必须进内部 `getFilesDir()`（ext4） |
| 5 | "HTTP CDN needs a downloader" | libugp 照 CLI 模式接 `FHttpChunkDownloader` + `FHttpModule::Get()` + `Options.Downloader` |
| 6 | **失败停驻**（需求约束） | `PatchActivity.showFailure()`：进度条隐藏、红字、重试按钮；失败不转场 |

**验证序（logcat 实证）**：
```
UGPPatch: start patch: install=<Saved/Paks> target=1.0.0        # :ugppatch 进程、引擎未启动
FIndexManifest: loaded '<index>' (1.0.0, 4 files, 9 chunks)     # HTTP 清单
FUpdateOrchestrator: downloaded 9 chunks (10358645 bytes)        # HTTP 下载
FUpdateOrchestrator: update to 1.0.0 complete
UGPPatch: 更新完成 -> 1.0.0                                      # 转场
[GameActivity] onCreate() → Mounted main OBB → global.utoc      # 引擎才启动
→ 二次启动: UGPPatch: 已是最新版本 1.0.0                          # no-op 直通
```

**容器挂载实证**：引擎扫到 APK 内 3 个容器 + UGP 安装的 `pakchunk1-Windows.*`
→ `Mounting pak file` → `Mounted Pak file` + `Mounted container`（IoDispatcher）。

### 已知项

1. **默认 `Saved/Paks` 扫描未命中**（`Found Pak file` 只有 APK 内 3 个）：引擎
   `GetPakFolders` 的 `ProjectSavedDir()` 相对路径（`../../../IOStoreDemo/Saved/Paks`）
   经 FAndroidPlatformFile 映射后未命中外部 FUSE 目录（待深挖映射规则）。
   **现行解法 = 官方机制**：非 Shipping 支持 `UECommandLine.txt`（固定位置
   `<BasePath>UECommandLine.txt`，LaunchAndroid.cpp:438）注入
   `-pakdir=<Saved/Paks 绝对路径>` → 4 个容器全部挂载 ✓。已写入设备。
   长期可考察 GameActivity 传参或把安装点对齐引擎实际扫描目录。
2. `LogAndroid: Error: <intl 路径>` 噪音（PreInit 的国际化目录探测）触发
   3 次 `RequestExit`——被 `AndroidThunkCpp_ForceQuit` 桩吸收，流程无害继续。

### 构建链提醒（第 5 次踩坑教训）

**libugp.so 是独立目标**：改动后必须完整走
`Build.bat UnrealGamePatcherLib Android` → **拷到
`Plugins/UGPAndroidPatch/Source/UGPAndroidPatch/libs/arm64-v8a/libugp.so`** →
`Build.bat IOStoreDemo Android`（重打 libUnreal.so?不需要，但 UAT 打包要重跑）
→ `RunUAT BuildCookRun`。只跑游戏构建不会更新 libugp（曾因此把旧库装进
APK 复现已修复的崩溃）。

---

## 九、M1 变体：零内置 APK（2026-11-03）

> 用户要求：APK **不内置 UE PAK 资源**，全部内容由 UGP 从 CDN 安装。
> 即可行性文档 §2.2 的 M1 方案落地。

### 改动

1. `IOStoreDemo.Target.cs`：`GlobalDefinitions.Add("UE_FORCE_USE_PAKS=1")`
   （Build.h:504——官方"零 pak 启动"开关，ChaosVisualDebugger.Target.cs:60 先例）。
   **代价：触发全量 arm64 重编**（GlobalDefinitions 进每个模块的编译命令）。
2. `PatchActivity.java`：InstallDir 从 `<BasePath>/Saved/Paks` 改为 **BasePath 本身**
   （`<files>/UnrealGame/<Project>`）——UGP 安装**整棵 staged 树**（ingest 本就
   递归，file id = 相对路径，install 侧 `CreateDirectoryTree` 建子目录），引擎
   的默认 `Content/Paks` 扫描与 loose 文件读取都以 BasePath 为根。
3. CDN：把 UAT 的 staged 树（`Saved/StagedBuilds/Android/IOStoreDemo`，含
   `Content/Paks/global.*` + `pakchunk0/1-Android.*` + Engine 配置等）整树
   ingest 为 **2.0.0**（11 文件 / 56 chunks / 62.6MB chunk 字节）。
4. 零内容 APK = `Build.bat IOStoreDemo Android`（不走 UAT cook/stage——
   gradle assembleDebug 直出，无 `main.obb.png`、无容器，含 libugp +
   PatchActivity；UPL 在此路径同样执行）。
5. 设备 `UECommandLine.txt`：`-pakdir=<BasePath>IOStoreDemo/Content/Paks/`
   （显式枚举容器层；默认扫描对 sandbox 相对路径的映射仍待挖，见 §八已知项 1）。

### 更新语义

设备原有 1.0.0（fixture：pakchunk1-Windows.* + boot.txt，装在旧 Saved/Paks）→
CDN 2.0.0（全树）→ diff = 全部 Added（新安装根）→ 全量下载 ~63MB → 引擎从
BasePath 挂载真游戏内容。


### 九·一、"找不到 uproject 文件"弹窗——静态根因（2026-11-03 夜，已闭环）

**现象**：零内容 APK 首启，UE 错误弹窗（MessageBox01 = `FAndroidMisc::MessageBoxExt`
的 Java 侧）报找不到 .uproject，主线程 sleep → ANR。

**因果链（全部引擎源码坐实）**：
1. `LaunchSetGameName`（LaunchEngineLoop.cpp:704 `#if UE_GAME` 分支）：可执行名
   ≠ "UnrealGame" → **无条件** `FPaths::SetProjectFilePath("../../../IOStoreDemo/IOStoreDemo.uproject")`
   （不看文件是否存在）。
2. PreInit（:2508）：游戏目标（非 `IS_PROGRAM`）**没有 FileExists 前置检查**，
   `IsProjectFilePathSet()` 即 `LoadProjectFile`。
3. `FAndroidFile` 把该相对路径映射到 `<BasePath>IOStoreDemo.uproject`
   （BasePath = `<files>/UnrealGame/IOStoreDemo`）。
4. **M1 第一版的安装树里没有它**：ingest 传了 `-staged=.../Android/IOStoreDemo`
   （stage 根的**子目录**），而 `IOStoreDemo.uproject` 在 **stage 根**下
   （index 里命中的 `"IOStoreDemo/IOStoreDemo.uproject"` 就是 stage 根视角路径）
   → 设备上无此文件 → `LoadProjectFile` 失败 →
   `"Could not find a valid project file, the engine will exit now."`（LaunchEngineLoop.cpp:2520）
   → GError → 弹窗 → ANR。

**正常发布为何不炸**：UAT 完整 stage 里 uproject 在包内 → `FileExists` ✓。

**M1 正确姿势（下轮真机执行）**：
1. `-staged=Saved/StagedBuilds/Android`（**stage 根**：含 `IOStoreDemo/{uproject,
   Content/, Manifest…}` + `Engine/…`）——重 ingest 为 2.0.1。
2. Java `InstallDir = <files>/UnrealGame`（**UnrealGame 根**）：安装后
   `<BasePath>IOStoreDemo.uproject` ✓、`<BasePath>Content/Paks/*` ✓、
   `<files>/UnrealGame/Engine/…` ✓——全部对齐引擎期望。
3. 附带收益：`UECommandLine.txt`（LaunchAndroid 固定读 `<BasePath>`）从此**不在
   UGP 安装树内**，§八的"stray 误删"问题自然消失；设备上写
   `-pakdir=<files>/UnrealGame/IOStoreDemo/Content/Paks/`（绝对路径已验证 3 pak
   可挂）。
4. 设备先 `rm -rf files/UnrealGame` 全新首装 2.0.1（id 前缀变了，增量会全量
   Removed+Added，不如直接清）。
5. 遗留疑点不变：utoc 的 IoDispatcher 挂载未发生（§八已知项 1）+ OnDemand
   HTTP dispatcher 抢占（LogHttpIoDispatcher 初始化）——若相对/绝对 -pakdir
   都不挂 utoc，下一刀是禁用 OnDemand 初始化对比。

> **注意：本节结论已被 §十二·三 修正。** 实测 UAT 的 Android stage 目录并不包含
> `IOStoreDemo.uproject`（`find Saved/StagedBuilds/Android -iname '*.uproject'` 为空）。
> 正解不是扩大安装树，而是 PatchActivity 自己生成 uproject 并以绝对路径作命令行首参
> （见 §十二·三 代码修正 1）。

**教训**：M1 的安装树必须 = **UAT stage 根**（不是其项目子目录）；UGP 的
InstallDir 语义要对齐 `GFilePathBase/UnrealGame`（LaunchAndroid 的 sandbox
根），而非 `BasePath/Saved/Paks`。

---

## 十、M1 改动自审（2026-11-03 深夜，提交前 review）

对全部未提交改动（Paths/FDownloadExecutor/UnrealGamePatcherLib/PatchActivity/
Target.cs/APL/配置）做提交前 review，抓到 **2 个真 bug + 1 个文档错误**，均已修复：

| # | 级别 | 发现 | 修复 |
|---|------|------|------|
| R1 | **High** | FDownloadExecutor 补洞的 **TOCTOU 竞态**：两段并发都读到旧 `FileSize`，后填者的零填充会**覆盖先段已写的数据**（SHA1 终验能兜住不落盘，但下载陷入重试循环）——补洞本身有洞 | 补洞+写段按 staging 文件**串行化**（`StagingLockFor` 每路径一把锁，进程生命周期）；网络传输仍并行，只有同文件落盘串行 |
| R2 | **High** | libugp **重试按钮必崩**：失败路径也走完整 `AppExit` 关闭引擎，Java 重试对已退出引擎二次 `PreInit`（同进程二次 PreInit 不支持） | 引擎**每进程只 boot 一次**（`bEngineReady` 门控）；失败路径跳过 shutdown（引擎保温），重试只重跑编排器（UGP 的事务恢复本就为此设计）；成功才走完整关闭。`FTaskTagScope` 提到函数级覆盖两条路径 |
| R3 | Medium | runbook §九 断言"UECommandLine.txt 不再被 stray 删除"**错误**——新布局下它仍在安装树内（`<InstallDir>IOStoreDemo/UECommandLine.txt`） | PatchActivity 在每次成功安装后重写该文件（含 `-pakdir`）；UGP 侧排除清单挂 M1 待办 |
| R4 | Low | `GUgpAssetManagerJava` 全局引用每次 RunPatch 泄漏一个 | 随 R2 一并收敛（assets 只在首-boot 捕获一次） |

**回归验证**：Windows Development 构建 0 error；`downloadtest` 132 PASS /
`updatetest` 129 PASS 全绿（核心下载器改动不破坏既有路径）；Android lib+game
构建 0 error。**全 15 套件回归与真机 2.0.1 首装留待提交前一并执行**。

### 遗留清单（下轮）

1. 真机：2.0.1（staged 树含 uproject）首装 → 验证 uproject 弹窗消失 → utoc
   IoDispatcher 挂载疑点（OnDemand 抢占）→ 游戏可玩。
2. Windows：全 15 套件回归。
3. M1 设计项：UGP 排除清单（`UECommandLine.txt`/引擎运行时文件不被 stray 删除）。
4. pakinplace 的 `BytesWritten==91` 精确 pin（§四放松项）可在全绿回归时顺手复核。

### 九·二、M1 就绪状态与明日真机清单（2026-11-06 凌晨）

**PC 侧备料（全部就绪）**：
- CDN：`D:/tmp_selfcheck/s1_cdn`，`latest.json` = **2.0.1**（11 文件/56 chunks/
  62.6MB，来自最新 staged 树；cdn_server 已在 :18080 运行）；
- 零内容 APK：`Binaries/Android/IOStoreDemo-arm64.apk`（125.5MB，obb=0/容器=0，
  libugp 含 11-03 全部 review 修复：补洞串行化 + 引擎保温重试）；
- `adb reverse tcp:18080 tcp:18080` 后 PatchActivity 会在安装成功后**自动写**
  `<BasePath>UECommandLine.txt`（含 `-pakdir=<…>/Content/Paks/`）。

**明日真机一步清单**：
```sh
adb devices                                  # 确认真的机（非模拟器）
adb reverse tcp:18080 tcp:18080
adb install -r D:\UEProject\IOStoreDemo\Binaries\Android\IOStoreDemo-arm64.apk
adb shell "rm -rf /storage/emulated/0/Android/data/com.YourCompany.IOStoreDemo/files/UnrealGame"
adb logcat -c
adb shell am start -n com.YourCompany.IOStoreDemo/com.epicgames.unreal.patch.PatchActivity
adb logcat -s UGPPatch UE UnrealGamePatcher
```
预期：未安装 → 全量 2.0.1（11 文件/56 chunks）→ PatchActivity 自动写
UECommandLine.txt → 转场 → **无 uproject 弹窗**（安装树已含 staged 根内容；
若仍弹，按 §九·一 用 `adb push` 把 `IOStoreDemo.uproject` 推到 BasePath 直接
验证根因假设）→ 引擎挂载（pak ×3 + utoc 待观察）→ 游戏表现。

**模拟器结论（已放弃）**：x86_64 API 30 模拟器能**装** arm64 APK（native
bridge），`:ugppatch` 进程也能起，但 ARM 翻译执行 libugp 在引擎 lite 初始化
深处 **SIGILL**（翻译层指令边界），且渲染层另有 GLES 3.1 vs 3.2 的硬缺口——
按约定放弃，真机为准。

**UBT 并行提醒**：引擎 BuildId 漂移（1101d180→f596c3ce，11-04）是并行会话
构建重写引擎清单所致；4 个手写引擎插件清单 + 项目/插件清单已统一刷为
f596c3ce。后续任何会话构建后若再报"module could not be found"，先对比
`Engine/Binaries/Win64/UnrealEditor.modules` 的 BuildId 再刷。

---

## 十一、x86_64 模拟器：更新链 100% 贯通（2026-11-06）

双架构落地：`bBuildForx8664=True`（AndroidRuntimeSettings）→ libugp 与游戏
arm64+x64 双编（游戏双全量 ~34 分钟）。**x86_64 模拟器（API 30）上 M1 更新链
端到端全通**：

```
UGPPatch: start patch: install=<BasePath> target=2.0.1
FIndexManifest: loaded '<index>' (2.0.1, 11 files, 56 chunks)
FUpdateOrchestrator: downloaded 56 chunks (65621520 bytes)   # ~2s localhost
UGPPatch: 更新完成 -> 2.0.1                                   # UECommandLine.txt 自动写入
[GameActivity] onCreate → 引擎运行时文件落 BasePath（Engine/、Manifest_UFS…）
```

**模拟器边界（已确认，真机项）**：GameActivity 渲染撞 GLES 3.1 缺口（UE5 需
3.2/Vulkan）——容器挂载与游戏表现只能在真机验证。ARM 翻译层跑 arm64 引擎在
初始化深处 SIGILL（已放弃）。

**本轮顺手修正**：`UECommandLine.txt` 曾被写到双层 `IOStoreDemo/IOStoreDemo/`
（PatchActivity 多拼一层 PROJECT_NAME）——已改为单层 BasePath，模拟器实测
落位与内容正确。

**APK 矩阵（Binaries/Android/）**：
- `IOStoreDemo-arm64.apk`（125.5MB）— 真机
- `IOStoreDemo-x64.apk`（136MB）— 模拟器
- 双份均零内容 + libugp 双架构装箱（`libs/{arm64-v8a,x86_64}/libugp.so`）

---

## 十二、真机实测（2026-11-06）：UGP 链路 100% 通，卡在 cook 内容

真机：Redmi 24091RPADC / Android 16 / arm64-v8a。

### 已验证通过（UGP 侧全部 OK）

```
UGPPatch: start patch: install=<BasePath> target=2.0.1
FIndexManifest: loaded (2.0.1, 11 files, 56 chunks)
FUpdateOrchestrator: downloaded 56 chunks (65621520 bytes)
UGPPatch: 更新完成 -> 2.0.1
LogIoDispatcher: Mounted container '.../global.utoc'        ← 关键突破
LogPakFile: Mounted IoStore container ".../pakchunk1-Android.utoc"
Mounted IoStore container ×6（3 pak + 3 utoc 全挂）
LogShaderLibrary: Loaded Shared Shader Library Global (1804 shaders)
LogRHI: Initializing OpenGL RHI — GLES 3.2 ✓
```

### 真机修掉的两个关键问题

**① `ProjectDir()` 决定一切（uproject 的真正作用）**
`IPlatformFilePak.cpp:5736` 用 `FPaths::ProjectContentDir()` 判定是否挂
global.utoc → `ProjectDir()` → 游戏分支在**没有 project file path 时**回退到
相对路径 `"../../../IOStoreDemo/"`，Android FAndroidFile 下解析不到 FUSE 绝对
目录 → 判定失败 → IoStoreFileBackend 不建 → 所有 utoc 静默跳过 → shader 库缺失
→ Fatal。
**解法（已验证）**：`UECommandLine.txt` 里把 **uproject 绝对路径**作为第一个
参数传入 → 引擎 `UE::ProjectUtilities` 调 `SetOverrideProjectDir()` →
`ProjectContentDir()` 正确 → 6 容器全挂。
（`-pakdir=` 只影响 pak 发现，**不改变** global.utoc 的判定路径——这是 M1 的
关键认知。）

**② RHI 选择**
APK 按 GLES 3.1 打包，设备默认想走 Vulkan → 必须带 `-OpenGL`（设备 GLES 3.2
向下兼容），否则 shader 平台不匹配。

### 未通过项：cook 内容不完整（与 UGP 无关）

`Fatal: Failed to find shader map for default material WorldGridMaterial ...
missing (TMobileBasePassPSFMobileDirectionalLightAndSHIndirectPoli…)`。

- global shader library **加载成功**（1804 shaders）、容器挂载成功；
- 但材质**内联 shader map** 缺失 → Development 配置下 fatal；
- 根因：staged 内容是 `-skipbuild` 复用旧 cook 产物，材质内联 shader 未随
  当前配置完整生成。

**下一步（内容侧，非 UGP）**：完整重 cook（去掉 `-skipbuild`，或
`-cookflavor=` 对齐），或改用 Shipping 配置打包验证。

### 今日修正汇总（都需在真机上带这两条）

`UECommandLine.txt` 必须是：
```
<BasePath>/IOStoreDemo.uproject -pakdir=<BasePath>/Content/Paks/ -OpenGL
```
且 `IOStoreDemo.uproject` 文件必须存在于 `<BasePath>`（内容侧需打进包，或
UGP 安装清单包含它——M1 待办）。


### 十二·一、真机后续：UGP 侧全通，卡在 mobile shader permutation

完整重 cook（清 `Saved/Cooked/Android` 后重跑，1498 行 BUILD SUCCESSFUL）后再测，
**同一 fatal 仍在**：

```
LogRHI: ShaderPlatform = GLSL_ES3_1_ANDROID      ← 运行侧正确
LogMaterial: Incomplete material WorldGridMaterial,
  missing (TMobileBasePassPSFMobileDirectionalShiftAndSHIndirectPoli…)
Fatal: Failed to find shader map for default material WorldGridMaterial
```

即：运行侧要 **mobile（ES3_1）的 BasePass 前向 permutation**，而 cook 产物里
没有。cook 日志侧证据：`Input: 3.33 MiB for 210 Inline shaders`（数量偏少），
且 `WorldGridMaterial` 在 cook 日志中**零次出现**——它被
`SkipOnlyEditorOnly`（LogCook:159 "unsolicited packages will not be cooked
unless referenced"）跳过了。

**结论**：UGP 侧（安装/挂载/增量/事务）在真机上已 100% 验证通过；剩余问题是
**cook 的内容覆盖度**（engine 默认材质未被 cook 进 ES3_1 目标）。

**明日验证路径（择一）**：
1. `-cookflavor=` 显式指定 ES3_1 + `-targetplatform=Android -shadertargets=`；
2. 或在 `Config/DefaultEngine.ini` 的 `[/Script/Engine.RendererSettings]` 确保
   `r.Mobile.*` 与 DeviceProfile 的 `OptionalLODBias` 组合被 cook 覆盖；
3. 或先验证 **Shipping** 配置（shader 打包策略不同，更完整）。

**同时确认的增量能力（真机实证）**：2.0.1 → 2.0.2 只下 **1 个 chunk
(1,351,855 bytes)** 即完成，UGP 增量干净生效。

### 真机每次必须准备的两件（M1 待办清单）

```sh
BASE=/storage/emulated/0/Android/data/com.YourCompany.IOStoreDemo/files/UnrealGame/IOStoreDemo
# ① uproject 必须在（决定 ProjectDir → global.utoc 判定）
adb shell "printf '{\"FileVersion\":3,\"EngineAssociation\":\"5.8\"}' > $BASE/IOStoreDemo.uproject"
# ② 命令行：uproject 绝对路径（首参）+ pakdir + OpenGL
adb shell "echo '$BASE/IOStoreDemo.uproject -pakdir=$BASE/Content/Paks/ -OpenGL' > $BASE/UECommandLine.txt"
```
注意：UGP 每次安装会把这些**非清单文件当 stray 清理掉**，所以必须在 PatchActivity
安装成功后重写（当前已实现 UECommandLine.txt，**uproject 尚未实现** → M1 待办）。

### 十二·二、静态分析定因：shader 库按 **pak 挂载目录** 发现（2026-11-06 夜）

**决定性对照实验**：把 20:21 完整 cook 出的 **官方内置 APK**（obb=1）直接装到同一台
真机跑 **官方 GameActivity 路径** → **完全正常**：

```
Mounted container '../../../IOStoreDemo/Content/Paks/global.utoc'
Mounted IoStore container "IOStoreDemo/Content/Paks/pakchunk1-Android.utoc"
LogShaderLibrary: Loaded Shared Shader Library Global      (1864 shaders)
LogShaderLibrary: Loaded Shared Shader Library IOStoreDemo_Chunk1
LogShaderLibrary: Loaded Shared Shader Library IOStoreDemo_Chunk0  (749 shaders)  ← M1 缺这个
LogLoad: LoadMap /Game/ThirdPerson/Lvl_ThirdPerson  ✓ 游戏正常运行
```

**内容字节级一致**（已验证）：CDN 2.0.2 与 APK 内 OBB 的 `global.utoc` /
`pakchunk0-Android.utoc` SHA1 **完全相同**（`c72530a8…` / `54d1f674…`）。

**唯一差异 = 挂载路径形式**：
- 官方：`../../../IOStoreDemo/Content/Paks/global.utoc`（**相对 pak 虚拟路径**）
- M1 ：`/storage/emulated/0/.../Content/Paks/global.utoc`（**绝对 FUSE 路径**）

**机制（ShaderCodeLibrary.cpp:205-245）**：
```cpp
void OnPakFileMounted(const FMountedPakFileInfo& MountInfo, const FString& Directory)
{
    FShaderLibraryChunkInfo ChunkInfo = GetLibraryChunkInfo(MountInfo);   // 由 PakFilename 解析 chunk id
    if (!GetPresentChunks().Contains(ChunkInfo.ChunkId))
        OpenShaderCode(Directory, ChunkInfo.ChunkLibraryName, ...);       // ← 用 Directory 找 shader 库
}
```
`OpenShaderCode(Directory, "IOStoreDemo_Chunk0", …)` 在 **Directory 内**查找
shader 库。M1 的绝对路径与引擎内部 pak 虚拟路径体系（`../../../`）不一致，
chunk0 的 shader 库因而未被打开 → 749 个 shader 缺失 → `WorldGridMaterial`
的 mobile BasePass permutation 找不到 → Fatal。

**结论**：M1 必须让引擎看到 **相对形式的容器路径**（`../../../IOStoreDemo/Content/Paks/`），
而不是绝对 FUSE 路径。下一步验证：
- `-pakdir=../../../IOStoreDemo/Content/Paks/`（相对形式，之前试过一次但当时
  uproject 未到位 → ProjectDir 错误，两个变量混在一起无法归因）
- **同时**保证 uproject 在 `<BasePath>`（ProjectDir 正确）——两者必须一起满足。

**下一步实验设计（无需再 cook）**：
```sh
BASE=/storage/emulated/0/Android/data/com.YourCompany.IOStoreDemo/files/UnrealGame/IOStoreDemo
adb shell "printf '{\"FileVersion\":3,\"EngineAssociation\":\"5.8\"}' > $BASE/IOStoreDemo.uproject"
adb shell "echo '$BASE/IOStoreDemo.uproject -pakdir=../../../IOStoreDemo/Content/Paks/ -OpenGL' > $BASE/UECommandLine.txt"
```
预期：ProjectDir 由 uproject 绝对路径正确设定 → global.utoc 判定通过（相对路径
在 pak 虚拟文件系统内可解析）→ chunk0 shader 库被打开 → 1864 shaders →
游戏可进。

### 十二·三、假说验证结果（2026-11-06 深夜，逐条实测）

上一节的预测**部分成立、部分被推翻**，实测记录如下：

| 假说 | 结果 |
|------|------|
| uproject 缺失 → ProjectDir 错 → utoc 不挂 | ✅ **成立**。补上 uproject（绝对路径作首参）后 6 容器全挂 |
| 相对 `-pakdir` 才能让 Chunk0 shader 库被发现 | ✅ **成立**。绝对路径下无 `IOStoreDemo_Chunk0` 日志；改相对后 **GLES 720 / Vulkan 749 shaders 全部加载** |
| 内容与官方不一致 | ❌ **推翻**。11 个容器逐字节比对，SHA1 与官方 OBB **全部一致**；Vulkan 下 shader 数也完全等于官方（Global 1864 / Chunk1 3 / Chunk0 749） |
| `-OpenGL` 是必需项 | ❌ **推翻**。Vulkan（`VULKAN_ES3_1_ANDROID`）走同一条路，shader 库数量与官方一致，同样 fatal |
| shader 库加载失败是 fatal 主因 | ❌ **推翻**。库加载数量已与官方完全一致，仍 fatal |

**当前 fatal 的真实位置（包数据 IO，不是 shader 库）**：
```
LogAssetRegistry: Error: Failed to load premade asset registry. LoadResult == 1.
LogStreaming: Display: Partially loaded package /Engine/EngineMaterials/WorldGridMaterial
              (state: WaitingForIo)
LogMaterial: Incomplete material WorldGridMaterial, missing (TMobileBasePassPSF…)
Fatal (MaterialShared.cpp:3167)
```
`WaitingForIo` + premade asset registry 加载失败 = **从容器读取包数据的异步 IO 未完成**，
而 shader 库（同样走 IoDispatcher）却能读出来。

**唯一剩下的结构性差异**：官方从 **APK 内 OBB（zip 只读层）** 读容器，M1 从
**`/storage/emulated/0` 的 FUSE 外部存储**读同一批字节。因此末位嫌疑 =
**FUSE 上 IoStore 的读模式不被满足**（`pread`/大块对齐/并发读）。

**下一步（需要构建，已按约定暂停）**：改用**内部存储**验证——但
`nativeSetGlobalActivity` 在非 Shipping 恒用 external
（`#if UE_BUILD_SHIPPING GFilePathBase = GInternalFilePath; #else = GExternalFilePath`），
所以需要 **Shipping 构建**（或用 `-userdir=`/自定义 BasePath 实验）。若 Shipping 下从内部
存储读到同一批容器后能进游戏，则根因确认为 FUSE，M1 需把安装点改到内部存储。

### 本轮代码修正（review 产出）

1. `PatchActivity`：安装成功后**自动生成 `<BasePath>/<Project>.uproject`**（缺失会导致
   ProjectDir 错 → utoc 全不挂），并把其绝对路径作为 `UECommandLine.txt` 首参。
2. `PatchActivity`：`-pakdir` 改为**相对形式** `../../../<Project>/Content/Paks/`
   （绝对形式会让 Chunk0 shader 库静默缺失——真机实测）。
3. `PatchActivity`：patch 尝试改为**单一常驻 worker 线程**（`HandlerThread`）。libugp
   失败保温不退出引擎，而引擎把 game-thread 身份绑定到跑 PreInit 的那个线程；
   原先每次 `new Thread` 重试会跑在失效的 game-thread id 下。
4. `FInstallLock`：Android 分支换/Open 失败日志指向真实 LockDir（原先误报 InstallDir）。

### 环境开关的取舍（提交前需知）

| 开关 | 作用 | 生产建议 |
|------|------|----------|
| `Source/IOStoreDemo.Target.cs`: `UE_FORCE_USE_PAKS=1` | 零容器 APK 也能起 pak 环境（M1 必需） | 保留 |
| `Config/DefaultEngine.ini`: `bBuildForx8664=True` | 双架构（模拟器可验更新链） | **量产关掉**——它让每次 Android 构建翻倍（实测 34 min → 需全量重编） |
| `Config/DefaultEngine.ini`: `bPackageDataInsideApk=True` | 内容进 APK（对照实验用） | 与 M1 无关，可按需关闭 |
| `IOStoreDemo.uproject`: 禁 Bridge / MetaHuman 系列 | 本机引擎缺这些插件的二进制，否则 cook 直接失败 | 仅本机需要，勿带入团队共享配置 |

所有被改的配置均有 `.bak-*` 备份在旁（`DefaultEngine.ini.bak-s1`、`IOStoreDemo.uproject.bak-mhdisable`、`IOStoreDemo.Target.cs.bak-m1`）。
