// Copyright Epic Games, Inc. All Rights Reserved.

#include "Update/FInPlaceIoStorePatcher.h"
#include "Formats/Chunk/FChunkAssembler.h"
#include "Formats/IoStore/FIoStoreReader.h"
#include "Formats/IoStore/FIoStoreWriter.h"
#include "Hash/FPatcherHash.h"
#include "IO/IoHash.h"
#include "Stats/FIOStats.h"
#include "UnrealGamePatcherLog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

namespace UnrealGamePatcher
{
	bool FInPlaceIoStorePatcher::Patch(
		const FIndexManifest& Remote,
		const FString& ChunkRoot,
		const FString& UcasId,
		const FString& LocalUcasPath,
		const FString& LocalUtocPath,
		FInPlaceIoStoreStats& OutStats,
		bool bVerifyLocalData,
		const TArray<FHole>& InAvailableHoles)
	{
		OutStats = FInPlaceIoStoreStats();
		const FIOStatsSnapshot IOStart = FIOStats::Get().Snapshot();

		const FIndexFileEntry* RemoteFile = Remote.Files.Find(UcasId);
		if (!RemoteFile || RemoteFile->IoStoreChunks.Num() == 0)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlaceIoStorePatcher: '%s' has no IoStore chunk metadata (re-ingest)"), *UcasId);
			return false;
		}
		const FString UtocId = UcasId.LeftChop(5) + TEXT(".utoc");

