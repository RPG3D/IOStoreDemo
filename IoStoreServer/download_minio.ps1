# Download MinIO server and client for Windows
$ErrorActionPreference = "Stop"

$Dir = Split-Path -Parent $MyInvocation.MyCommand.Path

$MinioUrl = "https://dl.min.io/server/minio/release/windows-amd64/minio.exe"
$McUrl    = "https://dl.min.io/client/mc/release/windows-amd64/mc.exe"

$MinioPath = Join-Path $Dir "minio.exe"
$McPath    = Join-Path $Dir "mc.exe"

Write-Host "Downloading MinIO server..." -ForegroundColor Cyan
if (-not (Test-Path $MinioPath)) {
    Invoke-WebRequest -Uri $MinioUrl -OutFile $MinioPath
    Write-Host "  -> Saved to $MinioPath" -ForegroundColor Green
} else {
    Write-Host "  -> Already exists: $MinioPath" -ForegroundColor Yellow
}

Write-Host "Downloading MinIO client (mc)..." -ForegroundColor Cyan
if (-not (Test-Path $McPath)) {
    Invoke-WebRequest -Uri $McUrl -OutFile $McPath
    Write-Host "  -> Saved to $McPath" -ForegroundColor Green
} else {
    Write-Host "  -> Already exists: $McPath" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Done! Next steps:" -ForegroundColor Cyan
Write-Host "  1. Run start.bat to start the MinIO server"
Write-Host "  2. Run setup_bucket.bat to create the ias-content bucket"
