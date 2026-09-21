# UnrealGamePatcher

针对 UE5 `pak` / `ucas` 的**定制化增量更新器**（独立 Program）。

传统 Steam/Wegame 类平台对 1 GB 的 pak 只更新 1 字节也要整体重写；本项目**客户端对比 pak 条目 / ucas chunk → 按 range 下载差异字节 → 原地写入**：
- **pak**：index 携带每个 pak 条目的元数据（路径/偏移/存储大小/内置 SHA1）；客户端用本地 pak 的内置哈希逐条对比，
  未变条目**零搬动**，变化条目**重追加**（优先回填由已提交 index 补集重算出的空洞），目录/footer 随新 payload 在尾部重写（512 扇区对齐）——只写差异字节，绝不整写文件。
- **ucas**：chunk id（内容 hash）判据，变化 chunk 追加 + utoc 整体重建（原子替换），未变 chunk 零搬动。
- 整文件重建路径（临时文件 + 原子替换）仍作为**回退**与普通文件更新方式存在，两者共用同一事务/回滚框架。

> 重要：**本工程不包含任何 HTTP 服务器实现**。CDN 服务器用 Python 提供（`Scripts/cdn_server.py`，
> 仅用标准库，带 Range 支持 + 运行时故障注入 `__ctl/fail_next`、`__ctl/truncate_next`、`__ctl/stats`），
> 测试通过拉起外部 CDN 服务器打真实网络。
> **不需要 Node 版**：客户端只走 HTTP，服务器实现语言对其不可见；Python 标准库已覆盖 Range/并发/故障注入/可观测，
> 且本机 Python 必然可用，Node 只会多引入一个运行时依赖而无任何功能增益。

- 输入 = **标准 UAT 输出**（staged build：pak/ucas/utoc 等打包产物），**不依赖 BuildPatchServices / 官方 manifest /
  BPT chunk 文件**——`FBuildIngestor` 直接用 FastCDC 内容定义切分（Gear + 归一化，256KB/1MB/4MB 三档）出自研
  chunk（内容寻址 SHA1，Oodle/raw 压缩），并导出**自研扩展清单** `UnrealGamePatcher.index.json`（XXH3-64 自校验，防篡改）。
- 自研格式层：pak 读写（FPakReader/FPakWriter）、自研 chunk 格式（FChunkDataLoader）、IoStore utoc/ucas 读写。
- 可插拔下载层：`IChunkDownloader` 抽象 + 默认 HTTP 后端——传输原语 `FHttpRangeRequest`
  （Range v2 语义 / 传输上限 / probe）+ 窗口执行器 `FDownloadExecutor`（journal 断点续传 /
  有界重试 / SHA1 终验）；`FHttpChunkDownloader` 是为旧调用方保留的轻量适配器。

## 文档

- **本文件（README.md）**：概念介绍 / 用法 / CDN 布局 / 阈值配置 / 构建接入。
- **`TECHNICAL.md`**：详尽技术方案（实现版）——构建 chunk、ugp check、版本对比、生成 diff、下载差异 chunk、
  原地写入、校验、事务/回滚/崩溃恢复、格式细节速查。对照代码与测试，直接回答"具体怎么做"。
- **`ROADMAP.md`**：改进计划——Phase 0 正确性快修 / Phase 1 下载器通用化+独立模块 / Phase 2 遗留项，
  含设计决策记录（段大小定容、公平调度、上限分层、开源库取舍）。
- **`DOWNLOAD.md`**：通用下载器组件规格——传输原语语义、内存安全边界、分段模型与公平调度、
  断点续传、使用方式与设计决策（Phase 1 拆为独立模块后即其 README）。
- **`TESTCASES.md`**：全部测试套件的权威刻度尺（每套解决什么问题/怎么做/预期效果）。
- **`review-2026-10-09-0206.md`**：最新完整 code review（发现 / 当前状态 / 修复记录；整合全部历史轮次）。

## 构建

```bat
D:\Code\UE5\Engine\Build\BatchFiles\Build.bat UnrealGamePatcher Win64 Development -Project=D:\UEProject\IOStoreDemo\IOStoreDemo.uproject -WaitMutex -FromMsBuild -architecture=x64
```

