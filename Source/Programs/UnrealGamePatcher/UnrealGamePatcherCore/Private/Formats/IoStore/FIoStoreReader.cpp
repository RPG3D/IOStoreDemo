// Copyright Epic Games, Inc. All Rights Reserved.

#include "Formats/IoStore/FIoStoreReader.h"
#include "Hash/FPatcherHash.h"
#include "IO/IoHash.h"
#include "Stats/FIOStats.h"
#include "UnrealGamePatcherLog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Compression.h"
#include "Misc/Paths.h"
#include "SecureHash.h"

namespace UnrealGamePatcher
{
	namespace
	{
		// IoStore TOC header (FIoStoreTocHeader, engine IoStore.h). Verified
		// against real UE5 packaging output.
		static constexpr const char* TocMagic = "-==--==--==--==-";
		static constexpr int32 HeaderSize = 144;

		// Packed 40-bit value: bytes [4]=LSB .. [0]=MSB (used by the offset-
		// length table and the block entries' little-endian uint64 mask).
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

		int64 AlignUp(int64 Value, int64 Alignment)
		{
			return (Value + Alignment - 1) / Alignment * Alignment;
		}

		bool ReadBlock(const uint8* Bytes, int64 Size, FIoStoreBlock& OutBlock)
		{
			if (Size < 12)
			{
				return false;
			}
			OutBlock.FileOffset = (int64)Read40Le(Bytes);
			OutBlock.CompressedSize = (int32)((ReadU32(Bytes + 4) >> 8) & 0xFFFFFF);
			OutBlock.UncompressedSize = (int32)(ReadU32(Bytes + 8) & 0xFFFFFF);
			OutBlock.MethodIndex = (int32)(ReadU32(Bytes + 8) >> 24);
			return true;
		}
	}

