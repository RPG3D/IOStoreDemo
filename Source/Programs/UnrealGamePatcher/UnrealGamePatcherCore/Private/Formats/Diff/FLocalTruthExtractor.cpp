// Copyright Epic Games, Inc. All Rights Reserved.

#include "Formats/Diff/FLocalTruthExtractor.h"
#include "Formats/IoStore/FIoStoreReader.h"
#include "Formats/Pak/FPakReader.h"
#include "Hash/FPatcherHash.h"
#include "UnrealGamePatcherLog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UnrealGamePatcher
{
	namespace
	{
		bool IsAllZero(const uint8* Data, int32 Len)
		{
			for (int32 I = 0; I < Len; ++I)
			{
				if (Data[I] != 0) return false;
			}
			return true;
		}

		/** Parses one pak file into OutEntry.PakEntries: mount-relative path,
		 *  stored/uncompressed sizes and the per-entry SHA1. Trust mode
		 *  (bVerify=false) reads the pak's BUILT-IN hash (payload header; the
		 *  encoded index stores no hash); verify mode RECOMPUTES it from the
		 *  payload bytes (decompressing if needed) so a damaged payload whose
		 *  stored-hash header is intact is still caught. An unparseable pak
		 *  leaves the entry empty - the diff then classifies it Changed and
		 *  rebuilds it, instead of aborting the whole update. */
		void ExtractPakEntries(const FString& PakPath, FIndexFileEntry& OutEntry, bool bVerify)
		{
			FPakReader Reader;
			if (!Reader.Open(PakPath) || !Reader.ReadIndex())
			{
				UE_LOG(LogUnrealGamePatcher, Warning,
					TEXT("FLocalTruthExtractor: cannot parse '%s' - no inner truth, flagged for rebuild"), *PakPath);
				return;
			}
			const FString MountPoint = Reader.GetMountPoint();
			for (const FPakReader::FPakEntryLocation& Loc : Reader.GetFiles())
			{
				FString Rel = Loc.Path;
				if (!MountPoint.IsEmpty() && Rel.StartsWith(MountPoint))
				{
					Rel = Rel.RightChop(MountPoint.Len());
				}
				FIndexPakEntry PE;
				PE.Path = Rel;
				PE.OffsetInFile = Loc.Entry.Offset;
				PE.StoredSize = Loc.Entry.Size;
				PE.UncompressedSize = Loc.Entry.UncompressedSize;
				uint8 Hash[20] = { 0 };
				if (bVerify)
				{
					if (!Reader.ComputePayloadHash(Loc.Entry, Hash))
					{
						// Unreadable payload: empty hash marks the entry Changed.
						PE.SHA1 = FString();
						OutEntry.PakEntries.Add(PE);
						continue;
					}
				}
				else
				{
					bool bHaveHash = false;
					if (Loc.Entry.HasValidHash())
					{
						FMemory::Memcpy(Hash, Loc.Entry.Hash, 20);
						bHaveHash = true;
					}
					else if (Reader.ReadPayloadHash(Loc.Entry, Hash) && !IsAllZero(Hash, 20))
					{
						bHaveHash = true;
					}
					if (!bHaveHash && !Reader.ComputePayloadHash(Loc.Entry, Hash))
					{
						PE.SHA1 = FString();
						OutEntry.PakEntries.Add(PE);
						continue;
					}
				}
				PE.SHA1 = FPatcherHash::BytesToHex(Hash, 20);
				OutEntry.PakEntries.Add(PE);
			}
		}
	}

	int32 FLocalTruthExtractor::ExtractPaks(const FString& PakDir, FIndexManifest& OutTruth)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (!PF.DirectoryExists(*PakDir))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FLocalTruthExtractor: pak directory not found: '%s'"), *PakDir);
			return -1;
		}

		int32 ParsedCount = 0;
		TArray<FString> PakFiles;
		IFileManager::Get().FindFiles(PakFiles, *(PakDir / TEXT("*.pak")), true, false);

		for (const FString& PakName : PakFiles)
		{
			const FString PakPath = PakDir / PakName;

			FPakReader Reader;
			if (!Reader.Open(PakPath) || !Reader.ReadIndex())
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FLocalTruthExtractor: failed to parse '%s'"), *PakPath);
				return -1;
			}

			// The mount point is a directory prefix baked into the pak. We store
			// paths relative to it so the same logical file has the same id in
			// the local truth and the remote manifest.
			const FString MountPoint = Reader.GetMountPoint();

			for (const FPakReader::FPakEntryLocation& Loc : Reader.GetFiles())
			{
				FString RelPath = Loc.Path;
				if (!MountPoint.IsEmpty() && RelPath.StartsWith(MountPoint))
				{
					RelPath = RelPath.RightChop(MountPoint.Len());
				}

				FIndexFileEntry E;
				E.Size = Loc.Entry.UncompressedSize > 0 ? Loc.Entry.UncompressedSize : Loc.Entry.Size;

				uint8 Hash[20] = { 0 };
				bool bHaveHash = false;
				if (Loc.Entry.HasValidHash())
				{
					FMemory::Memcpy(Hash, Loc.Entry.Hash, 20);
					bHaveHash = true;
				}
				else if (Reader.ReadPayloadHash(Loc.Entry, Hash) && !IsAllZero(Hash, 20))
				{
					bHaveHash = true;
				}

				if (!bHaveHash)
				{
					// Design D3 fallback: an entry without a built-in hash (old or
					// anomalous pak) gets a computed whole-payload hash so the
					// local truth always carries an integrity value.
					if (!Reader.ComputePayloadHash(Loc.Entry, Hash))
					{
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("FLocalTruthExtractor: no hash for '%s' in '%s'"), *RelPath, *PakPath);
						return -1;
					}
				}

				E.SHA1 = FPatcherHash::BytesToHex(Hash, 20);
				OutTruth.Files.Add(RelPath, E);
			}

			UE_LOG(LogUnrealGamePatcher, Display, TEXT("FLocalTruthExtractor: parsed '%s' (%d files, mount '%s')"),
				*PakPath, Reader.GetFiles().Num(), *MountPoint);
			++ParsedCount;
		}

		return ParsedCount;
	}

	bool FLocalTruthExtractor::HashWholeFile(const FString& Path, FString& OutSha1Hex, int64& OutSize)
	{
		OutSha1Hex = FPatcherHash::FileSHA1Hex(Path, &OutSize);
		if (OutSha1Hex.IsEmpty())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FLocalTruthExtractor: cannot hash '%s'"), *Path);
			return false;
		}
		return true;
	}

	bool FLocalTruthExtractor::ExtractAll(const FString& PakDir, FIndexManifest& OutTruth)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (!PF.DirectoryExists(*PakDir))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FLocalTruthExtractor: directory not found: '%s'"), *PakDir);
			return false;
		}

		const int32 PakCount = ExtractPaks(PakDir, OutTruth);
		if (PakCount < 0)
		{
			return false;
		}

		// Whole-file entries for every other regular file at the top level
		// (plain dll/exe and IoStore .ucas/.utoc until the FIoStoreReader lands).
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(PakDir / TEXT("*")), true, false);
		for (const FString& Name : Files)
		{
			const FString Path = PakDir / Name;
			{
				FString Sha1;
				int64 Size = 0;
				if (!HashWholeFile(Path, Sha1, Size))
				{
					return false;
				}
				FIndexFileEntry E;
				E.SHA1 = Sha1;
				E.Size = Size;
				OutTruth.Files.Add(Name, E);
			}
		}

		return true;
	}

	bool FLocalTruthExtractor::ExtractInstallNames(
		const FString& Dir,
		TSet<FString>& OutNames,
		TMap<FString, int64>& OutPlainSizes)
	{
		OutNames.Reset();
		OutPlainSizes.Reset();
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (!PF.DirectoryExists(*Dir))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FLocalTruthExtractor: install directory not found: '%s'"), *Dir);
			return false;
		}
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*"), true, false);
		for (const FString& Abs : Files)
		{
			const FString Name = Abs.RightChop(Dir.Len() + 1).Replace(TEXT("\\"), TEXT("/"));
			if (Name == TEXT("UnrealGamePatcher.state.json") || Name.StartsWith(TEXT(".ugp/")))
			{
				continue;
			}
			OutNames.Add(Name);
			const bool bIsContainer = Name.EndsWith(TEXT(".pak")) || Name.EndsWith(TEXT(".ucas")) || Name.EndsWith(TEXT(".utoc"));
			if (!bIsContainer)
			{
				// Container file sizes drift legitimately (in-place appends
				// change the physical layout); plain-file sizes do not.
				OutPlainSizes.Add(Name, IFileManager::Get().FileSize(*Abs));
			}
		}
		return true;
	}

	bool FLocalTruthExtractor::ExtractInstall(
		const FString& Dir,
		FIndexManifest& OutTruth,
		const FIndexManifest* TrustedIndex,
		bool bVerify)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (!PF.DirectoryExists(*Dir))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FLocalTruthExtractor: install directory not found: '%s'"), *Dir);
			return false;
		}

		// Scan the install RECURSIVELY: the manifest's file ids are the staged
		// game's relative paths (Engine/..., IOStoreDemo/...), so the local
		// truth must produce the same nested keys to diff against them.
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*"), true, false);
		for (const FString& Abs : Files)
		{
			const FString Name = Abs.RightChop(Dir.Len() + 1).Replace(TEXT("\\"), TEXT("/"));
			// Patcher metadata is not game content and must never be part of
			// the truth or be deleted as a "removed" file: the sidecar at the
			// install root (design D5) and the internal .ugp/ state dir.
			if (Name == TEXT("UnrealGamePatcher.state.json") || Name.StartsWith(TEXT(".ugp/")))
			{
				continue;
			}

			const bool bIsPak = Name.EndsWith(TEXT(".pak"));
			const bool bIsUcas = Name.EndsWith(TEXT(".ucas"));
			const bool bIsUtoc = Name.EndsWith(TEXT(".utoc"));

			FIndexFileEntry E;
			E.Size = IFileManager::Get().FileSize(*(Dir / Name));

			if (bIsUtoc)
			{
				// Container metadata: no truth of its own. The utoc is rewritten
				// atomically by in-place ucas patches, so its bytes legitimately
				// differ from the CDN copy - its status derives from the
				// sibling ucas (virtual-disk model).
				OutTruth.Files.Add(Name, E);
				continue;
			}

			// Trust mode: the last committed version's manifest may stand in for
			// the filesystem truth without re-reading the containers.
			const FIndexFileEntry* Trusted = (TrustedIndex && !bVerify) ? TrustedIndex->Files.Find(Name) : nullptr;

			if (bIsPak || bIsUcas)
			{
				// Container truth = inner sub-file hashes (per-entry SHA1 for
				// pak, per-chunk id for ucas) - NEVER the container whole-file
				// hash: in-place appends make the physical layout (and thus the
				// whole-file hash) differ per machine.
				if (Trusted && ((bIsPak && Trusted->PakEntries.Num() > 0) || (bIsUcas && Trusted->IoStoreChunks.Num() > 0)))
				{
					E.PakEntries = Trusted->PakEntries;
					E.IoStoreChunks = Trusted->IoStoreChunks;
				}
				else if (bIsPak)
				{
					ExtractPakEntries(Dir / Name, E, bVerify);
					if (E.PakEntries.Num() == 0 && E.SHA1.IsEmpty())
					{
						// An EMPTY pak (0 entries - e.g. a placeholder optional
						// container) has no inner truth to express, and the
						// diff's pak branch needs entries on BOTH sides before
						// it trusts per-entry hashes. Without a whole-file
						// hash here the entry falls to the plain SHA1 compare
						// with an EMPTY local hash - a phantom Changed that
						// never converges (check would flag a byte-perfect
						// file on every run; no mirror rule rescues paks).
						// The whole-file hash IS the truth for an empty pak;
						// a later in-place append changes it and correctly
						// routes the container through a rebuild.
						FString Sha;
						int64 Size = 0;
						if (HashWholeFile(Dir / Name, Sha, Size))
						{
							E.SHA1 = Sha;
						}
					}
				}
				else // ucas
				{
					const FString UtocName = Name.LeftChop(5) + TEXT(".utoc");
					FIoStoreReader Io;
					if (Io.Open(Dir / UtocName, Dir / Name))
					{
						for (const FIoStoreChunk& C : Io.GetChunks())
						{
							FIndexIoStoreChunk IC;
							IC.Id = C.Id;
							IC.RawOffset = C.RawOffset;
							IC.RawSize = C.RawSize;
						// The local container's content hash per chunk: the
						// diff compares (Id, MetaHash) pairs against the remote
						// manifest, which makes content edits inside existing
						// packages visible even when the engine's package-
						// derived chunk id is unchanged.
						IC.MetaHash = C.Hash;
							E.IoStoreChunks.Add(IC);
						}
					}
					// A container whose utoc cannot be parsed records no chunk
					// truth: the diff sees a chunk-set mismatch and rebuilds it.
				}
				OutTruth.Files.Add(Name, E);
				continue;
			}

			// Plain file: whole-file SHA1. Trust mode trusts a file whose size
			// matches the committed index (a size change is a change candidate).
			if (Trusted && Trusted->Size == E.Size && !Trusted->SHA1.IsEmpty())
			{
				E.SHA1 = Trusted->SHA1;
			}
			else if (!HashWholeFile(Dir / Name, E.SHA1, E.Size))
			{
				return false;
			}
			OutTruth.Files.Add(Name, E);
		}

		UE_LOG(LogUnrealGamePatcher, Display, TEXT("FLocalTruthExtractor: install truth: %d files in '%s'"),
			OutTruth.Files.Num(), *Dir);
		return true;
	}
}
