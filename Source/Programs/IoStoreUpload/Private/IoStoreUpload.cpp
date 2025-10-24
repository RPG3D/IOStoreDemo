// Copyright Epic Games, Inc. All Rights Reserved.

#include "IoStoreUpload.h"
#include "RequiredProgramMainCPPInclude.h"
#include "PakFileUtilities.h"
#include "IPlatformFilePak.h"
#include "ProjectUtilities.h"

IMPLEMENT_APPLICATION(IoStoreUpload, "IoStoreUpload");

INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
{
	FTaskTagScope Scope(ETaskTag::EGameThread);

	// Allows this program to accept a project argument on the commandline and use project-specific config
	UE::ProjectUtilities::ParseProjectDirFromCommandline(ArgC, ArgV);

	// start up the main loop,
	// add -nopak since we never want to pick up and mount any existing pak files from the project directory
	GEngineLoop.PreInit(ArgC, ArgV, TEXT("-nopak"));

	double StartTime = FPlatformTime::Seconds();

	int32 Result = 0;

	UE_LOG(LogPakFile, Display, TEXT("IoStoreUpload executed in %f seconds"), FPlatformTime::Seconds() - StartTime);


	GLog->Flush();

	RequestEngineExit(TEXT("IoStoreUpload Exiting"));

	FEngineLoop::AppPreExit();
	FModuleManager::Get().UnloadModulesAtShutdown();
	FEngineLoop::AppExit();

	return Result;
}

