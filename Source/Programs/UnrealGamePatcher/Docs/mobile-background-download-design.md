# UGP 移动端后台下载方案

Android 14 及以上用 UIDT 承载用户确认的版本更新，Android 10–13 用 `dataSync` 前台服务。
iOS 在 UE 5.9 的启动入口使用后台 `URLSession`。
UGP 按原有 chunk 粒度去重和校验。大型下载可以在 CDN 侧聚合成更少的对象，iOS 只使用整对象 GET 和单段 Range。

## Android 本地实现与验收

最低支持 Android 10（API 29）。`PatchActivity` 自动检查版本，有更新时显示 `Download update`。
用户确认后，Android 14 及以上把该版本提交为一个 UIDT Job，Android 10–13 启动一个 `dataSync` 前台服务。
两个入口都在 `:ugppatch` 进程内，共用同一个更新执行器。系统任务跟踪整次版本更新，
具体 chunk 和 Range 请求仍由 `libugp` 调度。两个系统入口由 `UGPTransfer.java` 管理。[UIDT](https://developer.android.com/develop/background-work/background-tasks/uidt)

UIDT 面向用户显式发起的长时间传输，适合大量内容的一次更新，并且免占普通 Job 配额。
面向 API 35 及以上的 `dataSync` 前台服务在 Android 15 及以上受每 24 小时累计 6 小时的后台预算约束。
WorkManager 的长任务在 Android 16 上仍可能消耗 Job 配额。系统 DownloadManager 以单个下载文件为单位，
接入它需要重新衔接 UGP 的分块和校验。[前台服务时限](https://developer.android.com/develop/background-work/services/fgs/timeout) · [WorkManager 长任务](https://developer.android.com/develop/background-work/background-tasks/persistent/how-to/long-running) · [DownloadManager](https://developer.android.com/reference/android/app/DownloadManager)

确认后的版本和索引会持久保存。系统停止任务时，取消信号经 JNI 传给更新执行器和 HTTP 请求。
缓存留到新目录正式发布，供重启复用。缓存文件在用于安装时校验内容，目录发布前再次检查取消状态。
断网冷启动先验证已提交的本地版本。有可用版本就进游戏，并保留已确认的下载任务。
收到 CDN 的 404 或 410 后重查 `latest`，若目标已经变更则请玩家确认新版本，保留可复用的 chunk。
下载完成后，`PatchActivity` 在前台才交接给游戏。安装目录继续由跨进程锁保护。[批次下载管理](../UnrealGamePatcherCore/Private/Download/FDownloadManager.cpp)

2026-11-15 的 Android 11 x86_64 模拟器验收覆盖前台服务启动、退到桌面后继续下载、停止后重试和发布。
重试复用了 55 个已校验的 chunk，验收截至后台发布完成。Windows 的 HTTP 取消、更新、Pak 和 IoStore 回归通过。
Windows 构建的 ARM64 APK 已通过签名及静态检查并覆盖安装到 Android 16 真机。
合入后的 5.9 QA APK 包含 UIDT、前台服务和持久恢复逻辑。Android 16 的 UIDT 通知、任务管理器停止、重启续传及挂载进图仍需真机验收。[Android 构建与设备验证](android-s1-runbook.md)

## iOS 后台传输选型

使用一个固定标识的后台 `URLSession`，启用 `sessionSendsLaunchEvents`。
iOS 可以在 App 挂起或被系统回收时继续传输，并在有会话事件时唤醒或重新拉起 App。
一个下载对象对应一个 `DownloadTask`，App 可以在一次前台操作里连续提交一批。实际并发和启动时机由系统调度。
用户从多任务界面强退会取消后台传输，重新打开后按持久缓存补缺。[Apple 后台下载指南](https://developer.apple.com/documentation/foundation/downloading-files-in-the-background) · [会话恢复与强退行为](https://developer.apple.com/documentation/foundation/urlsessionconfiguration/background%28withidentifier%3A%29)

音乐保活会把传输依赖在音频会话上，Apple 审核规则要求后台音频用于实际播放。
iOS 26 的 `BGContinuedProcessingTask` 适合将来评估用户发起的本地校验或合并，长时间网络传输统一由后台 `URLSession` 承担。[App Store 审核规则 §2.5.4](https://developer.apple.com/app-store/review/guidelines/#software-requirements) · [Apple 持续处理任务](https://developer.apple.com/videos/play/wwdc2025/227/)

版本与索引用临时 session 请求。UGP 将通过校验的目标索引和差异下载批次持久化。
后台会话在下载回调内校验 chunk，并移入 `Library/Application Support/UGPBackground/cache`。
系统唤醒时恢复同一会话，按任务列表和已校验文件对账。用户回到前台后，UGP 按已保存的索引执行合并及原子发布。
安装成功后清理缓存。已有可用内容时，传输失败或前台连续 60 秒无进展可回到已提交版本。
后续冷启动继续补齐缓存。首装失败时显示错误码和 Retry。[后台会话实现](../../../../Plugins/UGPIOSPatch/Source/UGPIOSPatch/Private/UGPIOSBackground.mm) · [iOS 启动及发布流程](ios-preinit-runbook.md)

收到目标清单或 chunk 的 HTTP 404 或 410 时，重查 `latest`。若发布目标已经变更，
撤下旧计划并保留缓存，当前启动尝试只自动切换一次。

UGP 通过 `IOSAppDelegate` 接收专用会话的事件。会话恢复、临时文件校验和补队列可以在游戏线程启动前完成。
低层 `FBackgroundURLSessionHandler` 的会话初始化需要 `GConfig`，这里使用独立的 Foundation 会话。
UGP 等回调文件落盘及任务对账完成后，在主线程调用 UIKit 的完成回调。[UE 5.9 引擎钩子](../../../../Plugins/UGPIOSPatch/EngineMod/5.9/README.md)

## CDN 分块与系统任务数量

仓库的发布工具 `-cli build` 会用 FastCDC 将 Pak 和 UCAS 切成内容寻址 chunk。
默认目标大小为 1 MiB，常规切分窗口为 256 KiB 到 4 MiB，重复哈希复用同一个 CDN 文件。
按 100 GB 原始内容和 1 MiB 实际平均块大小估算，去重前约 **95,368 个 chunk**。
具体数量由构建产物决定，iOS 队列预算按本次需要下载的对象数计算。[发布分块实现](../UnrealGamePatcherBuild/Private/FBuildIngestor.cpp#L197)

大型下载可将经常一起下载的新 chunk 写入不可变的 CDN 聚合对象。
索引记录 `chunk ID → 对象 URL、偏移、长度`，旧 chunk 引用之前发布的对象位置。
同一个 chunk 保持独立哈希，客户端仍可按现有缓存粒度复用。按实际传输量 100 GB 估算，任务数量如下。

| 平均下载对象大小 | iOS 下载任务数 |
| --- | ---: |
| 1 MiB | 95,368 |
| 128 MiB | 746 |
| 256 MiB | 373 |
| 512 MiB | 187 |
| 1 GiB | 94 |

首装可以按整对象下载。增量中，只缺少量 chunk 时，用现有 chunk URL 或覆盖所需连续字节的单段 Range。
HTTP 多段 Range 作用于同一 URL 对象，响应还涉及 `multipart/byteranges`，源站和 CDN 的支持程度各异。
整对象 GET 和单段 Range 足以实现首版聚合。服务端应提供可稳定寻址的对象、Range 支持以及 `ETag` 或 `Last-Modified`，以便系统续传。[HTTP Range 规范](https://www.rfc-editor.org/rfc/rfc9110.html#section-14.2) · [S3 GetObject](https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html) · [Apple 下载续传条件](https://developer.apple.com/documentation/foundation/urlsession/downloadtask%28withresumedata%3Acompletionhandler%3A%29)

## iOS 分批入队与续投

UGP 持久化计划和对象状态，iOS 维护已经提交的任务并调度传输。计划记录版本、对象 ID、目标文件和校验信息。
以文件落盘和校验通过作为完成条件。从同一个 session 取回系统任务列表后再对账，避免重复提交。
现有 `FDownloadManager` 的并发窗口控制进程内请求，
系统队列的待完成任务预算需要单独管理。[现有批量下载实现](../UnrealGamePatcherCore/Private/Download/FDownloadManager.cpp#L123) · [Apple 会话任务查询](https://developer.apple.com/documentation/foundation/urlsession/getalltasks%28completionhandler%3A%29)

默认最多提交 **128 个未完成任务**，降到 64 个再补到 128 个。Development 可用 `-ugpbackgroundtasks=4` 覆盖多批场景。
一批完成后，iOS 可以在后台唤醒 App，UGP 在这次回调里对账并补交下一批。
后台回调可以合并交付，新增任务也可能因系统调度而延迟。频繁唤醒后追加任务会触发逐步增加的限流。
回到前台时可重新连接系统任务。当前实现按现有 `.chunk` URL 下载，CDN 聚合留作大体量验证。
Apple 对数千个任务的建议是减少总任务数并增大单个传输。[Apple 后台唤醒规则](https://developer.apple.com/documentation/foundation/urlsessionconfiguration/sessionsendslaunchevents) · [Apple 调度与限流](https://developer.apple.com/documentation/foundation/downloading-files-in-the-background)

## iPad 验收

1. 从前台提交一批任务，锁屏或回桌面，记录系统回调、文件落盘和缓存恢复。可用已知能首装进图的 61 chunk 样本起步。[历史首装记录](ios-preinit-runbook.md#本机验证2026-11-11)
2. 人为设定较小的入队预算，让样本跨两到三批。保持 App 在后台，记录上一批完成、App 被唤醒、下一批提交和真正开跑的时间。检查重复提交与未完成任务的峰值。
3. 分别验证切换网络、系统回收进程和用户手动强退。比较已完成文件的校验结果和补下载量。
4. 确认回前台后按照已确认的版本完成合并。中断发布再重启时，已提交索引和旧版本仍可用于启动。使用接近 100 GB 的传输对象数验证排队时间和续投节奏。

2026-11-16 在 iPad Air 上使用独立原生测试壳运行会话协调代码。12 个 Raw chunk、队列上限 4。
切到设置后第一批 4 个文件校验落盘，回调补投第二批。杀进程后重新打开，CDN 未重发前 4 个文件，
沙盒中 12 个完整 chunk 均与 CDN 字节一致。另一轮在切到设置后对传输中的进程发送 SIGKILL。
系统自动拉起新进程并交付回调，回到测试壳前已从沙盒读到全部 4 个完整 chunk，逐字节比对通过。
完整 UE 客户端随后验证了杀进程后的 chunk 复用、安装提交及从 Saved 目录挂载进图。
首轮客户端失败的原因是 NSURLSession 交付的 `/.nofollow` 临时路径被 UE 的 iOS 文件层重新映射。
修正版先把文件移到缓存下的临时路径，再解压和校验，通过后才发布缓存文件。[完整客户端验收](ios-preinit-runbook.md#ue-59-后台下载测试包2026-11-16)
