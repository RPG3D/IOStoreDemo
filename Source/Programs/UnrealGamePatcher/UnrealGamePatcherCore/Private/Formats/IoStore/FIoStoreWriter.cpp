// Copyright Epic Games, Inc. All Rights Reserved.

#include "Formats/IoStore/FIoStoreWriter.h"
#include "Hash/FPatcherHash.h"
#include "Stats/FIOStats.h"
#include "UnrealGamePatcherLog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "IO/IoHash.h"
#include "Misc/Char.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SecureHash.h"

namespace UnrealGamePatcher
{
	namespace
	{
		static constexpr const char* TocMagic = "-==--==--==--==-";
		static constexpr int32 HeaderSize = 144;

		uint64 Read40MsbFirst(const uint8* P)
		{
			return (uint64(P[0]) << 32) | (uint64(P[1]) << 24) | (uint64(P[2]) << 16) | (uint64(P[3]) << 8) | uint64(P[4]);
		}

		uint64 Read40Le(const uint8* P)
		{
			return uint64(P[0]) | (uint64(P[1]) << 8) | (uint64(P[2]) << 16) | (uint64(P[3]) << 24) | (uint64(P[4]) << 32);
		}

		uint32 ReadU32(const uint8* P)
		{
			return uint32(P[0]) | (uint32(P[1]) << 8) | (uint32(P[2]) << 16) | (uint32(P[3]) << 24);
		}

		uint64 AlignUp64(uint64 Value, uint64 Alignment)
		{
			return (Value + Alignment - 1) / Alignment * Alignment;
		}

		void Write40Le(TArray<uint8>& Out, uint64 V)
		{
			Out.Add(V & 0xFF);
			Out.Add((V >> 8) & 0xFF);
			Out.Add((V >> 16) & 0xFF);
			Out.Add((V >> 24) & 0xFF);
			Out.Add((V >> 32) & 0xFF);
		}

		void WriteBlock(TArray<uint8>& Out, const FIoStoreBlock& B)
		{
			// 5 bytes offset | 3 bytes compressed size | 3 bytes uncompressed
			// size | 1 byte method = 12 bytes (FIoStoreTocCompressedBlockEntry).
			Write40Le(Out, (uint64)B.FileOffset);
			Out.Add(B.CompressedSize & 0xFF);
			Out.Add((B.CompressedSize >> 8) & 0xFF);
			Out.Add((B.CompressedSize >> 16) & 0xFF);
			Out.Add(B.UncompressedSize & 0xFF);
			Out.Add((B.UncompressedSize >> 8) & 0xFF);
			Out.Add((B.UncompressedSize >> 16) & 0xFF);
			Out.Add(B.MethodIndex & 0xFF);
		}

		void WriteU32(TArray<uint8>& Out, uint32 V)
		{
			Out.Add(V & 0xFF);
			Out.Add((V >> 8) & 0xFF);
			Out.Add((V >> 16) & 0xFF);
			Out.Add((V >> 24) & 0xFF);
		}

		void Write40MsbFirst(TArray<uint8>& Out, uint64 V)
		{
			Out.Add((V >> 32) & 0xFF);
			Out.Add((V >> 24) & 0xFF);
			Out.Add((V >> 16) & 0xFF);
			Out.Add((V >> 8) & 0xFF);
			Out.Add(V & 0xFF);
		}
	}

	bool FIoStoreWriter::Open(const FString& InUtocPath, const FString& InUcasPath)
	{
		UtocPath = InUtocPath;
		UcasPath = InUcasPath;
		Chunks.Reset();
		Blocks.Reset();
		AppendedUcasData.Reset();
		AppendedBlocks.Reset();
		DirectoryIndex.Reset();

		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		IFileHandle* Handle = PF.OpenRead(*UtocPath);
		if (!Handle)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreWriter: cannot open '%s'"), *UtocPath);
			return false;
		}
	const int64 UtocSize = Handle->Size();
	OriginalUcasSize = IFileManager::Get().FileSize(*UcasPath);

