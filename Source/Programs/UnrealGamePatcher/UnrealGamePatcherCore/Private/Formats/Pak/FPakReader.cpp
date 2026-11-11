// Copyright Epic Games, Inc. All Rights Reserved.

#include "Formats/Pak/FPakReader.h"
#include "Stats/FIOStats.h"
#include "UnrealGamePatcherLog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Compression.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"

namespace UnrealGamePatcher
{
	namespace
	{
		// --- Low level binary readers ----------------------------------------
		bool ReadBytes(const uint8*& P, const uint8* End, void* Out, int64 Size)
		{
			if (P + Size > End) return false;
			FMemory::Memcpy(Out, P, Size);
			P += Size;
			return true;
		}

		template <typename T>
		bool ReadLE(const uint8*& P, const uint8* End, T& Out)
		{
			if (P + (int64)sizeof(T) > End) return false;
			FMemory::Memcpy(&Out, P, sizeof(T));
			P += sizeof(T);
			return true;
		}

		bool ReadFString(const uint8*& P, const uint8* End, FString& Out)
		{
			int32 Len = 0;
			if (!ReadLE(P, End, Len)) return false;
			if (Len == 0)
			{
				Out.Empty();
				return true;
			}
			if (Len > 0)
			{
				// ANSI/UTF-8 characters including the null terminator.
				if (P + (int64)Len > End) return false;
				Out = FString(FStringView(ANSI_TO_TCHAR((const ANSICHAR*)P), Len - 1));
				P += Len;
				return true;
			}
			// Negative: UTF-16LE code units including the null terminator.
			const int32 Chars = -Len;
			if (P + (int64)Chars * 2 > End) return false;
			Out = FString(FStringView((const WIDECHAR*)P, Chars - 1));
			P += Chars * 2;
			return true;
		}

		// FUtf8String archive serialization: int32 length NOT including a null
		// terminator, followed by that many UTF-8 bytes. Used for the file names
		// inside the FullDirectoryIndex (V10+); the directory names are plain
		// FStrings (length including the terminator).
		bool ReadFUtf8String(const uint8*& P, const uint8* End, FString& Out)
		{
			int32 Len = 0;
			if (!ReadLE(P, End, Len)) return false;
			if (Len < 0) return false; // FUtf8String is always positive-length
			if (Len == 0)
			{
				Out.Empty();
				return true;
			}
			if (P + (int64)Len > End) return false;
			// No null terminator is stored; decode from a temporary buffer.
			TArray<ANSICHAR> Tmp;
			Tmp.SetNum(Len + 1);
			FMemory::Memcpy(Tmp.GetData(), P, Len);
			Tmp[Len] = 0;
			Out = UTF8_TO_TCHAR(Tmp.GetData());
			P += Len;
			return true;
		}

		// Footer geometry lives in FPakReader.h (GetPakFooterFixedSize) so the
		// writer and recovery scanner share one definition.

		int64 GetEntrySerializedSizeInternal(const FPakEntry& Entry, uint32 Version)
		{
			// Mirrors engine FPakEntry::GetSerializedSize: compression blocks
			// are only serialized when the entry is compressed.
			int64 Size = 8 + 8 + 8; // Offset, Size, UncompressedSize
			if (Version >= 8)
			{
				Size += 4; // CompressionMethodIndex
			}
			else
			{
				Size += 4; // LegacyCompressionMethod (int32)
			}
			if (Version <= 1) Size += 8; // Timestamp
			Size += 20;                  // Hash
			if (Version >= 3)
			{
				if (Entry.CompressionMethodIndex != 0)
				{
					Size += 4; // num blocks
					Size += (int64)Entry.CompressionBlocks.Num() * 16;
				}
				Size += 1;  // Flags
				Size += 4;  // CompressionBlockSize
			}
			return Size;
		}
	}

	FPakReader::~FPakReader()
	{
		if (ReaderHandle)
		{
			IFileHandle* Handle = static_cast<IFileHandle*>(ReaderHandle);
			delete Handle;
			ReaderHandle = nullptr;
		}
	}

	bool FPakReader::Open(const FString& InPakFilePath)
	{
		PakFilePath = InPakFilePath;
		Files.Empty();
		MountPoint.Empty();

		IFileHandle* Handle = FPlatformFileManager::Get().GetPlatformFile().OpenRead(*PakFilePath);
		if (!Handle)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: cannot open '%s'"), *PakFilePath);
			return false;
		}
		ReaderHandle = Handle;

