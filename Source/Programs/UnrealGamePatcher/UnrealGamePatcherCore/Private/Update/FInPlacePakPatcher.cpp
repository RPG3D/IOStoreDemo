// Copyright Epic Games, Inc. All Rights Reserved.

#include "Update/FInPlacePakPatcher.h"
#include "Formats/Chunk/FChunkDataLoader.h"
#include "Formats/Pak/FPakReader.h"
#include "Formats/Pak/FPakWriter.h"
#include "Hash/FPatcherHash.h"
#include "Stats/FIOStats.h"
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

		/**
		 * Placement holes of the COMMITTED container: the complement of every
		 * byte span the committed footer references - each entry's payload
		 * header + stored bytes, and the index/footer region at the tail
		 * (ROADMAP §5.3: the hole list is recomputed from the container on
		 * every patch; sidecar bookkeeping is gone).
		 *
		 * This makes the core invariant structural: a superseded entry is
		 * still referenced by the current footer, so its bytes are NOT in
		 * this complement and cannot be reused this generation - "mark dead"
		 * and "reuse the hole" are separated by one index commit for free.
		 *
		 * Returns false when an entry's span does not fit inside the file
		 * (corrupt index geometry - the caller falls back to the rebuild).
		 * Comparisons are subtraction-ordered so untrusted int64 fields
		 * cannot overflow.
		 */
		bool ComputePlacementHoles(
			const FString& PakId,
			uint32 Version,
			int64 FileEof,
			int64 IndexOffset,
			const TArray<FPakEntry>& Entries,
			TArray<FHole>& OutHoles)
		{
			struct FSpan { int64 Start; int64 End; };
			TArray<FSpan> Spans;
			Spans.Reserve(Entries.Num() + 1);

			for (const FPakEntry& E : Entries)
			{
				if (E.Offset < 0 || E.Size < 0)
				{
					return false;
				}
				const int64 HeaderSize = FPakReader::GetSerializedEntrySize(E, Version);
				if (E.Offset > FileEof || E.Size > FileEof - E.Offset || HeaderSize > FileEof - E.Offset - E.Size)
				{
					return false;
				}
				FSpan S;
				S.Start = E.Offset;
				S.End = E.Offset + HeaderSize + E.Size;
				Spans.Add(S);
			}
			if (IndexOffset < 0 || IndexOffset > FileEof)
			{
				return false;
			}
			FSpan IndexSpan;
			IndexSpan.Start = IndexOffset;
			IndexSpan.End = FileEof;
			Spans.Add(IndexSpan);

			Spans.Sort([](const FSpan& A, const FSpan& B) { return A.Start < B.Start; });

			OutHoles.Reset();
			int64 Cursor = 0;
			for (const FSpan& S : Spans)
			{
				const int64 Start = FMath::Max(S.Start, Cursor);
				if (S.Start > Cursor)
				{
					FHole H;
					H.File = PakId;
					H.Offset = Cursor;
					H.Size = S.Start - Cursor;
					OutHoles.Add(H);
				}
				if (S.End > Cursor)
				{
					Cursor = S.End;
				}
			}
			if (FileEof > Cursor)
			{
				FHole H;
				H.File = PakId;
				H.Offset = Cursor;
				H.Size = FileEof - Cursor;
				OutHoles.Add(H);
			}
			return true;
		}
	}

	bool FInPlacePakPatcher::Patch(
		const FIndexManifest& Remote,
		const FString& ChunkRoot,
		const FString& PakId,
		const FString& LocalPakPath,
		FInPlacePakStats& OutStats,
		bool bVerifyLocalData)
	{
		OutStats = FInPlacePakStats();
		const FIOStatsSnapshot IOStart = FIOStats::Get().Snapshot();

		const FIndexFileEntry* RemoteFile = Remote.Files.Find(PakId);
		if (!RemoteFile || RemoteFile->PakEntries.Num() == 0)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlacePakPatcher: '%s' has no pak entry metadata (re-ingest)"), *PakId);
			return false;
		}

		// CoW v2 gates: rewriting the index+footer breaks encryption and
		// invalidates signatures, so neither container kind is patched here -
		// the caller falls back to the whole-file rebuild.
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (PlatformFile.FileExists(*(LocalPakPath + TEXT(".sig"))) ||
			PlatformFile.FileExists(*(FPaths::ChangeExtension(LocalPakPath, TEXT("sig")))))
		{
			UE_LOG(LogUnrealGamePatcher, Display, TEXT("FInPlacePakPatcher: '%s' is signed - falling back to the whole-file rebuild"), *PakId);
			return false;
		}

		// Local entries by path. The reader must be destroyed before the
		// writer opens the file (the read handle's share flags would block it).
		TMap<FString, FPakEntry> LocalByPath;
		TArray<FPakEntry> LocalSpans;
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
			if (LocalReader.GetInfo().bEncryptedIndex)
			{
				UE_LOG(LogUnrealGamePatcher, Display, TEXT("FInPlacePakPatcher: '%s' has an encrypted index - falling back to the whole-file rebuild"), *PakId);
				return false;
			}
			// Snapshot for hole recomputation (the reader dies with this block).
			OldIndexOffset = LocalReader.GetInfo().IndexOffset;
			OldVersion = LocalReader.GetInfo().Version;
			for (const FPakReader::FPakEntryLocation& Loc : LocalReader.GetFiles())
			{
				LocalSpans.Add(Loc.Entry);
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

		// Entries present locally but absent from the remote metadata (a pak
		// entry DELETED between versions) cannot be expressed by the append
		// path: the re-committed index would still carry the stale entry, the
		// container would never match the target metadata, and every rerun
		// would report success while diverging. Bail to the whole-file
		// rebuild, which reproduces exactly the remote entry set.
		{
			TSet<FString> RemotePaths;
			for (const FIndexPakEntry& RemoteEntry : RemoteFile->PakEntries)
			{
				RemotePaths.Add(RemoteEntry.Path);
			}
			for (const TPair<FString, FPakEntry>& Pair : LocalByPath)
			{
				if (!RemotePaths.Contains(Pair.Key))
				{
					UE_LOG(LogUnrealGamePatcher, Display,
						TEXT("FInPlacePakPatcher: '%s' entry '%s' is gone from the remote metadata - falling back to the whole-file rebuild"),
						*PakId, *Pair.Key);
					return false;
				}
			}
		}

		// Changed compressed entries cannot be re-stored by this path: the
		// index carries no block table to rebuild, and a raw re-store would
		// decode to garbage. The same holds for changed encrypted entries
		// (the append path stores plaintext). Bail out of the CoW path
		// entirely - the whole-file rebuild handles them correctly, and its
		// SameChunks reuse keeps the actual wire delta to the changed chunks.
		for (const FIndexPakEntry& RemoteEntry : RemoteFile->PakEntries)
		{
			const FPakEntry* Local = LocalByPath.Find(RemoteEntry.Path);
			if (Local && (Local->CompressionMethodIndex != 0 || Local->IsEncrypted()))
			{
				const FString LocalHash = FPatcherHash::BytesToHex(Local->Hash, 20);
				if (!LocalHash.Equals(RemoteEntry.SHA1, ESearchCase::IgnoreCase) || Local->Size != RemoteEntry.StoredSize)
				{
					UE_LOG(LogUnrealGamePatcher, Display,
						TEXT("FInPlacePakPatcher: '%s' changed compressed/encrypted entry '%s' - falling back to the whole-file rebuild"),
						*PakId, *RemoteEntry.Path);
					return false;
				}
			}

			// A compressed REMOTE entry (StoredSize != UncompressedSize - the
			// manifest carries no method field) can only ride the zero-movement
			// path: the append path stores method-0 raw, which the engine
			// would read back as the compressed byte stream (garbage).
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

		// --- Placement holes recomputed from the committed container --------
		TArray<FHole> Holes;
		if (!ComputePlacementHoles(PakId, OldVersion, OldEof, OldIndexOffset, LocalSpans, Holes))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlacePakPatcher: '%s' has corrupt index geometry - falling back to the whole-file rebuild"), *PakId);
			return false;
		}

		// Entries this patch writes; the post-verify pass re-hashes them from
		// the stored bytes instead of trusting any header (C12 fix).
		TSet<FString> WrittenPaths;

		// The writer must be destroyed before the whole-file hash verify
		// (its open write handle would make the read stale/locked).
		{
			FPakWriter Writer;
			if (!Writer.Open(LocalPakPath))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlacePakPatcher: cannot open pak for patching '%s'"), *LocalPakPath);
				return false;
			}
			Writer.SetPlacementHoles(Holes);

			for (const FIndexPakEntry& RemoteEntry : RemoteFile->PakEntries)
			{
				const FPakEntry* Local = LocalByPath.Find(RemoteEntry.Path);

				// Same entry: decide FIRST, without fetching anything. The
				// verdict is metadata-only (the local pak's built-in hash vs
				// the manifest SHA1), and the download phase prefetches ONLY
				// the changed entries' covering windows ("Same entries are
				// verified locally: zero fetch"). Fetching a same entry's
				// bytes anyway reads (and, in network mode, FAILS on) windows
				// that were deliberately never downloaded - review 2026-10-09
				// H1: the old fetch-before-check order made the first same
				// entry abort the whole CoW path, silently degrading every
				// real-world (multi-window) pak update into a full-container
				// fetch + whole-file rebuild.
				if (Local && Local->HasValidHash() &&
					FPatcherHash::BytesToHex(Local->Hash, 20).Equals(RemoteEntry.SHA1, ESearchCase::IgnoreCase))
				{
					// Same entry: zero movement.
					++OutStats.NumSame;
					OutStats.BytesReused += RemoteEntry.StoredSize;
					continue;
				}

				// New or changed (any size - CoW never overwrites payload):
				// fetch the stored bytes and append them raw, dead ranges
				// first, EOF fallback.
				TArray64<uint8> RemoteBytes;
				if (!FetchRemoteBytes(Remote, ChunkRoot, PakId, RemoteEntry.OffsetInFile, RemoteEntry.StoredSize, RemoteBytes))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlacePakPatcher: cannot fetch bytes for '%s' (entry '%s')"), *PakId, *RemoteEntry.Path);
					return false;
				}

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
				WrittenPaths.Add(RemoteEntry.Path);
			}

			if (!Writer.Commit())
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlacePakPatcher: commit failed for '%s'"), *LocalPakPath);
				return false;
			}
			OutStats.NumHolesFilled = Writer.GetNumHolesFilled();
		} // writer closed here

		// Verify the patched pak PER-ENTRY against the remote metadata. The
		// container is a virtual disk: its physical layout may legitimately
		// differ from the CDN copy (per-machine append order), so the
		// whole-file hash is NOT a valid equality signal - the per-entry
		// content hashes are. Entries written by THIS patch are re-hashed
		// from the bytes now stored in the container (a corrupted/torn write
		// is caught even if every header claims otherwise); untouched entries
		// keep the cheap stored-hash comparison.
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
				const bool bWritten = WrittenPaths.Contains(Loc.Path);
				if (bWritten)
				{
					if (!Check.ComputePayloadHash(Loc.Entry, Hash))
					{
						UE_LOG(LogUnrealGamePatcher, Error,
							TEXT("FInPlacePakPatcher: cannot re-hash written entry '%s' in patched '%s'"), *Loc.Path, *LocalPakPath);
						return false;
					}
				}
				else if (Loc.Entry.HasValidHash())
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
						TEXT("FInPlacePakPatcher: entry '%s' of patched '%s' does not match remote metadata (CoW patch failed verification)"),
						*R.Path, *PakId);
					return false;
				}
			}
		}

		UE_LOG(LogUnrealGamePatcher, Display,
			TEXT("FInPlacePakPatcher: '%s': %d same (zero movement, %lld bytes reused), %d appended (CoW, %d holes filled); %lld bytes written"),
			*PakId, OutStats.NumSame, OutStats.BytesReused, OutStats.NumAppended, OutStats.NumHolesFilled, OutStats.BytesWritten);
		// Per-patch I/O report: a CoW patch must read only the windows of the
		// entries it writes - same entries are metadata-compared, never
		// fetched. Disk reads approaching the container size mean the patch
		// is secretly pulling everything (the H1 failure signature).
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("FInPlacePakPatcher: '%s' I/O: %s"),
			*PakId, *FIOStats::FormatDelta(IOStart, FIOStats::Get().Snapshot()));
		return true;
	}
}