## 用法（CLI）

```
UnrealGamePatcher.exe -cli <command> [options]
```

| 命令 | 说明 |
|------|------|
| `build -staged=<dir> -app=<name> -version=<v> -out=<cdn root>` | 构建侧 ingest：staged 打包输出 → 自研扩展清单 + CDN chunk 布局 |
| `diff -base=<i1> -target=<i2> -out=<dir> [-targetcdn=<cdn>] [-nocopy]` | 两版本差异与增量产物 |
| `update -cdn=<file:///...\|http(s)://...\|本地目录> -install=<dir> [-version=<v>] [-check] [-injectfail=N] [-crashafter=N]` | 客户端更新（后两个是回滚/崩溃恢复的测试注入钩子）。`-cdn` 是**唯一**数据源：`http(s)://` 全走 HTTP（latest.json + index + chunks），`file://` 或裸路径读本地目录；`-version` 缺省时解析 CDN 的 `latest.json` |
| `check -cdn= -version= -install=` | 差异 + 空洞/膨胀率报告（不写盘） |
| `compact -cdn= -version= -install=` | 重建带空洞文件，回收死重（临时文件 + 原子替换） |
| `repair -cdn= -version= -install=` | 只重建损坏（hash 不匹配）文件 |
| `pakdump` | pak 解析（逐条目校验，含压缩块解压） |
| `pakpatch -pak=<f> -out=<f> -ops=<增改描述>` | pak 增量编辑测试工具 |
| `iostoredump -utoc= -ucas=` | 转储 utoc chunk 清单 |
| `pakinplace` | pak 条目级原地写入测试（只写差异字节，不做整文件重写） |
| `iostoreinplace` | ucas chunk 级原地写入测试（只追加变化 chunk + 原子重写 utoc） |

自测命令：`selftest / statetest / indextest / difftest / buildtest / diffbuild / updatetest / maintest / downloadtest / benchtest / iostoretest / e2etest / perftest / pakinplace / iostoreinplace`（全部通过 = 退出码 0）。

GUI：不带 `-cli` 即打开 Slate 窗口（更新按钮 + 进度条 + 完成后启动游戏），
参数：`-cdn= -version= -install= [-launch=<游戏exe>] [-autoupdate]`（`-cdn` 同上，`-version` 缺省解析 latest）。


## CDN 产物布局

```
<cdn>/
  index/
    latest.json                    # 版本公告：客户端无 -version= 时解析目标版本
    1.0.0/UnrealGamePatcher.index.json   # 自研扩展清单（自校验 XXH3-64）
    1.0.1/...                      # 元数据按版本独立目录
  chunks/<h0><h1>/<h2><h3>/<rest>.chunk  # 自研 chunk 实体：FastCDC 内容定义切分，内容寻址 SHA1
                                         # 2/2/36 hex 分片（防单目录海量文件），Oodle/raw
```

- **元数据 / 实体分离**：`index/` 按版本放清单；`chunks/` 全局共享、内容寻址（键 = 未压缩窗口
  SHA1）——跨版本相同的窗口是 S3 上同一个对象，**不重复上传/存储**。
- 自研扩展清单：文件级（whole-file SHA1）+ chunk 级（**内容寻址 SHA1** = 未压缩窗口的 SHA1）清单；
  版本化（contentVersion/baseVersion）、更新策略标记、确定序列化（无时间戳）、XXH3-64 自校验（篡改/损坏即拒收）。
- 自研 chunk 文件：45B 头 `[8B "UGPCHNK1"][8B usz][8B csz][1B StoredAs 0=raw/1=Oodle][20B SHA1]`，
  键 = 未压缩窗口的 SHA1，加载时解压并校验该哈希——坏 chunk 绝不交给调用方。
- `latest.json`：`{currentVersion, minVersion?, force?, channel?}`——发布时**最后**写入（chunk +
  index 都就位后），客户端轮询它获知目标版本；是独立更新服务器的廉价替代，将来换真服务只改
  版本解析这一处。
