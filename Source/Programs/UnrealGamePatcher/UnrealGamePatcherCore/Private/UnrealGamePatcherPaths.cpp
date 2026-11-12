// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealGamePatcherPaths.h"
#include "Hash/FPatcherHash.h"
#include "UnrealGamePatcherLog.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/Timespan.h"
#if PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC
	#include <fcntl.h>
	#include <sys/file.h>
	#include <unistd.h>
	#include <cerrno>
#endif

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

	bool FInstallLock::Acquire(const FString& InstallDir, const FString& LockRootDir)
	{
#if PLATFORM_MAC
		return AcquireMac(InstallDir, false);
#elif PLATFORM_ANDROID || PLATFORM_IOS
		// Android: UE::FPlatformSystemWideMutex's POSIX implementation puts the
		// lock file under FPlatformProcess::ApplicationSettingsDir(), which
		// Android does not override - it degenerates to RootDir("/"), where an
		// app cannot create files. Lock a file under LockRootDir instead (the
		// caller passes the app's INTERNAL files dir: the emulated external
		// storage is FUSE and silently does not support flock).
		// iOS uses the app sandbox directly. flock releases on process death.
		if (PosixLockFd >= 0)
		{
			return true;
		}
		const FString LockDir = LockRootDir.IsEmpty() ? (InstallDir / TEXT(".ugp")) : (LockRootDir / TEXT("UnrealGamePatcher"));
		FString Key = InstallDir;
		Key.ReplaceInline(TEXT("\\"), TEXT("/"));
		Key.ToUpperInline();
		const uint64 NameHash = FPatcherHash::XXH64(Key.GetCharArray().GetData(), Key.Len() * sizeof(TCHAR));
		const FString LockPath = FPaths::Combine(LockDir,
			FString::Printf(TEXT("install.lock.%016llx"), NameHash));
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (!PF.CreateDirectoryTree(*FPaths::GetPath(LockPath)))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("Paths: cannot create the lock directory '%s' (lock file for '%s')"),
				*FPaths::GetPath(LockPath), *InstallDir);
			return false;
		}
		const int Fd = open(TCHAR_TO_UTF8(*LockPath), O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
		if (Fd < 0)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("Paths: cannot open the install lock file '%s' (errno %d)"), *LockPath, errno);
			return false;
		}
		if (flock(Fd, LOCK_EX | LOCK_NB) != 0)
		{
			close(Fd);
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("Paths: another patcher instance is working on '%s' - refusing to run concurrently"), *InstallDir);
			return false;
		}
		PosixLockFd = Fd;
		return true;
#else
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
#endif
	}

	void FInstallLock::Release()
	{
#if PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC
		if (PosixLockFd >= 0)
		{
			flock(PosixLockFd, LOCK_UN);
			close(PosixLockFd);
			PosixLockFd = -1;
		}
#endif
#if PLATFORM_MAC
		for (int Index = MacDirectoryLocks.Num() - 1; Index >= 0; --Index)
		{
			close(MacDirectoryLocks[Index]);
		}
		MacDirectoryLocks.Reset();
		MacInstallDir.Reset();
#endif
		if (Mutex.IsSet())
		{
			Mutex->Release();
			Mutex.Reset();
		}
	}
}
