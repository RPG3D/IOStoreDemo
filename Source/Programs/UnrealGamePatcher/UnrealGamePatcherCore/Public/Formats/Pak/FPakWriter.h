// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Formats/Pak/FPakReader.h"
#include "State/FPatcherState.h"

namespace UnrealGamePatcher
{
	/**
	 * Incremental pak writer: modifies an existing pak in place with zero data
	 * relocation (design D4):
	 *
	 *   [ data region ][ old directory ][ old footer ][ new data (32B aligned) ][ new directory ][ new footer ]
	 *
	 * - Overwrite(): the new stored payload has the same size and compression
	 *   layout as the existing entry -> pwrite over the old data, nothing moves.
	 * - AppendEntry(): new/changed entries are appended after the old footer,
	 *   aligned to 32 bytes; the old directory/footer region becomes a hole.
	 * - Commit(): rebuilds an unencoded (V10+) primary index from all entries
	 *   (old + new) and appends it with a fresh footer at the end of the file.
	 *
	 * The writer is chunk-oriented: the caller hands in the final stored bytes
	 * per compression block (this is exactly what a CDN chunk delivers for the
	 * patcher). The writer never compresses; it lays out blocks, computes the
	 * entry header (including the per-file SHA1 the engine verifies against),
	 * and writes the directory/footer.
	 */
	struct FPakWriter
	{
		/** One stored payload block: the exact bytes that go into the pak. */
		struct FStoredBlock
		{
			TArray64<uint8> Bytes;
		};

		/** Describes an entry to append. CompressionMethodIndex 0 = stored raw;
		 *  otherwise Chunks holds one compressed block per engine block (the
		 *  writer lays them out with 32-byte alignment between blocks). */
		struct FEntryToWrite
		{
			FString Path;                       // relative to the mount point
			uint32 CompressionMethodIndex = 0;  // index into CompressionMethods[]
			int64 UncompressedSize = 0;
			uint32 CompressionBlockSize = 0;    // uncompressed bytes per block (compressed entries)
			TArray<FStoredBlock> Blocks;        // uncompressed: 1 block = raw data
		};

		UNREALGAMEPATCHERCORE_API FPakWriter() = default;
		UNREALGAMEPATCHERCORE_API ~FPakWriter();

		FPakWriter(const FPakWriter&) = delete;
		FPakWriter& operator=(const FPakWriter&) = delete;

		/** Opens an existing pak for incremental modification and parses its
		 *  footer and directory (reusing FPakReader). Returns false on failure. */
		UNREALGAMEPATCHERCORE_API bool Open(const FString& PakFilePath);

		/** Overwrites an existing entry in place. Requires:
		 *    - the entry exists in the parsed directory
		 *    - NewEntry has the same stored Size and the same compression
		 *      layout (same method, same block count) as the existing entry
		 *      so the payload header keeps its size
		 *  Data must point at Size bytes of the new stored payload. */
		UNREALGAMEPATCHERCORE_API bool Overwrite(const FString& PathInPak, const FPakEntry& NewEntry, const uint8* Data, int64 Size);

		/** Appends a new entry (or a size-changed replacement) at the end of
		 *  the file. Entry.Offset is assigned by the writer (32-byte aligned,
		 *  after the current end of file). Returns false on I/O failure. */
		/** Returns the pak-serialization index of a compression method,
		 *  registering it into the footer's method table on first use (the
		 *  table is empty for paks that never compressed anything). Index 0
		 *  is the in-memory NAME_None slot; returns 0 when the table is
		 *  full (5 file slots max, engine format limit). */
		UNREALGAMEPATCHERCORE_API int32 RegisterCompressionMethod(FName Method);

		UNREALGAMEPATCHERCORE_API bool AppendEntry(const FEntryToWrite& InEntry, FPakEntry& OutEntry);

		/** Rebuilds the directory index and footer, appending them at the end
		 *  of the file. After Commit() the pak is fully valid; a subsequent
		 *  AppendEntry() may still be called and Commit() again. */
		UNREALGAMEPATCHERCORE_API bool Commit();

		/** Number of bytes appended so far (new data + directory + footer),
		 *  i.e. the new file size minus the size at Open(). Backfilled
		 *  entries do not grow the file and are not counted here. */
		int64 GetBytesAdded() const { return BytesAdded; }

		/** Free ranges (dead byte regions of THIS pak, recorded by earlier
		 *  patches) that AppendEntry() may backfill into instead of
		 *  appending at EOF. Entries land 32-byte aligned inside a range;
		 *  a filled range is replaced by its tail remainder. Backfill only
		 *  ever uses ranges handed in here - the ranges this patch itself
		 *  invalidates (old index/footer, superseded entry data) become
		 *  available to the NEXT patch. GetPlacementHoles() reflects the
		 *  post-placement state. */
		UNREALGAMEPATCHERCORE_API void SetPlacementHoles(const TArray<FHole>& InHoles) { PlacementHoles = InHoles; }
		UNREALGAMEPATCHERCORE_API const TArray<FHole>& GetPlacementHoles() const { return PlacementHoles; }
		UNREALGAMEPATCHERCORE_API int32 GetNumHolesFilled() const { return NumHolesFilled; }

		bool IsOpen() const { return WriteHandle != nullptr; }
		const FString& GetPakFilePath() const { return PakFilePath; }
		const FPakInfo& GetInfo() const { return Info; }

		/** The directory as parsed at Open(), updated as entries are appended.
		 *  Callers use this to look up existing entries (e.g. to inherit the
		 *  compression layout for an in-place overwrite). */
		const TArray<FPakReader::FPakEntryLocation>& GetFiles() const { return Files; }

		/** Finds an existing directory entry by path (case-insensitive). */
		const FPakReader::FPakEntryLocation* FindEntry(const FString& PathInPak) const
		{
			for (const FPakReader::FPakEntryLocation& Loc : Files)
			{
				if (Loc.Path.Equals(PathInPak, ESearchCase::IgnoreCase))
				{
					return &Loc;
				}
			}
			return nullptr;
		}

	private:
		FString PakFilePath;
		FPakInfo Info;

		// Parsed directory as read at Open(); grows as entries are appended.
		TArray<FPakReader::FPakEntryLocation> Files;
		FString MountPoint;

		// Opaque read/write handle to the open file (IFileHandle*).
		void* WriteHandle = nullptr;
		int64 FileSize = 0;    // end of file at last append
		int64 BytesAdded = 0;  // bytes written past the initial end of file

		// The first version that supports the V10+ primary index layout that
		// Commit() writes. We can read older paks but only rewrite modern ones.
		static constexpr uint32 MinWriteVersion = 10;

		// Backfill: dead ranges offered for entry placement + fill counter.
		TArray<FHole> PlacementHoles;
		int32 NumHolesFilled = 0;
	};
}
