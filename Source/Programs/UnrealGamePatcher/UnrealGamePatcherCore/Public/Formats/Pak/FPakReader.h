// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UnrealGamePatcher
{
	// Pak magic value: "0x5A6F12E1" in the footer (FPakInfo::PakFile_Magic).
	static constexpr uint32 PakFileMagic = 0x5A6F12E1;

	// Largest footer version we can parse (UE 5.8).
	static constexpr uint32 PakFileVersionMax = 12;

	/** Serialized size of the fixed part of the footer for a given pak version
	 *  (mirrors engine FPakInfo::GetSerializedSize). Shared by reader, writer
	 *  and the recovery scanner — all three must agree on footer geometry. */
	inline int64 GetPakFooterFixedSize(uint32 Version)
	{
		int64 Size = 0;
		if (Version >= 7) Size += 16; // EncryptionKeyGuid
		Size += 1;                    // bEncryptedIndex
		Size += 4;                    // Magic
		Size += 4;                    // Version
		Size += 8;                    // IndexOffset
		Size += 8;                    // IndexSize
		Size += 20;                   // IndexHash
		if (Version == 9) Size += 1;  // bIndexIsFrozen
		if (Version >= 8) Size += 160; // CompressionMethods (5 x FName)
		return Size;
	}

	/** One compression block of a compressed pak entry. Offsets are raw bytes
	 *  within the pak file. With RelativeChunkOffsets (version >= 5) the stored
	 *  start is relative to the entry start and must be offset by FPakEntry::Offset
	 *  when reading; with older versions it is already absolute. */
	struct FPakCompressedBlock
	{
		int64 CompressedStart = 0;
		int64 CompressedEnd = 0;

		int64 Size() const { return CompressedEnd - CompressedStart; }
	};

	/** Per-entry metadata as stored in the pak index. Follows the engine
	 *  FPakEntry serialization so any pak the engine produces parses here and
	 *  vice versa. Hash is the SHA1 of the UNCOMPRESSED entry data (engine
	 *  writes it only for entries stored uncompressed in the index; entries
	 *  written via the frozen index may have it zeroed). */
	struct FPakEntry
	{
		int64 Offset = -1;
		int64 Size = 0;
		int64 UncompressedSize = 0;
		uint32 CompressionMethodIndex = 0; // direct index into CompressionMethods[] (engine semantics)
		uint8 Hash[20] = { 0 };            // SHA1 of uncompressed data
		TArray<FPakCompressedBlock> CompressionBlocks;
		uint8 Flags = 0;
		uint32 CompressionBlockSize = 0;

		bool IsCompressed() const { return CompressionBlocks.Num() > 0; }
		bool IsEncrypted() const { return (Flags & 0x01) != 0; }
		void SetFlag_Encrypted(bool b) { Flags = b ? (Flags | 0x01) : (Flags & ~0x01); }
		bool HasValidHash() const
		{
			for (int32 I = 0; I < 20; ++I)
			{
				if (Hash[I] != 0) return true;
			}
			return false;
		}
	};

	/** Pak footer info (FPakInfo equivalent). */
	struct FPakInfo
	{
		uint32 Magic = PakFileMagic;
		uint32 Version = PakFileVersionMax;
		int64 IndexOffset = -1;
		int64 IndexSize = 0;
		uint8 IndexHash[20] = { 0 };
		bool bEncryptedIndex = false;
		bool bIndexIsFrozen = false;
		TArray<FName> CompressionMethods; // index 0 always empty

		FName GetCompressionMethod(uint32 MethodIndex) const
		{
			return CompressionMethods.IsValidIndex(MethodIndex) ? CompressionMethods[MethodIndex] : NAME_None;
		}
	};

	/**
	 * Reads the pak container format: footer, directory index and file entries,
	 * with support for multiple pak versions (V1..V12). Data access is read-only;
	 * writing is handled by FPakWriter.
	 *
	 * Usage:
	 *   FPakReader Reader;
	 *   if (Reader.Open(TEXT("D:/game/Content/Paks/game.pak")) && Reader.ReadIndex())
	 *   {
	 *       for (const FPakEntryLocation& L : Reader.GetFiles()) { ... }
	 *       TArray64<uint8> Data;
	 *       Reader.ReadEntryData(Entry, Data);
	 *   }
	 */
	struct FPakReader
	{
		/** A file found in the pak directory. */
		struct FPakEntryLocation
		{
			FString Path; // relative to the mount point, e.g. "../../../Game/Content/X.uasset"
			FPakEntry Entry;
		};

		UNREALGAMEPATCHERCORE_API FPakReader() = default;
		UNREALGAMEPATCHERCORE_API ~FPakReader();

		FPakReader(const FPakReader&) = delete;
		FPakReader& operator=(const FPakReader&) = delete;

		/** Opens the pak file and parses its footer. Detects the version by
		 *  trying every known footer layout (newest first) and validating the
		 *  magic. Returns false with a logged error on failure. */
		UNREALGAMEPATCHERCORE_API bool Open(const FString& PakFilePath);

		/** Parses the directory index (legacy or V10+ PathHashIndex). */
		UNREALGAMEPATCHERCORE_API bool ReadIndex();

		/** Reads and decompresses the full payload of an entry. */
		UNREALGAMEPATCHERCORE_API bool ReadEntryData(const FPakEntry& Entry, TArray64<uint8>& OutData) const;

		/** Reads the full payload of an entry referenced by path. */
		UNREALGAMEPATCHERCORE_API bool ReadFile(const FString& PathInPak, TArray64<uint8>& OutData) const;

		/** Reads the built-in SHA1 hash stored in the payload entry header that
		 *  precedes the file's data. This is the per-file hash the pak contains
		 *  for integrity checking (the index itself stores no hash for encoded
		 *  entries). Returns false when the header cannot be read. */
		UNREALGAMEPATCHERCORE_API bool ReadPayloadHash(const FPakEntry& Entry, uint8 OutHash[20]) const;

		/** Serialized size of an entry's payload header (the bytes between the
		 *  directory offset and the stored payload) for a given pak version.
		 *  Used to locate the payload start (in-place patching). */
		UNREALGAMEPATCHERCORE_API static int64 GetSerializedEntrySize(const FPakEntry& Entry, uint32 Version);

		/** Computes the SHA1 of the bytes actually stored in the pak payload,
		 *  the same way the engine hashes entries:
		 *    - uncompressed entries: the original data
		 *    - compressed entries: the compressed block payloads with the
		 *      32-byte inter-block alignment padding stripped
		 *  This is what the payload header's built-in hash covers (the engine
		 *  hashes the uncompressed buffer for plain entries and the compressed
		 *  buffer for compressed ones, see PakFileUtilities.cpp).
		 *  Returns false when the payload cannot be read. */
		UNREALGAMEPATCHERCORE_API bool ComputePayloadHash(const FPakEntry& Entry, uint8 OutHash[20]) const;

		/** True after Open() succeeded. */
		bool IsOpen() const { return ReaderHandle != nullptr; }
		const FString& GetPakFilePath() const { return PakFilePath; }
		const FPakInfo& GetInfo() const { return Info; }
		const FString& GetMountPoint() const { return MountPoint; }
		const TArray<FPakEntryLocation>& GetFiles() const { return Files; }

	private:
		FString PakFilePath;
		FPakInfo Info;
		FString MountPoint;
		TArray<FPakEntryLocation> Files;

		// Opaque handle to the open file (IFileHandle*). Kept as void* to avoid
		// pulling in IFileHandle in the header; all access goes through Core
		// file APIs inside the .cpp.
		void* ReaderHandle = nullptr;

		// Parses one full (non-encoded) FPakEntry from the index stream.
		bool ReadEntryFromStream(const uint8*& P, const uint8* End, FPakEntry& OutEntry) const;
		// Decodes one encoded entry from the encoded blob (engine bit field).
		bool DecodeEntry(const uint8* P, const uint8* End, FPakEntry& OutEntry) const;
	};
}