		// Local chunk ids by index. With bVerifyLocalData (verify mode) each
		// chunk's data is additionally re-hashed against the utoc's per-chunk
		// meta hash (the content FIoHash of the raw data - scheme-independent,
		// unlike the chunk id which may be package-derived): a damaged container
		// (corrupt data, intact utoc) cannot be patched in place - the caller
		// falls back to the whole-file rebuild. Trust mode skips the data pass
		// (the id-set was already accepted; the periodic verify / repair
		// catches silent damage).
		TArray<FString> LocalIds;
		TArray<FString> LocalMetaHashes;
		TArray<FHole> LocalSpans;   // physical byte span per chunk (hole source)
		TMap<FString, int32> LocalIndexById; // id -> local chunk index
		{
			FIoStoreReader Local;
			if (!Local.Open(LocalUtocPath, LocalUcasPath))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlaceIoStorePatcher: cannot read local container for '%s'"), *UcasId);
				return false;
			}
			const TArray<FIoStoreBlock>& LocalBlocks = Local.GetBlocks();
			for (int32 CI = 0; CI < Local.GetChunks().Num(); ++CI)
			{
				const FIoStoreChunk& C = Local.GetChunks()[CI];
				LocalIds.Add(C.Id);
				LocalMetaHashes.Add(C.Hash);
				LocalIndexById.Add(C.Id, CI);
				// The chunk's physical byte span: if this chunk is replaced,
				// its old blocks become dead space (a hole for future patches).
				FHole Span;
				Span.File = UcasId;
				if (C.NumBlocks > 0 && LocalBlocks.IsValidIndex(C.FirstBlockIndex))
				{
					const FIoStoreBlock& First = LocalBlocks[C.FirstBlockIndex];
					const int32 LastIdx = FMath::Min(C.FirstBlockIndex + C.NumBlocks - 1, LocalBlocks.Num() - 1);
					const FIoStoreBlock& Last = LocalBlocks[LastIdx];
					Span.Offset = First.FileOffset;
					Span.Size = FMath::Max<int64>(0, Last.FileOffset + Last.CompressedSize - First.FileOffset);
				}
				LocalSpans.Add(Span);
			}
			if (bVerifyLocalData)
			{
				for (int32 I = 0; I < LocalIds.Num(); ++I)
				{
					const FIoStoreChunk& C = Local.GetChunks()[I];
					TArray64<uint8> Data;
					if (!Local.ReadChunk(I, Data))
					{
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlaceIoStorePatcher: cannot read local chunk %d of '%s'"), I, *UcasId);
						return false;
					}
					const FString HashHex = FPatcherHash::FIoHashToHex(FIoHash::HashBuffer(Data.GetData(), Data.Num()));
					if (!C.Hash.Equals(HashHex, ESearchCase::IgnoreCase))
					{
						UE_LOG(LogUnrealGamePatcher, Error,
							TEXT("FInPlaceIoStorePatcher: local chunk %d of '%s' data does not match its utoc hash (container damaged) - falling back to whole-file rebuild"),
							I, *UcasId);
						return false;
					}
				}
			}
		}

		// Decode changed chunks WITHOUT assembling the full remote container:
		// only the (small) remote UTOC is assembled and parsed for the block
		// layout; each changed chunk's stored bytes are then read as ONE
		// physical span (the manifest records every chunk's block span) and
		// decoded from those bytes. A multi-GB container update with a small
		// delta therefore touches only the delta's windows on the cache/CDN
		// instead of materializing the whole remote container.
		FChunkAssembler::FChunkCache AssemblyCache;
		const FString TmpUtoc = FPaths::Combine(FPaths::GetPath(LocalUtocPath), TEXT(".ugp-remote.utoc"));
		if (!FChunkAssembler::AssembleToFile(Remote, ChunkRoot, UtocId, TmpUtoc, &AssemblyCache))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlaceIoStorePatcher: cannot assemble remote utoc for '%s'"), *UcasId);
			IFileManager::Get().Delete(*TmpUtoc);
			return false;
		}
		// The reader is heap-held (TUniquePtr) so it is DESTROYED before the
		// temp file is deleted: OpenRead keeps files open with FILE_SHARE_READ
		// only (non-editor targets), so a Delete issued while the reader was
		// alive failed silently on Windows.
		TUniquePtr<FIoStoreReader> RemoteToc = MakeUnique<FIoStoreReader>();
		if (!RemoteToc->OpenTocOnly(TmpUtoc))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlaceIoStorePatcher: cannot parse remote utoc for '%s'"), *UcasId);
			RemoteToc.Reset();
			IFileManager::Get().Delete(*TmpUtoc);
			return false;
		}

		// Open the writer for the local container (the local reader above is
		// already destroyed - the write handle needs the file).
		FIoStoreWriter Writer;
		if (!Writer.Open(LocalUtocPath, LocalUcasPath))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlaceIoStorePatcher: cannot open container for patching '%s'"), *UcasId);
			IFileManager::Get().Delete(*TmpUtoc);
			return false;
		}
		Writer.SetPlacementHoles(InAvailableHoles);

		// Match chunks by ID (multiset semantics), never by position: the
		// remote layout may insert/remove/reorder chunks (a cook reorders
		// freely), and position-based matching would misread same-content
		// chunks as changed - the old behavior re-fetched and re-wrote the
		// whole container on any insertion.
		TArray<FHole> NewHoles;
		TSet<int32> MatchedLocal;
		for (int32 I = 0; I < RemoteFile->IoStoreChunks.Num(); ++I)
		{
			const FIndexIoStoreChunk& RemoteChunk = RemoteFile->IoStoreChunks[I];

			const int32* LocalIdx = LocalIndexById.Find(RemoteChunk.Id);
			// Same chunk (zero movement) requires the id AND - when the
			// remote metadata records it - the CONTENT hash to match: engine
			// chunk ids are package-derived, so an id match alone does not
			// prove the content is current (that assumption is exactly what
			// hid real content edits from this patcher).
			const bool bSameChunk = LocalIdx &&
				(RemoteChunk.MetaHash.IsEmpty() ||
					LocalMetaHashes[*LocalIdx].Equals(RemoteChunk.MetaHash, ESearchCase::IgnoreCase));
			if (bSameChunk)
			{
				++OutStats.NumSame;
				OutStats.BytesReused += RemoteChunk.RawSize;
				MatchedLocal.Add(*LocalIdx);
				continue;
			}

			// Changed/new chunk: decode its raw bytes from the remote utoc's
			// block layout - the chunk's stored bytes are ONE physical span,
			// fetched from the covering CDN windows and decompressed here.
			TArray64<uint8> Raw;
			{
				int32 RemoteIdx = INDEX_NONE;
				const TArray<FIoStoreChunk>& RemoteChunks = RemoteToc->GetChunks();
				for (int32 K = 0; K < RemoteChunks.Num(); ++K)
				{
					if (RemoteChunks[K].Id.Equals(RemoteChunk.Id, ESearchCase::IgnoreCase))
					{
						RemoteIdx = K;
						break;
					}
				}
				int64 SpanOffset = 0, SpanSize = 0;
				TArray64<uint8> Stored;
				if (RemoteIdx == INDEX_NONE ||
					!RemoteToc->GetChunkStoredSpan(RemoteIdx, SpanOffset, SpanSize) ||
					!FChunkAssembler::AssembleRangeToBytes(Remote, ChunkRoot, UcasId, SpanOffset, SpanSize, Stored, &AssemblyCache) ||
					!RemoteToc->DecodeChunkFromStored(RemoteIdx, Stored, Raw))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlaceIoStorePatcher: cannot fetch chunk '%s' of '%s'"), *RemoteChunk.Id, *UcasId);
					RemoteToc.Reset();
					IFileManager::Get().Delete(*TmpUtoc);
					return false;
				}
			}

			if (LocalIdx)
			{
				// Changed chunk (same id, new content): replace the entry in
				// place; its old blocks become dead bytes (a hole).
				FString NewId;
				if (!Writer.ReplaceChunkEx(*LocalIdx, Raw, RemoteChunk.Id, RemoteChunk.MetaHash, NewId))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlaceIoStorePatcher: replace failed for chunk '%s' of '%s'"), *RemoteChunk.Id, *UcasId);
					RemoteToc.Reset();
					IFileManager::Get().Delete(*TmpUtoc);
					return false;
				}
				++OutStats.NumReplaced;
				MatchedLocal.Add(*LocalIdx);
				if (LocalSpans.IsValidIndex(*LocalIdx) && LocalSpans[*LocalIdx].Size > 0)
				{
					NewHoles.Add(LocalSpans[*LocalIdx]);
				}
			}
			else
			{
				// New chunk: append.
				FString NewId;
				if (!Writer.AppendChunkEx(Raw, RemoteChunk.Id, RemoteChunk.MetaHash, NewId))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlaceIoStorePatcher: append failed for chunk '%s' of '%s'"), *RemoteChunk.Id, *UcasId);
					RemoteToc.Reset();
					IFileManager::Get().Delete(*TmpUtoc);
					return false;
				}
				++OutStats.NumAppended;
			}
			OutStats.BytesWritten += Raw.Num();
		}

		// Local chunks the target no longer has (asset/package removal):
		// drop their TOC entries - their blocks stay as dead bytes (holes).
		{
			TArray<int32> Dropped;
			for (int32 I = 0; I < LocalIds.Num(); ++I)
			{
				if (!MatchedLocal.Contains(I))
				{
					Dropped.Add(I);
				}
			}
			for (int32 K = Dropped.Num() - 1; K >= 0; --K)
			{
				const int32 I = Dropped[K];
				if (!Writer.DropChunk(I))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlaceIoStorePatcher: drop failed for local chunk %d of '%s'"), I, *UcasId);
					RemoteToc.Reset();
					IFileManager::Get().Delete(*TmpUtoc);
					return false;
				}
				++OutStats.NumDropped;
				if (LocalSpans.IsValidIndex(I) && LocalSpans[I].Size > 0)
				{
					NewHoles.Add(LocalSpans[I]);
				}
			}
			if (Dropped.Num() > 0)
			{
				UE_LOG(LogUnrealGamePatcher, Display,
					TEXT("FInPlaceIoStorePatcher: '%s': %d local chunk(s) dropped (dead bytes recorded as holes)"),
					*UcasId, Dropped.Num());
			}
		}
		// Close the remote TOC reader BEFORE deleting the temp (see the
		// TUniquePtr comment above - the handle blocks DeleteFile otherwise).
		RemoteToc.Reset();
		IFileManager::Get().Delete(*TmpUtoc);

		if (!Writer.Commit(LocalUtocPath))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlaceIoStorePatcher: commit failed for '%s'"), *UcasId);
			return false;
		}
		OutStats.UpdatedHoles = Writer.GetPlacementHoles();
		for (const FHole& H : NewHoles)
		{
			OutStats.UpdatedHoles.Add(H);
		}
		OutStats.NumHolesFilled = Writer.GetNumHolesFilled();

		// Verify by chunk-id MULTISET equivalence (the in-place layout is
		// raw-appended, so the file is NOT byte-identical to the remote ucas,
		// and the local chunk ORDER may legitimately differ - the container is
		// a virtual disk).
		{
			FIoStoreReader Check;
			if (!Check.Open(LocalUtocPath, LocalUcasPath))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlaceIoStorePatcher: patched container does not parse ('%s')"), *UcasId);
				return false;
			}
			if (Check.GetChunkCount() != RemoteFile->IoStoreChunks.Num())
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlaceIoStorePatcher: patched chunk count %d != remote %d ('%s')"),
					Check.GetChunkCount(), RemoteFile->IoStoreChunks.Num(), *UcasId);
				return false;
			}
			// Identity = (Id, MetaHash) when the remote metadata records the
			// content hash - the rewritten utoc claims the remote's identity.
			bool bMeta = true;
			for (const FIndexIoStoreChunk& C : RemoteFile->IoStoreChunks)
			{
				if (C.MetaHash.IsEmpty()) { bMeta = false; break; }
			}
			TArray<FString> LIds, RIds;
			for (int32 I = 0; I < Check.GetChunkCount(); ++I)
			{
				LIds.Add(bMeta ? Check.GetChunkId(I) + TEXT("|") + Check.GetChunks()[I].Hash : Check.GetChunkId(I));
			}
			for (const FIndexIoStoreChunk& C : RemoteFile->IoStoreChunks)
			{
				RIds.Add(bMeta ? C.Id + TEXT("|") + C.MetaHash : C.Id);
			}
			LIds.Sort();
			RIds.Sort();
			for (int32 I = 0; I < LIds.Num(); ++I)
			{
				if (!LIds[I].Equals(RIds[I], ESearchCase::IgnoreCase))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FInPlaceIoStorePatcher: chunk id mismatch after patch ('%s')"), *UcasId);
					return false;
				}
			}
		}

		UE_LOG(LogUnrealGamePatcher, Display,
			TEXT("FInPlaceIoStorePatcher: '%s': %d same (zero movement), %d replaced, %d appended; %lld raw bytes written"),
			*UcasId, OutStats.NumSame, OutStats.NumReplaced, OutStats.NumAppended, OutStats.BytesWritten);
		// Per-patch I/O report (same purpose as the pak patcher's): a chunk-
		// level patch must read only the changed chunks' spans plus the small
		// remote utoc - read volume near the container size means the delta
		// path silently degraded.
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("FInPlaceIoStorePatcher: '%s' I/O: %s"),
			*UcasId, *FIOStats::FormatDelta(IOStart, FIOStats::Get().Snapshot()));
		return true;
	}
}
