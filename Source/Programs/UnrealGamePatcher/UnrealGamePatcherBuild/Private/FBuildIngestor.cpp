// Copyright Epic Games, Inc. All Rights Reserved.

#include "FBuildIngestor.h"
#include "Formats/Chunk/FChunkDataLoader.h"
#include "Formats/IoStore/FIoStoreReader.h"
#include "Formats/Pak/FPakReader.h"
#include "Hash/FPatcherHash.h"
#include "Stats/FIOStats.h"
#include "UnrealGamePatcherLog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SecureHash.h"

namespace UnrealGamePatcher
{
	// Gear table for content-defined chunking (FastCDC). Fixed-seed
	// splitmix64-style expansion - the sequence is deterministic across runs
	// and machines. FORMAT CONTRACT: the seed, the expansion constants and
	// the CDC parameters in Ingest are part of the content-address identity;
	// changing any of them shifts every cut point and invalidates
	// cross-version chunk dedup. Never tune between published versions.
	static const TArray<uint64>& GetGearTable()
	{
		static const TArray<uint64> Table = []()
		{
			TArray<uint64> G;
			G.SetNumUninitialized(256);
			uint64 S = 0x9E3779B97F4A7C15ull;
			for (int32 I = 0; I < 256; ++I)
			{
				S += 0xA0761D6478BD642Full;
				S ^= S >> 29;
				S *= 0xBF58476D1CE4E5B9ull;
				S ^= S >> 32;
				G[I] = S;
			}
			return G;
		}();
		return Table;
	}