		const int64 FileSize = Handle->Size();
		if (FileSize < GetPakFooterFixedSize(1))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: '%s' too small (%lld bytes)"), *PakFilePath, FileSize);
			return false;
		}

		// Read the largest possible footer window and locate the version by
		// trying each known footer layout, newest first. The magic lives at a
		// fixed offset from the end for every version.
		const int64 MaxFooter = GetPakFooterFixedSize(PakFileVersionMax);
		const int64 FooterStart = FMath::Max((int64)0, FileSize - MaxFooter);
		const int64 FooterBytes = FileSize - FooterStart;
	TArray64<uint8> FooterBuf;
	FooterBuf.SetNumUninitialized(FooterBytes);
	if (!Handle->Seek(FooterStart) || !Handle->Read(FooterBuf.GetData(), FooterBytes))
	{
		UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: failed reading footer of '%s'"), *PakFilePath);
		return false;
	}
	FIOStats::Get().AddDiskRead(FooterBytes);
		const uint8* Base = FooterBuf.GetData();
		const int64 DataEnd = FooterBuf.Num();

		// The version field is always at offset (16+1+4) from the end of the
		// serialized footer, and the magic at (16+1). Try version 12 down to 1.
		bool bFound = false;
		for (int32 V = PakFileVersionMax; V >= 1 && !bFound; --V)
		{
			const int64 Fixed = GetPakFooterFixedSize(V);
			if (Fixed > DataEnd) continue;
			const uint8* P = Base + DataEnd - Fixed;

			uint32 Magic = 0;
			uint32 Ver = 0;
			P += (V >= 7) ? 16 : 0; // EncryptionKeyGuid
			P += 1;                 // bEncryptedIndex
			if (!ReadLE(P, Base + DataEnd, Magic) || !ReadLE(P, Base + DataEnd, Ver))
			{
				continue;
			}
			if (Magic != PakFileMagic || Ver != (uint32)V)
			{
				continue;
			}

			// Parse the rest of this version's footer.
			Info.Magic = Magic;
			Info.Version = Ver;
			P = Base + DataEnd - Fixed;
			if (V >= 7)
			{
				P += 16; // EncryptionKeyGuid (not needed; no encryption in V1)
			}
			uint8 Enc = 0;
			if (!ReadLE(P, Base + DataEnd, Enc)) break;
			Info.bEncryptedIndex = Enc != 0;
			if (!ReadLE(P, Base + DataEnd, Info.Magic)) break;
			if (!ReadLE(P, Base + DataEnd, Info.Version)) break;
			if (!ReadLE(P, Base + DataEnd, Info.IndexOffset)) break;
			if (!ReadLE(P, Base + DataEnd, Info.IndexSize)) break;
			ReadBytes(P, Base + DataEnd, Info.IndexHash, 20);
			if (V == 9)
			{
				uint8 Frozen = 0;
				if (!ReadLE(P, Base + DataEnd, Frozen)) break;
				Info.bIndexIsFrozen = Frozen != 0;
			}
			if (V >= 8)
			{
				// 5 x FName slots, each 32 bytes of ANSI chars. The engine
				// always keeps NAME_None at in-memory index 0 and serializes
				// CompressionMethods[1..] into file slots [0..]. On load the
				// slots are appended after the default NAME_None entry, so we
				// mirror that: None at index 0, then each non-empty slot.
				Info.CompressionMethods.Reset();
				Info.CompressionMethods.Add(NAME_None);
				for (int32 I = 0; I < 5; ++I)
				{
					char Name[32] = { 0 };
					if (!ReadBytes(P, Base + DataEnd, Name, 32)) { break; }
					if (Name[0] != 0)
					{
						Name[31] = 0; // guard against garbage in the buffer
						Info.CompressionMethods.Add(FName(ANSI_TO_TCHAR(Name)));
					}
				}
			}
			bFound = true;
			break;
		}

		if (!bFound)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: '%s' is not a valid pak (bad magic/footer)"), *PakFilePath);
			return false;
		}

		UE_LOG(LogUnrealGamePatcher, Display, TEXT("FPakReader: opened '%s' (version %d, index at %lld size %lld, frozen=%d)"),
			*PakFilePath, Info.Version, Info.IndexOffset, Info.IndexSize, Info.bIndexIsFrozen ? 1 : 0);
		return true;
	}

	bool FPakReader::ReadIndex()
	{
		if (!ReaderHandle)
		{
			return false;
		}
		Files.Empty();
		MountPoint.Empty();

		if (Info.IndexSize <= 0 || Info.IndexOffset < 0)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: '%s' has no index"), *PakFilePath);
			return false;
		}

		IFileHandle* Handle = static_cast<IFileHandle*>(ReaderHandle);
		// The footer-declared index geometry is untrusted input: a corrupt
		// footer claiming a terabyte index must fail here, not allocate.
		if (Info.IndexOffset + Info.IndexSize > Handle->Size())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: index [%lld, %lld) exceeds file size %lld in '%s' (corrupt footer?)"),
				Info.IndexOffset, Info.IndexOffset + Info.IndexSize, Handle->Size(), *PakFilePath);
			return false;
		}
	TArray64<uint8> IndexData;
	IndexData.SetNumUninitialized(Info.IndexSize);
	if (!Handle->Seek(Info.IndexOffset) || !Handle->Read(IndexData.GetData(), Info.IndexSize))
	{
		UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: failed reading index of '%s'"), *PakFilePath);
		return false;
	}
	FIOStats::Get().AddDiskRead(Info.IndexSize);

		// Verify the index hash (SHA1 of the index data, engine computes it on
		// write). A mismatch means the index - which drives every entry offset
		// we would ever patch - is corrupt: fail closed (the engine treats this
		// as fatal too). Tolerate a mismatch only for frozen indexes which may
		// store the pre-freeze hash.
		if (!Info.bIndexIsFrozen)
		{
			FSHA1 Sha;
			Sha.Update(IndexData.GetData(), Info.IndexSize);
			Sha.Final();
			uint8 Computed[20];
			Sha.GetHash(Computed);
			if (FMemory::Memcmp(Computed, Info.IndexHash, 20) != 0)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: index hash mismatch in '%s' (corrupt index)"), *PakFilePath);
				return false;
			}
		}

		const uint8* P = IndexData.GetData();
		const uint8* End = P + IndexData.Num();

		// Mount point.
		if (!ReadFString(P, End, MountPoint))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: index of '%s' is truncated (mount point)"), *PakFilePath);
			return false;
		}

		if (Info.Version < 10)
		{
			// Legacy index: u32 count + (FString path + full FPakEntry)*count.
			uint32 Count = 0;
			if (!ReadLE(P, End, Count))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: legacy index of '%s' is truncated"), *PakFilePath);
				return false;
			}
			Files.Reserve(Count);
			for (uint32 I = 0; I < Count; ++I)
			{
				FPakEntryLocation Loc;
				if (!ReadFString(P, End, Loc.Path))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: legacy index of '%s' truncated at entry %u"), *PakFilePath, I);
					return false;
				}
				if (!ReadEntryFromStream(P, End, Loc.Entry))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: legacy index of '%s' truncated reading entry %u"), *PakFilePath, I);
					return false;
				}
				Files.Add(MoveTemp(Loc));
			}
			UE_LOG(LogUnrealGamePatcher, Display, TEXT("FPakReader: '%s' legacy index: %d files, mount '%s'"),
				*PakFilePath, Files.Num(), *MountPoint);
			return true;
		}

		// V10+ PathHashIndex primary index layout (engine PakFile.cpp):
		//   MountPoint (FString) | NumEntries (int32) | PathHashSeed (u64)
		//   | bHasPathHashIndex (int32) [+ (offset,size,hash)]   (bool is 4 bytes!)
		//   | bHasFullDirectoryIndex (int32) [+ (offset,size,hash)]
		//   | EncodedPakEntries (TArray<uint8>) | FilesNum (int32) + full entries
		// The offset/size fields of the secondary indexes (PathHashIndex /
		// FullDirectoryIndex) are ABSOLUTE file offsets, not offsets into the
		// index data: both blobs live between the index and the footer.
		int32 NumEntries = 0;
		if (!ReadLE(P, End, NumEntries))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: index of '%s' truncated (num entries)"), *PakFilePath);
			return false;
		}

		uint64 PathHashSeed = 0;
		if (!ReadLE(P, End, PathHashSeed))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: index of '%s' truncated (path hash seed)"), *PakFilePath);
			return false;
		}

		// Optional PathHashIndex reference (int32 bool + offset/size/hash).
		{
			int32 bHas = 0;
			if (!ReadLE(P, End, bHas)) return false;
			if (bHas)
			{
				int64 Off = 0; int64 Size = 0; uint8 Hash[20] = { 0 };
				if (!ReadLE(P, End, Off) || !ReadLE(P, End, Size)) return false;
				ReadBytes(P, End, Hash, 20);
			}
		}

		// Optional FullDirectoryIndex reference (int32 bool + offset/size/hash).
		int64 DirIndexOffset = -1;
		int64 DirIndexSize = 0;
		{
			int32 bHas = 0;
			if (!ReadLE(P, End, bHas)) return false;
			if (bHas)
			{
				int64 Off = 0; int64 Size = 0; uint8 Hash[20] = { 0 };
				if (!ReadLE(P, End, Off) || !ReadLE(P, End, Size)) return false;
				ReadBytes(P, End, Hash, 20);
				DirIndexOffset = Off;
				DirIndexSize = Size;
			}
		}

		// Encoded entries blob (TArray<uint8>).
		uint32 EncodedEntriesSize = 0;
		if (!ReadLE(P, End, EncodedEntriesSize)) return false;
		TArray64<uint8> EncodedBlob;
		EncodedBlob.SetNumUninitialized(EncodedEntriesSize);
		if (EncodedEntriesSize > 0 && !ReadBytes(P, End, EncodedBlob.GetData(), EncodedEntriesSize)) return false;

		// Non-encoded (full) entries follow.
		int32 NonEncodedCount = 0;
		if (!ReadLE(P, End, NonEncodedCount)) return false;
		TArray<FPakEntry> NonEncoded;
		NonEncoded.Reserve(NonEncodedCount);
		for (int32 I = 0; I < NonEncodedCount; ++I)
		{
			FPakEntry Entry;
			if (!ReadEntryFromStream(P, End, Entry)) return false;
			NonEncoded.Add(MoveTemp(Entry));
		}

		// The file list lives in the FullDirectoryIndex, which is a separate
		// blob stored in the pak file after the index (absolute offset).
		TArray64<uint8> DirData;
		const uint8* DirP = nullptr;
		const uint8* DirEnd = nullptr;
		if (DirIndexOffset >= 0 && DirIndexSize > 0)
		{
		DirData.SetNumUninitialized(DirIndexSize);
		if (!Handle->Seek(DirIndexOffset) || !Handle->Read(DirData.GetData(), DirIndexSize))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: failed reading FullDirectoryIndex of '%s'"), *PakFilePath);
			return false;
		}
		FIOStats::Get().AddDiskRead(DirIndexSize);
			DirP = DirData.GetData();
			DirEnd = DirP + DirData.Num();
		}
		else
		{
			DirP = P;
			DirEnd = End;
		}

		// FullDirectoryIndex: u32 dir_count + per dir: (FString name + u32 file_count
		// + per file: FUtf8String name + i32 encoded_offset).
		uint32 DirCount = 0;
		if (!ReadLE(DirP, DirEnd, DirCount))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: directory index of '%s' is truncated"), *PakFilePath);
			return false;
		}

		constexpr int32 DeletedMarker = MIN_int32; // 0x80000000
		for (uint32 D = 0; D < DirCount; ++D)
		{
			FString DirName;
			if (!ReadFString(DirP, DirEnd, DirName))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: directory index of '%s' truncated (dir %u)"), *PakFilePath, D);
				return false;
			}
			uint32 FileCount = 0;
			if (!ReadLE(DirP, DirEnd, FileCount))
			{
				return false;
			}
			for (uint32 F = 0; F < FileCount; ++F)
			{
				FString FileName;
				// File names in the FullDirectoryIndex use FUtf8String
				// serialization (length without null terminator), unlike the
				// directory names which are plain FStrings.
				if (!ReadFUtf8String(DirP, DirEnd, FileName))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: directory index of '%s' truncated (file %u in %s)"), *PakFilePath, F, *DirName);
					return false;
				}
				int32 EncodedOffset = 0;
				if (!ReadLE(DirP, DirEnd, EncodedOffset))
				{
					return false;
				}

				if (EncodedOffset == DeletedMarker)
				{
					continue; // record deleted during incremental rebuild
				}

				FPakEntryLocation Loc;
				if (DirName.IsEmpty())
				{
					Loc.Path = FileName;
				}
				else
				{
					Loc.Path = DirName / FileName;
				}

				if (EncodedOffset >= 0)
				{
					// Encoded entry inside the encoded blob.
					const uint8* EP = EncodedBlob.GetData() + EncodedOffset;
					if (!DecodeEntry(EP, EncodedBlob.GetData() + EncodedBlob.Num(), Loc.Entry))
					{
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: bad encoded entry at %d in '%s'"), EncodedOffset, *PakFilePath);
						return false;
					}
				}
				else
				{
					// Reference into the non-encoded entries: (-offset) - 1.
					const int32 Index = (-EncodedOffset) - 1;
					if (!NonEncoded.IsValidIndex(Index))
					{
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: non-encoded entry index %d out of range in '%s'"), Index, *PakFilePath);
						return false;
					}
					Loc.Entry = NonEncoded[Index];
				}

				Files.Add(MoveTemp(Loc));
			}
		}

		UE_LOG(LogUnrealGamePatcher, Display, TEXT("FPakReader: '%s' index: %d files, mount '%s'"),
			*PakFilePath, Files.Num(), *MountPoint);
		return true;
	}

	bool FPakReader::ReadEntryFromStream(const uint8*& P, const uint8* End, FPakEntry& OutEntry) const
	{
		OutEntry = FPakEntry();
		if (!ReadLE(P, End, OutEntry.Offset)) return false;
		if (!ReadLE(P, End, OutEntry.Size)) return false;
		if (!ReadLE(P, End, OutEntry.UncompressedSize)) return false;
		if (Info.Version >= 8)
		{
			if (!ReadLE(P, End, OutEntry.CompressionMethodIndex)) return false;
		}
		else
		{
			// Legacy COMPRESS_* bit flags map to fixed method slots (engine
			// FPakEntry::Serialize): None->0, ZLIB->1, GZIP->2, Custom->3.
			int32 LegacyMethod = 0;
			if (!ReadLE(P, End, LegacyMethod)) return false;
			if (LegacyMethod == 0)
			{
				OutEntry.CompressionMethodIndex = 0;
			}
			else if (LegacyMethod & 0x01) // COMPRESS_ZLIB_DEPRECATED
			{
				OutEntry.CompressionMethodIndex = 1;
			}
			else if (LegacyMethod & 0x02) // COMPRESS_GZIP_DEPRECATED
			{
				OutEntry.CompressionMethodIndex = 2;
			}
			else if (LegacyMethod & 0x04) // COMPRESS_Custom_DEPRECATED
			{
				OutEntry.CompressionMethodIndex = 3;
			}
			else
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: unknown legacy compression method 0x%x in '%s'"), LegacyMethod, *PakFilePath);
				return false;
			}
		}
		if (Info.Version <= 1)
		{
			int64 Timestamp = 0;
			if (!ReadLE(P, End, Timestamp)) return false;
		}
		if (!ReadBytes(P, End, OutEntry.Hash, 20)) return false;

		if (Info.Version >= 3)
		{
			// Order matches engine FPakEntry::Serialize: compression blocks are
			// written first (only when the entry is compressed), then Flags and
			// CompressionBlockSize.
			if (OutEntry.CompressionMethodIndex != 0)
			{
				uint32 BlockCount = 0;
				if (!ReadLE(P, End, BlockCount)) return false;
				// Each block costs 16 bytes in the stream; a count that cannot
				// possibly fit is corrupt - allocating it (u32 * 16B ≈ 64 GB)
				// would OOM instead of failing cleanly.
				if ((uint64)BlockCount * 16 > (uint64)(End - P)) return false;
				OutEntry.CompressionBlocks.SetNum(BlockCount);
				for (uint32 B = 0; B < BlockCount; ++B)
				{
					if (!ReadLE(P, End, OutEntry.CompressionBlocks[B].CompressedStart)) return false;
					if (!ReadLE(P, End, OutEntry.CompressionBlocks[B].CompressedEnd)) return false;
				}
			}
			if (!ReadLE(P, End, OutEntry.Flags)) return false;
			if (!ReadLE(P, End, OutEntry.CompressionBlockSize)) return false;
		}
		return true;
	}

