# UGP 移动端后台下载方案

Android 14 及以上用 UIDT 承载用户确认的版本更新，Android 10–13 用 `dataSync` 前台服务。
iOS 采用后台 `URLSession`，优先验证 UE 自带的后台下载接口能否在启动前复用。
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

系统停止任务时，取消信号经 JNI 传给更新执行器和 HTTP 请求。已校验的 chunk 留在缓存，目录发布前再次检查取消状态。
下载完成后，`PatchActivity` 在前台才交接给游戏。安装目录继续由跨进程锁保护。[批次下载管理](../UnrealGamePatcherCore/Private/Download/FDownloadManager.cpp)

2026-11-15 的 Android 11 x86_64 模拟器验收覆盖前台服务启动、退到桌面后继续下载、停止后重试和发布。
重试复用了 55 个已校验的 chunk，验收截至后台发布完成。Windows 的 HTTP 取消、更新、Pak 和 IoStore 回归通过。
ARM64 APK 已通过签名及静态检查并覆盖安装到 Android 16 真机。UIDT 通知、系统停止和恢复留待该设备解锁后验收。[Android 构建与设备验证](android-s1-runbook.md)

## iOS 后台传输选型

使用一个固定标识的后台 `URLSession`，启用 `sessionSendsLaunchEvents`。
iOS 可以在 App 挂起或被系统回收时继续传输，并在有会话事件时唤醒或重新拉起 App。
一个下载对象对应一个 `DownloadTask`，App 可以在一次前台操作里连续提交一批。实际并发和启动时机由系统调度。
用户从多任务界面强退会取消后台传输，重新打开后按持久缓存补缺。[Apple 后台下载指南](https://developer.apple.com/documentation/foundation/downloading-files-in-the-background) · [会话恢复与强退行为](https://developer.apple.com/documentation/foundation/urlsessionconfiguration/background%28withidentifier%3A%29)

音乐保活会把传输依赖在音频会话上，Apple 审核规则要求后台音频用于实际播放。
iOS 26 的 `BGContinuedProcessingTask` 适合将来评估用户发起的本地校验或合并，长时间网络传输统一由后台 `URLSession` 承担。[App Store 审核规则 §2.5.4](https://developer.apple.com/app-store/review/guidelines/#software-requirements) · [Apple 持续处理任务](https://developer.apple.com/videos/play/wwdc2025/227/)

当前 iOS 更新器用临时 session 的 `dataTask`，响应在进程内累积，再交由 UGP 校验。
后台方案需要由系统先下载到文件，并在下载回调返回前把临时文件移入持久缓存。
缓存放在活动 `Saved/Paks` 和候选目录之外。用户返回前台后，UGP 复用既有校验、合并和原子发布流程。
版本检查、下载计划和待安装版本都关联到同一次更新，应用重启后可以接回同一个 session。[当前 iOS 下载器](../../../UGPIOSPatch/Private/UGPIOSDownload.mm) · [iOS 启动及发布流程](ios-preinit-runbook.md)

UE 5.9 的 `IOSAppDelegate` 已处理 `handleEventsForBackgroundURLSession`。
优先验证 `BackgroundHttpIOS` 的 `FBackgroundURLSessionHandler` 能否在 UGP 的 `PreInit` 阶段提交任务、移动下载文件和恢复会话。
它的 `OnDownloadsCompletedWhileAppWasNotRunning` 广播发生在 UE 调用系统完成回调之前。
注册需要覆盖系统后台拉起路径。若对账或入队是异步的，完成回调的所有者需要等本次处理结束再通知 iOS。
高层 `BackgroundHTTP` 的 manager 依赖 GameThread tick，启动前验证应从底层接口开始。

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

可以从 **128 个已提交未完成任务**开始压测，前台降到 64 个再补到 128 个。
一批完成后，iOS 可以在后台唤醒 App，UGP 在这次回调里对账并补交下一批。
后台回调可以合并交付，新增任务也可能因系统调度而延迟。频繁唤醒后追加任务会触发逐步增加的限流。
回到前台时再次对账补队列。128 和 64 是可调整的压测参数。Apple 对数千个任务的建议仍是减少总任务数并增大单个传输。[Apple 后台唤醒规则](https://developer.apple.com/documentation/foundation/urlsessionconfiguration/sessionsendslaunchevents) · [Apple 调度与限流](https://developer.apple.com/documentation/foundation/downloading-files-in-the-background)

## iPad 验收

1. 从前台提交一批任务，锁屏或回桌面，记录系统回调、文件落盘和缓存恢复。可用已知能首装进图的 61 chunk 样本起步。[历史首装记录](ios-preinit-runbook.md#本机验证2026-11-11)
2. 人为设定较小的入队预算，让样本跨两到三批。保持 App 在后台，记录上一批完成、App 被唤醒、下一批提交和真正开跑的时间。检查重复提交与未完成任务的峰值。
3. 分别验证切换网络、系统回收进程和用户手动强退。比较已完成文件的校验结果和补下载量。
4. 确认回前台后按照已确认的版本完成合并。中断发布再重启时，已提交索引和旧版本仍可用于启动。使用接近 100 GB 的传输对象数验证排队时间和续投节奏。