	bool FBuildIngestor::Ingest(
		const FString& StagedDir,
		const FString& AppId,
		const FString& ContentVersion,
		const FString& OutCdnDir,
		FIngestResult& OutResult)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (!PF.DirectoryExists(*StagedDir))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FBuildIngestor: staged dir not found: '%s'"), *StagedDir);
			return false;
		}

		// A trailing separator would corrupt every relative file id below:
		// Rel = Abs.RightChop(Dir.Len() + 1) assumes exactly one separator
		// between the dir and the found path, but Windows IterateDirectory
		// (FPathViews::Append) does not double one that is already there - the
		// first character of every filename would be eaten and a self-check
		// valid, completely wrong manifest would get published.
		FString StagedRoot = StagedDir;
		while (StagedRoot.EndsWith(TEXT("/")) || StagedRoot.EndsWith(TEXT("\\")))
		{
			StagedRoot = StagedRoot.LeftChop(1);
		}

		FIndexManifest Index;
		Index.AppId = AppId;
		Index.ContentVersion = ContentVersion;
		Index.BaseVersion = FString();

		// Layout: metadata per version under <cdn>/index/<version>/, chunk
		// entities GLOBAL under <cdn>/chunks/ (content-addressed by SHA1 - a
		// window shared across versions is one object, uploaded/stored once).
		const FString IndexDir = FPaths::Combine(OutCdnDir, TEXT("index"), ContentVersion);
		const FString ChunkOutDir = FPaths::Combine(OutCdnDir, TEXT("chunks"));
		if (!PF.CreateDirectoryTree(*ChunkOutDir) || !PF.CreateDirectoryTree(*IndexDir))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FBuildIngestor: cannot create CDN dirs '%s' / '%s'"), *ChunkOutDir, *IndexDir);
			return false;
		}

		// --- Scan the staged build (recursively - the real layout nests
		// IOStoreDemo/Content/Paks/... under the platform dir).
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *StagedRoot, TEXT("*"), true, false);
		if (Files.Num() == 0)
		{
			// Publishing an empty index would classify EVERY file of every
			// client install as Removed - the next update would delete the
			// whole install (live-reproduced). An empty result always means a
			// wrong -staged= directory, never a legitimate release.
			UE_LOG(LogUnrealGamePatcher, Error,
				TEXT("FBuildIngestor: staged dir '%s' contains no files - refusing to ingest (wrong -staged= directory?)"), *StagedDir);
			return false;
		}

		for (const FString& Abs : Files)
		{
			const FString Rel = Abs.RightChop(StagedRoot.Len() + 1).Replace(TEXT("\\"), TEXT("/"));
			const bool bIsPak = Rel.EndsWith(TEXT(".pak"));
			const bool bIsUcas = Rel.EndsWith(TEXT(".ucas"));

			// Whole-file SHA1 (the manifest's file-level truth).
			const FString Sha1Hex = FPatcherHash::FileSHA1Hex(Abs);
			if (Sha1Hex.IsEmpty())
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FBuildIngestor: cannot hash '%s'"), *Abs);
				return false;
			}

			FIndexFileEntry E;
			E.SHA1 = Sha1Hex;
			E.Size = IFileManager::Get().FileSize(*Abs);
			if (E.Size < 0)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FBuildIngestor: cannot stat '%s'"), *Abs);
				return false;
			}

			// Container metadata parsed DIRECTLY from the staged file (no
			// chunk reassembly - the files are right here).
			if (bIsPak)
			{
				FPakReader Reader;
				if (Reader.Open(Abs) && Reader.ReadIndex())
				{
					const FString Mount = Reader.GetMountPoint();
					for (const FPakReader::FPakEntryLocation& Loc : Reader.GetFiles())
					{
						FString RelPath = Loc.Path;
						if (!Mount.IsEmpty() && RelPath.StartsWith(Mount))
						{
							RelPath = RelPath.RightChop(Mount.Len());
						}
						FIndexPakEntry PE;
						PE.Path = RelPath;
						PE.OffsetInFile = Loc.Entry.Offset + FPakReader::GetSerializedEntrySize(Loc.Entry, Reader.GetInfo().Version);
						PE.StoredSize = Loc.Entry.Size;
						PE.UncompressedSize = Loc.Entry.UncompressedSize;
						uint8 Hash[20] = { 0 };
						if (Loc.Entry.HasValidHash())
						{
							FMemory::Memcpy(Hash, Loc.Entry.Hash, 20);
						}
						else if (Reader.ReadPayloadHash(Loc.Entry, Hash))
						{
							// stored hash
						}
						PE.SHA1 = FPatcherHash::BytesToHex(Hash, 20);
						E.PakEntries.Add(PE);
					}
					UE_LOG(LogUnrealGamePatcher, Display, TEXT("FBuildIngestor: '%s' entry metadata: %d pak entries"), *Rel, E.PakEntries.Num());
				}
				else
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FBuildIngestor: cannot parse staged pak '%s'"), *Rel);
					return false;
				}
			}
			else if (bIsUcas)
			{
				const FString UtocRel = Rel.LeftChop(5) + TEXT(".utoc");
				const FString UtocAbs = FPaths::Combine(StagedRoot, UtocRel);
				FIoStoreReader Io;
				if (Io.Open(UtocAbs, Abs))
				{
					for (const FIoStoreChunk& C : Io.GetChunks())
					{
						FIndexIoStoreChunk IC;
						IC.Id = C.Id;
						IC.RawOffset = C.RawOffset;
						IC.RawSize = C.RawSize;
						IC.MetaHash = C.Hash;
						// Physical block span (download-phase window targeting).
						if (C.NumBlocks > 0 && Io.GetBlocks().IsValidIndex(C.FirstBlockIndex))
						{
							const FIoStoreBlock& First = Io.GetBlocks()[C.FirstBlockIndex];
							const int32 LastIdx = FMath::Min(C.FirstBlockIndex + C.NumBlocks - 1, Io.GetBlocks().Num() - 1);
							const FIoStoreBlock& Last = Io.GetBlocks()[LastIdx];
							IC.FileOffset = First.FileOffset;
							IC.FileSize = FMath::Max<int64>(0, Last.FileOffset + Last.CompressedSize - First.FileOffset);
						}
						E.IoStoreChunks.Add(IC);
					}
					UE_LOG(LogUnrealGamePatcher, Display, TEXT("FBuildIngestor: '%s' IoStore metadata: %d chunks"), *Rel, E.IoStoreChunks.Num());
				}
				else
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FBuildIngestor: cannot parse IoStore container for '%s'"), *Rel);
					return false;
				}
			}
			// --- Chunk the file: CONTENT-DEFINED chunking (FastCDC - Gear
			// rolling hash + normalized cut points), content-addressed by the
			// SHA1 of the uncompressed window. Identical windows (dedup across
			// files and versions) share one chunk file. Boundaries are a
			// function of the CONTENT only: the Gear state carries ~64 bytes
			// of history, so after an insertion/deletion the rolling state
			// re-synchronizes with the unshifted stream and the following cut
			// points land identically - fixed-size windows would shift
			// wholesale (a 1-byte edit at the start of a 10GB pak would
			// re-chunk/re-upload everything). Files are STREAMED through a
			// bounded read-ahead buffer - staged builds carry multi-GB
			// paks/pdbs and peak memory stays O(CdcMax), not O(file).
			//
			// FORMAT CONTRACT (with GetGearTable): these parameters are part
			// of the content-address identity - changing any of them shifts
			// every cut point and invalidates cross-version dedup.
			constexpr int64 CdcMin = 256 << 10;
			constexpr int64 CdcAvg = 1 << 20;
			constexpr int64 CdcMax = 4 << 20;
			constexpr uint64 CdcMaskS = (1ull << 20) - 1; // below the average: higher cut probability
			constexpr uint64 CdcMaskL = (1ull << 21) - 1; // above the average: lower cut probability

			const TArray<uint64>& Gear = GetGearTable();
			IFileHandle* Handle = PF.OpenRead(*Abs);
			if (!Handle)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FBuildIngestor: cannot open '%s'"), *Abs);
				return false;
			}
			bool bChunkOk = true;
			int64 Off = 0;
			uint64 GearState = 0; // rolling state carried across chunks (the resync memory)
			TArray64<uint8> Buf;  // read-ahead buffer covering the current scan
			int64 BufBase = 0;
			while (Off < E.Size && bChunkOk)
			{
				// (Re)fill the read-ahead buffer from Off - one sequential
				// read per chunk, clamped to the max chunk size (the OS page
				// cache absorbs the overlap between consecutive fills).
				const int64 Need = FMath::Min<int64>(CdcMax, E.Size - Off);
				if (Off < BufBase || Off + Need > BufBase + Buf.Num())
				{
					Buf.SetNumUninitialized(Need);
					if (Need > 0 && (!Handle->Seek(Off) || !Handle->Read(Buf.GetData(), Need)))
					{
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("FBuildIngestor: read failed on '%s' at offset %lld"), *Abs, Off);
						bChunkOk = false;
						break;
					}
					if (Need > 0)
					{
						FIOStats::Get().AddDiskRead(Need);
					}
					BufBase = Off;
				}

				// FastCDC scan: hash through the mandatory minimum without
				// testing, then look for the first mask hit with normalization
				// around the average (higher cut probability before it, lower
				// after) - the distribution's shape without the tiny-chunk
				// explosion of a single fixed mask.
				uint64 H = GearState;
				int64 I = Off;
				const int64 MinEnd = FMath::Min(Off + CdcMin, E.Size);
				const int64 AvgEnd = FMath::Min(Off + CdcAvg, E.Size);
				const int64 HardEnd = FMath::Min(Off + CdcMax, E.Size);
				while (I < MinEnd)
				{
					H = (H << 1) + Gear[(int32)(Buf[I - BufBase] & 0xFF)];
					++I;
				}
				if (I < AvgEnd)
				{
					while (I < AvgEnd && (H & CdcMaskS) != 0)
					{
						H = (H << 1) + Gear[(int32)(Buf[I - BufBase] & 0xFF)];
						++I;
					}
				}
				if (I >= AvgEnd)
				{
					// Only reached when the S zone found no cut (a cut in the
					// S zone must stand - the L scan continues from wherever
					// the S scan stopped, which is AvgEnd here).
					while (I < HardEnd && (H & CdcMaskL) != 0)
					{
						H = (H << 1) + Gear[(int32)(Buf[I - BufBase] & 0xFF)];
						++I;
					}
				}
				const int64 Win = I - Off;

				// The window bytes are already in the buffer (no re-read).
				const uint8* WindowPtr = Buf.GetData() + (Off - BufBase);
				FSHA1 Hasher;
				Hasher.Update(WindowPtr, (int32)Win);
				uint8 Hash[20] = { 0 };
				Hasher.Final();
				Hasher.GetHash(Hash);
				FString ChunkSha1 = FPatcherHash::BytesToHex(Hash, 20);

				if (!Index.Chunks.Contains(ChunkSha1))
				{
					const FString ChunkFile = FChunkDataLoader::MakeChunkRelPath(ChunkSha1);
					const FString ChunkPath = FPaths::Combine(OutCdnDir, ChunkFile);
					uint8 StoredAs = 0;
					if (!FChunkDataLoader::Write(ChunkPath, WindowPtr, Win, ChunkSha1, StoredAs))
					{
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("FBuildIngestor: cannot write chunk for '%s'"), *Rel);
						bChunkOk = false;
						break;
					}
					FIndexChunkEntry CE;
					CE.SHA1 = ChunkSha1;
					CE.Size = IFileManager::Get().FileSize(*ChunkPath);
					CE.File = ChunkFile;
					Index.Chunks.Add(ChunkSha1, CE);
					OutResult.NumChunkBytesWritten += CE.Size;
					++OutResult.NumChunks;
				}

				FIndexFileChunkRef Ref;
				Ref.Chunk = ChunkSha1;
				Ref.Offset = 0; // each window is one whole chunk - the part covers it from byte 0
				Ref.Size = Win;
				E.Parts.Add(Ref);

				GearState = H;
				Off = I;
			}
			delete Handle;
			if (!bChunkOk)
			{
				return false;
			}

			Index.Files.Add(Rel, E);
			++OutResult.NumFiles;
		}

		// --- Write the extension manifest (deterministic).
		const FString IndexPath = FPaths::Combine(IndexDir, TEXT("UnrealGamePatcher.index.json"));
		if (!FIndexManifestFile::Save(IndexPath, Index))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FBuildIngestor: failed writing extension manifest '%s'"), *IndexPath);
			return false;
		}
		OutResult.IndexPath = IndexPath;

		UE_LOG(LogUnrealGamePatcher, Display,
			TEXT("FBuildIngestor: ingested %s -> %s (%d files, %d chunks, %lld chunk bytes)"),
			*StagedDir, *IndexPath, OutResult.NumFiles, OutResult.NumChunks, OutResult.NumChunkBytesWritten);
		// Publish-side I/O report: staged bytes read (incl. the SHA1 passes),
		// chunk files written, compression volume.
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("FBuildIngestor: ingest I/O %s"),
			*FIOStats::Get().FormatSummary(*FString::Printf(TEXT("ingest %s"), *ContentVersion)));
		return true;
	}
}
