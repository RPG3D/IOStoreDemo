# Mac Slate 启动器

启动器使用独立的 Slate Program。窗口线程显示检查和下载进度，工作线程解析版本、执行 UGP 更新。下载时进度按字节显示。交互模式的版本查询失败时显示 `UGP-1001`，Retry 会重新读取 CDN 的 `latest.json`。内容提交后可以 Play。窗口会在自己启动的游戏进程存活期间禁用更新。

## 构建和运行

使用 UE 5.7 源码引擎。从项目根目录执行：

```sh
ENGINE_ROOT=/path/to/UE5
"$ENGINE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" UnrealGamePatcher Mac Development \
  -Project="$PWD/IOStoreDemo.uproject" -MaxParallelActions=2
```

用 `UnrealGamePatcher.app` 启动。以下命令将下载目录和游戏包分开。游戏使用 Development 目标支持的 `-pakdir` 挂载可选内容。

```sh
PATCHER="$PWD/Binaries/Mac/UnrealGamePatcher.app/Contents/MacOS/UnrealGamePatcher"
CONTENT="$HOME/Library/Application Support/IOStoreDemo/Downloaded Paks"
GAME=/path/to/IOStoreDemo.app

"$PATCHER" -cdn=http://127.0.0.1:18080 "-install=$CONTENT" "-launch=$GAME" -autoupdate \
  -- "-pakdir=$CONTENT"
```

`-autoupdate` 自动检查和更新，失败时仍可在窗口中 Retry。`-autoplay` 更新成功后启动游戏并关闭启动器。更新或进程创建失败时，`-autoplay` 返回非零状态。`--` 后的参数传给游戏。Mac 上的 `-launch` 接受完整 `.app` 路径或可执行文件路径。

## 内容目录与发布

- CDN 的更新清单应以待更新的容器目录为根。`-install` 应使用专用内容目录。Mac 启动器解析符号链接和现存父目录，通过 `.app` 或包内可执行文件启动时均拒绝重叠的安装路径。签名会封存 bundle 内的资源，发布后的内容写入 bundle 外。[Apple 代码签名说明](https://developer.apple.com/library/archive/technotes/tn2206/)
- 游戏在 UE 挂载前取得 `.ugp/install.lock` 的共享读锁并持有到进程退出。`update`、`repair` 和 `compact` 写入时取得排他锁，冲突时返回 `UGP-9001`。启动器通过 `-ugpinstall` 传入内容根目录。直接启动游戏时可传相同参数，Development 的 `-pakdir` 也会识别带 `.ugp` 状态的目录。应使用包含同一锁协议的启动器、CLI 和游戏构建。
- Mac 写入入口解析安装根的别名，在创建元数据前检查安装树。安装树内的符号链接、嵌套安装和包内安装根会被拒绝。写入进程同时锁住实际目录，父子目录的写入互斥，同级安装仍可独立更新。各级 `.ugp` 都保留给更新器使用。
- Development 验证使用游戏包内的基础容器和 `global.utoc`，外部目录安装 `pakchunk1-Mac.pak` 及配套的 `.utoc`、`.ucas`。UE 在 Shipping 下不会读 `-pakdir`。若要把基础内容及 `global.utoc` 一并外置，需要在游戏的 Pak 初始化阶段接入正式内容路径。
- 本机原始游戏包启用了 App Sandbox。独立启动器和游戏共用外部内容时，发行配置需要让两个进程都能访问该目录。采用 App Sandbox 时，可使用同一团队签名的 App Group 容器。[Apple 沙盒文件访问说明](https://developer.apple.com/documentation/security/accessing-files-from-the-macos-app-sandbox)

## 本机验收

使用本地临时签名的 Development QA 包。从包中移除 DLC 三件套后重新签名，测试期间保持游戏包封存。CDN 暂缺 `latest.json` 时启动器记录 `UGP-1001`。恢复文件后，Slate 启动器从空内容目录下载 13 个 chunk，安装三个容器文件。下载前后的文件 SHA-256 一致。游戏通过 `--` 收到含空格的 `-pakdir`，挂载外部的 Pak 和 IoStore 容器，进入 `/Game/DLC/Maps/NewMap`。游戏包在测试后通过签名校验。

本机日志位于 `Saved/MacLauncherQA/`。启动器在 `-autoplay` 成功后自行退出，游戏冒烟测试使用 `-nullrhi -nosound -ExecCmds=quit`，进图后自动退出。

手动复测在同一个 Slate 窗口观察到 `UGP-1001` 和 Retry。恢复 CDN 后，用 Tab 和 Enter 触发 Retry，窗口立即显示按字节计的下载进度。完成后用键盘触发 Play，游戏从新下载的内容挂载 DLC 并进入 `NewMap`。

鼠标对照测试记录了 Cocoa 和 Slate 两层事件。Computer Use 发送了正确的窗口点击坐标，系统光标停在窗口外。UE 的 Mac 输入层按系统光标做命中检测，Slate 没有命中 Retry。将诊断窗口移到系统光标下后，用同一工具点击 Retry 和 Play，两个按钮都收到了按下和抬起。游戏挂载新下载的 DLC 并进入 `NewMap`，容器的 SHA-256 与发布文件一致。日志位于 `Saved/MacLauncherQA/mouse-routing.7toyyf/`。测试后恢复了原启动器，并关闭启动器、游戏和测试 CDN。

安装保护回归覆盖包内可执行文件、符号链接和包内尚未创建的安装路径，均在写入前退出。缺少 CDN 版本和游戏可执行文件时，`-autoplay` 返回 1。重链接的 Development QA 游戏在写锁占用时拒绝挂载。通过启动器进入 `NewMap` 后，从同一内容目录的符号链接执行 `update`、`repair` 和 `compact` 均返回 `UGP-9001`，索引、事务状态和容器哈希一致。游戏退出后更新成功。日志位于 `Saved/MacLauncherQA/review-fixes.U9WfVx/`。

安装树回归使用临时目录和 CLI。`update`、`repair` 和 `compact` 拒绝进入树内链接。父目录更新拒绝覆盖子安装，子目录的内容、索引和已持有的锁文件保持一致。安装根的符号链接可以解析到同一真实目录。还验证了已有父安装、尚未创建元数据时的父子写锁、清单中的保留路径和事务日志中的包外路径。日志位于 `Saved/MacLauncherQA/tree-guards.LK33uS/`。
