# UGP 自动化调试环境（MCP 驱动）

> 用 MCP 调试工具（断点/单步/栈帧/求值）验证 UnrealGamePatcher 的运行时行为。
> 搭建于 2026-10-10；断点行号以提交 acc7588 的源码为基准。
>
> **主通路：CodingWithCalvin/VS-MCPServer 扩展**（用户选定）——VS 扩展内嵌 HTTP MCP
> server（localhost:5050，77 工具），**evaluate/callstack/locals 全部可用**（扩展 in-proc
> 实现，外部 DTE COM 拿不到）。见下文「CW 扩展通路」。
> **备胎 1：自研 DTE 桥**（Tools/VSDebug/vs_mcp_bridge.py）——扩展失效时兜底；断点/F5/
> step/continue 已验证（evaluate 在 VS 18 DTE 下不可用）。见下文「自研 DTE 桥」。
> **备胎 2：debugmcp + codelldb**——不依赖 VS 的裸 DAP 调试。见文末。
>
> **事实澄清**：VS 2026 官方（微软自带）没有"IDE 操作 MCP server"——内置的只有 NuGet MCP
> 与 Binlog MCP；上面主/备通路全是第三方。社区对比（2026-10-10）：CodingWithCalvin
> 80★/11 contributors/活跃 + debugger 面完整 → 选为主；dhq-boiler/Unofficial-VS-MCP
> 115 工具但 1 contributor + beta + 100% AI 生成；zerodensity/vsdbgmcp C++ 专精但
> 1 contributor。微软内置 NuGet/Binlog MCP 与调试无关。

## CW 扩展通路（主通路，2026-10-10 实测 PASS）

- **安装**：`CodingWithCalvin.MCPServer.vsix`（GitHub Releases，v2026.10.5.94，48MB；
  本地存档 `Saved/UGPDebug/cw-mcpserver.vsix`）→ `VSIXInstaller.exe /quiet` 安装。
  Marketplace 搜 "MCP Server" 同源。支持 VS 2022/2026。
- **启动**：VS 里 Tools > MCP Server > Start Server（命令 `MCPServer.MCPServer.StartServer`，
  可用 DTE 桥的 `vs_execute_command` 执行）；设置页可开 auto-start。
  **HTTP 传输：`http://localhost:5050`**（扩展随 VS 生命周期，VS 必须开着）。
- **ZCode 注册**：`.zcode/config.json` 的 `vs` server = `{"type":"http","url":"http://localhost:5050"}`。
  **重启 ZCode 会话生效**。
- **UGP 实测**（`python Tools/VSDebug/cw_verify.py`，先 `Tools/UGPDebug/prepare_scenario.bat`）：
  launch → 断点命中 `UpdateWithOptions:449`（`CurrentProcessName=UnrealGamePatcher.exe`）→
  **callstack**（完整 JSON 栈：函数/文件/行/模块）→ **locals**（Name/Value/Type/IsValidValue）→
  **evaluate**（`Version`/`InstallDir` IsValidValue=true）→ step_over → continue 穿过全部
  停点（CLI 调两次 UpdateWithOptions：check-only + apply）→ Design → 产物断言全过。
