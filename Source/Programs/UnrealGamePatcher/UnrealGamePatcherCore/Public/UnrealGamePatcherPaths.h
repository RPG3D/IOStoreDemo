// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformMutex.h"
#include "Misc/Optional.h"

/**
 * Well-known file / directory layout helpers for the patcher.
 *
 * Local install sidecar (one per install dir, design D5):
 *   <install>/UnrealGamePatcher.state.json
 */
namespace UnrealGamePatcher::Paths
{
	// --- Local install sidecar --------------------------------------------
	UNREALGAMEPATCHERCORE_API FString GetStatePath(const FString& InstallDir);

#if PLATFORM_MAC
	// Resolve aliases through the deepest existing parent, including for fresh installs.
	UNREALGAMEPATCHERCORE_API bool ResolveMacPath(const FString& Path, FString& OutPath);
#endif

	/** Atomically replaces DestPath with SrcPath (both full paths): the
	 *  existing DestPath is first renamed aside to DestPath + ".prev", SrcPath
	 *  is moved into place, then the .prev copy is deleted. Every step is
	 *  recoverable - a failure moves the previous file back, so the classic
	 *  delete-then-move window that could lose the only good copy (crash or
	 *  transient MoveFile failure in between; UE's MoveFile cannot replace an
	 *  existing destination) is closed. Returns false, with DestPath left
	 *  holding its previous content, when SrcPath could not be moved in. */
	UNREALGAMEPATCHERCORE_API bool AtomicReplaceFile(const FString& SrcPath, const FString& DestPath);

	/** Single-writer guard for one install directory (fixplan L-M5): a named
	 *  system-wide mutex derived from the case-normalized install path, built
	 *  on the engine's UE::FPlatformSystemWideMutex (named kernel mutex on
	 *  Windows and flock on POSIX platforms). On Mac, readers and writers
	 *  lock the same file under the install directory. The OS
	 *  releases it on process death, so there is no stale-lock state to
	 *  recover - a crashed run never blocks the next one. */
	struct UNREALGAMEPATCHERCORE_API FInstallLock
	{
		FInstallLock() = default;
		~FInstallLock();
		FInstallLock(const FInstallLock&) = delete;
		FInstallLock& operator=(const FInstallLock&) = delete;

		/** Takes the writer lock; returns false when the directory is in use.
		 *  Re-entrant within one process is NOT supported
		 *  - a second Acquire on the same directory fails by design.
		 *  LockRootDir (Android/iOS) selects the lock file's directory.
		 *  On Android, use the app's internal files dir for POSIX locks. */
		bool Acquire(const FString& InstallDir, const FString& LockRootDir = FString());
#if PLATFORM_MAC
		bool AcquireReader(const FString& InstallDir);
		const FString& GetMacInstallDir() const { return MacInstallDir; }
		const FString& GetMacFailureDetail() const { return MacFailureDetail; }
		bool IsMacWriter() const { return !MacDirectoryLocks.IsEmpty(); }
#endif
		void Release();

	private:
#if PLATFORM_MAC
		bool AcquireMac(const FString& InstallDir, bool bReader);
		TArray<int> MacDirectoryLocks;
		FString MacInstallDir;
		FString MacFailureDetail;
#endif
		TOptional<UE::FPlatformSystemWideMutex> Mutex;
		int PosixLockFd = -1;
	};
}