int64 FPakReader::GetSerializedEntrySize(const FPakEntry& Entry, uint32 Version)
{
	return GetEntrySerializedSizeInternal(Entry, Version);
}

bool FPakReader::DecodeEntry(const uint8* P, const uint8* End, FPakEntry& OutEntry) const
{
	// Mirrors engine FPakFile::DecodePakEntry (PakFile.cpp). Layout:
	//   u32 Flags (bitfield below)
	//   [if packed block size == 0x3F] u32 full compression block size
	//   Offset (u32 or u64, per bit31)
	//   UncompressedSize (u32 or u64, per bit30)
	//   [if compression method != 0] Size (u32 or u64, per bit29)
	//   [if blocks > 1 || (blocks == 1 && encrypted)] per-block u32 compressed size
	// Bitfield: Bit31=Offset32safe, Bit30=Uncompressed32safe, Bit29=Size32safe,
	// Bits28..23=CompressionMethodIndex, Bit22=Encrypted,
	// Bits21..6=CompressionBlockCount, Bits5..0=BlockSize (0x3F -> u32, else << 11).
	uint32 Value = 0;
	if (!ReadLE(P, End, Value)) return false;

	OutEntry = FPakEntry();

	const uint32 BitOffset32Safe = 1u << 31;
	const uint32 BitUncompressed32Safe = 1u << 30;
	const uint32 BitSize32Safe = 1u << 29;
	const uint32 CompressionMethodShift = 23;
	const uint32 CompressionMethodMask = 0x3F;
	const uint32 BitEncrypted = 1u << 22;
	const uint32 CompressionBlockCountShift = 6;
	const uint32 CompressionBlockCountMask = 0xFFFF;
	const uint32 BlockSizeMask = 0x3F;

	// Compression block size comes right after the flags.
	uint32 CompressionBlockSize = 0;
	if ((Value & BlockSizeMask) == BlockSizeMask)
	{
		if (!ReadLE(P, End, CompressionBlockSize)) return false;
	}
	else
	{
		CompressionBlockSize = (Value & BlockSizeMask) << 11;
	}

	const uint32 CompressionMethod = (Value >> CompressionMethodShift) & CompressionMethodMask;
	const uint32 CompressionBlocksCount = (Value >> CompressionBlockCountShift) & CompressionBlockCountMask;
	const bool bOffset32Safe = (Value & BitOffset32Safe) != 0;
	const bool bUncompressed32Safe = (Value & BitUncompressed32Safe) != 0;
	const bool bSize32Safe = (Value & BitSize32Safe) != 0;
	const bool bEncrypted = (Value & BitEncrypted) != 0;

	OutEntry.CompressionMethodIndex = CompressionMethod;
	OutEntry.Flags = 0;
	if (bEncrypted) OutEntry.Flags |= 1;

	// Offset (points at the payload entry header in the data region).
	if (bOffset32Safe)
	{
		uint32 Tmp = 0;
		if (!ReadLE(P, End, Tmp)) return false;
		OutEntry.Offset = (int64)Tmp;
	}
	else
	{
		if (!ReadLE(P, End, OutEntry.Offset)) return false;
	}

	// UncompressedSize.
	if (bUncompressed32Safe)
	{
		uint32 Tmp = 0;
		if (!ReadLE(P, End, Tmp)) return false;
		OutEntry.UncompressedSize = (int64)Tmp;
	}
	else
	{
		if (!ReadLE(P, End, OutEntry.UncompressedSize)) return false;
	}

	// Size is only stored when compressed; otherwise Size == UncompressedSize.
	if (CompressionMethod != 0)
	{
		if (bSize32Safe)
		{
			uint32 Tmp = 0;
			if (!ReadLE(P, End, Tmp)) return false;
			OutEntry.Size = (int64)Tmp;
		}
		else
		{
			if (!ReadLE(P, End, OutEntry.Size)) return false;
		}
	}
	else
	{
		OutEntry.Size = OutEntry.UncompressedSize;
	}

	// NOTE: no allocation guard needed here - CompressionBlocksCount is a u16
	// field (<= 65535 blocks = 1 MiB max, intrinsically bounded), and in the
	// ENCODED entry record the block array is not part of this stream (the
	// block data lives in the payload header at OutEntry.Offset). The u32
	// unbounded count only exists on the legacy-index path, which has its own
	// stream-based guard.
	OutEntry.CompressionBlocks.SetNum(CompressionBlocksCount);
	OutEntry.CompressionBlockSize = 0;

	if (CompressionBlocksCount > 0)
	{
		// Read the per-block compressed sizes when the engine writes them
		// (blocks > 1, or a single encrypted block).
		if (CompressionBlocksCount > 1 || bEncrypted)
		{
			// Absolute offsets for compression blocks:
			//   v>=5 (RelativeChunkOffsets): relative to the payload header
			//       start (Entry.Offset) -> add Entry.Offset when reading.
			//   v<5: absolute within the file.
			// We store *relative-to-header* offsets so ReadEntryData only adds
			// Entry.Offset in relative mode.
			int64 Offset = GetEntrySerializedSizeInternal(OutEntry, Info.Version);
			for (uint32 B = 0; B < CompressionBlocksCount; ++B)
			{
				uint32 BlockSize = 0;
				if (!ReadLE(P, End, BlockSize)) return false;
				FPakCompressedBlock& Block = OutEntry.CompressionBlocks[B];
				Block.CompressedStart = Offset;
				Block.CompressedEnd = Offset + BlockSize;
				// The engine packs blocks back-to-back: the block alignment is
				// 1 for unencrypted paks (only encrypted blocks align to the
				// AES block size - see FPakFile::DecodePakEntry).
				Offset += BlockSize;
			}
		}
		else
		{
			// Single unencrypted block: derived from data start + Size.
			const int64 HeaderSize = GetEntrySerializedSizeInternal(OutEntry, Info.Version);
			OutEntry.CompressionBlocks[0].CompressedStart = HeaderSize;
			OutEntry.CompressionBlocks[0].CompressedEnd = HeaderSize + OutEntry.Size;
		}
		OutEntry.CompressionBlockSize = (CompressionBlocksCount == 1)
			? IntCastChecked<uint32>(OutEntry.UncompressedSize)
			: CompressionBlockSize;
	}

	// The encoded index entry stores no hash; it lives in the payload header.
	FMemory::Memset(OutEntry.Hash, 0, sizeof(OutEntry.Hash));
	return true;
}

