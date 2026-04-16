# 工具使用说明

本文档介绍项目中各工具的使用方法。

## FindUE5.ps1

查找已安装的 UE5 引擎。

### 位置

`Tools/FindUE5.ps1`

### 功能

从 Windows 注册表读取 UE5 引擎安装信息，并解析 `Version.h` 获取精确版本号。

### 用法

```powershell
# 查找所有 UE5 引擎（简单格式）
powershell -File Tools\FindUE5.ps1

# 列出所有引擎详情
powershell -File Tools\FindUE5.ps1 -List

# JSON 格式输出
powershell -File Tools\FindUE5.ps1 -Json

# 仅输出引擎路径（供 batch 脚本调用）
powershell -File Tools\FindUE5.ps1 -EnginePath

# 按版本筛选
powershell -File Tools\FindUE5.ps1 -FindUE57   # 查找 5.7.*
powershell -File Tools\FindUE5.ps1 -FindUE56   # 查找 5.6.*
powershell -File Tools\FindUE5.ps1 -FindUE55   # 查找 5.5.*
powershell -File Tools\FindUE5.ps1 -FindUE54   # 查找 5.4.*
powershell -File Tools\FindUE5.ps1 -FindUE53   # 查找 5.3.*
```

### 参数

| 参数 | 说明 |
|------|------|
| `-List` | 显示详细列表，包含版本号和路径 |
| `-Json` | JSON 格式输出 |
| `-EnginePath` | 仅输出路径（供 batch 脚本调用） |
| `-FindUE57` | 筛选 UE 5.7.* 版本 |
| `-FindUE56` | 筛选 UE 5.6.* 版本 |
| `-FindUE55` | 筛选 UE 5.5.* 版本 |
| `-FindUE54` | 筛选 UE 5.4.* 版本 |
| `-FindUE53` | 筛选 UE 5.3.* 版本 |

### 注册表路径

脚本检查以下注册表位置：
- `HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds`
- `HKLM:\SOFTWARE\Epic Games\Unreal Engine\Builds`

---

## download_minio.ps1

自动下载 MinIO 服务端和客户端工具。

### 位置

`IoStoreServer/download_minio.ps1`

### 功能

从 MinIO 官方下载 `minio.exe`（服务端）和 `mc.exe`（客户端）到 IoStoreServer 目录。

### 用法

```powershell
# 首次设置时运行
powershell -File IoStoreServer\download_minio.ps1
```

### 下载源

- minio.exe: `https://dl.min.io/server/minio/release/windows-amd64/minio.exe`
- mc.exe: `https://dl.min.io/client/mc/release/windows-amd64/mc.exe`

### 输出

- `IoStoreServer/minio.exe`
- `IoStoreServer/mc.exe`

---

## OnDemandCacheAnalyzer.py

解析 OnDemand 下载缓存目录。

### 位置

`Tools/OnDemandCacheAnalyzer.py`

### 功能

分析 `PersistentDownloadDir` 目录，解析 CAS Journal 和 IAS 缓存文件格式。

### 用法

```powershell
python Tools\OnDemandCacheAnalyzer.py "<cache_directory>" [-v]
```

### 示例

```powershell
# 分析缓存目录
python Tools\OnDemandCacheAnalyzer.py "Archives\Windows\IOStoreDemo\Saved\PersistentDownloadDir"

# 详细模式
python Tools\OnDemandCacheAnalyzer.py "Archives\Windows\IOStoreDemo\Saved\PersistentDownloadDir" -v
```

### 输出示例

```
============================================================
IoStoreOnDemand Cache Analysis Report
============================================================

Directory: E:\...\PersistentDownloadDir

----------------Summary-----------------
  Total Files: 3
  Total Size:  6.36 MB

---------CAS Journal Statistics---------
  Total Entries:      150
  Chunk Locations:    100
  Blocks Created:     50
  Blocks Deleted:     0
  Blocks Accessed:    25
============================================================
```

### 文件格式

| 文件 | 说明 |
|------|------|
| `cas.jrn` | CAS Journal，包含 chunk location 和 block 操作记录 |
| `ias.cache.0` | IAS 缓存数据块 |
| `ias.cache.0.jrn` | IAS 缓存日志 |

---

## mc.exe (MinIO Client)

MinIO 命令行客户端工具。

### 位置

- `IoStoreServer/mc.exe`
- `Tools/mc.exe`

### 功能

用于与 MinIO 服务交互，如创建桶、设置策略、上传下载文件等。

### 在项目中的用途

`setup_bucket.bat` 使用 mc.exe 执行以下操作：

```powershell
# 配置别名
mc alias set iaslocal http://localhost:9000 minioadmin minioadmin

# 创建桶
mc mb iaslocal/ias-content

# 设置桶策略（公开读取）
mc anonymous set download iaslocal/ias-content
```

### 常用命令

```powershell
# 列出所有桶
mc ls iaslocal

# 查看桶内容
mc ls iaslocal/ias-content

# 上传文件
mc cp <file> iaslocal/ias-content/

# 下载文件
mc cp iaslocal/ias-content/<file> ./
```

---

## minio.exe (MinIO Server)

MinIO 对象存储服务端。

### 位置

- `IoStoreServer/minio.exe`
- `Tools/minio.exe`

### 用法

通过 `IoStoreServer/start.bat` 启动，参见 [IoStoreServer/README.txt](../IoStoreServer/README.txt)。

### 默认配置

| 配置项 | 值 |
|--------|-----|
| API 端口 | 9000 |
| Console 端口 | 9001 |
| AccessKey | minioadmin |
| SecretKey | minioadmin |