- **已知问题（实测）**：`solution_open` 对"已在 VS 中打开的同路径 sln"报错（先关闭或忽略）；
  `debugger_add_breakpoint` 在 VS 已有同文件同行断点时报错（VS 残留断点仍会命中，忽略）；
  evaluate 的 FString Value 显示 `{...}`——**DTE/CW 求值通道不渲染 natvis**（agent 限制）；
  项目级 natvis 已装（人用 VS IDE Watch 可读），agent 要值走场景日志或 codelldb 通道。
  **natvis 全局化（2026-10-11）**：引擎 `Unreal.natvis` 另装一份到用户级
  `Documents\Visual Studio 18\Visualizers\`——VS 对**所有项目**的调试会话自动加载，
  不只 UGP（UE 升级不会覆盖此副本；引擎 natvis 有更新时需手动同步）。
- **工具面速查**：`solution_*` / `build_*`（含 build_diagnostics）/ `document_*` / `editor_*` /
  `find_references` / `goto_definition` / `debugger_*`（17 个，断点支持条件+命中次数规则）/
  `terminal_run`（⚠️ 本机命令执行）/ 输出窗 / 错误列表。完整清单 `tools/list`（77 个）。

## 自研 DTE 桥（备胎 1：Tools/VSDebug/vs_mcp_bridge.py）

**背景**：2026-10-10 彻查（VS 安装全盘 / learn 文档 TOC / 官方 MCP catalog / microsoft org
GitHub / Learn 搜索 API）确认 **VS 2026 官方没有"暴露 IDE 操作给外部 agent"的 MCP server**——
内置的只有 NuGet MCP 与 Binlog MCP（VS-as-MCP-client 的 Copilot 不算）。自建桥：
`Tools/VSDebug/vs_mcp_bridge.py`（stdio MCP server）通过 **DTE/COM（EnvDTE，
ProgID `VisualStudio.DTE.18.0`）** 驱动真实 VS。DTE 是 VS 官方自动化模型，debug 走的就是
VS 调试器本体（断点/F5/命中帧与 IDE 内完全一致）。

**组成**：
- `Tools/VSDebug/UGPDebugOnly.sln` —— mini solution（只含 UnrealGamePatcher.vcxproj，
  避免打开几百个项目的 UE 主 sln）；配置 Development|x64 / Debug|x64 / DebugGame|x64；
- `Intermediate/ProjectFiles/UnrealGamePatcher.vcxproj.user` —— Development 配置的调试
  参数（file:// CDN 全新安装场景；**UE 重新生成工程文件不会删 .user**，备份 `.bak` 同目录）；
- `Tools/VSDebug/vs_mcp_bridge.py` —— MCP 桥（15 个工具，见 `tools/list`）；
- ZCode 注册：`.zcode/config.json` 的 `vs` server（`python Tools/VSDebug/vs_mcp_bridge.py serve`）。

**工具面**（stdio MCP，`tools/list` 可枚举）：`vs_status` / `vs_launch` / `vs_open_solution` /
`vs_set_breakpoint(file,line,condition?)` / `vs_remove_breakpoint` / `vs_list_breakpoints` /
`vs_start_debug`（F5，异步 + 轮询至 Run/Break）/ `vs_debug_state`（模式+命中帧+栈顶）/ `vs_continue` /
`vs_step(kind)` / `vs_stop_debug` / `vs_evaluate` / `vs_build`（SolutionBuild 同步构建 = 走 UBT）/
`vs_output_pane` / `vs_execute_command`（DTE ExecuteCommand 万能兜底）/ `vs_set_f5_never_build` /
`vs_quit`。

**验证**：`python Tools/VSDebug/vs_verify.py`（先跑 `Tools/UGPDebug/prepare_scenario.bat`）。
2026-10-10 实测 **PASS**：launch 4.1s 就绪 → 双断点绑定 → F5（免构建）2.2s →
**Break at `FUpdateOrchestrator::UpdateWithOptions:449`** → continue →
**Break at `RebuildFileFromChunks:257`** → 跑完 Design → 产物断言全过。

**无人值守的五个关键设计**（踩坑实录，全部实测）：
1. **模态框自动扫杀**：VS 会弹 browse/IntelliSense 数据库提示框，全部 COM 调用会被模态框
   阻塞（表现 = 调用永久挂起）。桥在每次 COM 取用与 launch 轮询中 `dismiss_modal_dialogs()`
   ——枚举 devenv 的可见 `#32770`，有 OK 按钮才自动勾选"Do not prompt"+点 OK，
   **没有 OK 按钮的对话框绝不碰**（需要真决策）。
2. **F5 必须免构建**：NMake 项目的 F5 构建链在 VS 宿主内**稳定挂死**
   （MSBuild 空转无子进程，清 .vs 缓存无效）。`vs_set_f5_never_build` 把
   "On Run, when projects are out of date" 设为 Never build（DTE Properties，
   实测生效：F5 0.4s 直接启动）。UGP 构建本来就该走 Build.bat/UBT。
3. **F5 启动哪个 exe 由 ActiveConfiguration 决定，.suo 会记住旧值**——实测 suo 记住
   Debug 配置 → 启动 9 月老 exe + 旧参数（UGP 秒退、无 debuggee）。`vs_start_debug`
   先 `SolutionConfigurations.Item("Development").Activate()`。
4. **.user 文件 VS 运行时会被重写**（ToolsVersion 改 "Current"、手写配置被覆盖）——
   改 .user 必须**在 VS 关闭时**做；Development 配置的调试三元组
   （LocalDebuggerCommand/Arguments/WorkingDirectory + DebuggerFlavor）已写入
   （备份 `.bak` 同目录）。UE 重新生成工程文件不会删 .user。
5. **无人值守轮询**：`vs_start_debug`/`vs_continue` 都是 Go(False)/异步 + 短间隔
   轮询 CurrentMode（见下条坑），不做阻塞调用。