bool FPakReader::ReadPayloadHash(const FPakEntry& Entry, uint8 OutHash[20]) const
{
	// The built-in per-file SHA1 lives in the payload entry header that
	// precedes each file's data (engine FPakFile::VerifyPakEntries reads it
	// the same way).
	if (!ReaderHandle || Entry.Offset < 0)
	{
		return false;
	}
	IFileHandle* Handle = static_cast<IFileHandle*>(ReaderHandle);
	const int64 HeaderSize = GetEntrySerializedSizeInternal(Entry, Info.Version);
	TArray64<uint8> Raw;
	Raw.SetNumUninitialized(HeaderSize);
	if (!Handle->Seek(Entry.Offset) || !Handle->Read(Raw.GetData(), HeaderSize))
	{
		return false;
	}
	FIOStats::Get().AddDiskRead(HeaderSize);
	const uint8* P = Raw.GetData();
	const uint8* End = P + Raw.Num();
	FPakEntry Header;
	if (!ReadEntryFromStream(P, End, Header))
	{
		return false;
	}
	FMemory::Memcpy(OutHash, Header.Hash, 20);
	return true;
}

bool FPakReader::ComputePayloadHash(const FPakEntry& Entry, uint8 OutHash[20]) const
{
	// Mirrors the engine's per-entry hashing (PakFileUtilities.cpp): the
	// built-in hash covers the bytes written to the pak. For uncompressed
	// entries that is the original data; for compressed entries it is the
	// compressed block payloads (packed back-to-back for unencrypted paks).
	if (!ReaderHandle || Entry.Offset < 0)
	{
		return false;
	}
	IFileHandle* Handle = static_cast<IFileHandle*>(ReaderHandle);
	const bool bRelativeChunkOffsets = Info.Version >= 5;
	const int64 HeaderSize = GetEntrySerializedSizeInternal(Entry, Info.Version);

	FSHA1 Hasher;

	if (!Entry.IsCompressed())
	{
		TArray64<uint8> Raw;
		Raw.SetNumUninitialized(Entry.Size);
		if (!Handle->Seek(Entry.Offset + HeaderSize) || !Handle->Read(Raw.GetData(), Entry.Size))
		{
			return false;
		}
		FIOStats::Get().AddDiskRead(Entry.Size);
		if (Entry.Size > 0)
		{
			Hasher.Update(Raw.GetData(), Entry.Size);
			FIOStats::Get().AddHashed(Entry.Size);
		}
	}
	else
	{
		for (int32 BlockIndex = 0; BlockIndex < Entry.CompressionBlocks.Num(); ++BlockIndex)
		{
			const FPakCompressedBlock& Block = Entry.CompressionBlocks[BlockIndex];
			const int64 BlockOffset = bRelativeChunkOffsets ? Entry.Offset + Block.CompressedStart : Block.CompressedStart;
			const int64 BlockBytes = Block.Size();
			if (BlockBytes < 0 || BlockOffset < 0)
			{
				return false;
			}
			TArray64<uint8> Compressed;
			Compressed.SetNumUninitialized(BlockBytes);
			if (!Handle->Seek(BlockOffset) || !Handle->Read(Compressed.GetData(), BlockBytes))
			{
				return false;
			}
			FIOStats::Get().AddDiskRead(BlockBytes);
			if (BlockBytes > 0)
			{
				Hasher.Update(Compressed.GetData(), BlockBytes);
				FIOStats::Get().AddHashed(BlockBytes);
			}
		}
	}

	Hasher.Final();
	Hasher.GetHash(OutHash);
	return true;
}

