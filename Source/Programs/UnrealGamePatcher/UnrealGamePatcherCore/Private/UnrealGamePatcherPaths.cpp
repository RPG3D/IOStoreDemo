// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealGamePatcherPaths.h"
#include "Hash/FPatcherHash.h"
#include "UnrealGamePatcherLog.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Windows/WindowsHWrapper.h"

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
		if (MutexHandle)
		{
			return true;
		}
		// Case-fold + slash-normalize so different spellings of the same
		// directory map to one lock; the 64-bit hash keeps the kernel object
		// name short and free of path characters.
		FString Key = InstallDir;
		Key.ReplaceInline(TEXT("/"), TEXT("\\"));
		Key.ToUpperInline();
		const uint64 NameHash = FPatcherHash::XXH64(Key.GetCharArray().GetData(), Key.Len() * sizeof(TCHAR));
		const FString MutexName = FString::Printf(TEXT("Local\\UnrealGamePatcher_Install_%016llx"), NameHash);
		void* Handle = CreateMutexW(nullptr, 0, *MutexName);
		if (!Handle)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("Paths: cannot create install lock for '%s' (win error %u)"), *InstallDir, GetLastError());
			return false;
		}
		if (GetLastError() == ERROR_ALREADY_EXISTS)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("Paths: another patcher instance is working on '%s' - refusing to run concurrently"), *InstallDir);
			CloseHandle((HANDLE)Handle);
			return false;
		}
		MutexHandle = Handle;
		return true;
	}

	void FInstallLock::Release()
	{
		if (MutexHandle)
		{
			CloseHandle((HANDLE)MutexHandle);
			MutexHandle = nullptr;
		}
	}
}