**VS 2026 DTE 的实测限制/坑**（debug 能力现状）：
- **Debugger 接口没有 `Continue`**：GetIDsOfNames 实证（Go=6, Break=7, Stop=8,
  SetNextStatement=9，无 Continue），`ExecuteCommand("Debug.Continue")` 也报
  "not valid"。**Break 态下 `Go(False)` 就是继续**（F5 语义）。同理 Step/Stop 走
  `ExecuteCommand("Debug.StepOver/..."/"Debug.StopDebugging")` 命令系统更稳。
- **Breakpoint.CurrentHits 恒 0**（bound=0 的 pending 断点）——命中计数不可用，
  新停点判定用 mode 序列（Run→Break）+ lastHit 行变化 + 超时重发 Go 兜底
  （CLI 会调 UpdateWithOptions 两次：check-only + apply，同一行合法停两次）。
- **`vs_evaluate`（GetExpression）间歇被拒**（"Call was rejected by callee"，
  MTA/STA 无关、等待 60s 无效）——VS 18 的 DTE 求值通道不可靠。**表达式求值请走
  debugmcp 通路**（codelldb `?expr`），VS 桥负责 launch/构建/断点/F5/流程控制。
- **pywin32 gen_py 陈旧缓存**（"Version 7.0 Object Model" = VS 2003 typelib）会让
  `Debugger` 缺方法（无 Continue 的 AttributeError）——删
  `%LOCALAPPDATA%\Temp\gen_py\<pyver>\` 重新生成。
- **COM 走 MTA**：`CoInitializeEx(COINIT_MULTITHREADED)`——STA 消息泵是
  "rejected by callee" 间歇拒绝的放大器。
- 多实例时 `GetActiveObject` 拿到哪个不确定——保持单实例约定（vs_launch 已防堆叠）。

## debugmcp 通路（headless 备胎）

| 组件 | 版本 | 角色 |
|------|------|------|
| `debugmcp`（microsoft/DebugMCP 独立 CLI） | 1.0.0（npm 全局） | MCP stdio server：把 DAP 调试暴露为 MCP 工具 |
| `codelldb`（vadimcn/codelldb） | v1.12.3 | DAP 适配器：LLDB 引擎读 MSVC PDB，免 VS Code 运行 |
| VS 2026 内置 NuGet MCP | VS 18 Community | `NuGet.Mcp.Server.exe`（包管理，与调试无关） |
| ZCode workspace 配置 | `.zcode/config.json` | 注册 `debugmcp`（stdio）+ `vs-nuget`（本机 NuGet.Mcp.Server.exe 绝对路径） |

**为什么不是 "VS2026 自带调试 MCP"**：实测本机 VS 18 Community 没有内置调试 MCP server
（仅 NuGet MCP + Binlog MCP，Copilot 只作为 MCP 客户端；Learn 文档亦只有 VS-as-client）。
DebugMCP CLI 是微软官方（microsoft/DebugMCP）的免 IDE 调试 MCP。**新 MCP server 在
ZCode 里要重启会话才连接**（会话启动时建连）。

## 一次性安装记录（本机已完成）

```bat
npm install --global debugmcp
REM codelldb v1.12.3 VSIX（50MB，GitHub 直连慢，走 gh-proxy 镜像）解压到：
REM   Saved\UGPDebug\codelldb\   （extension\adapter\codelldb.exe + extension\lldb\bin\liblldb.dll）
REM 注册（用户级，%APPDATA%\debugmcp\config.json；路径写死本机，换机器需重跑）：
debugmcp adapter add cpp --user --command "D:/UEProject/IOStoreDemo/Saved/UGPDebug/codelldb/extension/adapter/codelldb.exe" --args --liblldb "D:/UEProject/IOStoreDemo/Saved/UGPDebug/codelldb/extension/lldb/bin/liblldb.dll" --launch "{type:codelldb, request:launch, stopOnEntry:true, program:<exe>, args:[update 场景], cwd:<repo>}"
debugmcp adapter validate cpp   REM DAP 握手验证
```

`--launch` 里写死了**全新安装场景**：`UnrealGamePatcher.exe -cli update
-cdn=file:///D:/UEProject/IOStoreDemo/Saved/UGPDebug/cdn -install=.../install`。
换场景 = 重新 `adapter add` 改 args（或以后加第二套注册，用 configurationName 区分）。

## 使用（AI 驱动的 MCP 工具流）

```bat
:: 0. 重置场景（重建 file:// CDN v1 + 删 install 目录）
Tools\UGPDebug\prepare_scenario.bat
```

