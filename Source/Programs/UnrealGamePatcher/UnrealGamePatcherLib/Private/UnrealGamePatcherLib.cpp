// Copyright Epic Games, Inc. All Rights Reserved.

// libugp.so - Android entry for the pre-boot patcher (PatchActivity model).
//
// Lifecycle (docs/android-port-feasibility-2026-11-02.md §2.3):
//   PatchActivity (own ":ugppatch" process) -> System.loadLibrary("ugp")
//   -> nativeRunPatch(...): engine-lite init (JNI env + PreInit with -NoPak,
//   no window, no pak mount) -> FUpdateOrchestrator::UpdateWithOptions
//   (progress via FUpdateOptions::Events -> JNI callback).
//   Java records readiness and finishes the process. GameActivity boots the real engine, which
//   mounts the freshly patched containers in the Android staged tree.
//
// The whole patch runs ON the calling (Java) thread: Java spawns the worker
// thread, so JNI callbacks need no extra attach and no engine threading
// beyond the task-tag scope.

#include "RequiredProgramMainCPPInclude.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UnrealGamePatcherLog.h"
#include "UnrealGamePatcherPaths.h"
#include "Update/FUpdateOrchestrator.h"
#include "Download/FHttpChunkDownloader.h"
#include "HttpModule.h"

IMPLEMENT_APPLICATION(UnrealGamePatcherLib, "UnrealGamePatcherLib");

#if PLATFORM_ANDROID

#include "Android/AndroidJavaEnv.h"
#include "Android/AndroidPlatformMisc.h"
#include "Android/AndroidJniGameActivity.h"
#include "Android/AndroidApplication.h"
#include "Android/AndroidEventManager.h"
#include <jni.h>
#include <android/native_activity.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/native_window.h>
#include <cerrno>
#include <cstdio>

// Core/Private/Android/AndroidJavaEnv.cpp - set from our JNI entry before
// touching UE::Jni (FEnv's constructor CHECKs it). Must be at file scope:
// inside the anonymous namespace it would get internal linkage.
extern JavaVM* GJavaVM;
extern FString GFilePathBase;
extern FString GInternalFilePath;
extern FString GExternalFilePath;

// Real AssetManager handles captured in RunPatch (defined with the stubs
// below); engine code (FAndroidPlatformFile) reads them via the thunks.
extern jobject GUgpAssetManagerJava;
extern AAssetManager* GUgpAssetManagerNative;

namespace
{
	FString JStringToFString(JNIEnv* Env, jstring S)
	{
		if (!S)
		{
			return FString();
		}
		const char* Utf8 = Env->GetStringUTFChars(S, nullptr);
		FString Out = UTF8_TO_TCHAR(Utf8);
		Env->ReleaseStringUTFChars(S, Utf8);
		return Out;
	}

	jstring FStringToJString(JNIEnv* Env, const FString& S)
	{
		return Env->NewStringUTF(TCHAR_TO_UTF8(*S));
	}

	/** Marshals FUpdateEvent (emitted on this thread by the orchestrator)
	 *  into PatchActivity.OnUpdateEventListener.onUpdateEvent. */
	struct FJavaEventSink
	{
		JNIEnv* Env = nullptr;
		jobject Listener = nullptr; // global ref
		jmethodID Method = nullptr;

		~FJavaEventSink()
		{
			if (Env && Listener)
			{
				Env->DeleteGlobalRef(Listener);
			}
		}

		void operator()(const UnrealGamePatcher::FUpdateEvent& Ev) const
		{
			if (!Env || !Listener || !Method)
			{
				return;
			}
			jstring Msg = FStringToJString(Env, Ev.Message);
			Env->CallVoidMethod(Listener, Method,
				(jint)Ev.Phase, (jint)Ev.Error, (jboolean)(Ev.bIsError ? JNI_TRUE : JNI_FALSE),
				(jint)Ev.Current, (jint)Ev.Total, Msg);
			if (Env->ExceptionCheck())
			{
				Env->ExceptionDescribe();
				Env->ExceptionClear();
			}
			Env->DeleteLocalRef(Msg);
		}
	};

	bool RenameDirectory(const FString& From, const FString& To)
	{
		if (rename(TCHAR_TO_UTF8(*From), TCHAR_TO_UTF8(*To)) == 0)
		{
			return true;
		}
		UE_LOG(LogUnrealGamePatcher, Error, TEXT("UGP Android: rename '%s' to '%s' failed (errno %d)"), *From, *To, errno);
		return false;
	}