- 客户端本地状态：`<install>/.ugp/`（index.json 内容索引 + backup 回滚备份）与
  `<install>/UnrealGamePatcher.state.json`（sidecar：空洞列表 + 事务日志）。
  网络模式下的下载缓存 `.ugp/chunkcache` 在**更新成功后即删除**（复用数据来自已安装容器，不落双份数据）。

## 事务 / 回滚 / 崩溃恢复

- 每次 apply 前先把全部变更写入 sidecar（pending），逐文件先备份原块再替换；
  任一失败 → 从备份回滚（RolledBack），验证注入失败后文件与更新前字节一致。
- 下次启动发现 pending 事务 → 先回滚恢复，再应用新版本（kill -9 模拟后 rerun 结果正确）。
- **空洞回填**：原地 patch 记录自己产生的死区（旧条目数据、旧目录/footer、被替换
  chunk 的旧块区间）到 sidecar；下一次更新的**追加优先回填**这些空洞（对齐 + best-fit，
  逻辑块序稠密不变），填不满的余量保留、整文件重建时清空；
  `check` 报告真实膨胀率（>10% 或 >512MB 提示），`compact` 手动兜底统一回收。

## 下载层

- `IChunkDownloader`：`DownloadRange(Url, Offset, Size, Out)` 可插拔后端（自定义 CDN / P2P / 加密）。
- 默认 `FHttpRangeRequest`：FHttpModule + Range 头 + `CompleteOnHttpThread`（HTTP 链路完全交给引擎）。
- `FDownloadManager`：窗口驱动执行器（回调驱动单锁状态机，零专用线程；4MB 分段 /
  update 路径默认窗口 8，executor 裸默认 24、业务可配）。**网络模式按 diff 乐观预取**——
  容器只下变化条目/chunk 覆盖的窗口（4 字节更新 ≈ 1-2 个窗口），整文件重建回退前懒补；
  传输重试（默认 3 次）、journal 位图断点续传（只补缺段，中断损失 ≤ 4MB）、
  校验通过才落盘（损坏数据绝不落盘），任一失败整体失败并报告失败 id。

## 打包管线接入（构建脚本）

`Scripts/package_update.bat`：官方 UAT BuildCookRun（build + cook + stage + pak）→ UGP `build`（staged → CDN）。
失败传播退出码（CI 可直接调用）。引擎路径写死（本机 `D:\Code\UE5`），.uproject 自动探测。

```bat
Scripts\package_update.bat -Version 1.0.1 [-CloudDir <cdn根>]
```

`Scripts/publish_update.bat`：只做"已打包输出 → CDN"（跳过 cook/stage），可选的版本 diff 增量产物。

```bat
Scripts\publish_update.bat -Staged <staged dir> -App <name> -Version <v> -CdnRoot <cdn根>
Scripts\publish_update.bat -Staged ... -App ... -Version ... -CdnRoot ... -BaseIndex <上一版本index.json> -NewIndex <本版本index.json> -DeltaOut <增量目录>
```

