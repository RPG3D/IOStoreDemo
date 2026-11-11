# iOS 启动前内容更新

`LaunchIOS.cpp` 在 GameThread 调用 `RunUGPIOSBeforeEnginePreInit()`。更新器在 UIKit 主线程显示
`UGPIOSStartupView`，用 `NSURLSession` 下载清单和 chunk，将更新交给现有
`FUpdateOrchestrator`。失败停留在原生视图，重试沿用同一条线程。完成后移除视图，由
`FAppEntry::Init()` 继续执行唯一一次 `GEngineLoop.PreInit()`，按正常路径挂载容器。

UGP 通过 iOS 物理文件层将内容安装到 `FPaths::ProjectSavedDir()/Paks`。UE 在
`FPakPlatformFile::Initialize()` 首次扫描该目录，随后才加载项目和游戏配置。
IoStore 的 `global.utoc` 也可从这个目录挂载。EarlyStartupScreen 结束后的挂载
可用于额外内容，基础内容需要在首次文件层初始化时就可用。

曾经下载到沙盒 `Content/Paks` 的 UGP 安装会在启动时迁移。已提交且目标目录尚未建立时，
直接移动目录并复用索引和内容。未完成的安装或与新目录并存的旧安装会移到原路径旁边，
离开 UE 的 Pak 扫描范围。签名包保留 iOS 启动资源，移除整个 `cookeddata`。

事务已提交、版本与内容索引一致且索引列出的容器都存在时，版本相同或 CDN 不可达都可继续启动。存在 Pending
事务或提交记录不完整时，必须连上 CDN 走 UGP 的恢复和校验流程，即使远端版本等于旧索引版本。

已有可启动版本时，增量更新在 `Paks.ugp-update` 中执行。iOS 优先用 APFS 文件克隆复制当前版本，
UGP 在副本中写入容器和索引。全部提交后原子交换两个目录，UE 始终从 `Saved/Paks` 读取。
写入失败时清理更新目录并启动旧版本。写入中进程退出后，下一次启动先清理遗留目录。
全新安装仍须在下载并提交成功后才能启动。

