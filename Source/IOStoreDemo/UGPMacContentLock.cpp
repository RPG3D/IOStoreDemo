// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if PLATFORM_MAC && !WITH_EDITOR

#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealGamePatcherPaths.h"

#include <crt_externs.h>
#include <cstdlib>

void LockUGPMacContentBeforeMount()
{
	// PreMainInit runs before FCommandLine is populated and before Pak mount.
	TArray<FString> InstallDirs;
	for (int Index = 1; Index < *_NSGetArgc(); ++Index)
	{
		FString Argument(UTF8_TO_TCHAR((*_NSGetArgv())[Index]));
		if (Argument.RemoveFromStart(TEXT("-ugpinstall=")))
		{
			InstallDirs.AddUnique(Argument.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(Argument));
		}
#if !UE_BUILD_SHIPPING
		else if (Argument.RemoveFromStart(TEXT("-pakdir=")))
		{
			TArray<FString> PakDirs;
			Argument.ParseIntoArray(PakDirs, TEXT("*"), true);
			for (const FString& PakDir : PakDirs)
			{
				const FString Root = FPaths::ConvertRelativePathToFull(PakDir);
				if (FPaths::DirectoryExists(Root / TEXT(".ugp")))
				{
					InstallDirs.AddUnique(Root);
				}
			}
		}
#endif
	}

	static TArray<TUniquePtr<UnrealGamePatcher::Paths::FInstallLock>> ContentLocks;
	for (const FString& InstallDir : InstallDirs)
	{
		TUniquePtr<UnrealGamePatcher::Paths::FInstallLock> Lock = MakeUnique<UnrealGamePatcher::Paths::FInstallLock>();
		if (InstallDir.IsEmpty() || !FPaths::DirectoryExists(InstallDir / TEXT(".ugp")) || !Lock->AcquireReader(InstallDir))
		{
			FPlatformMisc::LowLevelOutputDebugStringf(TEXT("UGP-9001: Cannot lock '%s' for game startup.\n"), *InstallDir);
			::_Exit(EXIT_FAILURE);
		}
		ContentLocks.Add(MoveTemp(Lock));
		UE_LOG(LogTemp, Display, TEXT("UGP Mac: locked '%s' for game startup"), *InstallDir);
	}
}

#endif // PLATFORM_MAC && !WITH_EDITOR
