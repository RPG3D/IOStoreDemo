<#
.SYNOPSIS
    FindUE5 - Find installed Unreal Engine 5 installations

.DESCRIPTION
    Reads UE5 engine installations from Windows registry and parses Version.h for accurate version info.
    Supports filtering by major/minor version and multiple output formats.

.EXAMPLE
    Find-UE5                    # Find all UE5 engines (simple format)
    Find-UE5 -List              # List all engines with details
    Find-UE5 -Json              # Output as JSON
    Find-UE5 -Major 5 -Minor 7  # Find UE 5.7.* engines
    Find-UE57                   # Shortcut: Find UE 5.7.* engine
    Find-UE5 -EnginePath        # Output only the path (for batch scripts)

.NOTES
    Registry paths checked:
    - HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds
    - HKLM:\SOFTWARE\Epic Games\Unreal Engine\Builds
#>

[CmdletBinding(DefaultParameterSetName='Default')]
param(
    [switch]$List,
    
    [switch]$Json,
    
    [switch]$EnginePath,
    
    [Parameter(ParameterSetName='ByVersion')]
    [int]$MajorVersion = -1,

    [Parameter(ParameterSetName='ByVersion')]
    [int]$MinorVersion = -1,

    [switch]$FindUE57,

    [switch]$FindUE56,

    [switch]$FindUE55,

    [switch]$FindUE54,

    [switch]$FindUE53
)

begin {
    $engines = @()
    
    $regPaths = @(
        "HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds",
        "HKLM:\SOFTWARE\Epic Games\Unreal Engine\Builds"
    )
    
    foreach ($regPath in $regPaths) {
        if (Test-Path $regPath) {
            $item = Get-ItemProperty $regPath -ErrorAction SilentlyContinue
            if ($item) {
                $item.PSObject.Properties | Where-Object {
                    $_.Name -match "^\{[A-F0-9-]+\}$" -and $_.Value -match ":/"
                } | ForEach-Object {
                    $engineBase = $_.Value.Replace("/", "\")
                    $engineDir = Join-Path $engineBase "Engine"
                    $versionFile = Join-Path $engineDir "Source\Runtime\Launch\Resources\Version.h"
                    
                    if (Test-Path $versionFile) {
                        $content = Get-Content $versionFile -ErrorAction SilentlyContinue
                        
                        $major = 0
                        $minor = 0
                        $patch = 0
                        
                        foreach ($line in $content) {
                            if ($line -match "^#define\s+ENGINE_MAJOR_VERSION\s+(\d+)") {
                                $major = [int]$Matches[1]
                            }
                            if ($line -match "^#define\s+ENGINE_MINOR_VERSION\s+(\d+)") {
                                $minor = [int]$Matches[1]
                            }
                            if ($line -match "^#define\s+ENGINE_PATCH_VERSION\s+(\d+)") {
                                $patch = [int]$Matches[1]
                            }
                        }
                        
                        $engines += [PSCustomObject]@{
                            Path = $engineDir
                            Major = $major
                            Minor = $minor
                            Patch = $patch
                            Version = "$major.$minor.$patch"
                        }
                    }
                }
            }
        }
    }
    
    # Remove duplicates
    $engines = $engines | Sort-Object Path -Unique
    
    # Determine target version from shortcuts
    $targetMajor = $MajorVersion
    $targetMinor = $MinorVersion
    
    if ($FindUE57) { $targetMajor = 5; $targetMinor = 7 }
    if ($FindUE56) { $targetMajor = 5; $targetMinor = 6 }
    if ($FindUE55) { $targetMajor = 5; $targetMinor = 5 }
    if ($FindUE54) { $targetMajor = 5; $targetMinor = 4 }
    if ($FindUE53) { $targetMajor = 5; $targetMinor = 3 }
    
    # Filter by version if specified
    if ($targetMajor -ge 0) {
        $engines = $engines | Where-Object { $_.Major -eq $targetMajor }
    }
    if ($targetMinor -ge 0) {
        $engines = $engines | Where-Object { $_.Minor -eq $targetMinor }
    }
    
    # Output
    if ($EnginePath) {
        # Output only path (for batch scripts)
        if ($engines.Count -gt 0) {
            Write-Output $engines[0].Path
        }
        exit 0
    }
    
    if ($Json) {
        $engines | ConvertTo-Json -Compress
        exit 0
    }
    
    if ($List) {
        Write-Host "Found $($engines.Count) UE5 engine(s):"
        Write-Host ""
        foreach ($engine in $engines) {
            Write-Host "  [$($engine.Version)] $($engine.Path)"
        }
        exit 0
    }
    
    # Simple format (default)
    if ($engines.Count -eq 0) {
        if ($targetMajor -ge 0 -or $targetMinor -ge 0) {
            Write-Host "UE $targetMajor.$targetMinor engine not found in registry" -ForegroundColor Red
        } else {
            Write-Host "No UE5 engines found in registry" -ForegroundColor Red
        }
        exit 1
    }
    
    # If filtering by version, output only the first match (for batch scripts)
    if ($targetMajor -ge 0 -or $targetMinor -ge 0) {
        Write-Output $engines[0].Path
        exit 0
    }
    
    # Otherwise output all paths
    $engines | ForEach-Object { Write-Output $_.Path }
}
