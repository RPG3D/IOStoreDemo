// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealGamePatcherPaths.h"
#include "Hash/FPatcherHash.h"
#include "UnrealGamePatcherLog.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/Timespan.h"

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

	FInstallLock::~FInstallLock()
	{
		Release();
	}

	bool FInstallLock::Acquire(const FString& InstallDir)
	{
		if (Mutex.IsSet())
		{
			return true;
		}
		// Case-fold + slash-normalize so different spellings of the same
		// directory map to one lock; the 64-bit hash keeps the lock name
		// short and free of path characters. The normalization itself is
		// platform-independent, so one directory maps to the same lock name
		// on every platform.
		FString Key = InstallDir;
		Key.ReplaceInline(TEXT("\\"), TEXT("/"));
		Key.ToUpperInline();
		const uint64 NameHash = FPatcherHash::XXH64(Key.GetCharArray().GetData(), Key.Len() * sizeof(TCHAR));
#if PLATFORM_WINDOWS
		// The "Local/" prefix selects the Windows session-local namespace.
		const FString LockName = FString::Printf(TEXT("Local/UnrealGamePatcher_Install_%016llx"), NameHash);
#else
		// POSIX: the name becomes a lock file in the per-user settings dir
		// (engine-managed FPlatformSystemWideMutex = exclusive flock).
		const FString LockName = FString::Printf(TEXT("UnrealGamePatcher_Install_%016llx"), NameHash);
#endif
		// FTimespan::Zero() = single try-lock attempt (non-blocking).
		Mutex.Emplace(LockName, FTimespan::Zero());
		if (!Mutex->IsValid())
		{
			Mutex.Reset();
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("Paths: cannot acquire the install lock for '%s' - another patcher instance may be working on this directory"), *InstallDir);
			return false;
		}
		return true;
	}

	void FInstallLock::Release()
	{
		if (Mutex.IsSet())
		{
			Mutex->Release();
			Mutex.Reset();
		}
	}
}