bool FPakReader::ReadEntryData(const FPakEntry& Entry, TArray64<uint8>& OutData) const
{
	if (!ReaderHandle)
	{
		return false;
	}
	IFileHandle* Handle = static_cast<IFileHandle*>(ReaderHandle);

	const bool bRelativeChunkOffsets = Info.Version >= 5;
	OutData.Reset();

	if (Entry.Offset < 0)
	{
		UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: entry has invalid offset %lld"), Entry.Offset);
		return false;
	}

	// Entry.Offset points at the payload entry header; the data follows it.
	const int64 HeaderSize = GetEntrySerializedSizeInternal(Entry, Info.Version);

	if (!Entry.IsCompressed())
	{
		// Uncompressed: read Size bytes straight after the header.
		OutData.SetNumUninitialized(Entry.Size);
		if (!Handle->Seek(Entry.Offset + HeaderSize) || !Handle->Read(OutData.GetData(), Entry.Size))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: failed reading %lld bytes at %lld"), Entry.Size, Entry.Offset + HeaderSize);
			OutData.Reset();
			return false;
		}
		FIOStats::Get().AddDiskRead(Entry.Size);
		return true;
	}

	// Compressed: decompress each block and concatenate.
	const FName Method = Info.GetCompressionMethod(Entry.CompressionMethodIndex);
	if (Method == NAME_None)
	{
		UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: entry uses unknown compression method index %u"), Entry.CompressionMethodIndex);
		return false;
	}

	const int64 CompressedBlockSize = Entry.CompressionBlockSize > 0 ? Entry.CompressionBlockSize : 64 * 1024;
	OutData.SetNumUninitialized(Entry.UncompressedSize);

	int64 TotalWritten = 0;
	for (int32 BlockIndex = 0; BlockIndex < Entry.CompressionBlocks.Num(); ++BlockIndex)
	{
		const FPakCompressedBlock& Block = Entry.CompressionBlocks[BlockIndex];
		const int64 BlockOffset = bRelativeChunkOffsets ? Entry.Offset + Block.CompressedStart : Block.CompressedStart;
		const int64 BlockBytes = Block.Size();
		if (BlockBytes < 0 || BlockOffset < 0)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: bad compression block %d"), BlockIndex);
			return false;
		}

		TArray64<uint8> Compressed;
		Compressed.SetNumUninitialized(BlockBytes);
		if (!Handle->Seek(BlockOffset) || !Handle->Read(Compressed.GetData(), BlockBytes))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: failed reading compression block %d at %lld"), BlockIndex, BlockOffset);
			return false;
		}
		FIOStats::Get().AddDiskRead(BlockBytes);

		// Uncompress into the output (the last block may decompress short).
		const int32 UncompressedCapacity = (int32)FMath::Min<int64>(CompressedBlockSize, Entry.UncompressedSize - TotalWritten);
		if (!FCompression::UncompressMemory(Method, OutData.GetData() + TotalWritten, (int32)UncompressedCapacity,
			Compressed.GetData(), (int32)Compressed.Num(), COMPRESS_NoFlags))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: decompression failed for block %d (method %s)"),
				BlockIndex, *Method.ToString());
			OutData.Reset();
			return false;
		}
		FIOStats::Get().AddDecompress(BlockBytes, UncompressedCapacity);
		TotalWritten += UncompressedCapacity;
	}

	if (TotalWritten != Entry.UncompressedSize)
	{
		UE_LOG(LogUnrealGamePatcher, Warning, TEXT("FPakReader: decompressed %lld bytes, entry claims %lld"),
			TotalWritten, Entry.UncompressedSize);
		OutData.SetNum(Entry.UncompressedSize);
	}
	return true;
}

bool FPakReader::ReadFile(const FString& PathInPak, TArray64<uint8>& OutData) const
{
	for (const FPakEntryLocation& Loc : Files)
	{
		if (Loc.Path.Equals(PathInPak, ESearchCase::IgnoreCase))
		{
			return ReadEntryData(Loc.Entry, OutData);
		}
	}
	UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakReader: file '%s' not found in '%s'"), *PathInPak, *PakFilePath);
	return false;
}
}
