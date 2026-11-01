// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealGamePatcherApp.h"
#include "RequiredProgramMainCPPInclude.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#elif PLATFORM_MAC
#include "Mac/CocoaThread.h"
#include "Apple/ApplePlatformMisc.h"
#include "HAL/PlatformMisc.h"
#endif
#include "Modules/ModuleManager.h"
#include "Misc/CommandLine.h"
#include "UnrealGamePatcherCLI.h"
#include "UnrealGamePatcherGUI.h"

IMPLEMENT_APPLICATION(UnrealGamePatcher, "UnrealGamePatcher");

namespace
{
	/**
	 * Shared engine-loop startup + CLI/GUI dispatch + shutdown, in the exact
	 * order the original WinMain used (Windows behavior is byte-identical).
	 * Returns the process exit code.
	 */
	int32 RunUnrealGamePatcherEntry(const TCHAR* CmdLine)
	{
		// The CLI/GUI entries parse with FParse straight off the raw line; the
		// caller hands over the command line without the executable token.
		int32 Result = 0;
		if (FParse::Param(CmdLine, TEXT("cli")))
		{
			Result = RunUnrealGamePatcherCLI(CmdLine);
		}
		else
		{
			Result = RunUnrealGamePatcherGUI(CmdLine);
		}

		GLog->Flush();
		RequestEngineExit(TEXT("UnrealGamePatcher Exiting"));
		FEngineLoop::AppPreExit();
		FModuleManager::Get().UnloadModulesAtShutdown();
		FEngineLoop::AppExit();
		return Result;
	}

	/** Loopback HTTP (local test CDNs) must never route through a system proxy. */
	void DisableProxyForLoopback()
	{
		// proxy tools hijack 127.0.0.1 and the CDN becomes unreachable (HTTP
		// 502). libcurl honors NO_PROXY even when it picks up the WinINET
		// system proxy - exempt just the loopback; remote CDNs keep the proxy.
		// FPlatformMisc::SetEnvironmentVar is the engine's cross-platform
		// wrapper (SetEnvironmentVariableW on Windows, setenv on POSIX).
		FPlatformMisc::SetEnvironmentVar(TEXT("NO_PROXY"), TEXT("127.0.0.1,localhost"));
	}
}

#if PLATFORM_WINDOWS

/**
 * WinMain, called when the application is started.
 *
 * This is the GUI-subsystem entry point (matches SlateViewerMainWindows.cpp):
 * the program links no Launch Windows startup (LaunchWindows.cpp is excluded for
 * Program targets), so we provide WinMain ourselves and dispatch to the shared
 * entry points in CLI / GUI modules based on the command line.
 */
int WINAPI WinMain(_In_ HINSTANCE hInInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR, _In_ int nCmdShow)
{
	FTaskTagScope Scope(ETaskTag::EGameThread);
	DisableProxyForLoopback();
	GEngineLoop.PreInit(GetCommandLineW());
	ProcessNewlyLoadedUObjects();
	FModuleManager::Get().StartProcessingNewlyLoadedObjects();

	const TCHAR* CmdLine = FCommandLine::RemoveExeName(GetCommandLineW());
	return RunUnrealGamePatcherEntry(CmdLine);
}

#elif PLATFORM_MAC

/**
 * Mac entry, following the CrashReportClient pattern (the engine's reference
 * for GUI Slate Program targets shipping on macOS): CLI mode is a plain
 * console flow on the main thread, GUI mode runs the Slate UI on the game
 * thread inside the Cocoa event loop.
 */
static FString GMacCommandLine;

@interface UnrealGamePatcherAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation UnrealGamePatcherAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)Notification
{
	RunGameThread(self, @selector(runGameThread:));
}

- (void)runGameThread:(id)Arg
{
	FTaskTagScope Scope(ETaskTag::EGameThread);
	DisableProxyForLoopback();
	GEngineLoop.PreInit(*GMacCommandLine);
	ProcessNewlyLoadedUObjects();
	FModuleManager::Get().StartProcessingNewlyLoadedObjects();
	RunUnrealGamePatcherEntry(*GMacCommandLine);

	dispatch_async(dispatch_get_main_queue(), ^{
		[NSApp terminate: nil];
	});
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)Sender
{
	return NSTerminateNow;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)app
{
	return false;
}

@end

int main(int argc, char* argv[])
{
	// Build the command line from argv (without the executable token). Values
	// containing spaces are re-quoted as name="value" for FParse::Token (same
	// rule as CrashReportClient's Mac main).
	for (int32 Option = 1; Option < argc; Option++)
	{
		FString Argument(ANSI_TO_TCHAR(argv[Option]));
		if (Argument.Contains(TEXT(" ")))
		{
			if (Argument.Contains(TEXT("=")))
			{
				FString ArgName;
				FString ArgValue;
				Argument.Split(TEXT("="), &ArgName, &ArgValue);
				Argument = FString::Printf(TEXT("%s=\"%s\""), *ArgName, *ArgValue);
			}
			else
			{
				Argument = FString::Printf(TEXT("\"%s\""), *Argument);
			}
		}
		GMacCommandLine += TEXT(" ");
		GMacCommandLine += Argument;
	}

	// CLI mode: pure console flow, no Cocoa event loop.
	const TCHAR* CmdLine = *GMacCommandLine;
	if (FParse::Param(CmdLine, TEXT("cli")))
	{
		FTaskTagScope Scope(ETaskTag::EGameThread);
		DisableProxyForLoopback();
		GEngineLoop.PreInit(*GMacCommandLine);
		ProcessNewlyLoadedUObjects();
		FModuleManager::Get().StartProcessingNewlyLoadedObjects();
		return RunUnrealGamePatcherEntry(*GMacCommandLine);
	}

	// GUI mode: Slate runs on the game thread inside the Cocoa run loop.
	SCOPED_AUTORELEASE_POOL;
	[NSApplication sharedApplication];
	[NSApp setDelegate:[UnrealGamePatcherAppDelegate new]];
	[NSApp run];
	return 0;
}

#endif
