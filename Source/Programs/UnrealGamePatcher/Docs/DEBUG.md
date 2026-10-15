# UnrealGamePatcher 调试手册

> 面向手工验证 / 排障。自动化测试见 `TESTCASES.md`；架构见 `TECHNICAL.md` / `DOWNLOAD.md`；
> **MCP 驱动的断点调试环境（debugmcp + codelldb）见 `DEBUGGER.md`**。
> 所有命令在仓库根目录（`D:\UEProject\IOStoreDemo`）执行，**均为单行**——cmd /
> PowerShell / Git Bash 直接整段粘贴即可（本文不使用续行符；如需折行，cmd 用 `^`、
> PowerShell 用反引号 `` ` `` 且行尾不能有空格）。示例 CDN = ElectricDreamsEnv
> 真·HTTP（rustfs S3：CDN 目录 7.3GB = v1+v2 双版本全部 chunk 对象；安装体积 7.78GB 见 §1）。
> **代理无需处理**：程序入口自带 `NO_PROXY=127.0.0.1,localhost`（loopback 直连，远程 CDN 走代理不受影响）。

## 0. 起环境（仅首次 / rustfs 未运行时）

```bat
:: 1. 启动 rustfs S3 服务器（:9000，数据在 <project>\Saved\rustfs-data）
Source\Programs\UnrealGamePatcher\Scripts\start_rustfs.bat

:: 2. 发布 CDN（已发布过可跳过；重跑默认续传——HEAD+MD5 跳过已传对象，--full 强制全传；
::    连接类故障指数退避重试，单对象重试耗尽记失败继续，连续 10 失败才中止）
python Source\Programs\UnrealGamePatcher\Scripts\s3_sync.py --endpoint http://127.0.0.1:9000 --access-key ugpadmin --secret-key ugpadmin123 --bucket ugp-eds --create --public --sync D:\UEProject\ElectricDreamsEnv\Saved\UGP_Cloud
```

> 2026-10-14 排障注记：全量传输中断报 `WinError 10060`（连接超时）——rustfs 一直在运行、
> 未重启，根因是它在同盘大流量 PUT 下 accept 停顿超过了客户端 120s 超时；旧脚本无重试无续传，
> 一次瞬时超时即整体中止。现脚本已加重试+续传，直接重跑即从断点继续（实测 10011/10011 全部
> MD5 校验一致，0 重传）。

CDN 构建源（两次 cook 差异 = 真实小更新）：`RunUAT BuildCookRun -build -cook -stage -pak` →
`UnrealGamePatcher.exe -cli build -staged=<staged> -app=ElectricDreamsEnv -version=<v> -out=<cloud>`。

## 1. 新玩家全新下载（7.78GB / 10006 chunks / 真实 HTTP + Range；update 路径默认窗口 8）

```bat
Binaries\Win64\UnrealGamePatcher.exe -cli update -cdn="http://127.0.0.1:9000/ugp-eds" -install="D:\UEProject\ElectricDreamsEnv\Saved\UGP_Test\player5" -LogCmds="LogUnrealGamePatcher Verbose"
```

- 装完可直接启动 `player5\ElectricDreamsSample.exe` 验证引擎加载。
- 日志：`LogUnrealGamePatcher Verbose` = diff/事务/stage 进度；`LogUnrealGamePatcherDownload`
  = 原语层 settle 日志（Verbose；执行器本身只有 Warning 级重试/验证失败日志）。
  落盘日志实测在 **`D:\UEProject\Engine\Programs\UnrealGamePatcher\Saved\Logs\UnrealGamePatcher.log`**
  （Program 的 ProjectDir 解析到引擎侧 Program 目录，非 `<引擎根>\Engine\...` 直推；
  2026-10-09 实测验证；`-StdOut` 直出控制台）。

## 2. 复跑 = no-op（信任模式零磁盘内容读取）

```bat
Binaries\Win64\UnrealGamePatcher.exe -cli update -cdn="http://127.0.0.1:9000/ugp-eds" -install="D:\UEProject\ElectricDreamsEnv\Saved\UGP_Test\player5"
```
预期 `files 61 same / 0 changed / 0 added / 0 removed; 0 chunks fetched`。

## 3. 老玩家版本更新（v1 → v2 增量）

```bat
:: 先锁 v1
Binaries\Win64\UnrealGamePatcher.exe -cli update -cdn="http://127.0.0.1:9000/ugp-eds" -install="D:\UEProject\ElectricDreamsEnv\Saved\UGP_Test\player6" -version=1.0.0
:: 升 latest（v2）——真实差异只有 2 chunks / ~36KB
Binaries\Win64\UnrealGamePatcher.exe -cli update -cdn="http://127.0.0.1:9000/ugp-eds" -install="D:\UEProject\ElectricDreamsEnv\Saved\UGP_Test\player6" -LogCmds="LogUnrealGamePatcher Verbose"
```

## 4. 版本修复（同尺寸损坏只有验证模式能抓）

```bat
:: 损坏一个 exe（中间字节翻转，尺寸不变）
python -c "import os; p=r'D:\UEProject\ElectricDreamsEnv\Saved\UGP_Test\player5\ElectricDreamsSample.exe'; f=open(p,'r+b'); f.seek(os.path.getsize(p)//2); f.write(b'\xFF'); f.close()"

