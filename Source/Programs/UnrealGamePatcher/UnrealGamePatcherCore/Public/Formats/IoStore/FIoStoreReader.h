// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UnrealGamePatcher
{
	/** One compression block: the chunk's raw data region is split into
	 *  fixed-size blocks (CompressionBlockSize), each stored in the .ucas as
	 *  compressed bytes at FileOffset. */
	struct FIoStoreBlock
	{
		int64 FileOffset = 0;   // offset of the block's bytes in the .ucas
		int32 CompressedSize = 0;
		int32 UncompressedSize = 0;
		int32 MethodIndex = 0;  // 0 = raw (None), 1+ = CompressionMethodNames[MethodIndex]
	};

	/** One chunk (TOC entry): a contiguous raw range of the container. */
	struct FIoStoreChunk
	{
		FString Id;          // 24 lowercase hex (12-byte FIoChunkId)
		int64 RawOffset = 0; // raw (uncompressed) container offset
		int64 RawSize = 0;   // raw length
		FString Hash;        // 40 hex FIoHash of the raw chunk data (TOC meta)
		bool bIsCompressed = false;
		int32 FirstBlockIndex = 0;
		int32 NumBlocks = 0;
	};

	/**
	 * Self-written reader for engine IoStore containers (task 7.1): parses
	 * the .utoc TOC (header / chunk ids / offset-lengths / perfect-hash seeds
	 * / compression blocks / method names / directory index size / meta
	 * hashes) and reads chunk RAW data out of the .ucas, decompressing each
	 * block with the engine FCompression (raw / Oodle / ...). Format verified
	 * against real UE5 packaging output.
	 */
	struct FIoStoreReader
	{
		UNREALGAMEPATCHERCORE_API ~FIoStoreReader() { Close(); }

		UNREALGAMEPATCHERCORE_API bool Open(const FString& UtocPath, const FString& UcasPath);
		/** TOC-only open: parses the .utoc without a .ucas handle - used to
		 *  inspect a remote container's block layout from its downloaded utoc
		 *  alone (chunk DATA reads fail until a ucas is associated). */
		UNREALGAMEPATCHERCORE_API bool OpenTocOnly(const FString& UtocPath);
		UNREALGAMEPATCHERCORE_API void Close();

		UNREALGAMEPATCHERCORE_API int32 GetChunkCount() const { return Chunks.Num(); }
		UNREALGAMEPATCHERCORE_API int32 GetBlockCount() const { return Blocks.Num(); }
		UNREALGAMEPATCHERCORE_API const TArray<FIoStoreChunk>& GetChunks() const { return Chunks; }
		UNREALGAMEPATCHERCORE_API const TArray<FIoStoreBlock>& GetBlocks() const { return Blocks; }
		UNREALGAMEPATCHERCORE_API int32 GetCompressionBlockSize() const { return CompressionBlockSize; }

		/** Size of the trailing container-header stub some engine builds
		 *  append after the block data of a .ucas ({FIoContainerHeader::
		 *  Signature, version} = 8 bytes). The compressed blocks never cover
		 *  it, so "blocks tile the container" means up to UcasSize - this. */
		UNREALGAMEPATCHERCORE_API int32 GetUcasTrailerSize() const { return UcasTrailerSize; }

		/** Returns the RAW (uncompressed) bytes of the chunk at index,
		 *  decompressed and verified against the TOC meta hash. */
		UNREALGAMEPATCHERCORE_API bool ReadChunk(int32 Index, TArray64<uint8>& OutData) const;

		/** Returns the raw bytes for the chunk whose 24-hex id matches. */
		UNREALGAMEPATCHERCORE_API bool ReadChunkById(const FString& IdHex, TArray64<uint8>& OutData) const;

		/** Chunk id at index (24 hex), or empty. */
		UNREALGAMEPATCHERCORE_API FString GetChunkId(int32 Index) const;

		/** Physical byte span of the chunk's blocks inside the .ucas the TOC
		 *  describes. False for empty/TOC-only-unresolvable chunks. */
		UNREALGAMEPATCHERCORE_API bool GetChunkStoredSpan(int32 Index, int64& OutOffset, int64& OutSize) const;

		/** Decodes one chunk's RAW data from its STORED (block-layout) bytes -
		 *  the physical span GetChunkStoredSpan describes, fetched from
		 *  anywhere (the ucas patcher reads it from CDN windows without
		 *  assembling the remote container). Verifies the TOC meta hash. */
		UNREALGAMEPATCHERCORE_API bool DecodeChunkFromStored(int32 Index, const TArray64<uint8>& StoredBytes, TArray64<uint8>& OutData) const;

	private:
		struct FCompressionMethod
		{
			FString Name; // "None" for raw
		};

		bool ReadUcasRange(int64 Offset, int32 Size, TArray64<uint8>& OutData) const;

		FString UtocPath;
		FString UcasPath;
		IFileHandle* UcasHandle = nullptr;

		// Parsed TOC.
		int32 CompressionBlockSize = 0;
		TArray<FString> ChunkIds;
		TArray<FIoStoreChunk> Chunks;
		TArray<FIoStoreBlock> Blocks;
		TArray<FCompressionMethod> CompressionMethods;
		int32 UcasTrailerSize = 0;
	};
}
