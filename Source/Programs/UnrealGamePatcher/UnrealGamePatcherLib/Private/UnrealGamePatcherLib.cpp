// Copyright Epic Games, Inc. All Rights Reserved.

// libugp.so - Android entry for the pre-boot patcher (PatchActivity model).
//
// Lifecycle (docs/android-port-feasibility-2026-11-02.md §2.3):
//   PatchActivity (own ":patcher" process) -> System.loadLibrary("ugp")
//   -> nativeRunPatch(...): engine-lite init (JNI env + PreInit with -NoPak,
//   no window, no pak mount) -> FUpdateOrchestrator::UpdateWithOptions
//   (progress via FUpdateOptions::Events -> JNI callback) -> engine shutdown.
//   Java then finishes the process; GameActivity boots the real engine, which
//   mounts the freshly patched containers from Saved/Paks.
//
// The whole patch runs ON the calling (Java) thread: Java spawns the worker
// thread, so JNI callbacks need no extra attach and no engine threading
// beyond the task-tag scope.

#include "RequiredProgramMainCPPInclude.h"
#include "Misc/CommandLine.h"
#include "Modules/ModuleManager.h"
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

// Core/Private/Android/AndroidJavaEnv.cpp - set from our JNI entry before
// touching UE::Jni (FEnv's constructor CHECKs it). Must be at file scope:
// inside the anonymous namespace it would get internal linkage.
extern JavaVM* GJavaVM;

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

		void operator()(const UnrealGamePatcher::FUpdateEvent& Ev)
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

	/** Java -> native patch entry contract:
	 *  boolean nativeRunPatch(String cmdline, String cdn, String version,
	 *                        String installDir, String internalDir,
	 *                        AssetManager assets,
	 *                        OnUpdateEventListener listener)
	 *  Returns true when the update flow completed (success OR clean failure
	 *  reported through the listener); false when init itself failed. */
	jboolean RunPatch(JNIEnv* Env, jclass, jstring JCmdline, jstring JCdn,
		jstring JVersion, jstring JInstallDir, jstring JInternalDir, jobject JAssets, jobject JListener)
	{
		const FString CmdLine = JStringToFString(Env, JCmdline);
		const FString Cdn = JStringToFString(Env, JCdn);
		const FString Version = JStringToFString(Env, JVersion);
		const FString InstallDir = JStringToFString(Env, JInstallDir);
		const FString InternalDir = JStringToFString(Env, JInternalDir);

		// Engine-lite boot, ONCE per process: JVM/env setup, the real
		// AssetManager (the game process gets these from libUnreal's
		// JNI_OnLoad - the patcher process never loads libUnreal), then
		// PreInit with -NoPak (nothing mounts; file APIs hit writable dirs).
		// A FAILED run must NOT shut the engine down (the failure path below
		// skips the shutdown sequence) so the Java retry button can re-run
		// the orchestrator - a second PreInit in one process is unsupported.
		static bool bEngineReady = false;
		FTaskTagScope Scope(ETaskTag::EGameThread);
		if (!bEngineReady)
		{
			// In the game process libUnreal's JNI_OnLoad sets the Core-global
			// GJavaVM; set it from the JNIEnv we were called with -
			// UE::Jni::FEnv's constructor CHECKs it.
			JavaVM* VM = nullptr;
			if (Env->GetJavaVM(&VM) != JNI_OK || !VM)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("libugp: GetJavaVM failed"));
				return false;
			}
			GJavaVM = VM;
			AndroidJavaEnv::InitializeJavaEnv();

			// Engine code assumes a real AAssetManager (FAndroidPlatformFile's
			// asset paths call AAssetManager_open without a null check). Hand
			// it the activity's real one - the patcher APK is this app's APK,
			// so its assets are exactly what the engine would see.
			AAssetManager* NativeAssets = JAssets ? AAssetManager_fromJava(Env, JAssets) : nullptr;
			GUgpAssetManagerJava = JAssets ? Env->NewGlobalRef(JAssets) : nullptr;
			GUgpAssetManagerNative = NativeAssets;
			if (!NativeAssets)
			{
				UE_LOG(LogUnrealGamePatcher, Warning, TEXT("libugp: no AssetManager passed - engine file init may fail"));
			}

			FCommandLine::Set(*CmdLine);
			if (GEngineLoop.PreInit(0, nullptr, FCommandLine::Get()) != 0)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("libugp: engine PreInit failed"));
				return false;
			}
			bEngineReady = true;
		}

		// Progress listener, per run (the Java side may pass a fresh one).
		FJavaEventSink Sink;
		if (JListener)
		{
			Sink.Env = Env;
			Sink.Listener = Env->NewGlobalRef(JListener);
			jclass Cls = Env->GetObjectClass(JListener);
			Sink.Method = Env->GetMethodID(Cls, "onUpdateEvent",
				"(IIZIILjava/lang/String;)V"); // (phase, error, isError, current, total, message)
			if (Env->ExceptionCheck())
			{
				Env->ExceptionDescribe();
				Env->ExceptionClear();
				return false;
			}
		}

		// Single-writer guard (Android: flock on a file in the app's INTERNAL
		// dir - the emulated external storage cannot flock; see Paths).
		UnrealGamePatcher::Paths::FInstallLock Lock;
		if (!Lock.Acquire(InstallDir, InternalDir))
		{
			GLog->Flush();
			return false;
		}

		// HTTP CDN mode needs the HTTP chunk downloader wired into the options
		// (same as the CLI's -cdn http(s):// path). Function scope on purpose:
		// Options.Downloader is a raw pointer the orchestrator keeps using.
		UnrealGamePatcher::FHttpChunkDownloader Downloader;
		const bool bCdnIsHttp = Cdn.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase) ||
			Cdn.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase);

		UnrealGamePatcher::FUpdateOptions Options;
		Options.ChunkCacheDir = InstallDir / TEXT(".ugp") / TEXT("chunkcache");
		Options.Events = [&Sink](const UnrealGamePatcher::FUpdateEvent& Ev)
		{
			Sink(Ev);
		};
		if (bCdnIsHttp)
		{
			// Force-load FHttpModule on this thread first (see downloadtest).
			FHttpModule::Get();
			Options.Downloader = &Downloader;
			Options.BaseUrl = Cdn;
		}

		UnrealGamePatcher::FUpdateStats Stats;
		const bool bOk = UnrealGamePatcher::FUpdateOrchestrator::UpdateWithOptions(
			Cdn, Version, InstallDir, /*bApply=*/true, Options, Stats);
		Lock.Release();
		GLog->Flush();

		if (!bOk)
		{
			// Keep the engine alive so the Java retry can re-run in-process
			// (a second PreInit in one process is unsupported).
			return JNI_FALSE;
		}
		// Success: same shutdown sequence as the standalone patcher's entry.
		RequestEngineExit(TEXT("UnrealGamePatcherLib Exiting"));
		FEngineLoop::AppPreExit();
		FModuleManager::Get().UnloadModulesAtShutdown();
		FEngineLoop::AppExit();
		return JNI_TRUE;
	}
}

// PatchActivity calls System.loadLibrary("ugp") then binds via these names.
// Kept as plain Java_ exports (no RegisterNatives) so the Java side stays
// free of engine-only glue.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_epicgames_unreal_patch_PatchActivity_nativeRunPatch(
	JNIEnv* Env, jclass Cls, jstring JCmdline, jstring JCdn,
	jstring JVersion, jstring JInstallDir, jstring JInternalDir, jobject JAssets, jobject JListener)
{
	return RunPatch(Env, Cls, JCmdline, JCdn, JVersion, JInstallDir, JInternalDir, JAssets, JListener);
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
// activity binding, no APK asset manager (UGP reads regular files only),
// no virtual keyboard, no meta-data/intent access. If the link ever reports
// more missing glue symbols, extend this block the same way.
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