MCP 工具调用顺序（会话重启后 `debugmcp` 工具可用）：

1. `add_breakpoint(fileFullPath, line)` —— 断点预设见 `Tools/UGPDebug/breakpoints.json`；
2. `start_debugging(fileFullPath=<任一 .cpp 源文件>, workingDirectory=<repo>, configurationName="cpp")`
   —— 返回时已**停在入口**（stopOnEntry），符号加载完毕、断点重绑完成；
3. 循环：`get_debug_status`（可 `waitForPauseSeconds` 阻塞等停）→ 命中时读
   `state.frameName / currentLine / stackTrace` → `get_variables_values` /
   `evaluate_expression` → `continue_execution`；
4. 结束后核对产物（`breakpoints.json` 的 assert 段）。

**回归校验一条命令**（自写 MCP stdio 客户端驱动全程 + 断言）：

```bat
python Tools\UGPDebug\verify_mcp_debug.py
```

2026-10-10 实测 PASS：入口停 → `UpdateWithOptions:449` 命中（CdnDir/Version/InstallDir
可见、`?Version` 求值 OK）→ `RebuildFileFromChunks:256` 命中 ×3（每个 added 文件一次）
→ 跑完退出 → 3 产物 + state.json + .ugp/index.json 全过。

## 当前断点预设（行号会随源码漂移，改完代码要同步 breakpoints.json）

| 文件 | 行 | 命中点 |
|------|----|--------|
| `FUpdateOrchestrator.cpp` | 449 | `UpdateWithOptions` 体首（CLI update 主入口） |
| `FUpdateOrchestrator.cpp` | 256 | `RebuildFileFromChunks` 体首（整文件重建，每 added 文件一次） |

断点扩展速查（函数**体行**才可绑，签名行不行）：
`FInPlacePakPatcher.cpp:167 签名 → 体首约 :170+`；`FPakRecovery` / `FDownloadExecutor`
同理。**被完全内联的函数不要用**——符号存在但断点永不命中（实测 `FPakWriter::Commit`
在 Development /O2 下被内联，独立函数体无调用）。

## 已踩坑（实测）

| 坑 | 事实 | 处置 |
|----|------|------|
| 符号加载竞态 | PDB 300MB，异步加载；断点在加载完成前解析为 0 locations，进程可能已跑过目标行 | 适配器 launch 配置 `stopOnEntry: true`——入口先停，符号就位后再放行 |
| 内联函数断点不命中 | `FPakWriter::Commit`（/O2 全内联）断点 1 location 但永不停 | 选非内联函数/调用者行 |
| 行断点必须用"体行" | 函数签名行（如 `:428`）无行表条目，解析 0 locations | 用函数体第一行（`{` 或其下一条语句） |
| lldb CLI 全路径断点失败 | `breakpoint set -f <绝对路径>` 0 locations；basename 正常 | 走 codelldb 时无此问题（它自己做路径匹配）；手工 lldb 调试用 basename |
| codelldb 求值要 `?` 前缀 | REPL 处于 commands 模式，`Version` 被当 LLDB 命令报错 | `evaluate_expression` 传 `?Version`；`?Version.Num()` 方法调用会 Syntax error，只支持变量路径 |
| UE 日志混入适配器流 | codelldb 把 debuggee stdout 混进 DAP 字节流，直连 DAP 的客户端要做容错 | `dap_probe.py` 已做行级容错 |
| 停止判定要看栈顶 | `state.stackTrace` 含调用者帧，按 trace 匹配会张冠李戴 | 用 `state.frameName`（栈顶）匹配 |

## 直连诊断工具（绕过 MCP）

- `Tools/UGPDebug/dap_probe.py [line]` —— 裸 DAP 客户端直连 codelldb：验证适配器/断点/
  命中（MCP 层怀疑出问题时分层定位）；
- `Tools/UGPDebug/mcp_probe.py` —— 列 debugmcp 工具清单与 schema（API 变了先跑它）；
- codelldb 自带 lldb：`Saved/UGPDebug/codelldb/extension/lldb/bin/lldb.exe -b -o "target
  create <exe>" -o "breakpoint set -f <basename> -l <体行>" -o "process launch -- <args>"`。

## 相关文档

- `Docs/DEBUG.md` —— 手工排障手册（日志路径/常见坑）
- `Docs/TESTCASES.md` —— 测试套件刻度尺（FIOStats 刻度：断点命中处的 disk write /
  chunk loads 可与套件断言互相印证）
- review `review-2026-10-09-0206.md` —— 断点选点参考（§三 开放项对应的函数）
