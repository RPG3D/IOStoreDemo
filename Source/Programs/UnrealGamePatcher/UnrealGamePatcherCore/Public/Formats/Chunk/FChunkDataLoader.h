// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Formats/Index/FIndexManifest.h"

namespace UnrealGamePatcher
{
	/**
	 * Self-designed chunk format (we no longer consume BuildPatchServices
	 * output - the chunker runs on the staged build directly). Files are split
	 * into CONTENT-DEFINED chunks (FastCDC: Gear rolling hash + normalized cut
	 * points, see FBuildIngestor) of 256 KiB..4 MiB (average ~1 MiB); each
	 * chunk is one chunk file:
	 *
	 *   [ 8B  magic "UGPCHNK1" ]
	 *   [ 8B  UncompressedSize  u64 LE ]
	 *   [ 8B  CompressedSize    u64 LE ]  (size of the data section)
	 *   [ 1B  StoredAs: 0 = raw, 1 = Oodle ]
	 *   [ 20B SHA1 of the UNCOMPRESSED data ]
	 *   [ data (raw or Oodle-compressed) ]
	 *
	 * Header = 45 bytes, no version branching. The chunk key / content address
	 * is the SHA1 of the uncompressed window (chunks/<sha1hex>.chunk on the
	 * CDN). The manifest's chunk SHA1 covers the UNCOMPRESSED data; the loader
	 * always returns uncompressed bytes and verifies that hash, so callers can
	 * treat a chunk as a trusted byte source keyed by its SHA1.
	 */
	struct FChunkDataLoader
	{
		static constexpr int64 WindowSize = 1048576;      // legacy: fixed window size before content-defined chunking (kept for migration math; chunk sizes are now 256 KiB..4 MiB)
		static constexpr const char* Magic = "UGPCHNK1";  // 8 bytes
		static constexpr int32 HeaderSize = 8 + 8 + 8 + 1 + 20; // 45
		static constexpr uint8 StoredAsRaw = 0;
		static constexpr uint8 StoredAsOodle = 1;
		// Keep Oodle only when it saves at least this much of the window
		// (compressed < (100-MinOodleSavePercent)% of uncompressed). The
		// 90%-100% band is incompressible content (already-compressed
		// textures/audio/ucas chunks) where compression is pure overhead, so
		// those windows are stored raw - bandwidth cost is negligible while
		// the client skips pointless decompression.
		static constexpr int32 MinOodleSavePercent = 10; // ratio < 0.90

		/** CDN-relative chunk path with 2/2/36 hex sharding (content-address
		 *  like UE's chunk stores), so a large content set never ends up as a
		 *  single flat directory: chunks/<c0><c1>/<c2><c3>/<rest>.chunk
		 *  e.g. chunks/5a/9f/1eb0c1fc6268b90ee71f32ce5f7ed9c2c986.chunk.
		 *  Empty for a non-40-hex input. */
		static FString MakeChunkRelPath(const FString& Sha1Hex)
		{
			if (Sha1Hex.Len() != 40)
			{
				return FString();
			}
			return FString::Printf(TEXT("chunks/%s/%s/%s.chunk"),
				*Sha1Hex.Mid(0, 2), *Sha1Hex.Mid(2, 2), *Sha1Hex.RightChop(4));
		}

		/** Reads ChunkFilePath, parses the header, decompresses when stored
		 *  compressed (Oodle), and verifies the SHA1 against Entry.SHA1.
		 *  OutData is the uncompressed chunk payload. Returns false with a
		 *  logged error on any mismatch - a bad chunk is never handed to the
		 *  caller. */
		UNREALGAMEPATCHERCORE_API static bool Load(const FString& ChunkFilePath, const FIndexChunkEntry& Entry, TArray64<uint8>& OutData);

		/** Writes a chunk file from one uncompressed window. Tries Oodle; when
		 *  the compressed size is not < (100-MinOodleSavePercent)% of the
		 *  window the chunk is stored raw (StoredAs flag covers both, the
		 *  loader reads either). OutSha1Hex is the content address (chunk key). */
		UNREALGAMEPATCHERCORE_API static bool Write(const FString& ChunkFilePath, const TArray64<uint8>& Uncompressed, FString& OutSha1Hex, uint8& OutStoredAs);

		/** Zero-copy variant for bytes already resident in a caller buffer
		 *  (the CDC chunker scans a read-ahead buffer and hands the window
		 *  straight over). Same format/hash rules as the array overload. */
		UNREALGAMEPATCHERCORE_API static bool Write(const FString& ChunkFilePath, const uint8* Uncompressed, int64 UncompressedSize, FString& OutSha1Hex, uint8& OutStoredAs);
	};
}