:: check 强制验证模式——同尺寸损坏【会报】（repair 才修）；不报的是 update 主流程的信任模式
Binaries\Win64\UnrealGamePatcher.exe -cli check -cdn="http://127.0.0.1:9000/ugp-eds" -version=2.0.0 -install="D:\UEProject\ElectricDreamsEnv\Saved\UGP_Test\player5"

:: repair（强制验证模式：逐条目重算 hash + 逐 chunk 对照 utoc meta hash）
Binaries\Win64\UnrealGamePatcher.exe -cli repair -cdn="http://127.0.0.1:9000/ugp-eds" -version=2.0.0 -install="D:\UEProject\ElectricDreamsEnv\Saved\UGP_Test\player5"
:: 复跑应显示 "0 files repaired, 61 intact"（收敛）
```

## 5. 只读报告（不写盘）

```bat
Binaries\Win64\UnrealGamePatcher.exe -cli check -cdn="http://127.0.0.1:9000/ugp-eds" -version=2.0.0 -install="D:\UEProject\ElectricDreamsEnv\Saved\UGP_Test\player5"
```

## 6. 下载层专项（自测 + 基准，无需 rustfs）

```bat
Binaries\Win64\UnrealGamePatcher.exe -cli downloadtest   :: 127 项：range/续传（含自然 journal）/并发/重试/上限/探测回退/自定义后端
Binaries\Win64\UnrealGamePatcher.exe -cli benchtest      :: 吞吐（限速下窗口 vs 单连接）+ 内存峰值
```

## 7. 容器体检

```bat
:: pak 逐条目校验（含压缩块解压，Oodle 多块 entry）
Binaries\Win64\UnrealGamePatcher.exe -cli pakdump -pak="<install>\ElectricDreamsEnv\Content\Paks\ElectricDreamsEnv-Windows.pak"
:: 预期 "1805/1805 entries verified"

:: utoc/ucas 结构 dump
Binaries\Win64\UnrealGamePatcher.exe -cli iostoredump -utoc=<f.utoc> -ucas=<f.ucas>
```

## 8. 安装目录内部结构（排障用）

```
<install>\
├── UnrealGamePatcher.state.json     事务状态（committed/rolledback/pending）+ holes + lastVerifiedAt
├── .ugp\
│   ├── index.json                   本地 index（上次提交的远端 manifest 副本 = 信任模式真值 + chunk 复用状态）
│   ├── backup\                      事务回滚备份（committed 后清空）
│   └── chunkcache\                  网络模式的临时 chunk 缓存（提交后删除）
```

- `state.json` 损坏 → 信任 update 无 pending 恢复（自愈继续）；`check`/`compact` 会显式报错。
- `.ugp/index.json` 丢失 → update 自动重建（信任模式降级为全量本地哈希扫描一次，零网络）。

## 9. 常见坑

| 症状 | 原因 | 处置 |
|---|---|---|
| CDN 请求 HTTP 502 | 系统代理劫持 loopback（旧版本程序） | 升级到带 `NO_PROXY` 豁免的版本；或代理工具加 `127.0.0.1` bypass |
| 多段文件内容清零 | 旧版段写入截断 bug | 已修复（`FILEWRITE_Append`）；复现于旧构建 |
| repair 永不收敛 | 旧版 fpak 压缩块 32 对齐误读 | 已修复（引擎对齐=1）；升级构建后 repair 一次即收敛 |
| update 挂起无输出 | 旧版回调丢 OnDone | 已修复；确认 exe 为最新构建 |
| CDN index 报 `FAILED self-check (embedded X, recomputed Y)` 且两次重跑 recomputed 恒定 | 2026-09-02 前的发布器用 pretty-print 写 index，selfCheck 绑定旧字节布局；新代码紧凑重序列化必然不同 = 假阳性（2026-10-15 实战定位，CDN 对象 MD5 与发布源一致） | 已修复：Load 在 canonical 重算不匹配时回退 raw-body 校验（`JsonIo.h` `MatchesRawBodySelfCheck`）。若升级后仍报错才是真损坏/真篡改 |
