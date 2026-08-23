// Copyright Epic Games, Inc. All Rights Reserved.

#include "Update/FInPlacePakPatcher.h"
#include "Formats/Chunk/FChunkDataLoader.h"
#include "Formats/Pak/FPakReader.h"
#include "Formats/Pak/FPakWriter.h"
#include "Hash/FPatcherHash.h"
#include "UnrealGamePatcherLog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

namespace UnrealGamePatcher
{
	namespace
	{
		/** Fetches [Offset, Offset+Size) of the remote file (PakId) by loading
		 *  the covering chunk slices from the CDN. */
		bool FetchRemoteBytes(
			const FIndexManifest& Remote,
			const FString& ChunkRoot,
			const FString& PakId,
			int64 Offset,
			int64 Size,
			TArray64<uint8>& OutBytes)
		{
			const FIndexFileEntry* File = Remote.Files.Find(PakId);
			if (!File || Size < 0)
			{
				return false;
			}
			OutBytes.Reset();
			OutBytes.Reserve(Size);

			// Parts are contiguous slices of the file in order; find the ones
			// overlapping [Offset, Offset+Size).
			int64 FilePos = 0;
			for (const FIndexFileChunkRef& Part : File->Parts)
			{
				const int64 PartEnd = FilePos + Part.Size;
				if (PartEnd <= Offset)
				{
					FilePos = PartEnd;
					continue;
				}
				if (FilePos >= Offset + Size)
				{
					break;
				}
				const int64 SliceStart = FMath::Max<int64>(FilePos, Offset);
				const int64 SliceEnd = FMath::Min<int64>(PartEnd, Offset + Size);
				if (SliceEnd <= SliceStart)
				{
					FilePos = PartEnd;
					continue;
				}

				const FIndexChunkEntry* Chunk = Remote.Chunks.Find(Part.Chunk);
				if (!Chunk || Chunk->File.IsEmpty())
				{
					return false;
				}
				TArray64<uint8> ChunkData;
				if (!FChunkDataLoader::Load(FPaths::Combine(ChunkRoot, Chunk->File), *Chunk, ChunkData))
				{
					return false;
				}
				const int64 ChunkOff = Part.Offset + (SliceStart - FilePos);
				const int64 Len = SliceEnd - SliceStart;
				if (ChunkOff + Len > (int64)ChunkData.Num())
				{
					return false;
				}
				OutBytes.Append(ChunkData.GetData() + ChunkOff, Len);
				FilePos = PartEnd;
			}
			return OutBytes.Num() == Size;
		}
	}

	bool FInPlacePakPatcher::Patch(
		const FIndexManifest& Remote,
		const FString& ChunkRoot,
		const FString& PakId,
		const FString& LocalPakPath,
		FInPlacePakStats& OutStats,
		bool bVerifyLocalData,
		const TArray<FHole>& InAvailableHoles)
	{
		OutStats = FInPlacePakStats();

		const FIndexFileEntry* RemoteFile = Remote.Files.Find(PakId);
		if (!RemoteFile || RemoteFile->PakEntries.Num() == 0)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlacePakPatcher: '%s' has no pak entry metadata (re-ingest)"), *PakId);
			return false;
		}

