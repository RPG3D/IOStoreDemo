// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealGamePatcherPaths.h"

#if PLATFORM_MAC

#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UnrealGamePatcherLog.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace UnrealGamePatcher::Paths
{
	bool ResolveMacPath(const FString& Path, FString& OutPath)
	{
		if (Path.IsEmpty())
		{
			return false;
		}
		FString Existing = FPaths::ConvertRelativePathToFull(Path);
		while (Existing.Len() > 1 && Existing.RemoveFromEnd(TEXT("/")))
		{
		}
		FString Missing;
		for (;;)
		{
			const FTCHARToUTF8 NativePath(*Existing);
			char Resolved[PATH_MAX];
			if (realpath(NativePath.Get(), Resolved))
			{
				OutPath = UTF8_TO_TCHAR(Resolved);
				if (!Missing.IsEmpty())
				{
					OutPath /= Missing;
				}
				return true;
			}
			// Missing components are allowed for a fresh install. A dangling
			// link is an existing entry and cannot be used as a missing path.
			struct stat Info;
			if (errno != ENOENT || Existing == TEXT("/") || lstat(NativePath.Get(), &Info) == 0 || errno != ENOENT)
			{
				return false;
			}
			const FString Leaf = FPaths::GetCleanFilename(Existing);
			Missing = Missing.IsEmpty() ? Leaf : Leaf / Missing;
			Existing = FPaths::GetPath(Existing);
			if (Existing.IsEmpty())
			{
				Existing = TEXT("/");
			}
		}
	}

	namespace
	{
		constexpr int SearchDirectoryFlags = O_SEARCH | O_CLOEXEC | O_NOFOLLOW;
		constexpr int ReadDirectoryFlags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;

		bool IsInsideAppBundle(FString Path)
		{
			while (!Path.IsEmpty())
			{
				if (Path.EndsWith(TEXT(".app"), ESearchCase::IgnoreCase))
				{
					return true;
				}
				Path = FPaths::GetPath(Path);
			}
			return false;
		}

		bool ValidateTree(int DirFd, const FString& Path, bool bRoot, FString& Error)
		{
			// Each descent opens the directory relative to its parent without
			// following links. The walk includes the updater's own metadata.
			const int ReadFd = openat(DirFd, ".", ReadDirectoryFlags);
			DIR* Dir = ReadFd < 0 ? nullptr : fdopendir(ReadFd);
			if (!Dir)
			{
				if (ReadFd >= 0)
				{
					close(ReadFd);
				}
				Error = FString::Printf(TEXT("Could not inspect the install directory '%s'."), *Path);
				return false;
			}
			ON_SCOPE_EXIT { closedir(Dir); };

			for (;;)
			{
				errno = 0;
				const dirent* Entry = readdir(Dir);
				if (!Entry)
				{
					if (errno == 0)
					{
						return true;
					}
					Error = FString::Printf(TEXT("Could not read the install directory '%s'."), *Path);
					return false;
				}
				const char* Name = Entry->d_name;
				if (std::strcmp(Name, ".") == 0 || std::strcmp(Name, "..") == 0)
				{
					continue;
				}

				const FString EntryName(UTF8_TO_TCHAR(Name));
				const FString EntryPath = Path / EntryName;
				struct stat Info;
				if (fstatat(DirFd, Name, &Info, AT_SYMLINK_NOFOLLOW) != 0 || (!S_ISDIR(Info.st_mode) && !S_ISREG(Info.st_mode)))
				{
					Error = FString::Printf(TEXT("The install tree contains a symbolic link or unsupported entry: '%s'."), *EntryPath);
					return false;
				}
				if (!bRoot && EntryName.Equals(TEXT(".ugp"), ESearchCase::IgnoreCase))
				{
					Error = FString::Printf(TEXT("The install tree overlaps another UGP install: '%s'."), *Path);
					return false;
				}
				if (S_ISDIR(Info.st_mode))
				{
					const int Child = openat(DirFd, Name, SearchDirectoryFlags);
					if (Child < 0)
					{
						Error = FString::Printf(TEXT("Could not open the install directory '%s'."), *EntryPath);
						return false;
					}
					ON_SCOPE_EXIT { close(Child); };
					if (!ValidateTree(Child, EntryPath, false, Error))
					{
						return false;
					}
				}
			}
		}

		bool LockWriterDirectories(const FString& InstallDir, TArray<int>& Locks, FString& Error)
		{
			TArray<FString> Components;
			InstallDir.ParseIntoArray(Components, TEXT("/"), true);
			FString Path = TEXT("/");
			int DirFd = open("/", SearchDirectoryFlags);
			for (int32 Index = 0; Index <= Components.Num(); ++Index)
			{
				const bool bRoot = Index == Components.Num();
				if (DirFd < 0)
				{
					Error = FString::Printf(TEXT("Could not open the install path '%s'."), *Path);
					return false;
				}
				Locks.Add(DirFd);
				// A writer excludes every writer rooted beneath this directory.
				// Ancestor locks are shared, so sibling installs remain independent.
				if (flock(DirFd, (bRoot ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0)
				{
					Error = FString::Printf(TEXT("The install path '%s' is in use."), *Path);
					return false;
				}
				if (bRoot)
				{
					return ValidateTree(DirFd, Path, true, Error);
				}

				struct stat Info;
				if (fstatat(DirFd, ".ugp", &Info, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
				{
					Error = FString::Printf(TEXT("The install path overlaps another UGP install: '%s'."), *Path);
					return false;
				}
				const FTCHARToUTF8 Name(*Components[Index]);
				const int Parent = DirFd;
				DirFd = openat(Parent, Name.Get(), SearchDirectoryFlags);
				if (DirFd < 0 && errno == ENOENT &&
					(mkdirat(Parent, Name.Get(), 0777) == 0 || errno == EEXIST))
				{
					DirFd = openat(Parent, Name.Get(), SearchDirectoryFlags);
				}
				Path /= Components[Index];
			}
			return false;
		}
	}

	bool FInstallLock::AcquireReader(const FString& InstallDir)
	{
		return AcquireMac(InstallDir, true);
	}

	bool FInstallLock::AcquireMac(const FString& InstallDir, bool bReader)
	{
		const auto Fail = [this](FString Message)
		{
			MacFailureDetail = MoveTemp(Message);
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("Paths: %s"), *MacFailureDetail);
			return false;
		};
		if (PosixLockFd >= 0)
		{
			return Fail(TEXT("The install lock is already held."));
		}

		FString Root;
		if (!ResolveMacPath(InstallDir, Root) || Root == TEXT("/") || IsInsideAppBundle(Root) ||
			IsInsideAppBundle(FPaths::ConvertRelativePathToFull(InstallDir)))
		{
			return Fail(FString::Printf(TEXT("Invalid content install directory '%s'."), *InstallDir));
		}

		TArray<int> DirectoryLocks;
		ON_SCOPE_EXIT
		{
			for (int32 Index = DirectoryLocks.Num() - 1; Index >= 0; --Index)
			{
				close(DirectoryLocks[Index]);
			}
		};
		int RootFd;
		if (bReader)
		{
			RootFd = open(TCHAR_TO_UTF8(*Root), O_SEARCH | O_CLOEXEC | O_NOFOLLOW_ANY);
			if (RootFd < 0)
			{
				return Fail(FString::Printf(TEXT("Could not open the content directory '%s'."), *Root));
			}
		}
		else
		{
			FString Error;
			if (!LockWriterDirectories(Root, DirectoryLocks, Error))
			{
				return Fail(MoveTemp(Error));
			}
			RootFd = DirectoryLocks.Last();
		}
		ON_SCOPE_EXIT { if (bReader) { close(RootFd); } };

		if (!bReader && mkdirat(RootFd, ".ugp", 0777) != 0 && errno != EEXIST)
		{
			return Fail(FString::Printf(TEXT("Could not create the install metadata directory in '%s'."), *Root));
		}
		const int Metadata = openat(RootFd, ".ugp", SearchDirectoryFlags);
		if (Metadata < 0)
		{
			return Fail(FString::Printf(TEXT("Could not open the install metadata directory in '%s'."), *Root));
		}
		ON_SCOPE_EXIT { close(Metadata); };

		// The game reader keeps the existing per-install lock protocol. It
		// only opens metadata in an already established install directory.
		const int Fd = openat(Metadata, "install.lock",
			(bReader ? O_RDONLY : O_WRONLY) | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0666);
		if (Fd < 0)
		{
			return Fail(FString::Printf(TEXT("Could not open the install lock in '%s'."), *Root));
		}
		struct stat Info;
		if (fstat(Fd, &Info) != 0 || !S_ISREG(Info.st_mode) || flock(Fd, (bReader ? LOCK_SH : LOCK_EX) | LOCK_NB) != 0)
		{
			close(Fd);
			return Fail(FString::Printf(TEXT("Could not lock '%s' for %s."), *Root, bReader ? TEXT("reading") : TEXT("updating")));
		}

		PosixLockFd = Fd;
		MacDirectoryLocks = MoveTemp(DirectoryLocks);
		MacInstallDir = MoveTemp(Root);
		MacFailureDetail.Reset();
		return true;
	}
}

#endif // PLATFORM_MAC