需要人工重试时，iOS 在同一个英文对话框中显示 `UGP-xxxx`、失败信息、`Failed retries` 和
`Retry`。首次失败计数为 0。点击 `Retry` 会立即收起对话框。正常更新页底部显示尝试次数、
当前阶段、进度条和活动指示。重试失败后，对话框重新出现并将计数加一。自动回退到已安装版本时，
日志记录诊断码和启动版本。
各阶段含义见 [更新错误码](README.md#更新错误码)。

## 构建

使用 UE 5.7 源码引擎、iPhoneOS SDK 和该应用的开发签名。以下命令从项目根目录执行。

```sh
PROJECT_DIR="$PWD"
UGP_DIR="$PROJECT_DIR/Source/Programs/UnrealGamePatcher"
ENGINE_ROOT=/path/to/UE5

# 每个引擎工作区应用一次。已应用时可以用 git apply --reverse --check 确认。
git -C "$ENGINE_ROOT" apply "$UGP_DIR/Scripts/UE5.7-UGPIOSPreInit.patch"
git -C "$ENGINE_ROOT" apply "$UGP_DIR/Scripts/UE5.7-UGPMobileSavedPaks.patch"

"$ENGINE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" IOStoreDemo IOS Development \
  -Project="$PROJECT_DIR/IOStoreDemo.uproject" -Architecture=arm64
```

旧版 `UE5.7-UGPIOSPreInit.patch` 曾同时修改 `IPlatformFilePak.cpp`。如果引擎已经有该修改，
将已有的 `WITH_UGP_IOS_PREINIT` 条件更新为共享补丁中的条件即可。

`IOStoreDemo` 的 iOS Target 链接 `UGPIOSPatch` 并启用引擎钩子。对于测试包，在
`Config/IOS/IOSEngine.ini` 的 `AdditionalPlistData` 中加入 `UGPCdnURL`，填入设备可访问的 HTTPS CDN 根路径。

```ini
[/Script/IOSRuntimeSettings.IOSRuntimeSettings]
AdditionalPlistData=<key>UGPCdnURL</key><string>https://cdn.example.com/ugp-ios</string>
```

Development 也支持从 Xcode 或 `devicectl` 启动时传入 `-ugpcdn=<CDN URL>`。URL 未配置时
会走普通游戏启动，适用于包含容器的常规包。

## 发布 iOS 内容和打包

1. 用 `RunUAT.sh BuildCookRun -project="$PROJECT_DIR/IOStoreDemo.uproject" -platform=IOS -clientconfig=Development -cook -stage -pak` 生成 iOS stage。添加 `-iostore` 可同时生成 IoStore 容器，Pak-only 验证可加 `-skipiostore`。
2. 运行 `prepare_ios_ugp_stage.py`。脚本将散装的 UE 启动资源合入未签名的基础 Pak，将下载内容输出到 `Content`，并生成无 `cookeddata` 的 `Stage`。IoStore 伴随容器随 Pak 一起进入 `Content`。发布完整 `index/` 和 `chunks/`，最后更新 `index/latest.json`。

   ```sh
   python3 "$UGP_DIR/Scripts/prepare_ios_ugp_stage.py" \
     --stage "$PROJECT_DIR/Saved/StagedBuilds/IOS" \
     --unreal-pak "$ENGINE_ROOT/Engine/Binaries/Mac/UnrealPak" \
     --output "$PROJECT_DIR/Saved/UGPIOSShell"

   "$PROJECT_DIR/Binaries/Mac/UnrealGamePatcher" -cli build \
     -staged="$PROJECT_DIR/Saved/UGPIOSShell/Content" \
     -app=IOStoreDemo -version=1.0.0 -out=/path/to/ugp-ios-cdn
   ```

3. Xcode 封装签名时，将 `UE_OVERRIDE_STAGE_DIR` 指向 `Saved/UGPIOSShell/Stage`。
   把签名的 `IOStoreDemo.app` 放入 `Payload` 后压成 IPA。解包确认无 `cookeddata`、
   `.pak`、`.utoc`、`.ucas` 或散装 UE 资源，确认 `Info.plist` 的 CDN URL。

## 真机验收

| 场景 | 预期 |
| --- | --- |
| 全新安装 | 显示原生更新视图。日志先出现 `UGP: before engine PreInit`，再出现 `UGP: content ready, continuing engine PreInit`，随后挂载容器并进入地图 |
| 已安装同版本或 CDN 断开 | 仅在提交记录、索引和文件均完整时进入地图 |
| 发布新版本 | 下载差异内容，提交后再挂载 |
| 下载失败后重试 | 首次安装保留重试按钮，网络恢复后在同一进程完成安装。已有可用版本时启动该版本。 |
| 增量写入失败 | 旧索引和旧容器字节一致，从旧版本进入地图。Development 可用 `-ugpinjectfail=1` 在第一个目标写完后注入失败。 |
| 写入中强停 | 已有可用版本时，断网重启仍可从旧版本进图。首次安装中断时，恢复网络后继续安装。 |

用同一个空内容包验证首装和增量更新。首装只发布 `pakchunk0-ios.pak` 和
`pakchunk0optional-ios.pak`，确认 UE 进入 `/Game/ThirdPerson/Lvl_ThirdPerson`。
第二版加入 `pakchunk1-ios.pak`，保留设备沙盒，更新 CDN 的 `latest.json` 后以
`-map=/Game/DLC/Maps/NewMap` 启动。两轮均记录 UGP 提交完成、UE 挂载 Pak 和地图加载完成的先后顺序，
并将设备沙盒中的 Pak 与对应 stage 逐个比对 SHA-256。解包检查 IPA 中没有 `.pak`、`.utoc`、
`.ucas` 或散装地图文件。

首轮验证使用未签名、未加密的 UGP 内容容器。应用本身仍使用正常的 iOS 签名。

## 本机验证（2026-11-11）

UE 5.7.4 与 iPhoneOS SDK 18.5 下，arm64 Development 编译、链接和 iOS cook 均通过。
无签名验证用 `UE_BUILD_FROM_XCODE=1` 跳过 UBT 的 `.app` 封装。Pak-only stage 的
3 个文件生成 65 个 chunk，IoStore stage 的 11 个文件生成 70 个 chunk。使用 Mac UGP CLI
从临时 CDN 目录安装后，两种 stage 的文件都通过逐文件 SHA-256 对照。

在 iPad Air（M3）上安装 arm64 Development 测试包。已解包核对签名 IPA，内置
`cookeddata`、Pak、IoStore 容器和散装 UE 资源均为 0。

| 场景 | iPad 结果 |
| --- | --- |
| 旧目录迁移 | 旧安装迁入 `Documents/IOStoreDemo/Saved/Paks`，复用 3 个 Pak 并进入 `NewMap`。 |
| 默认目录升级 | 迁移后的旧安装在默认 `Saved/Paks` 更新基础 Pak，提交 `2.0.1-saved-map`，随后进入 `NewMap`。 |
| 首装 `2.0.0-saved-base` | 空白 Saved 目录从 HTTP CDN 下载 33 个 chunk，UE 挂载两个基础 Pak 并进入 `Lvl_ThirdPerson`。 |
| 增量 `2.0.1-saved-map` | 同一 Saved 目录下载新增 Pak 的 15 个 chunk，UE 挂载 `pakchunk1-ios.pak` 并进入 `NewMap`。 |
| IoStore 首装 `3.0.0-saved-iostore` | 下载 61 个 chunk，UE 从 Saved 目录挂载 `global.utoc`、三个包的 IoStore 容器及其 Pak，并进入 `NewMap`。 |

首装、增量和 IoStore 测试使用 `-saveddirsuffix` 隔离测试内容。设备上的三个 Pak-only 文件
和 11 个 IoStore/Pak 文件均与 CDN 构建输入逐字节一致。IPA 审计、iPad 控制台和哈希记录位于
本机 `Saved/IOSPersistentQA/`。

### 写入失败回退

iPad 在默认 `Saved/Paks` 安装 `2.0.1-saved-map` 后，CDN 发布含两个写入目标的
`2.0.2-write-fail-qa`。用 `-ugpinjectfail=1` 在更新副本写完第一个目标
`pakchunk1-ios.pak` 后注入失败。候选版的该 Pak 已移除 `NewMap`。
同一进程回退到 `2.0.1-saved-map`，UE 从 `Saved/Paks` 挂载旧 Pak 并进入 `NewMap`。

故障前后的旧索引、三个 Pak 和事务状态逐字节一致，第二个目标未出现在活动目录。
关闭注入后，在独立 Saved 测试目录验证候选版可原子提交并进入基础地图。
随后以不可用的 CDN 地址重启，默认 `Saved/Paks` 仍可加载旧版 `NewMap`。
日志、哈希比对和签名 IPA 位于本机 `Saved/IOSRollbackQA/`。

### 写入中强杀

iPad 在独立的 `Saved_KillQA/Paks` 安装 `2.0.1-saved-map`。调试器在写完首个目标后断下，
从设备拉回更新副本，确认首个 Pak 已是候选版、第二个 Pak 尚未写入，事务状态为 Pending。
随后杀进程，设备报告退出状态 9。停止测试 CDN 监听，让原 URL 连接失败，iPad 保持联网。

重启清除了更新副本，旧索引、三个 Pak 和事务状态的 SHA-256 均与强杀前一致，UE 加载旧版
`NewMap`。恢复同一个 CDN 后，同一安装提交 `2.0.2-write-fail-qa` 并进入基础地图。新版索引和
四个 Pak 均与发布文件逐字节一致。证据位于本机 `Saved/IOSKillQA/`。

### 错误码和重试

在 iPad 的空白 Saved 测试目录启动，测试 CDN 停止监听时显示 `UGP-1001`。
用调试器确认错误码标签和「重试」按钮都在可见的同一个对话框内。
恢复 CDN 后，通过调试器触发该按钮，同一进程重新请求 `latest.json`，下载 48 个 chunk 并进入
`NewMap`。真机日志、诊断码检查和零容器签名 IPA 位于本机 `Saved/IOSErrorQA/`。

另一次空白 Saved 测试中，服务端正常返回公告和清单，在 52.2% 的内容进入缓存后截断下载响应。
iPad 显示 `UGP-4001` 和重试按钮。恢复服务端后在同一进程触发重试，复用 21 个完整 chunk，
只请求剩余的 27 个并进入 `NewMap`。设备上的索引和三个 Pak 均与发布文件逐字节一致。
日志与比对结果位于本机 `Saved/IOSDownloadRetryQA/`。

同一 UIKit 视图在本机 iPad 模拟器验证了英文弹窗、失败计数从 0 到 2、重试中的阶段提示、
活动指示和按钮防重复触发。日志和新签名 IPA 位于本机 `Saved/IOSRetryCountQA/`。

该签名包随后装到 iPad，在空白 Saved 目录中实际显示 `Failed retries: 0` 至 `11`。
用户在服务恢复后触发第 12 次重试，同一进程复用 21 个 chunk、补下 27 个并进入 `NewMap`。
服务器请求、索引和 Pak 比对结果见 `Saved/IOSRetryCountQA/iPad-final-verification.log`。

修正后的 UIKit 视图在本机模拟器验证了点 `Retry` 立即隐藏对话框，正常页的阶段和进度条位于
底部，失败时才恢复对话框并累加计数。修正版装到 iPad 后，测试 CDN 将版本响应延迟 2.5 秒
以便观察切换。用户触发的失败计数从 0 到 6，恢复 CDN 后第 7 次重试复用 21 个 chunk、
只补下 27 个并进入 `NewMap`。索引和 Pak 均通过逐字节比对。签名包和验证日志位于本机
`Saved/IOSRetryFlowQA/`。