	bool RunContentUpdate(const FString& Cdn, const FString& InstallDir, const FString& InternalDir,
		const FJavaEventSink& Sink)
	{
		using namespace UnrealGamePatcher;

		const auto Fail = [&Sink](EPatcherError Error, const TCHAR* Message)
		{
			Sink({ EUpdatePhase::Failed, Error, true, 0, 0, Message });
			return false;
		};
		// Keep the startup lock independent of the per-update lock and of
		// directories exchanged on external app storage (FUSE).
		Paths::FInstallLock StartupLock;
		if (!StartupLock.Acquire(InstallDir, InternalDir / TEXT("UGPStartup")))
		{
			return Fail(EPatcherError::InstallLockFailed, TEXT("Could not lock the game content directory."));
		}

		IPlatformFile& File = FPlatformFileManager::Get().GetPlatformFile();
		const FString StagedDir = InstallDir + TEXT(".ugp-update");
		const FString PreviousDir = InstallDir + TEXT(".ugp-previous");
		// If the process died between the two directory renames, restore
		// the old root before resolving a CDN version or starting the game.
		if (File.DirectoryExists(*PreviousDir) && !File.DirectoryExists(*InstallDir) &&
			!RenameDirectory(PreviousDir, InstallDir))
		{
			return Fail(EPatcherError::RollbackFailed, TEXT("Could not restore the previous game content."));
		}
		FString LocalVersion;
		bool bInstalled = FUpdateOrchestrator::ReadCommittedVersion(InstallDir, LocalVersion);
		if (File.DirectoryExists(*PreviousDir) && !bInstalled)
		{
			FString PreviousVersion;
			if (!FUpdateOrchestrator::ReadCommittedVersion(PreviousDir, PreviousVersion) ||
				(File.DirectoryExists(*StagedDir) && !File.DeleteDirectoryRecursively(*StagedDir)) ||
				!RenameDirectory(InstallDir, StagedDir) || !RenameDirectory(PreviousDir, InstallDir))
			{
				return Fail(EPatcherError::RollbackFailed, TEXT("Could not restore the previous game content."));
			}
			LocalVersion = MoveTemp(PreviousVersion);
			bInstalled = true;
		}

		const bool bStagingAvailable =
			(!File.DirectoryExists(*StagedDir) || File.DeleteDirectoryRecursively(*StagedDir)) &&
			(!File.DirectoryExists(*PreviousDir) || File.DeleteDirectoryRecursively(*PreviousDir));

		FHttpModule::Get();
		FHttpChunkDownloader Downloader;
		FUpdateOptions Options;
		Options.Downloader = &Downloader;
		Options.BaseUrl = Cdn;
		Options.ChunkCacheDir = InstallDir + TEXT(".ugp-cache");
		Options.InstallLockRootDir = InternalDir;
		Options.bForceVerify = !bInstalled;
		Options.bPreserveUntrackedFiles = true;
		EPatcherError LastError = EPatcherError::Internal;
		Options.Events = [&Sink, &LastError](const FUpdateEvent& Event)
		{
			if (Event.bIsError)
			{
				LastError = Event.Error;
			}
			Sink(Event);
		};
		const auto UseOldVersion = [&LocalVersion](EPatcherError Error)
		{
			UE_LOG(LogUnrealGamePatcher, Warning, TEXT("UGP Android: [UGP-%04d] using committed content %s"),
				static_cast<int32>(Error), *LocalVersion);
			return true;
		};
		FString Version;
		if (!FUpdateOrchestrator::ResolveLatest(Cdn, Options, Version))
		{
			return bInstalled ? UseOldVersion(LastError) : false;
		}
		if (bInstalled && LocalVersion == Version)
		{
			return true;
		}

		const auto Apply = [&](const FString& TargetDir)
		{
			Sink({ EUpdatePhase::Manifest, EPatcherError::Ok, false, 0, 0, TEXT("Loading content manifest...") });
			FUpdateStats Stats;
			if (!FUpdateOrchestrator::UpdateWithOptions(Cdn, Version, TargetDir, true, Options, Stats))
			{
				return false;
			}
			FString InstalledVersion;
			if (!FUpdateOrchestrator::ReadCommittedVersion(TargetDir, InstalledVersion) || InstalledVersion != Version)
			{
				LastError = EPatcherError::InstalledContentInvalid;
				return Fail(LastError, TEXT("The installed content is missing a mountable Pak or has an invalid version."));
			}
			return true;
		};
		if (!bInstalled)
		{
			// Legacy pending transactions store absolute paths, so only
			// recover them in their original directory, with the CDN online.
			return Apply(InstallDir);
		}
		if (!bStagingAvailable)
		{
			return UseOldVersion(EPatcherError::StagingCleanupFailed);
		}

		ON_SCOPE_EXIT { File.DeleteDirectoryRecursively(*StagedDir); };
		Sink({ EUpdatePhase::Diff, EPatcherError::Ok, false, 0, 0, TEXT("Preparing installed content...") });
		if (!File.CopyDirectoryTree(*StagedDir, *InstallDir, false))
		{
			return UseOldVersion(EPatcherError::StagingSnapshotFailed);
		}
		if (!Apply(StagedDir))
		{
			return UseOldVersion(LastError);
		}
		if (!RenameDirectory(InstallDir, PreviousDir))
		{
			return UseOldVersion(EPatcherError::ActivateContentFailed);
		}
		if (!RenameDirectory(StagedDir, InstallDir))
		{
			if (!RenameDirectory(PreviousDir, InstallDir))
			{
				return Fail(EPatcherError::RollbackFailed, TEXT("Could not restore the previous game content."));
			}
			return UseOldVersion(EPatcherError::ActivateContentFailed);
		}
		File.DeleteDirectoryRecursively(*PreviousDir);
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("UGP Android: activated content %s in %s"), *Version, *InstallDir);
		return true;
	}

	jboolean RunPatch(JNIEnv* Env, jstring JCmdline, jstring JCdn,
		jstring JInstallDir, jstring JInternalDir, jobject JAssets, jobject JListener)
	{
		using namespace UnrealGamePatcher;

		const FString CmdLine = JStringToFString(Env, JCmdline);
		const FString Cdn = JStringToFString(Env, JCdn);
		const FString InstallDir = JStringToFString(Env, JInstallDir);
		const FString InternalDir = JStringToFString(Env, JInternalDir);

		FJavaEventSink Sink;
		if (JListener)
		{
			Sink.Env = Env;
			Sink.Listener = Env->NewGlobalRef(JListener);
			jclass Cls = Env->GetObjectClass(JListener);
			Sink.Method = Env->GetMethodID(Cls, "onUpdateEvent", "(IIZIILjava/lang/String;)V");
			Env->DeleteLocalRef(Cls);
			if (Env->ExceptionCheck())
			{
				Env->ExceptionDescribe();
				Env->ExceptionClear();
				return JNI_FALSE;
			}
		}

		enum class EEngineState { NotStarted, Ready, Unavailable };
		static EEngineState EngineState = EEngineState::NotStarted;
		FTaskTagScope Scope(ETaskTag::EGameThread);
		if (EngineState == EEngineState::NotStarted)
		{
			// A failed PreInit cannot be attempted again in the same process.
			EngineState = EEngineState::Unavailable;
			JavaVM* VM = nullptr;
			AAssetManager* NativeAssets = JAssets ? AAssetManager_fromJava(Env, JAssets) : nullptr;
			if (Env->GetJavaVM(&VM) == JNI_OK && VM && NativeAssets)
			{
				GJavaVM = VM;
				GUgpAssetManagerJava = Env->NewGlobalRef(JAssets);
				GUgpAssetManagerNative = NativeAssets;
				GInternalFilePath = InternalDir;
				GFilePathBase = FPaths::GetPath(FPaths::GetPath(InstallDir));
				GExternalFilePath = GFilePathBase;
				AndroidJavaEnv::InitializeJavaEnv();
				FCommandLine::Set(*CmdLine);
				if (GEngineLoop.PreInit(0, nullptr, FCommandLine::Get()) == 0)
				{
					EngineState = EEngineState::Ready;
				}
			}
		}
		if (EngineState != EEngineState::Ready)
		{
			Sink({ EUpdatePhase::Failed, EPatcherError::Internal, true, 0, 0,
				TEXT("Could not initialize the content updater. Reopen the app to try again.") });
			return JNI_FALSE;
		}
		const bool bOk = RunContentUpdate(Cdn, InstallDir, InternalDir, Sink);
		GLog->Flush();
		// Keep the updater initialized until Java records readiness and ends
		// this process, so failure to record readiness can still be retried.
		return bOk ? JNI_TRUE : JNI_FALSE;
	}
}

