// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Formats/Index/FIndexManifest.h"

namespace UnrealGamePatcher
{
	/**
	 * Rebuilds a file's bytes from its BPT chunk parts. Shared by the build
	 * ingestor (metadata extraction) and the in-place patchers (temp-file
	 * reassembly) so whole-file assembly lives in exactly one place.
	 */
	struct FChunkAssembler
	{
		/** Verified uncompressed chunk bytes, keyed by guid. Sharing one cache
		 *  across several assemblies skips the re-read / re-decompress /
		 *  re-verify (SHA1) of a chunk referenced by more than one file -
		 *  content addressing can share windows between files, and each
		 *  duplicate reference would otherwise repeat the whole load. */
		struct FChunkCache
		{
			TMap<FString, TArray64<uint8>> Verified;
		};

		/** Assembles FileId's full bytes from its parts under ChunkRoot.
		 *  Pass a shared Cache to avoid re-loading/verifying chunks that
		 *  appear in more than one part list.
		 *  Returns false when the entry/parts are missing or a part is malformed. */
		UNREALGAMEPATCHERCORE_API static bool AssembleToBytes(
			const FIndexManifest& Remote,
			const FString& ChunkRoot,
			const FString& FileId,
			TArray64<uint8>& OutBytes,
			FChunkCache* Cache = nullptr);

		/** Range variant: assembles only FileId's bytes [Offset, Offset+Size)
		 *  from its parts (the patchers read exactly the bytes they need -
		 *  e.g. a changed chunk's stored span - without assembling a
		 *  multi-GB container). Returns false when the range is negative or
		 *  the parts cannot serve it completely. */
		UNREALGAMEPATCHERCORE_API static bool AssembleRangeToBytes(
			const FIndexManifest& Remote,
			const FString& ChunkRoot,
			const FString& FileId,
			int64 Offset,
			int64 Size,
			TArray64<uint8>& OutBytes,
			FChunkCache* Cache = nullptr);

		/** Assembles into OutPath (e.g. a temp file for the in-place patchers). */
		UNREALGAMEPATCHERCORE_API static bool AssembleToFile(
			const FIndexManifest& Remote,
			const FString& ChunkRoot,
			const FString& FileId,
			const FString& OutPath,
			FChunkCache* Cache = nullptr);
	};
}