**把整个更新模块装进另一个 UE 工程**（一键）：`Scripts/install_update_module.bat` 把
`Source\Programs\UnrealGamePatcher\`（C++ 模块 + 全部脚本 + 文档）和 `UnrealGamePatcher.exe`
复制到目标工程。所有脚本用自身位置推导工程根（装进去即可定位新工程）；注意 `package_update.bat` / `install_update_module.bat` 的引擎路径 `ENGINE_ROOT` 是写死的，跨机器需改一行。

```bat
Scripts\install_update_module.bat -Project <目标UE工程根目录>   REM 复制预编译 exe
Scripts\install_update_module.bat -Project <目标UE工程> -Build   REM 为目标工程重新编译
Scripts\install_update_module.bat -Project <目标UE工程> -WithRustfs   REM 连带 rustfs.exe
```

## 发布到 S3 兼容存储（rustfs / MinIO / 云厂商）

CDN 是纯静态的，S3 API 是对象存储的事实标准（rustfs / MinIO / AWS S3 / 阿里 OSS / 腾讯 COS
都兼容）。**客户端只做无签名的 HTTP GET + Range**，所以桶设为公开读即可直接服务。

本地开发用 rustfs（`Scripts/rustfs.exe`，Rust 写的 MinIO 级 S3 服务端）：

```bat
Scripts\start_rustfs.bat                     # 启动 :9000（数据在 Saved\rustfs-data）
python Scripts\s3_sync.py --endpoint http://127.0.0.1:9000 --access-key ugpadmin --secret-key ugpadmin123 --bucket ugp-cdn --create --public --sync Saved\UGP_Package\CloudDir        # 建桶+公开读+上传（零依赖 SigV4）
```

客户端接入——`-cdn` 是唯一数据源，本地目录或 HTTP 基址二选一：

```bat
REM 本地目录模式（直接读 CDN 文件）
UnrealGamePatcher.exe -cli update -cdn=file:///D:/UEProject/IOStoreDemo/Saved/UGP_Package/CloudDir -install=<安装目录>
REM HTTP 模式（latest.json + index + chunks 全走 HTTP）
UnrealGamePatcher.exe -cli update -cdn=http://127.0.0.1:9000/ugp-cdn -install=<安装目录>
```

`-version=` 缺省时客户端从 CDN 的 `latest.json` 解析目标版本（显式给则锁定该版本，跳过解析）。

正式发布换云厂商 S3 兼容 + CDN 时，用 `aws s3 sync` / `mc mirror` / `rclone` 传同一目录即可
（`s3_sync.py` 的 SigV4 逻辑与它们等价），客户端只需把 `-cdn` 换成正式 URL。

## 性能参考（perftest）

以 IOStoreDemo 真实打包输出（fixture 安装体积 ~10.4 MB，9 个 chunk）为例：
- 小版本变更（单文件 readme 增量）：下载 9.4 KB（**全量的 0.1%**）、写入 20 KB（**0.2%**），
  对比全量重装的 10 MB。未变化窗口直接从本地容器复用（见 TECHNICAL.md §3.6），这是比值
  远低于早期版本（8.7%/2.4%）的原因。
- 未变化 chunk 从本地容器复用（同 hash ⇒ 同内容），零下载零搬动——机制详见 `TECHNICAL.md` §3.6。
- **刻度条件**：上述数字来自 IOStoreDemo fixture（10.4MB 级）；每次 update/compact/
  原地 patch 结束都会打印 I/O 统计（FIOStats：磁盘读写 / 网络 / 压缩解压 / chunk 加载 /
  哈希量）。**小增量必须是小数字**——出现容器级体积即发生了静默回退（见
  `review-2026-10-09-0206.md` §二 H1 案例与 downloadtest bigpak 回归）。

## 性能参考（pakinplace / iostoreinplace）

- **pak**（真实 pak 939 B，2 条目 → 改 1 条目 + 增 1 条目）：**只写 91 字节（编排器精确断言）**，
  1 条变化条目 CoW 重追加 + 1 条追加，1 条未变条目零搬动；结果与目标字节完全一致。
- **ucas**（真实 10.3 MB 容器，135 chunk → 换 1 chunk + 增 1 chunk）：**只追加 12 KB**（全量的 0.12%），
  134 个未变 chunk 零搬动；utoc 原子重写，结果按 chunk id 逻辑等价；重跑收敛为 no-op（chunk-id 判据）。

## 限制

- update 主流程已接入两条原地路径：pak 条目级（有条目元数据且数据完整时自动走）与 ucas chunk 级
  （有 IoStore 元数据且本地容器逐 chunk 数据完整时自动走）；损坏/无元数据/校验不过时自动回退整文件重建。
- ucas 判据 = **（chunk id，meta hash）多重集**：真实容器的 chunk id 由包名派生
  （引擎 `FPackageId::FromName`，内容修改不改 id），内容信号是 utoc 的 meta hash
  （chunk 原始数据的 FIoHash）。diff 与原地写的 Same 判据均使用二者——2026-09-08
  修复：此前 id-only 判据会漏掉"改既有资产"类更新。旧格式清单（无 metaHash）回退
  id-only 语义，重新 ingest 即迁移。
- `-cdn` 为 `http(s)://` 时 latest.json / index / chunks 全走 HTTP；`check`/`compact`/`repair`
  是本地维护命令，`-cdn` 用本地目录（裸路径或 `file://`）。
- 首版不含 AES/.sig 加密容器支持（IoStore encrypted / signed 容器直接报错）。