// PatchActivity calls System.loadLibrary("ugp") then binds via these names.
// Kept as plain Java_ exports (no RegisterNatives) so the Java side stays
// free of engine-only glue.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_epicgames_unreal_patch_PatchActivity_nativeRunPatchV2(
	JNIEnv* Env, jclass, jstring JCmdline, jstring JCdn,
	jstring JInstallDir, jstring JInternalDir, jobject JAssets, jobject JListener)
{
	return RunPatch(Env, JCmdline, JCdn, JInstallDir, JInternalDir, JAssets, JListener);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_epicgames_unreal_patch_PatchActivity_nativeBuildConfigurationV2(JNIEnv* Env, jclass)
{
	return Env->NewStringUTF("UGP_JNI_V2:" PREPROCESSOR_TO_STRING(UGP_BUILD_CONFIGURATION));
}

// UBT's Android version script (AndroidToolChain.cs) exports JNI_OnLoad and
// ANativeActivity_onCreate from every shared binary and links with
// --no-undefined, so both must exist. JNI_OnLoad doubles as the standard
// load hook; the NativeActivity entry is never invoked (this library is not
// an activity implementation).
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*)
{
	return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL ANativeActivity_onCreate(ANativeActivity*, void*, size_t)
{
}

// ---------------------------------------------------------------------------
// Launch-module Android glue stubs. Android Program targets never compile
// Launch/Private/Android/*.cpp (UBT inlines only LaunchEngineLoop.cpp into
// the launch-module TU), but Android Core's platform files reference the
// glue unconditionally - game targets get the definitions from the linked
// Launch module. A headless patcher provides inert defaults instead: no
// activity binding, virtual keyboard or intent access. The real AssetManager
// is captured from PatchActivity for platform file initialization.
// ---------------------------------------------------------------------------
struct android_app;
struct android_app* GNativeAndroidApp = nullptr; // LaunchAndroid.cpp:309

void AndroidThunkCpp_ForceQuit()
{
	UE_LOG(LogUnrealGamePatcher, Warning, TEXT("libugp: AndroidThunkCpp_ForceQuit (headless stub)"));
}

// Real handles captured in RunPatch (global ref + native peer).
jobject GUgpAssetManagerJava = nullptr;
AAssetManager* GUgpAssetManagerNative = nullptr;

AAssetManager* AndroidThunkCpp_GetAssetManager()
{
	return GUgpAssetManagerNative;
}

jobject AndroidJNI_GetJavaAssetManager()
{
	return GUgpAssetManagerJava;
}

int32 AndroidThunkCpp_GetMetaDataInt(const FString& /*Key*/)
{
	return 0;
}

FString AndroidThunkCpp_GetIntentDataAsString()
{
	return FString();
}

FString AndroidThunkCpp_GetAndroidId()
{
	return FString();
}

int32 AndroidThunkCpp_GetNetworkConnectionType()
{
	return 0;
}

bool AndroidThunkCpp_GetSharedPreferenceStringTypeSafe(const FString& /*Group*/, const FString& /*Key*/, FString& /*OutValue*/)
{
	return false;
}

void AndroidThunkJava_AddNetworkListener()
{
}

FString FAndroidMisc::GetGPUFamily()
{
	return FString();
}

void FAndroidMisc::GetValidTargetPlatforms(TArray<FString>& TargetPlatformNames)
{
	FGenericPlatformMisc::GetValidTargetPlatforms(TargetPlatformNames);
}

// FAndroidAppEntry is a namespace of engine-boot glue (window/EGL lifecycle);
// a headless patcher never drives it.
void FAndroidAppEntry::PlatformInit()
{
}

// The full UE::Jni::FGameActivity native table (Core references every entry
// via its registration array). All are Java-called activity lifecycle glue -
// the patcher's Java side never invokes them; the definitions exist so the
// table links. (AndroidJniGameActivity.h)
void UE::Jni::FGameActivity::nativeHandleNewIntentData(JNIEnv*, jobject, jstring) {}
void UE::Jni::FGameActivity::nativeVirtualKeyboardVisible(JNIEnv*, jobject, jboolean) {}
void UE::Jni::FGameActivity::nativeVirtualKeyboardShown(JNIEnv*, jobject, jint, jint, jint, jint) {}
void UE::Jni::FGameActivity::nativeVirtualKeyboardResult(JNIEnv*, jobject, jboolean, jstring) {}
void UE::Jni::FGameActivity::nativeVirtualKeyboardDismissed(JNIEnv*, jobject, jboolean, jstring) {}
void UE::Jni::FGameActivity::nativeVirtualKeyboardChanged(JNIEnv*, jobject, jstring) {}
void UE::Jni::FGameActivity::nativeVirtualKeyboardSendKey(JNIEnv*, jobject, jint) {}
void UE::Jni::FGameActivity::nativeVirtualKeyboardSendSelection(JNIEnv*, jobject, jint, jint) {}
void UE::Jni::FGameActivity::nativeSetObbFilePaths(JNIEnv*, jobject, jstring, jstring, jstring, jstring) {}
void UE::Jni::FGameActivity::nativeSetGlobalActivity(JNIEnv*, jobject, jboolean, jboolean, jstring, jstring, jboolean, jstring) {}
jboolean UE::Jni::FGameActivity::nativeIsShippingBuild(JNIEnv*, jobject) { return JNI_TRUE; }
void UE::Jni::FGameActivity::nativeOnActivityResult(JNIEnv*, jobject, FGameActivity*, jint, jint, Android::Content::FIntent*) {}
void UE::Jni::FGameActivity::nativeHandleSensorEvents(JNIEnv*, jobject, Java::Lang::TArray<jfloat>*, Java::Lang::TArray<jfloat>*, Java::Lang::TArray<jfloat>*, Java::Lang::TArray<jfloat>*) {}
void UE::Jni::FGameActivity::nativeOnThermalStatusChangedListener(JNIEnv*, jobject, jint) {}
void UE::Jni::FGameActivity::nativeOnTrimMemory(JNIEnv*, jobject, jint) {}
void UE::Jni::FGameActivity::nativeOnOrientationChanged(JNIEnv*, jobject, jint) {}
void UE::Jni::FGameActivity::nativeConsoleCommand(JNIEnv*, jobject, jstring) {}
void UE::Jni::FGameActivity::nativeInitHMDs(JNIEnv*, jobject) {}
void UE::Jni::FGameActivity::nativeSetAndroidVersionInformation(JNIEnv*, jobject, jstring, jint, jstring, jstring, jstring, jstring, jstring) {}
void UE::Jni::FGameActivity::nativeOnInitialDownloadStarted(JNIEnv*, jobject) {}
void UE::Jni::FGameActivity::nativeOnInitialDownloadCompleted(JNIEnv*, jobject) {}
// NOTE: nativeCrashContextSet*Key, nativeNetworkChanged, nativeOnAudioFocusChanged,
// nativeSetAffinityInfo, nativeSetAndroidStartupState, nativeSetConfigRulesVariables,
// nativeSetObbInfo, nativeSetSafezoneInfo/SurfaceViewInfo/WindowInfo are defined
// by Core's own Android files (AndroidPlatformCrashContext.cpp et al) - do not
// stub them here (duplicate symbols).
void UE::Jni::FGameActivity::nativeResumeMainInit(JNIEnv*, jobject) {}
jint UE::Jni::FGameActivity::nativeGetCPUFamily(JNIEnv*, jobject) { return 0; }
jboolean UE::Jni::FGameActivity::nativeSupportsNEON(JNIEnv*, jobject) { return JNI_TRUE; }

// Remaining C++ thunks referenced from Core's Android platform code.
FString AndroidThunkCpp_GetMetaDataString(const FString& /*Key*/)
{
	return FString();
}

void AndroidThunkCpp_ClipboardCopy(const FString& /*Text*/) {}
FString AndroidThunkCpp_ClipboardPaste() { return FString(); }
void AndroidThunkCpp_ReleaseWifiManagerMulticastLock() {}
void AndroidThunkCpp_AcquireWifiManagerMulticastLock() {}

// Android app-event machinery (Launch/Private/Android/AndroidEventManager.cpp
// is not compiled into Program targets). Core enqueues pause/resume/window
// events through this singleton - a headless patcher sinks them.
FAppEventData::FAppEventData(ANativeWindow* /*WindowIn*/)
	: WindowWidth(-1), WindowHeight(-1)
{
}

FAppEventManager* FAppEventManager::GetInstance()
{
	static FAppEventManager Instance;
	return &Instance;
}

FAppEventManager::FAppEventManager() = default;

void FAppEventManager::EnqueueAppEvent(EAppEventState /*InState*/, FAppEventData&& /*InData*/)
{
}

void FAppEventManager::SetEventHandlerEvent(FEvent* /*InEventHandlerEvent*/) {}
void FAppEventManager::ResumePausedGameThread() {}
void FAppEventManager::KeepGameThreadPaused() {}

FString FAndroidMisc::GetGLVersion()
{
	return FString();
}

#else // !PLATFORM_ANDROID

// Non-Android hosts (sanity compiles only): a minimal no-op entry so the
// program target still links. The real patcher lives in UnrealGamePatcher.
int32 main(int argc, char* argv[])
{
	return 0;
}

#endif // PLATFORM_ANDROID
