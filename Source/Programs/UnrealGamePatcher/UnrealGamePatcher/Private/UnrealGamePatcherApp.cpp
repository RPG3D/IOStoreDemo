// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealGamePatcherApp.h"
#include "RequiredProgramMainCPPInclude.h"
#include "Windows/WindowsHWrapper.h"
#include "Modules/ModuleManager.h"
#include "Misc/CommandLine.h"
#include "UnrealGamePatcherCLI.h"
#include "UnrealGamePatcherGUI.h"

IMPLEMENT_APPLICATION(UnrealGamePatcher, "UnrealGamePatcher");

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
	// Loopback HTTP (local test CDNs) must never route through a system
	// proxy: proxy tools hijack 127.0.0.1 and the CDN becomes unreachable
	// (HTTP 502). libcurl honors NO_PROXY even when it picks up the WinINET
	// system proxy - exempt just the loopback; remote CDNs keep the proxy.
	SetEnvironmentVariableW(TEXT("NO_PROXY"), TEXT("127.0.0.1,localhost"));
	GEngineLoop.PreInit(GetCommandLineW());
	ProcessNewlyLoadedUObjects();
	FModuleManager::Get().StartProcessingNewlyLoadedObjects();

	// The CLI/GUI entries parse with FParse straight off the raw line; hand
	// them the command line without the leading executable token.
	const TCHAR* CmdLine = FCommandLine::RemoveExeName(GetCommandLineW());

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
