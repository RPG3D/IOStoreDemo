---
description: 编译 UE 编辑器
---

检查编辑器进程是否运行，若未运行则编译编辑器。

检查进程：
!`powershell -NoProfile -Command "Get-Process UnrealEditor* -ErrorAction SilentlyContinue"`

编译编辑器：
!`Tools\build_editor.bat`
