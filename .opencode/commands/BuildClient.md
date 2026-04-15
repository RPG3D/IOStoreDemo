---
description: 打包游戏客户端并上传 OnDemand 内容
---

打包游戏客户端并上传 OnDemand 内容到 MinIO。

前置条件：
1. MinIO 服务已启动 (`IoStoreServer\start.bat`)
2. 游戏进程未运行

检查进程：
!`powershell -NoProfile -Command "Get-Process IOStoreDemo* -ErrorAction SilentlyContinue"`

打包上传：
!`IoStoreServer\build_and_upload.bat`

输出目录: `Archives\Windows\IOStoreDemo\`
