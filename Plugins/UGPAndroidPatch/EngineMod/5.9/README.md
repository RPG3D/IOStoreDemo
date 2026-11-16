# UGP engine changes for UE 5.9

Optional local snapshots under `Engine/` retain their engine-relative paths and are ignored by Git.
The replay patches mark each change with `UGP Begin` and `UGP End` and use one line of context.
The baseline is UE 5.9.0 at
`75677fb604ac0e4e6a955ec1a24ce21fd6996d4d`.

| Patch | Engine change |
| --- | --- |
| `UGPMobileSavedPaks.patch` | Looks for `global.utoc` in `Saved/Paks` during initial mounting. Shared with the iOS plugin. |
| `UGPAndroidMedia.patch` | Guards the Engine-only color converter when building the standalone `libugp.so`. |
| `UGPAndroidPermissions.patch` | Uses Android permission checks in `GameActivity.java.template` when the engine is built without Google VR. |

Apply from the project root on PowerShell. The reverse check skips a patch that is already applied.

```powershell
$engine = 'D:/Code/UE5'
$mods = Join-Path $PWD 'Plugins/UGPAndroidPatch/EngineMod/5.9'
foreach ($name in 'UGPMobileSavedPaks.patch', 'UGPAndroidMedia.patch', 'UGPAndroidPermissions.patch') {
    $patch = Join-Path $mods $name
    git -C $engine apply --reverse --check $patch 2>$null
    if ($LASTEXITCODE -eq 0) { continue }
    git -C $engine apply --check $patch
    if ($LASTEXITCODE -ne 0) { throw "Cannot apply $patch" }
    git -C $engine apply $patch
    if ($LASTEXITCODE -ne 0) { throw "Cannot apply $patch" }
}
```

If the engine already has an earlier Saved/Paks hook, update its existing marked block to
match `UGPMobileSavedPaks.patch`. The Android game startup gate is injected by the plugin's UPL.
