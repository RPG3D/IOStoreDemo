# UGP engine changes for UE 5.9

Baseline `75677fb604ac0e4e6a955ec1a24ce21fd6996d4d` (UE 5.9.0).
Optional local snapshots under `Engine/` retain their engine-relative paths and are ignored by Git.
The replay patches mark each change with `UGP Begin` and `UGP End` and use one line of context.

Apply the patch from the project root. Applying the diff preserves unrelated changes in the target engine checkout.

```sh
ENGINE_ROOT=/path/to/UE5.9
MODS="$PWD/Plugins/UGPIOSPatch/EngineMod/5.9"
for name in UGPMobileSavedPaks.patch UGPIOS.patch; do
    patch="$MODS/$name"
    if git -C "$ENGINE_ROOT" apply --reverse --check "$patch" 2>/dev/null; then
        continue
    fi
    git -C "$ENGINE_ROOT" apply --check "$patch" && git -C "$ENGINE_ROOT" apply "$patch" || break
done
```

The Saved/Paks patch is shared with the Android plugin. If the engine already has an earlier
Saved/Paks hook, update its marked block to match `UGPMobileSavedPaks.patch`.
When porting to a different engine revision, use the marked sections to resolve any context conflicts.

| Engine file | Change |
| --- | --- |
| `IOSAppDelegate.cpp` | Routes the UGP background session to the plugin before Unreal initializes. |
| `LaunchIOS.cpp` | Calls the plugin before `GEngineLoop.PreInit`. |
| `IPlatformFilePak.cpp` | Looks for `global.utoc` in `Saved/Paks` on iOS when it is absent from the bundle. |

The iOS app delegate and launch entry use weak imports so the engine also links for targets that omit the plugin.