	bool FIoStoreReader::Open(const FString& InUtocPath, const FString& InUcasPath)
	{
		if (!OpenTocOnly(InUtocPath))
		{
			return false;
		}
		UcasPath = InUcasPath;
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		UcasHandle = PF.OpenRead(*UcasPath);
		if (!UcasHandle)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: cannot open '%s'"), *UcasPath);
			return false;
		}
		return true;
	}

	bool FIoStoreReader::OpenTocOnly(const FString& InUtocPath)
	{
		Close();
		UtocPath = InUtocPath;
		UcasPath.Empty();

		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		IFileHandle* UtocHandle = PF.OpenRead(*UtocPath);
		if (!UtocHandle)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: cannot open '%s'"), *UtocPath);
			return false;
		}
		const int64 UtocSize = UtocHandle->Size();
		TArray<uint8> Toc;
		Toc.SetNumUninitialized(UtocSize);
		if (!UtocHandle->Read(Toc.GetData(), UtocSize))
		{
			delete UtocHandle;
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: read failed on '%s'"), *UtocPath);
			return false;
		}
		delete UtocHandle;
		FIOStats::Get().AddDiskRead(UtocSize);

		const uint8* P = Toc.GetData();
		if (UtocSize < HeaderSize || FMemory::Memcmp(P, TocMagic, 16) != 0)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: '%s' is not a valid IoStore TOC"), *UtocPath);
			return false;
		}

		const uint8 Version = P[16];
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

		if (TocHeaderSize != HeaderSize || BlockEntrySize != 12)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: '%s' header mismatch (header %u, block entry %u)"),
				*UtocPath, TocHeaderSize, BlockEntrySize);
			return false;
		}
		if (EntryCount == 0 || BlockCount == 0 || CompressionBlockSize <= 0)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: '%s' implausible TOC (entries %u, blocks %u)"),
				*UtocPath, EntryCount, BlockCount);
			return false;
		}
		(void)Version;
		(void)PartitionCount;
		(void)ContainerFlags;

		// Layout (verified): header | chunk ids (12B each) | offset-lengths
		// (10B each, 40-bit MSB-first) | perfect-hash seeds (int32) | chunk
		// indices without perfect hash (int32) | compression blocks (12B) |
		// method names | [signatures] | directory index | meta (24B).
		int64 Offset = HeaderSize;
		if (Offset + (int64)EntryCount * 12 > UtocSize)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: '%s' truncated (chunk ids)"), *UtocPath);
			return false;
		}
		ChunkIds.SetNum(EntryCount);
		for (uint32 I = 0; I < EntryCount; ++I)
		{
			ChunkIds[I] = FPatcherHash::BytesToHex(P + Offset + I * 12, 12);
		}
		Offset += (int64)EntryCount * 12;

		if (Offset + (int64)EntryCount * 10 > UtocSize)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: '%s' truncated (offset-lengths)"), *UtocPath);
			return false;
		}
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

		if (Offset + (int64)BlockCount * 12 > UtocSize)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: '%s' truncated (blocks)"), *UtocPath);
			return false;
		}
		Blocks.SetNum(BlockCount);
		for (uint32 I = 0; I < BlockCount; ++I)
		{
			FIoStoreBlock B;
			if (!ReadBlock(P + Offset + I * 12, 12, B))
			{
				return false;
			}
			Blocks[I] = B;
		}
		Offset += (int64)BlockCount * 12;

		if (Offset + (int64)NameCount * NameLength > UtocSize)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: '%s' truncated (method names)"), *UtocPath);
			return false;
		}
		// FString(const ANSICHAR*) scans for the NUL terminator - with
		// NameLength = 0, or a name whose NUL lies past its slot at the end of
		// the buffer, that scan runs off the TOC. Bound each name to its slot.
		if (NameCount > 0 && NameLength == 0)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: '%s' has %u method names of length 0 (corrupt)"), *UtocPath, NameCount);
			return false;
		}
		CompressionMethods.Reset();
		FCompressionMethod None;
		None.Name = TEXT("None");
		CompressionMethods.Add(None);
		for (uint32 I = 0; I < NameCount; ++I)
		{
			const ANSICHAR* NameStart = (const ANSICHAR*)(P + Offset + I * NameLength);
			const int64 SlotRemaining = (int64)NameLength;
			FCompressionMethod M;
			if (SlotRemaining > 0)
			{
				// Copy within the slot; a missing NUL truncates instead of
				// reading past the buffer.
				TArray<ANSICHAR> NameBuf;
				NameBuf.SetNumUninitialized(SlotRemaining + 1);
				FMemory::Memcpy(NameBuf.GetData(), NameStart, SlotRemaining);
				NameBuf[SlotRemaining] = 0;
				M.Name = FString(ANSI_TO_TCHAR(NameBuf.GetData()));
			}
			CompressionMethods.Add(M);
		}
		Offset += (int64)NameCount * NameLength;

		// Skip signatures (signed containers), then directory index.
		Offset += DirectoryIndexSize;

		// Meta (chunk hashes): 20-byte FIoHash + 1-byte flags + 3 pad.
		if (Offset + (int64)EntryCount * 24 > UtocSize)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: '%s' truncated (meta)"), *UtocPath);
			return false;
		}
		for (uint32 I = 0; I < EntryCount; ++I)
		{
			const uint8* M = P + Offset + I * 24;
			Chunks[I].Hash = FPatcherHash::BytesToHex(M, 20);
			Chunks[I].bIsCompressed = (M[20] & 0x01) != 0;
		}

		// Chunk -> block ranges.
		for (FIoStoreChunk& C : Chunks)
		{
			const int64 RawEnd = C.RawOffset + C.RawSize;
			C.FirstBlockIndex = (int32)(C.RawOffset / CompressionBlockSize);
			C.NumBlocks = (int32)((AlignUp(RawEnd, CompressionBlockSize) - 1) / CompressionBlockSize) - C.FirstBlockIndex + 1;
			if (C.FirstBlockIndex < 0 || C.FirstBlockIndex + C.NumBlocks > (int32)BlockCount)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: chunk '%s' block range out of bounds"), *C.Id);
				return false;
			}
		}

		// TOC-only mode: no .ucas handle (chunk data reads fail cleanly via
		// ReadUcasRange's null-handle guard).

		UE_LOG(LogUnrealGamePatcher, Display,
			TEXT("FIoStoreReader: '%s' version %u: %d chunks, %d blocks, block size %d, methods %d (%s)"),
			*UtocPath, Version, Chunks.Num(), Blocks.Num(), CompressionBlockSize,
			CompressionMethods.Num() - 1,
			CompressionMethods.Num() > 1 ? *CompressionMethods[1].Name : TEXT("none"));
		return true;
	}

	void FIoStoreReader::Close()
	{
		if (UcasHandle)
		{
			delete UcasHandle;
			UcasHandle = nullptr;
		}
		ChunkIds.Reset();
		Chunks.Reset();
		Blocks.Reset();
		CompressionMethods.Reset();
		CompressionBlockSize = 0;
	}

	bool FIoStoreReader::ReadUcasRange(int64 Offset, int32 Size, TArray64<uint8>& OutData) const
	{
		if (!UcasHandle || Size < 0 || Offset < 0 || Offset + Size > UcasHandle->Size())
		{
			return false;
		}
		FIOStats::Get().AddDiskRead(Size);
		OutData.SetNumUninitialized(Size);
		return Size == 0 || (UcasHandle->Seek(Offset) && UcasHandle->Read(OutData.GetData(), Size));
	}

	bool FIoStoreReader::ReadChunk(int32 Index, TArray64<uint8>& OutData) const
	{
		if (Index < 0 || Index >= Chunks.Num())
		{
			return false;
		}
		const FIoStoreChunk& C = Chunks[Index];

		OutData.Reset();
		OutData.Reserve(C.RawSize);
		for (int32 I = 0; I < C.NumBlocks; ++I)
		{
			const FIoStoreBlock& B = Blocks[C.FirstBlockIndex + I];
			if (B.MethodIndex < 0 || B.MethodIndex >= CompressionMethods.Num())
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: chunk '%s' block %d has unknown method %d"), *C.Id, I, B.MethodIndex);
				return false;
			}

			TArray64<uint8> Compressed;
			if (!ReadUcasRange(B.FileOffset, B.CompressedSize, Compressed))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: cannot read block %d of chunk '%s'"), I, *C.Id);
				return false;
			}

			if (B.MethodIndex == 0)
			{
				// Raw block: the stored bytes ARE the uncompressed data.
				OutData.Append(Compressed);
			}
			else
			{
				TArray64<uint8> Uncompressed;
				Uncompressed.SetNumUninitialized(B.UncompressedSize);
				const FName MethodName = FName(*CompressionMethods[B.MethodIndex].Name);
				if (!FCompression::UncompressMemory(
					MethodName, Uncompressed.GetData(), B.UncompressedSize,
					Compressed.GetData(), B.CompressedSize))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: %s decompress failed for chunk '%s' block %d"),
						*CompressionMethods[B.MethodIndex].Name, *C.Id, I);
					return false;
				}
				FIOStats::Get().AddDecompress(B.CompressedSize, B.UncompressedSize);
				OutData.Append(Uncompressed);
			}
		}

		if (OutData.Num() != C.RawSize)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: chunk '%s' decoded %lld bytes, expected %lld"),
				*C.Id, OutData.Num(), C.RawSize);
			return false;
		}

		// Verify the whole-chunk hash (FIoHash of the raw data) against the
		// utoc's meta hash - a decoded-but-corrupt chunk is a read FAILURE,
		// never handed back as success (fulfills the header contract).
		{
			const FString HashHex = FPatcherHash::FIoHashToHex(FIoHash::HashBuffer(OutData.GetData(), OutData.Num()));
			FIOStats::Get().AddHashed(OutData.Num());
			if (!C.Hash.Equals(HashHex, ESearchCase::IgnoreCase))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: chunk '%s' meta hash mismatch (utoc %s, data %s)"),
					*C.Id, *C.Hash, *HashHex);
				return false;
			}
		}
		return true;
	}

	bool FIoStoreReader::ReadChunkById(const FString& IdHex, TArray64<uint8>& OutData) const
	{
		for (int32 I = 0; I < Chunks.Num(); ++I)
		{
			if (Chunks[I].Id.Equals(IdHex, ESearchCase::IgnoreCase))
			{
				return ReadChunk(I, OutData);
			}
		}
		return false;
	}

	bool FIoStoreReader::GetChunkStoredSpan(int32 Index, int64& OutOffset, int64& OutSize) const
	{
		if (Index < 0 || Index >= Chunks.Num() || Chunks[Index].NumBlocks <= 0 || Blocks.Num() == 0)
		{
			return false;
		}
		const FIoStoreChunk& C = Chunks[Index];
		if (!Blocks.IsValidIndex(C.FirstBlockIndex) ||
			!Blocks.IsValidIndex(C.FirstBlockIndex + C.NumBlocks - 1))
		{
			return false;
		}
		const FIoStoreBlock& First = Blocks[C.FirstBlockIndex];
		const FIoStoreBlock& Last = Blocks[C.FirstBlockIndex + C.NumBlocks - 1];
		OutOffset = First.FileOffset;
		OutSize = FMath::Max<int64>(0, Last.FileOffset + Last.CompressedSize - First.FileOffset);
		return OutSize > 0;
	}

	bool FIoStoreReader::DecodeChunkFromStored(int32 Index, const TArray64<uint8>& StoredBytes, TArray64<uint8>& OutData) const
	{
		if (Index < 0 || Index >= Chunks.Num())
		{
			return false;
		}
		const FIoStoreChunk& C = Chunks[Index];

		OutData.Reset();
		OutData.Reserve(C.RawSize);
		int64 Cursor = 0;
		for (int32 I = 0; I < C.NumBlocks; ++I)
		{
			const FIoStoreBlock& B = Blocks[C.FirstBlockIndex + I];
			if (B.MethodIndex < 0 || B.MethodIndex >= CompressionMethods.Num())
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: chunk '%s' block %d has unknown method %d"), *C.Id, I, B.MethodIndex);
				return false;
			}
			if (Cursor + B.CompressedSize > (int64)StoredBytes.Num())
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: chunk '%s' stored bytes too short for block %d"), *C.Id, I);
				return false;
			}

			if (B.MethodIndex == 0)
			{
				// Raw block: the stored bytes ARE the uncompressed data.
				OutData.Append(StoredBytes.GetData() + Cursor, B.CompressedSize);
			}
			else
			{
				TArray64<uint8> Uncompressed;
				Uncompressed.SetNumUninitialized(B.UncompressedSize);
				const FName MethodName = FName(*CompressionMethods[B.MethodIndex].Name);
				if (!FCompression::UncompressMemory(
					MethodName, Uncompressed.GetData(), B.UncompressedSize,
					StoredBytes.GetData() + Cursor, B.CompressedSize))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: %s decompress failed for chunk '%s' block %d"),
						*CompressionMethods[B.MethodIndex].Name, *C.Id, I);
					return false;
				}
				FIOStats::Get().AddDecompress(B.CompressedSize, B.UncompressedSize);
				OutData.Append(Uncompressed);
			}
			Cursor += B.CompressedSize;
		}

		if (OutData.Num() != C.RawSize)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: chunk '%s' decoded %lld bytes, expected %lld"),
				*C.Id, OutData.Num(), C.RawSize);
			return false;
		}

		// Same contract as ReadChunk: a decoded-but-corrupt chunk is a FAILURE.
		{
			const FString HashHex = FPatcherHash::FIoHashToHex(FIoHash::HashBuffer(OutData.GetData(), OutData.Num()));
			FIOStats::Get().AddHashed(OutData.Num());
			if (!C.Hash.Equals(HashHex, ESearchCase::IgnoreCase))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIoStoreReader: chunk '%s' meta hash mismatch (utoc %s, data %s)"),
					*C.Id, *C.Hash, *HashHex);
				return false;
			}
		}
		return true;
	}

	FString FIoStoreReader::GetChunkId(int32 Index) const
	{
		return Index >= 0 && Index < Chunks.Num() ? Chunks[Index].Id : FString();
	}
}
