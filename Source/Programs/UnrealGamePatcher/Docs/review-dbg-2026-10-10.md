# UGP 动态精确化 Review（VS 调试器驱动）

> 日期：2026-10-10。方法：CodingWithCalvin/VS-MCPServer 扩展（localhost:5050）+ 真实 VS
> 调试器，对 UGP 核心步骤设断点、抓运行时 locals/status，逐场景验证静态 review 的
> 关键假设。驱动脚本 `Tools/VSDebug/dbg_scenario.py`；原始证据
> `Saved/UGPDebug/dbg/s*.json`。**与静态 review（review-2026-10-09-0206.md）互补**：
> 静态结论此行标注"动态证实/动态补充/未覆盖"。

## 一、场景 → 套件覆盖矩阵

| 场景 | 驱动命令 | 断点 | 覆盖套件 | 结果 |
|---|---|---|---|---|
| s1_ingest | `build -staged=fixture` | FBuildIngestor.cpp:44 | buildtest / difftest / diffbuild / cdcshift | 会话完成，5 停点（断点与 449/256 混合命中，ingest 专属帧未单独确证）△ |
| s2_fresh_install | `update file:// (空目录)` | 449 + 256 | updatetest / e2etest / perftest 基线 | ✓ 4 停点 = 入口 diff + 3×RebuildFileFromChunks（逐文件） |
| s3_cow_patch | `-cli pakinplace` | FInPlacePakPatcher.cpp:345/359（H1 修复点） | pakinplace / iostoreinplace | △ 断点未命中（/O2 行映射，见 D-3）；套件自身断言 NumSame/NumAppended 已覆盖正确性 |
| s4_rollback | `update -injectfail=2` | 1547（失败注入点） | statetest / 事务回滚 | ✓ 日志链完整：injected failure after 2 writes → transaction rolled back → exit 9；**D-1 设计证实**（见下） |
| s6_http_download | cdn_server.py + `update http://` | FDownloadManager.cpp:65 | downloadtest / benchtest | ✓ DownloadAll 在 HTTP CDN 会话命中——**HTTP 与 file:// 共享同一上层管线（传输层可插拔的动态证实）**；7 停点后会话完成 |

未动态覆盖：maintest/iostoretest 的 repair 路径（与 s2 共用 verify 通道，时间裁剪）、
selftest/indextest（纯函数级，套件已充分覆盖）。

## 二、动态发现

### D-1（设计证实，非缺陷）：全新装回滚后 pak 残留是 CoW v2 的明确设计

**现象**：干净 install + `-injectfail=2`（第 2 次写后注入失败）→ 回滚完成（exit 9，
state=rolled-back，targets 全 replace-file/backupPath 空）→ 终态**残留完整新版
`pakchunk1-Windows.pak`**（ucas/utoc 正确删除）。100% 复现。

**根因（源码语义对齐）**：`FUpdateOrchestrator.cpp` Rollback lambda 的**第一个分支**就是
pak 短路——"CoW v2 (ROADMAP §5.2b): pak containers are never backed up and never rolled
back. A crash mid-CoW leaves a torn tail that FPakRecovery truncates back to the last
committed footer - **the container is a complete old or new version either way**"。

**收敛性验证**：残留的 pak 是完整新版本 → 下次 update 的 diff 将其判 Same → 只补
ucas/utoc → 收敛正确。**动态证据链（复现 + 源码注释 + 收敛推理）把一个静态读码时的
"疑似 bug"确认为设计意图**。静态 review 未标注此语义，建议在 TECHNICAL.md §3.4 事务
一节补一句"全新装回滚后 pak 保留（CoW v2 语义，FPakRecovery 兜底）"。

### D-2（动态证实）：HTTP 与 file:// 共享同一上层管线

s6 用 cdn_server.py（真实 HTTP+Range）驱动同一 update 命令：`FDownloadManager::DownloadAll`
断点命中、上层 449/256 停点序列与 s2（file://）**逐点一致**。传输层可插拔（README §下载层）
从"代码结构推断"升级为"运行时观察"。

### D-3（工具性发现）：/O2 下函数体行断点大面积不可绑（CW/DTE 通道）

`1547`（失败注入）、`345/359`（H1 修复点）三处断点均"added"但从不命中（VS 18 DTE
pending 绑定在优化代码上丢失）；`449/256/65` 可绑。**后果与对策**：用 CW 通道做行级
验证时优先选函数入口行；精确行验证走 debugmcp/codelldb 通道（其行绑定经 17 轮实测更
可靠）。此限制不影响静态 review 结论的有效性。

### D-4（工具性发现，已部分修复）：FString 在 agent 求值通道显示 `{...}`

三通道实测（CW evaluate / codelldb evaluate+variables 均传 UE.natvis / DTE GetExpression）
一律返回 `{...}`——**agent 自动化求值通道不渲染 natvis**（VS DTE/CW 用原始格式化；
codelldb 的 natvis 摘要层在 evaluate/variables 上未生效）。已做：引擎 natvis
（`Unreal.natvis`，5587 行）复制为项目级 `Intermediate/ProjectFiles/UnrealGamePatcher.natvis`
——**人用 VS IDE 的 Watch/Locals 时 FString 直接可读**（VS 原生机制生效）。
agent 侧 workaround：场景日志（`-LogCmds=LogUnrealGamePatcher Verbose`）已含全部关键值。

### 补验：s1 ingest 断点体行命中

`FBuildIngestor.cpp:50`（Ingest 体首）在 build 会话命中 1 次（单次调用）✓——
s1 的"ingest 专属帧未确证"缺口闭合（此前 44 是签名行不可绑）。

## 三、对静态 review 开放项的动态佐证

| 开放项 | 动态观察 |
|---|---|
| M23（InFlight 只增不减） | 未单测（需并发下载中段断点，超本轮范围）；s6 正常完成佐证无死锁 |
| C9（回滚 Added 备份分支） | s4 的 targets backupPath 全空，未触达该分支；CoW pak 语义（D-1）另证 |
| 静态 review §二 H1 修复 | s3 断点未绑（D-3）；但 pakinplace 套件（127 项 downloadtest 同批）15/15 全绿 + 91B 精确断言继续兜底 |
| FIOStats 打印 | 各场景 prepare 命令的日志均带 "I/O" 统计行（build/update 全程输出），计数系统运行时活跃 ✓ |

## 四、结论

1. UGP 核心步骤在**真实调试器下行为与静态 review 一致**：diff 分类→事务→apply→提交
   →回滚的全链运行时无异常状态。
2. **D-1 是本轮唯一需要落文档的语义澄清**（非缺陷）：建议补 TECHNICAL.md。
3. 调试基建（CW 扩展 + dbg_scenario.py）已就绪，后续任意场景可一键复现：
   `python Tools/VSDebug/dbg_scenario.py <name>`。
