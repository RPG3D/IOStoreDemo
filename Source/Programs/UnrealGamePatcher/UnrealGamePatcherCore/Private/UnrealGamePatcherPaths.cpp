// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealGamePatcherPaths.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

namespace UnrealGamePatcher::Paths
{
	FString GetStatePath(const FString& InstallDir)
	{
		return FPaths::Combine(*InstallDir, TEXT("UnrealGamePatcher.state.json"));
	}

	bool AtomicReplaceFile(const FString& SrcPath, const FString& DestPath)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		const FString PrevPath = DestPath + TEXT(".prev");
		PF.DeleteFile(*PrevPath);
		const bool bHadPrev = PF.FileExists(*DestPath) && PF.MoveFile(*PrevPath, *DestPath);
		// MoveFile cannot replace an existing destination, which is why the
		// previous file was renamed aside first - DestPath is now free.
		if (!PF.MoveFile(*DestPath, *SrcPath))
		{
			// Src could not be moved in - restore the previous file so the
			// caller sees the old content, never a missing file.
			if (bHadPrev)
			{
				PF.MoveFile(*DestPath, *PrevPath);
			}
			return false;
		}
		if (bHadPrev)
		{
			PF.DeleteFile(*PrevPath);
		}
		return true;
	}
}