	// Detect the trailing container-header stub (see FIoStoreReader::Open):
	// appended blocks must go BEFORE it and Commit re-appends it last.
	UcasTrailerSize = 0;
	{
		IFileHandle* UcasTail = PF.OpenRead(*UcasPath);
		uint8 Tail[8] = {};
		if (UcasTail && UcasTail->Size() >= 16 && UcasTail->Seek(UcasTail->Size() - 8) &&
			UcasTail->Read(Tail, 8))
		{
			const uint32 Sig = (uint32)Tail[0] | ((uint32)Tail[1] << 8) | ((uint32)Tail[2] << 16) | ((uint32)Tail[3] << 24);
			const uint32 Ver = (uint32)Tail[4] | ((uint32)Tail[5] << 8) | ((uint32)Tail[6] << 16) | ((uint32)Tail[7] << 24);
			if (Sig == 0x496F436E && Ver >= 1 && Ver <= 5) // FIoContainerHeader::Signature, Initial..Latest
			{
				FMemory::Memcpy(UcasTrailer, Tail, 8);
				UcasTrailerSize = 8;
				OriginalUcasSize -= UcasTrailerSize;
			}
		}
		delete UcasTail;
	}
	TArray<uint8> Toc;
	Toc.SetNumUninitialized(UtocSize);
	if (!Handle->Read(Toc.GetData(), UtocSize))
	{
		delete Handle;
		return false;
	}
	delete Handle;
	FIOStats::Get().AddDiskRead(UtocSize);

