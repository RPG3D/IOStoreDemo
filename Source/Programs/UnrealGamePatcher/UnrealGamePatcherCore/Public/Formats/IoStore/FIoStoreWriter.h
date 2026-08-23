// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Formats/IoStore/FIoStoreReader.h"
#include "State/FPatcherState.h"

namespace UnrealGamePatcher
{
	/**
	 * Self-written IoStore container updater (tasks 7.2/7.3): appends new /
	 * replaced chunk data as raw blocks at the end of the .ucas (block
	 * aligned, unchanged chunks are never moved) and rebuilds the .utoc
	 * entirely (temp file + atomic replace, design D6).
	 *
	 * Chunk identity: with no explicit target, the new chunk id/meta are the
	 * FIoHash (BLAKE3-160) of the raw data (content-addressed - the scheme
	 * UGP's own crafted containers use). REAL engine containers derive chunk
	 * ids from the PACKAGE name (FPackageId::FromName), so the caller passes
	 * the remote metadata's (Id, MetaHash) explicitly: the rebuilt utoc then
	 * claims the remote's identity while the appended raw bytes hash exactly
	 * to the remote's meta hash.
	 *
	 * Layout rule: a new chunk is placed at raw offset BlockCount*BlockSize
	 * and gets exactly ceil(Size/BlockSize) new block entries appended, so
	 * the block array stays dense and the reader's index math holds.
	 */
	struct FIoStoreWriter
	{
		/** Parses the existing container (must already exist). */
		UNREALGAMEPATCHERCORE_API bool Open(const FString& UtocPath, const FString& UcasPath);

		/** Replaces chunk Index's data: the old blocks stay in place (dead
		 *  weight, reclaimable by compact), the chunk entry is redirected to
		 *  the appended raw blocks. OutNewId is the chunk's id. */
		bool ReplaceChunk(int32 Index, const TArray64<uint8>& NewData, FString& OutNewId)
		{
			return ReplaceChunkEx(Index, NewData, FString(), FString(), OutNewId);
		}

		/** Replace with an explicit target identity (the remote metadata's
		 *  id + meta hash). Empty targets = derive from the data. */
		UNREALGAMEPATCHERCORE_API bool ReplaceChunkEx(int32 Index, const TArray64<uint8>& NewData,
			const FString& InTargetId, const FString& InTargetMetaHash, FString& OutNewId);

		/** Appends a brand-new chunk (content-addressed identity). */
		bool AppendChunk(const TArray64<uint8>& Data, FString& OutNewId)
		{
			return AppendChunkEx(Data, FString(), FString(), OutNewId);
		}

		/** Append with an explicit target identity (remote metadata). */
		UNREALGAMEPATCHERCORE_API bool AppendChunkEx(const TArray64<uint8>& Data,
			const FString& InTargetId, const FString& InTargetMetaHash, FString& OutNewId);

		/** Removes chunk Index from the rebuilt TOC (in-place chunk removal):
		 *  its blocks stay in the .ucas as dead bytes (recordable as a hole)
		 *  and other chunks' block indices are unaffected - the dense block
		 *  array is kept intact, only the id/offset entries are dropped. */
		UNREALGAMEPATCHERCORE_API bool DropChunk(int32 Index);

		/** Appends the new blocks to the .ucas and writes the rebuilt .utoc
		 *  to OutUtocPath (atomic). The source .utoc is left untouched. */
		UNREALGAMEPATCHERCORE_API bool Commit(const FString& OutUtocPath);

		UNREALGAMEPATCHERCORE_API int32 GetChunkCount() const { return Chunks.Num(); }
		UNREALGAMEPATCHERCORE_API const FIoStoreChunk& GetChunk(int32 Index) const { return Chunks[Index]; }

		/** Free ranges (dead byte regions of THIS .ucas, recorded by earlier
		 *  patches - e.g. a replaced chunk's old block span) that raw chunk
		 *  appends may backfill into instead of appending at EOF. Chunks land
		 *  block-aligned inside a range; the logical block order stays dense
		 *  regardless (engine compatibility: block indices derive from the
		 *  chunk's RawOffset, physical placement comes from the block
		 *  entries' FileOffset). A filled range is replaced by its tail
		 *  remainder; GetPlacementHoles() reflects the post-placement state. */
		UNREALGAMEPATCHERCORE_API void SetPlacementHoles(const TArray<FHole>& InHoles) { PlacementHoles = InHoles; }
		UNREALGAMEPATCHERCORE_API const TArray<FHole>& GetPlacementHoles() const { return PlacementHoles; }
		UNREALGAMEPATCHERCORE_API int32 GetNumHolesFilled() const { return NumHolesFilled; }

	private:
		bool AppendRawBlocks(const TArray64<uint8>& Data, int64 PhysicalBase, int64& OutRawOffset, int32& OutFirstBlock, int32& OutNumBlocks);

		// Parsed state (copy of the original).
		FString UtocPath;
		FString UcasPath;
		int32 CompressionBlockSize = 0;
		TArray<FString> ChunkIds;
		TArray<FIoStoreChunk> Chunks;
		TArray<FIoStoreBlock> Blocks;
		TArray<FString> CompressionMethods;
		TArray<uint8> DirectoryIndex;
		int32 OriginalBlockCount = 0;
		int64 OriginalUcasSize = 0;
		// Working state for Commit.
		TArray64<uint8> AppendedUcasData; // new blocks to append to the .ucas
		TArray<FIoStoreBlock> AppendedBlocks;

		// Backfill: dead ranges offered for raw-block placement, and the
		// hole-placed data Commit() must pwrite (vs the EOF-appended buffer).
		TArray<FHole> PlacementHoles;
		int32 NumHolesFilled = 0;
		struct FPendingHoleWrite
		{
			int64 PhysicalOffset = 0;
			TArray64<uint8> Data;
		};
		TArray<FPendingHoleWrite> HoleWrites;

		/** Best-fitting block-aligned hole for Bytes bytes, or -1 (EOF).
		 *  Consumes the range (tail remainder kept). */
		int64 PickHoleBase(int64 Bytes);
	};
}
