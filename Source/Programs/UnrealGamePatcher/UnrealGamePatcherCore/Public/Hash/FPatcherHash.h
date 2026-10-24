// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Hash/xxhash.h"
#include "IO/IoHash.h"
#include "IO/IoChunkId.h"

namespace UnrealGamePatcher
{
	/**
	 * Deterministic non-crypto hashes used for integrity checks and CDN chunk
	 * naming (design D3 / D8).
	 *
	 * - XXH3-64 (engine FXxHash64): extension-manifest self-check, CDN chunk
	 *   download validation, and CDN chunk file naming.
	 * - FIoHash (BLAKE3-160): utoc chunk content hash (chunk id semantic).
	 * - FIoChunkId: utoc chunk identifier, used as the CDN chunk file name.
	 *
	 * Byte order: hex forms are emitted big-endian (network order) so they match
	 * xxhsum -H2 and UE's LexToString output used by external tooling.
	 */
	struct FPatcherHash
	{
		// --- XXH3-64 (FXxHash64) ----------------------------------------------
		UNREALGAMEPATCHERCORE_API static uint64 XXH64(const void* Data, uint64 Size);

		// 16 lowercase hex characters, big-endian (matches xxhsum -H2).
		UNREALGAMEPATCHERCORE_API static FString XXH64ToHex(const void* Data, uint64 Size);
		UNREALGAMEPATCHERCORE_API static FString XXH64ToHex(const FXxHash64& Hash);

		// Parses a 16-character hex string (case-insensitive) into an FXxHash64.
		// Non-16-length or non-hex input returns a default (zero) hash.
		UNREALGAMEPATCHERCORE_API static FXxHash64 XXH64FromHex(const FString& Hex);

		// --- FIoHash (BLAKE3-160) ---------------------------------------------
		// 40 lowercase hex characters.
		UNREALGAMEPATCHERCORE_API static FString FIoHashToHex(const FIoHash& Hash);
		// Parses a 40-character hex string into an FIoHash. Non-40-length or
		// non-hex input returns an invalid (zero) hash.
		UNREALGAMEPATCHERCORE_API static FIoHash FIoHashFromHex(const FString& Hex);

		// --- FIoChunkId (utoc chunk identifier) -------------------------------
		// 24 hex characters (12 bytes). Uses the engine LexToString / FromHex.
		UNREALGAMEPATCHERCORE_API static FString FIoChunkIdToHex(const FIoChunkId& ChunkId);
		UNREALGAMEPATCHERCORE_API static FIoChunkId FIoChunkIdFromHex(const FString& Hex);

		// --- Generic byte <-> hex ----------------------------------------------
		// Lowercase hex string of raw bytes (SHA1 digests, chunk data, ...).
		UNREALGAMEPATCHERCORE_API static FString BytesToHex(const uint8* Data, int32 Len);
		// Parses a hex string (case-insensitive) into bytes (appends to OutBytes);
		// returns false for odd-length or non-hex input (empty output). The
		// engine parser is only fed exact even-length hex after this gate.
		UNREALGAMEPATCHERCORE_API static bool HexToBytes(const FString& Hex, TArray<uint8>& OutBytes);
		// Streamed SHA1 of a file (1 MiB windows); 40-hex or empty on error.
		UNREALGAMEPATCHERCORE_API static FString FileSHA1Hex(const FString& Path, int64* OutSize = nullptr);
	};
}