		const uint8* P = Toc.GetData();
		if (UtocSize < HeaderSize || FMemory::Memcmp(P, TocMagic, 16) != 0)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreWriter: '%s' is not a valid IoStore TOC"), *UtocPath);
			return false;
		}

		const uint32 TocHeaderSize = ReadU32(P + 20);
		const uint32 EntryCount = ReadU32(P + 24);
		const uint32 BlockCount = ReadU32(P + 28);
		const uint32 BlockEntrySize = ReadU32(P + 32);
		const uint32 NameCount = ReadU32(P + 36);
		const uint32 NameLength = ReadU32(P + 40);
		CompressionBlockSize = (int32)ReadU32(P + 44);
		const uint32 DirectoryIndexSize = ReadU32(P + 48);
		const uint32 PartitionCount = ReadU32(P + 52);
		const uint32 ContainerFlags = ReadU32(P + 80);
		const uint32 PerfectHashSeedsCount = ReadU32(P + 84);
		const uint32 ChunksWithoutPerfectHashCount = ReadU32(P + 96);

		if (TocHeaderSize != HeaderSize || BlockEntrySize != 12 || EntryCount == 0 || BlockCount == 0)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreWriter: '%s' bad TOC header"), *UtocPath);
			return false;
		}
		// Validate the declared layout against the actual TOC size BEFORE any
		// allocation or table read - a truncated/corrupt utoc must fail with a
		// logged error, not with an out-of-bounds read or a divide-by-zero
		// (the reader does this; the writer's copy of the parse used to skip it).
		if (CompressionBlockSize <= 0)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreWriter: '%s' bad compression block size %d"), *UtocPath, CompressionBlockSize);
			return false;
		}
		{
			const int64 Needed = (int64)HeaderSize
				+ (int64)EntryCount * (12 + 10 + 24)
				+ (int64)PerfectHashSeedsCount * 4
				+ (int64)ChunksWithoutPerfectHashCount * 4
				+ (int64)BlockCount * 12
				+ (int64)NameCount * NameLength
				+ (int64)DirectoryIndexSize;
			if (Needed > (int64)UtocSize)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreWriter: '%s' TOC tables (%lld bytes) exceed file size (%lld bytes) - corrupt container"),
					*UtocPath, Needed, (int64)UtocSize);
				return false;
			}
		}
		OriginalBlockCount = (int32)BlockCount;

		int64 Offset = HeaderSize;
		ChunkIds.SetNum(EntryCount);
		for (uint32 I = 0; I < EntryCount; ++I)
		{
			ChunkIds[I] = FPatcherHash::BytesToHex(P + Offset + I * 12, 12);
		}
		Offset += (int64)EntryCount * 12;

		Chunks.SetNum(EntryCount);
		for (uint32 I = 0; I < EntryCount; ++I)
		{
			const uint8* E = P + Offset + I * 10;
			Chunks[I].Id = ChunkIds[I];
			Chunks[I].RawOffset = (int64)Read40MsbFirst(E);
			Chunks[I].RawSize = (int64)Read40MsbFirst(E + 5);
		}
		Offset += (int64)EntryCount * 10;
		Offset += (int64)PerfectHashSeedsCount * 4;
		Offset += (int64)ChunksWithoutPerfectHashCount * 4;

		Blocks.SetNum(BlockCount);
		for (uint32 I = 0; I < BlockCount; ++I)
		{
			const uint8* B = P + Offset + I * 12;
			Blocks[I].FileOffset = (int64)Read40Le(B);
			Blocks[I].CompressedSize = (int32)((ReadU32(B + 4) >> 8) & 0xFFFFFF);
			Blocks[I].UncompressedSize = (int32)(ReadU32(B + 8) & 0xFFFFFF);
			Blocks[I].MethodIndex = (int32)(ReadU32(B + 8) >> 24);
		}
		Offset += (int64)BlockCount * 12;

		CompressionMethods.Reset();
		CompressionMethods.Add(TEXT("None"));
		for (uint32 I = 0; I < NameCount; ++I)
		{
			CompressionMethods.Add(FString(ANSI_TO_TCHAR((const ANSICHAR*)(P + Offset + I * NameLength))));
		}
		Offset += (int64)NameCount * NameLength;

		if (DirectoryIndexSize > 0)
		{
			DirectoryIndex.SetNum(DirectoryIndexSize);
			FMemory::Memcpy(DirectoryIndex.GetData(), P + Offset, DirectoryIndexSize);
		}
		Offset += DirectoryIndexSize;

		// Meta hashes (updates live in Chunks[i].Hash).
		for (uint32 I = 0; I < EntryCount; ++I)
		{
			const uint8* M = P + Offset + I * 24;
			Chunks[I].Hash = FPatcherHash::BytesToHex(M, 20);
			Chunks[I].bIsCompressed = (M[20] & 0x01) != 0;
		}

		// Block ranges.
		for (FIoStoreChunk& C : Chunks)
		{
			const int64 RawEnd = C.RawOffset + C.RawSize;
			C.FirstBlockIndex = (int32)(C.RawOffset / CompressionBlockSize);
			C.NumBlocks = (int32)((AlignUp64(RawEnd, CompressionBlockSize) - 1) / CompressionBlockSize) - C.FirstBlockIndex + 1;
		}

		(void)PartitionCount;
		(void)ContainerFlags;
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("FIoStoreWriter: opened '%s' (%d chunks, %d blocks)"), *UtocPath, Chunks.Num(), Blocks.Num());
		return true;
	}

	int64 FIoStoreWriter::PickHoleBase(int64 Bytes)
	{
		if (PlacementHoles.Num() == 0 || CompressionBlockSize <= 0 || Bytes <= 0)
		{
			return -1;
		}
		const int64 Needed = (int64)FMath::DivideAndRoundUp(Bytes, (int64)CompressionBlockSize) * CompressionBlockSize;
		int64 BestIdx = INDEX_NONE;
		int64 BestBase = -1;
		int64 BestWaste = TNumericLimits<int64>::Max();
		for (int32 Idx = 0; Idx < PlacementHoles.Num(); ++Idx)
		{
			const FHole& H = PlacementHoles[Idx];
			const int64 Base = (int64)FMath::DivideAndRoundUp(H.Offset, (int64)CompressionBlockSize) * CompressionBlockSize;
			const int64 End = H.Offset + H.Size;
			// Holes must sit inside the parsed container - the raw block
			// entries' FileOffsets are 40-bit and the region must exist.
			if (Base + Needed <= End && Base + Needed <= OriginalUcasSize)
			{
				const int64 Waste = End - (Base + Needed);
				if (Waste < BestWaste)
				{
					BestWaste = Waste;
					BestIdx = Idx;
					BestBase = Base;
				}
			}
		}
		if (BestIdx == INDEX_NONE)
		{
			return -1;
		}
		// Consume: keep the unaligned head sliver (< one block, can never
		// host a block but is still dead bytes the bloat report must count)
		// and the tail remainder.
		const FHole Filled = PlacementHoles[BestIdx];
		const int64 UsedEnd = BestBase + Needed;
		PlacementHoles.RemoveAt(BestIdx);
		int32 InsertAt = BestIdx;
		if (BestBase > Filled.Offset)
		{
			FHole Head;
			Head.File = Filled.File;
			Head.Offset = Filled.Offset;
			Head.Size = BestBase - Filled.Offset;
			PlacementHoles.Insert(Head, InsertAt);
			++InsertAt;
		}
		if (UsedEnd < Filled.Offset + Filled.Size)
		{
			FHole Tail;
			Tail.File = Filled.File;
			Tail.Offset = UsedEnd;
			Tail.Size = Filled.Offset + Filled.Size - UsedEnd;
			PlacementHoles.Insert(Tail, InsertAt);
		}
		++NumHolesFilled;
		return BestBase;
	}

	bool FIoStoreWriter::AppendRawBlocks(const TArray64<uint8>& Data, int64 PhysicalBase, int64& OutRawOffset, int32& OutFirstBlock, int32& OutNumBlocks)
	{
		// Place the new chunk at the first raw offset past the block array
		// (including blocks appended by earlier edits this session), so its
		// block indices start exactly at the current array length (dense).
		// The LOGICAL order is dense regardless of physical placement; when
		// PhysicalBase >= 0 the bytes backfill a recorded dead region and
		// Commit() pwrites them there instead of appending at EOF.
		const int32 CurrentBlockCount = OriginalBlockCount + AppendedBlocks.Num();
		OutRawOffset = (int64)CurrentBlockCount * CompressionBlockSize;
		OutFirstBlock = CurrentBlockCount;

		const int32 BS = CompressionBlockSize;
		const bool bBackfill = PhysicalBase >= 0;
		int32 NumBlocks = 0;
		if (bBackfill)
		{
			FPendingHoleWrite W;
			W.PhysicalOffset = PhysicalBase;
			int64 Cursor = 0;
			while (Cursor < (int64)Data.Num())
			{
				const int32 BlockSize = (int32)FMath::Min<int64>(BS, Data.Num() - Cursor);
				FIoStoreBlock B;
				B.FileOffset = PhysicalBase + (int64)NumBlocks * BS;
				B.CompressedSize = BlockSize;
				B.UncompressedSize = BlockSize;
				B.MethodIndex = 0; // raw
				AppendedBlocks.Add(B);
				Cursor += BlockSize;
				++NumBlocks;
			}
			if (NumBlocks == 0)
			{
				FIoStoreBlock B;
				B.FileOffset = PhysicalBase;
				B.CompressedSize = 0;
				B.UncompressedSize = 0;
				B.MethodIndex = 0;
				AppendedBlocks.Add(B);
				++NumBlocks;
			}
			W.Data = Data;
			HoleWrites.Add(MoveTemp(W));
			OutNumBlocks = NumBlocks;
			return true;
		}

		int64 Offset = 0;
		while (Offset < (int64)Data.Num())
		{
			const int32 BlockSize = (int32)FMath::Min<int64>(BS, Data.Num() - Offset);
			FIoStoreBlock B;
			B.FileOffset = OriginalUcasSize + (int64)AppendedUcasData.Num();
			B.CompressedSize = BlockSize;
			B.UncompressedSize = BlockSize;
			B.MethodIndex = 0; // raw
			AppendedBlocks.Add(B);
			AppendedUcasData.Append(Data.GetData() + Offset, BlockSize);
			Offset += BlockSize;
			++NumBlocks;
		}
		if (NumBlocks == 0)
		{
			// Empty chunk: one empty block (never read; the reader skips it).
			FIoStoreBlock B;
			B.FileOffset = OriginalUcasSize + (int64)AppendedUcasData.Num();
			B.CompressedSize = 0;
			B.UncompressedSize = 0;
			B.MethodIndex = 0;
			AppendedBlocks.Add(B);
			++NumBlocks;
		}
		OutNumBlocks = NumBlocks;
		return true;
	}

	/** Content-addressed identity for a chunk: the TOC chunk id is the first
	 *  12 bytes of the content hash (FIoHash, BLAKE3-160), the meta hash is
	 *  the full 20 bytes. */
	static void MakeChunkIdentity(const TArray64<uint8>& Data, FString& OutId, FString& OutHash)
	{
		const FIoHash NewHash = FIoHash::HashBuffer(Data.GetData(), Data.Num());
		const FString FullHex = FPatcherHash::FIoHashToHex(NewHash);
		OutHash = FullHex;
		OutId = FullHex.Left(24); // first 12 bytes
	}

	bool FIoStoreWriter::ReplaceChunkEx(int32 Index, const TArray64<uint8>& NewData,
		const FString& InTargetId, const FString& InTargetMetaHash, FString& OutNewId)
	{
		if (Index < 0 || Index >= Chunks.Num())
		{
			return false;
		}
		FString NewId, NewHash;
		if (!InTargetId.IsEmpty() && !InTargetMetaHash.IsEmpty())
		{
			// The remote metadata's identity: the id may be package-
			// derived (engine containers), and the meta hash describes
			// exactly the raw bytes being written - claim it verbatim.
			NewId = InTargetId;
			NewHash = InTargetMetaHash;
		}
		else
		{
			MakeChunkIdentity(NewData, NewId, NewHash);
		}

		int64 RawOffset = 0;
		int32 FirstBlock = 0, NumBlocks = 0;
		if (!AppendRawBlocks(NewData, PickHoleBase(NewData.Num()), RawOffset, FirstBlock, NumBlocks))
		{
			return false;
		}

		ChunkIds[Index] = NewId;
		Chunks[Index].Id = NewId;
		Chunks[Index].RawOffset = RawOffset;
		Chunks[Index].RawSize = NewData.Num();
		Chunks[Index].Hash = NewHash;
		Chunks[Index].bIsCompressed = false;
		Chunks[Index].FirstBlockIndex = FirstBlock;
		Chunks[Index].NumBlocks = NumBlocks;

		OutNewId = NewId;
		return true;
	}

	bool FIoStoreWriter::AppendChunkEx(const TArray64<uint8>& Data,
		const FString& InTargetId, const FString& InTargetMetaHash, FString& OutNewId)
	{
		FString NewId, NewHash;
		if (!InTargetId.IsEmpty() && !InTargetMetaHash.IsEmpty())
		{
			NewId = InTargetId;
			NewHash = InTargetMetaHash;
		}
		else
		{
			MakeChunkIdentity(Data, NewId, NewHash);
		}

		int64 RawOffset = 0;
		int32 FirstBlock = 0, NumBlocks = 0;
		if (!AppendRawBlocks(Data, PickHoleBase(Data.Num()), RawOffset, FirstBlock, NumBlocks))
		{
			return false;
		}

		FIoStoreChunk C;
		C.Id = NewId;
		C.RawOffset = RawOffset;
		C.RawSize = Data.Num();
		C.Hash = NewHash;
		C.bIsCompressed = false;
		C.FirstBlockIndex = FirstBlock;
		C.NumBlocks = NumBlocks;
		ChunkIds.Add(NewId);
		Chunks.Add(C);

		OutNewId = NewId;
		return true;
	}

	bool FIoStoreWriter::DropChunk(int32 Index)
	{
		if (Index < 0 || Index >= Chunks.Num())
		{
			return false;
		}
		// Drop only the id/offset entries: the chunk's blocks stay in the
		// dense block array (unreferenced = dead bytes, reclaimable by
		// compact) so every other chunk's FirstBlockIndex remains valid and
		// the engine's RawOffset -> block-index derivation is unchanged.
		ChunkIds.RemoveAt(Index);
		Chunks.RemoveAt(Index);
		return true;
	}

	bool FIoStoreWriter::Commit(const FString& OutUtocPath)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

		// 1. Append the new blocks to the .ucas. The write is POSITIONED at
		// the end of the block data (OriginalUcasSize, i.e. before any
		// trailing container-header stub) and the stub is re-written last,
		// so the container keeps a fresh engine container's EOF layout:
		// [block data][appended blocks][stub].
		// The handle MUST be opened with bAppend=true: OpenWrite(bAppend=
		// false) TRUNCATES the file on POSIX/Apple (ftruncate) while it only
		// recreates-on-open on Windows. bAppend=true positions at EOF on
		// both and still allows explicit Seek + positioned writes.
		if (AppendedUcasData.Num() > 0 || UcasTrailerSize > 0)
		{
			IFileHandle* Ucas = PF.OpenWrite(*UcasPath, /*bAppend=*/true);
			if (!Ucas)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreWriter: cannot append to '%s'"), *UcasPath);
				return false;
			}
		const bool bOk = Ucas->Seek(OriginalUcasSize) &&
			(AppendedUcasData.Num() == 0 || Ucas->Write(AppendedUcasData.GetData(), AppendedUcasData.Num())) &&
			(UcasTrailerSize == 0 || Ucas->Write(UcasTrailer, UcasTrailerSize));
		delete Ucas;
		if (!bOk)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreWriter: failed appending to '%s'"), *UcasPath);
			return false;
		}
		FIOStats::Get().AddDiskWrite(AppendedUcasData.Num() + UcasTrailerSize);
		}

		// 1b. Backfilled blocks: pwrite into the recorded dead regions (the
		// logical block order stays dense - physical placement is free).
		for (const FPendingHoleWrite& W : HoleWrites)
		{
			IFileHandle* Ucas = PF.OpenWrite(*UcasPath, /*bAppend=*/true);
			if (!Ucas)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreWriter: cannot open '%s' for backfill"), *UcasPath);
				return false;
			}
		const bool bWrote = W.Data.Num() == 0 ||
			(Ucas->Seek(W.PhysicalOffset) && Ucas->Write(W.Data.GetData(), W.Data.Num()));
		delete Ucas;
		if (!bWrote)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreWriter: failed backfilling '%s' at %lld"), *UcasPath, W.PhysicalOffset);
			return false;
		}
		FIOStats::Get().AddDiskWrite(W.Data.Num());
		}

		// 2. Rebuild the .utoc completely.
		const int32 EntryCount = Chunks.Num();
		const int32 BlockCount = OriginalBlockCount + AppendedBlocks.Num();
		const uint32 DirectoryIndexSize = DirectoryIndex.Num();

		TArray<uint8> Out;
		// Header.
		{
			const char* Magic = "-==--==--==--==-";
			for (int32 I = 0; I < 16; ++I) { Out.Add((uint8)Magic[I]); }
			Out.Add(8);              // Version
			Out.Add(0);              // Reserved0
			Out.Add(0); Out.Add(0);  // Reserved1
			WriteU32(Out, HeaderSize);
			WriteU32(Out, EntryCount);
			WriteU32(Out, BlockCount);
			WriteU32(Out, 12);
			WriteU32(Out, CompressionMethods.Num() - 1);
			WriteU32(Out, 32);
			WriteU32(Out, CompressionBlockSize);
			WriteU32(Out, DirectoryIndexSize);
			WriteU32(Out, 1); // PartitionCount
			for (int32 I = 0; I < 8; ++I) { Out.Add(0); } // ContainerId
			for (int32 I = 0; I < 16; ++I) { Out.Add(0); } // EncryptionKeyGuid
			Out.Add(0x09); // ContainerFlags: Compressed|Indexed (matching the source)
			Out.Add(0);    // Reserved3
			Out.Add(0); Out.Add(0); // Reserved4
			WriteU32(Out, 0);        // PerfectHashSeedsCount
			Out.Add(0xFF); Out.Add(0xFF); Out.Add(0xFF); Out.Add(0xFF); Out.Add(0xFF); Out.Add(0xFF); Out.Add(0xFF); Out.Add(0xFF); // PartitionSize
			WriteU32(Out, 0);        // ChunksWithoutPerfectHashCount
			WriteU32(Out, 0);        // Reserved7
			for (int32 I = 0; I < 40; ++I) { Out.Add(0); } // Reserved8[5]
		}

		// Chunk ids.
		for (const FString& Id : ChunkIds)
		{
			TArray<uint8> Bytes;
			if (!FPatcherHash::HexToBytes(Id, Bytes))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreWriter: chunk id '%s' is not valid hex"), *Id);
				return false;
			}
			Out.Append(Bytes);
		}
		// Offset-lengths.
		for (const FIoStoreChunk& C : Chunks)
		{
			Write40MsbFirst(Out, C.RawOffset);
			Write40MsbFirst(Out, C.RawSize);
		}
		// (No perfect hash seeds, no without-perfect-hash indices.)

		// Compression blocks: original + appended.
		for (const FIoStoreBlock& B : Blocks)
		{
			WriteBlock(Out, B);
		}
		for (const FIoStoreBlock& B : AppendedBlocks)
		{
			WriteBlock(Out, B);
		}

		// Compression method names.
		for (int32 I = 1; I < CompressionMethods.Num(); ++I)
		{
			// Hoist the converter: TCHAR_TO_ANSI would leave 'Name' pointing
			// into a temporary that dies at the end of the full-expression
			// (UB; clang -Wdangling rejects it, MSVC happened to tolerate it).
			const auto Converted = StringCast<ANSICHAR>(*CompressionMethods[I]);
			const ANSICHAR* Name = Converted.Get();
			const int32 Len = FMath::Min(31, (int32)FCStringAnsi::Strlen(Name));
			Out.Append((const uint8*)Name, Len);
			for (int32 J = Len; J < 32; ++J) { Out.Add(0); }
		}

		// Directory index (unchanged).
		Out.Append(DirectoryIndex);

		// Meta.
		for (const FIoStoreChunk& C : Chunks)
		{
			TArray<uint8> Bytes;
			if (!FPatcherHash::HexToBytes(C.Hash, Bytes) || Bytes.Num() != 20)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreWriter: chunk hash '%s' is not valid 40-hex"), *C.Hash);
				return false;
			}
			Out.Append(Bytes);
			Out.Add(C.bIsCompressed ? 0x01 : 0x00); // flags
			Out.Add(0); Out.Add(0); Out.Add(0);
		}

		// 3. Write the new .utoc atomically.
		if (!PF.CreateDirectoryTree(*FPaths::GetPath(OutUtocPath)))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreWriter: cannot create dir for '%s'"), *OutUtocPath);
			return false;
		}
	const FString TempPath = OutUtocPath + TEXT(".tmp");
	if (!FFileHelper::SaveArrayToFile(Out, *TempPath))
	{
		UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreWriter: cannot write '%s'"), *TempPath);
		return false;
	}
	FIOStats::Get().AddDiskWrite(Out.Num());
		if (PF.FileExists(*OutUtocPath))
		{
			PF.DeleteFile(*OutUtocPath);
		}
		if (!PF.MoveFile(*OutUtocPath, *TempPath))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreWriter: cannot replace '%s'"), *OutUtocPath);
			return false;
		}

		UE_LOG(LogUnrealGamePatcher, Display,
			TEXT("FIoStoreWriter: wrote '%s' (%d chunks, %d blocks, %lld bytes appended to ucas)"),
			*OutUtocPath, EntryCount, BlockCount, AppendedUcasData.Num());
		return true;
	}
}