		// Local entries by path. The reader must be destroyed before the
		// writer opens the file (the read handle's share flags would block it).
		TMap<FString, FPakEntry> LocalByPath;
		int64 OldIndexOffset = -1;
		uint32 OldVersion = 0;
		const int64 OldEof = IFileManager::Get().FileSize(*LocalPakPath);
		{
			FPakReader LocalReader;
			if (!LocalReader.Open(LocalPakPath) || !LocalReader.ReadIndex())
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlacePakPatcher: cannot read local pak '%s'"), *LocalPakPath);
				return false;
			}
			// Snapshot for hole accounting (the reader dies with this block).
			OldIndexOffset = LocalReader.GetInfo().IndexOffset;
			OldVersion = LocalReader.GetInfo().Version;
			for (const FPakReader::FPakEntryLocation& Loc : LocalReader.GetFiles())
			{
				FPakEntry E = Loc.Entry;
				uint8 Hash[20] = { 0 };
				if (bVerifyLocalData)
				{
					// Verify mode: recompute the entry's CONTENT hash from its
					// payload bytes - a same-size corruption with an intact
					// stored-hash header is still caught (the stored built-in
					// hash is not a trustworthy content signal).
					if (!LocalReader.ComputePayloadHash(Loc.Entry, Hash))
					{
						// Unreadable payload: treat as different -> the entry
						// is re-written from the remote bytes (or the patch
						// falls back to the whole-file rebuild).
						FMemory::Memset(E.Hash, 0xFF, 20);
					}
					else
					{
						FMemory::Memcpy(E.Hash, Hash, 20);
					}
				}
				else
				{
					// Trust mode: the built-in hash lives in the payload header
					// (the encoded index stores no hash).
					if (!E.HasValidHash() && LocalReader.ReadPayloadHash(Loc.Entry, Hash))
					{
						FMemory::Memcpy(E.Hash, Hash, 20);
					}
				}
				LocalByPath.Add(Loc.Path, E);
			}
		}

		// Compressed entries whose bytes changed cannot be re-stored in
		// place: the index carries no block table, so neither the overwrite
		// layout nor a raw re-store can represent changed compressed bytes
		// (a raw re-store would decode to garbage). Bail out of the in-place
		// path entirely - the whole-file rebuild handles them correctly, and
		// its SameChunks reuse keeps the actual wire delta to the changed
		// chunks.
		for (const FIndexPakEntry& RemoteEntry : RemoteFile->PakEntries)
		{
			const FPakEntry* Local = LocalByPath.Find(RemoteEntry.Path);
			if (Local && Local->CompressionMethodIndex != 0)
			{
				const FString LocalHash = FPatcherHash::BytesToHex(Local->Hash, 20);
				if (!LocalHash.Equals(RemoteEntry.SHA1, ESearchCase::IgnoreCase) || Local->Size != RemoteEntry.StoredSize)
				{
					UE_LOG(LogUnrealGamePatcher, Display,
						TEXT("FInPlacePakPatcher: '%s' changed compressed entry '%s' - falling back to the whole-file rebuild"),
						*PakId, *RemoteEntry.Path);
					return false;
				}
			}

			// A compressed REMOTE entry (StoredSize != UncompressedSize - the
			// manifest carries no method field) can be neither overwritten nor
			// appended: the encoded index has no block table to rebuild, and
			// the append path stores method-0 raw, which the engine would read
			// back as the compressed byte stream (garbage). Unchanged
			// compressed entries are fine (zero movement below); a changed or
			// new one bails to the whole-file rebuild, which reproduces the
			// real compressed layout. This also closes the
			// local-uncompressed/remote-compressed same-stored-size overwrite
			// hole.
			if (RemoteEntry.StoredSize != RemoteEntry.UncompressedSize)
			{
				const bool bSameEntry = Local && Local->HasValidHash() &&
					Local->Size == RemoteEntry.StoredSize &&
					FPatcherHash::BytesToHex(Local->Hash, 20).Equals(RemoteEntry.SHA1, ESearchCase::IgnoreCase);
				if (!bSameEntry)
				{
					UE_LOG(LogUnrealGamePatcher, Display,
						TEXT("FInPlacePakPatcher: '%s' new/changed compressed entry '%s' - falling back to the whole-file rebuild"),
						*PakId, *RemoteEntry.Path);
					return false;
				}
			}
		}

		// --- Hole accounting: the byte regions this patch turns into dead
		// space (superseded entry data + the old directory/footer). They are
		// recorded for FUTURE patches - this patch only backfills from
		// InAvailableHoles, never from regions it invalidates itself (the
		// old index bytes must stay readable until Commit rewrites them).
		TArray<FHole> NewHoles;
		{
			auto AddDead = [&](int64 Offset, int64 Size)
			{
				if (Offset >= 0 && Size > 0)
				{
					FHole H;
					H.File = PakId;
					H.Offset = Offset;
					H.Size = Size;
					NewHoles.Add(H);
				}
			};
			for (const TPair<FString, FPakEntry>& Pair : LocalByPath)
			{
				const FPakEntry& E = Pair.Value;
				const FIndexPakEntry* R = nullptr;
				for (const FIndexPakEntry& RE : RemoteFile->PakEntries)
				{
					if (RE.Path.Equals(Pair.Key, ESearchCase::IgnoreCase))
					{
						R = &RE;
						break;
					}
				}
				const bool bSame = R && E.HasValidHash() &&
					FPatcherHash::BytesToHex(E.Hash, 20).Equals(R->SHA1, ESearchCase::IgnoreCase) &&
					E.Size == R->StoredSize;
				const bool bOverwritten = R && !bSame &&
					E.Size == R->StoredSize && E.UncompressedSize == R->UncompressedSize;
				if (!bSame && !bOverwritten)
				{
					// Superseded data: re-appended (size changed) or dropped
					// (absent from the remote set). Header + stored bytes.
					AddDead(E.Offset, FPakReader::GetSerializedEntrySize(E, OldVersion) + E.Size);
				}
			}
			if (OldIndexOffset > 0 && OldEof > OldIndexOffset)
			{
				AddDead(OldIndexOffset, OldEof - OldIndexOffset);
			}
		}

		// The writer must be destroyed before the whole-file hash verify
		// (its open write handle would make the read stale/locked).
		{
		FPakWriter Writer;
		if (!Writer.Open(LocalPakPath))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlacePakPatcher: cannot open pak for patching '%s'"), *LocalPakPath);
			return false;
		}
		Writer.SetPlacementHoles(InAvailableHoles);

		for (const FIndexPakEntry& RemoteEntry : RemoteFile->PakEntries)
		{
			const FPakEntry* Local = LocalByPath.Find(RemoteEntry.Path);

			TArray64<uint8> RemoteBytes;
			if (!FetchRemoteBytes(Remote, ChunkRoot, PakId, RemoteEntry.OffsetInFile, RemoteEntry.StoredSize, RemoteBytes))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlacePakPatcher: cannot fetch bytes for '%s' (entry '%s')"), *PakId, *RemoteEntry.Path);
				return false;
			}

			if (Local && Local->HasValidHash() &&
				FPatcherHash::BytesToHex(Local->Hash, 20).Equals(RemoteEntry.SHA1, ESearchCase::IgnoreCase))
			{
				// Same entry: zero movement.
				++OutStats.NumSame;
				OutStats.BytesReused += RemoteEntry.StoredSize;
				continue;
			}

			if (Local && Local->Size == RemoteEntry.StoredSize && RemoteEntry.UncompressedSize == Local->UncompressedSize)
			{
				// Same stored size + same uncompressed size: overwrite in place.
				FPakEntry NewEntry = *Local;
				if (NewEntry.CompressionMethodIndex != 0 && NewEntry.CompressionBlocks.Num() > 1)
				{
					// Rebuild the compressed block table the way the engine lays
					// it out: blocks packed back-to-back starting at the first
					// block (whose offset equals the serialized header size and
					// is layout-independent). Each block's own size survives any
					// uniform offset skew (End - Start), so the table can be
					// reconstructed from a misread one without re-parsing.
					int64 Cursor = NewEntry.CompressionBlocks[0].CompressedStart;
					for (FPakCompressedBlock& Block : NewEntry.CompressionBlocks)
					{
						const int64 BlockBytes = Block.CompressedEnd - Block.CompressedStart;
						Block.CompressedStart = Cursor;
						Block.CompressedEnd = Cursor + BlockBytes;
						Cursor = Block.CompressedEnd;
					}
				}
				TArray<uint8> HashBytes;
				if (FPatcherHash::HexToBytes(RemoteEntry.SHA1, HashBytes) && HashBytes.Num() == 20)
				{
					FMemory::Memcpy(NewEntry.Hash, HashBytes.GetData(), 20);
				}
				if (!Writer.Overwrite(RemoteEntry.Path, NewEntry, RemoteBytes.GetData(), RemoteBytes.Num()))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlacePakPatcher: in-place overwrite failed for '%s'"), *RemoteEntry.Path);
					return false;
				}
				++OutStats.NumOverwritten;
				OutStats.BytesWritten += RemoteBytes.Num();
				continue;
			}

			// New or size-changed: append at the end (stored raw).
			FPakWriter::FEntryToWrite W;
			W.Path = RemoteEntry.Path;
			W.CompressionMethodIndex = 0;
			W.UncompressedSize = RemoteEntry.UncompressedSize > 0 ? RemoteEntry.UncompressedSize : RemoteEntry.StoredSize;
			FPakWriter::FStoredBlock Block;
			Block.Bytes = MoveTemp(RemoteBytes);
			W.Blocks.Add(MoveTemp(Block));
			FPakEntry OutEntry;
			if (!Writer.AppendEntry(W, OutEntry))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlacePakPatcher: append failed for '%s'"), *RemoteEntry.Path);
				return false;
			}
			++OutStats.NumAppended;
			OutStats.BytesWritten += W.Blocks[0].Bytes.Num();
		}

		if (!Writer.Commit())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlacePakPatcher: commit failed for '%s'"), *LocalPakPath);
			return false;
		}
		OutStats.UpdatedHoles = Writer.GetPlacementHoles();
		for (const FHole& H : NewHoles)
		{
			OutStats.UpdatedHoles.Add(H);
		}
		OutStats.NumHolesFilled = Writer.GetNumHolesFilled();
		} // writer closed here

		// Verify the patched pak PER-ENTRY against the remote metadata. The
		// container is a virtual disk: its physical layout may legitimately
		// differ from the CDN copy (per-machine append order), so the
		// whole-file hash is NOT a valid equality signal - the per-entry
		// built-in hashes are. Every remote entry must be present locally
		// with a matching hash (extra stale entries are dead bytes, compact
		// reclaims them).
		{
			FPakReader Check;
			if (!Check.Open(LocalPakPath) || !Check.ReadIndex())
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlacePakPatcher: patched pak does not parse ('%s')"), *LocalPakPath);
				return false;
			}
			TMap<FString, FString> LocalHashes;
			for (const FPakReader::FPakEntryLocation& Loc : Check.GetFiles())
			{
				uint8 Hash[20] = { 0 };
				if (Loc.Entry.HasValidHash())
				{
					FMemory::Memcpy(Hash, Loc.Entry.Hash, 20);
				}
				else if (!Check.ReadPayloadHash(Loc.Entry, Hash))
				{
					UE_LOG(LogUnrealGamePatcher, Error,
						TEXT("FInPlacePakPatcher: cannot read payload hash for '%s' in patched '%s'"), *Loc.Path, *LocalPakPath);
					return false;
				}
				LocalHashes.Add(Loc.Path, FPatcherHash::BytesToHex(Hash, 20));
			}
			for (const FIndexPakEntry& R : RemoteFile->PakEntries)
			{
				const FString* H = LocalHashes.Find(R.Path);
				if (!H || !H->Equals(R.SHA1, ESearchCase::IgnoreCase))
				{
					UE_LOG(LogUnrealGamePatcher, Error,
						TEXT("FInPlacePakPatcher: entry '%s' of patched '%s' does not match remote metadata (in-place patch failed verification)"),
						*R.Path, *PakId);
					return false;
				}
			}
		}

		UE_LOG(LogUnrealGamePatcher, Display,
			TEXT("FInPlacePakPatcher: '%s': %d same (zero movement, %lld bytes reused), %d overwritten in place, %d appended; %lld bytes written"),
			*PakId, OutStats.NumSame, OutStats.BytesReused, OutStats.NumOverwritten, OutStats.NumAppended, OutStats.BytesWritten);
		return true;
	}
}
