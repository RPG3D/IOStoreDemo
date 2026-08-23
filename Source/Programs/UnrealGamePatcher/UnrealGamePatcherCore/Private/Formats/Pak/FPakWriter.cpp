// Copyright Epic Games, Inc. All Rights Reserved.

#include "Formats/Pak/FPakWriter.h"
#include "UnrealGamePatcherLog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "SecureHash.h"

namespace UnrealGamePatcher
{
	namespace
	{
		// --- Low level binary writers (mirrors the reader helpers) -----------

		template <typename T>
		void WriteLE(TArray64<uint8>& Out, T Value)
		{
			Out.Append((const uint8*)&Value, sizeof(T));
		}

		void WriteFString(TArray64<uint8>& Out, const FString& Str)
		{
			// Same encoding as the reader: positive length = ANSI bytes
			// (including the null terminator). Write the chars, then an
			// explicit null - never read past the buffer.
			const FTCHARToUTF8 Utf8(*Str, Str.Len());
			const int32 Chars = Utf8.Length();
			WriteLE<int32>(Out, Chars + 1);
			Out.Append((const uint8*)Utf8.Get(), Chars);
			Out.Add(0);
		}

		int64 GetEntrySerializedSize(const FPakEntry& Entry, uint32 Version)
		{
			// Mirrors engine FPakEntry::GetSerializedSize (same as reader).
			int64 Size = 8 + 8 + 8; // Offset, Size, UncompressedSize
			Size += 4;              // CompressionMethodIndex (int32)
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

		/** Serializes one full FPakEntry (payload header or directory entry). */
		void SerializePakEntry(TArray64<uint8>& Out, const FPakEntry& Entry, uint32 Version)
		{
			WriteLE<int64>(Out, Entry.Offset);
			WriteLE<int64>(Out, Entry.Size);
			WriteLE<int64>(Out, Entry.UncompressedSize);
			WriteLE<uint32>(Out, Entry.CompressionMethodIndex);
			if (Version <= 1)
			{
				WriteLE<int64>(Out, 0); // Timestamp
			}
			Out.Append(Entry.Hash, 20);
			if (Version >= 3)
			{
				if (Entry.CompressionMethodIndex != 0)
				{
					WriteLE<uint32>(Out, Entry.CompressionBlocks.Num());
					for (const FPakCompressedBlock& Block : Entry.CompressionBlocks)
					{
						WriteLE<int64>(Out, Block.CompressedStart);
						WriteLE<int64>(Out, Block.CompressedEnd);
					}
				}
				WriteLE<uint8>(Out, Entry.Flags);
				WriteLE<uint32>(Out, Entry.CompressionBlockSize);
			}
		}

		void ComputeEntryHash(const FPakEntry& Entry, const TArray<FPakWriter::FStoredBlock>& Blocks, uint8 OutHash[20])
		{
			// Mirrors the engine (PakFileUtilities.cpp): uncompressed entries
			// hash the raw data; compressed entries hash the concatenated
			// compressed block payloads (padding stripped).
			FSHA1 Hasher;
			for (const auto& Block : Blocks)
			{
				if (Block.Bytes.Num() > 0)
				{
					Hasher.Update(Block.Bytes.GetData(), Block.Bytes.Num());
				}
			}
			Hasher.Final();
			Hasher.GetHash(OutHash);
		}
	}

	FPakWriter::~FPakWriter()
	{
		if (WriteHandle)
		{
			IFileHandle* Handle = static_cast<IFileHandle*>(WriteHandle);
			delete Handle;
			WriteHandle = nullptr;
		}
	}

	bool FPakWriter::Open(const FString& InPakFilePath)
	{
		PakFilePath = InPakFilePath;
		Files.Empty();
		MountPoint.Empty();
		FileSize = 0;
		BytesAdded = 0;

		// Parse the existing directory through the reader first. It must be
		// destroyed before we open the write handle: the read handle's Windows
		// share flags would otherwise block opening the same file for writing.
		{
			FPakReader Reader;
			if (!Reader.Open(PakFilePath) || !Reader.ReadIndex())
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakWriter: cannot parse '%s'"), *PakFilePath);
				return false;
			}
			if (Reader.GetInfo().Version < MinWriteVersion)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakWriter: pak version %u not writable (need >= %u)"),
					Reader.GetInfo().Version, MinWriteVersion);
				return false;
			}

			Info = Reader.GetInfo();
			MountPoint = Reader.GetMountPoint();
			Files = Reader.GetFiles();
		}

		// Open a read/write handle positioned at the end (append mode). The
		// Windows file API maps bAppend=true to OPEN_ALWAYS (no truncate) and
		// starts the write position at the end of file; Seek still works for
		// in-place overwrites.
		IFileHandle* Handle = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*PakFilePath, true, true);
		if (!Handle)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakWriter: cannot open '%s' for writing"), *PakFilePath);
			return false;
		}
		WriteHandle = Handle;
		FileSize = Handle->Size();
		BytesAdded = 0;

		UE_LOG(LogUnrealGamePatcher, Display, TEXT("FPakWriter: opened '%s' (version %u, %d files, size %lld)"),
			*PakFilePath, Info.Version, Files.Num(), FileSize);
		return true;
	}

	int32 FPakWriter::RegisterCompressionMethod(FName Method)
	{
		if (Method == NAME_None)
		{
			return 0;
		}
		// In-memory slot 0 is always NAME_None (engine convention).
		for (int32 I = 1; I < Info.CompressionMethods.Num(); ++I)
		{
			if (Info.CompressionMethods[I] == Method)
			{
				return I;
			}
		}
		if (Info.CompressionMethods.Num() >= 6) // NAME_None + 5 footer slots
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakWriter: compression method table full (cannot register '%s')"), *Method.ToString());
			return 0;
		}
		Info.CompressionMethods.Add(Method);
		return Info.CompressionMethods.Num() - 1;
	}

	bool FPakWriter::Overwrite(const FString& PathInPak, const FPakEntry& NewEntry, const uint8* Data, int64 Size)
	{
		if (!WriteHandle)
		{
			return false;
		}

		// Find the existing directory entry.
		FPakReader::FPakEntryLocation* Existing = nullptr;
		for (FPakReader::FPakEntryLocation& Loc : Files)
		{
			if (Loc.Path.Equals(PathInPak, ESearchCase::IgnoreCase))
			{
				Existing = &Loc;
				break;
			}
		}
		if (!Existing)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakWriter: cannot overwrite '%s': not in pak"), *PathInPak);
			return false;
		}

		// In-place overwrite requires an unchanged stored size and unchanged
		// compression layout so the payload header keeps its serialized size.
		const int64 OldHeaderSize = GetEntrySerializedSize(Existing->Entry, Info.Version);
		const int64 NewHeaderSize = GetEntrySerializedSize(NewEntry, Info.Version);
		if (NewEntry.Size != Existing->Entry.Size || NewHeaderSize != OldHeaderSize)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakWriter: cannot overwrite '%s' in place: stored size/layout changed (%lld vs %lld bytes)"),
				*PathInPak, NewEntry.Size, Existing->Entry.Size);
			return false;
		}

		IFileHandle* Handle = static_cast<IFileHandle*>(WriteHandle);

		// The payload write is unbounded by the format: reject a byte count
		// larger than the entry's stored size, or the next entry's payload
		// would be silently overwritten while the index still claims the old
		// layout (undetectable corruption).
		if (Size > Existing->Entry.Size)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakWriter: cannot overwrite '%s': %lld bytes into a %lld-byte stored slot"),
				*PathInPak, Size, Existing->Entry.Size);
			return false;
		}

		// Rewrite the payload header (keeps the same size) at the entry offset.
		TArray64<uint8> Header;
		SerializePakEntry(Header, NewEntry, Info.Version);
		if (!Handle->Seek(Existing->Entry.Offset) ||
			!Handle->Write(Header.GetData(), Header.Num()))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakWriter: failed writing header for '%s'"), *PathInPak);
			return false;
		}

		// Rewrite the stored payload in place.
		if (Size > 0)
		{
			if (!Handle->Seek(Existing->Entry.Offset + OldHeaderSize) ||
				!Handle->Write(Data, Size))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakWriter: failed writing data for '%s'"), *PathInPak);
				return false;
			}
		}

		// The directory entry keeps its ON-DISK offset (the in-place patch
		// never moves data); the caller's NewEntry may carry a fresh offset
		// value, so preserve the original one explicitly.
		const int64 KeptOffset = Existing->Entry.Offset;
		Existing->Entry = NewEntry;
		Existing->Entry.Offset = KeptOffset;

		UE_LOG(LogUnrealGamePatcher, Display, TEXT("FPakWriter: overwrote '%s' in place (%lld bytes at %lld)"),
			*PathInPak, Size, Existing->Entry.Offset);
		return true;
	}

	bool FPakWriter::AppendEntry(const FEntryToWrite& InEntry, FPakEntry& OutEntry)
	{
		if (!WriteHandle)
		{
			return false;
		}
		if (InEntry.Blocks.Num() == 0 || (InEntry.CompressionMethodIndex == 0 && InEntry.Blocks.Num() != 1))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakWriter: entry '%s' has invalid block layout"), *InEntry.Path);
			return false;
		}

		// Build the entry. Offset is assigned 32-byte aligned at the end of file.
		FPakEntry Entry;
		Entry.CompressionMethodIndex = InEntry.CompressionMethodIndex;
		Entry.UncompressedSize = InEntry.UncompressedSize;
		Entry.CompressionBlockSize = InEntry.CompressionBlockSize;
		if (Entry.CompressionMethodIndex != 0)
		{
			// Size the compression block table up front: GetEntrySerializedSize
			// depends on it, and block offsets are relative to the entry start
			// (V5+ RelativeChunkOffsets), so the first block begins right after
			// the full header.
			Entry.CompressionBlocks.SetNum(InEntry.Blocks.Num());
		}

		const int64 HeaderSize = GetEntrySerializedSize(Entry, Info.Version);

		// Total serialized footprint (header + stored blocks) - what a
		// placement candidate must accommodate.
		int64 Needed = HeaderSize;
		for (const FStoredBlock& B : InEntry.Blocks)
		{
			Needed += B.Bytes.Num();
		}

		// Backfill: prefer the best-fitting recorded dead range over
		// appending at EOF. Entry placement is layout-agnostic (V5+ block
		// offsets are relative to the entry start), so a hole-placed entry
		// is byte-identical to an EOF-placed one. The sub-32B head sliver
		// of a filled range is dropped (it can never host an entry).
		int64 HoleIdx = INDEX_NONE;
		int64 HoleAligned = 0;
		int64 BestWaste = TNumericLimits<int64>::Max();
		for (int32 Idx = 0; Idx < PlacementHoles.Num(); ++Idx)
		{
			const FHole& H = PlacementHoles[Idx];
			const int64 Aligned = Align(H.Offset, 32);
			const int64 Waste = (H.Offset + H.Size) - (Aligned + Needed);
			if (Waste >= 0 && Waste < BestWaste)
			{
				BestWaste = Waste;
				HoleIdx = Idx;
				HoleAligned = Aligned;
			}
		}
		if (HoleIdx != INDEX_NONE)
		{
			Entry.Offset = HoleAligned;
			const FHole Filled = PlacementHoles[HoleIdx];
			const int64 UsedEnd = HoleAligned + Needed;
			PlacementHoles.RemoveAt(HoleIdx);
			if (UsedEnd < Filled.Offset + Filled.Size)
			{
				FHole Tail;
				Tail.File = Filled.File;
				Tail.Offset = UsedEnd;
				Tail.Size = Filled.Offset + Filled.Size - UsedEnd;
				PlacementHoles.Insert(Tail, HoleIdx);
			}
			++NumHolesFilled;
		}
		else
		{
			Entry.Offset = Align(FileSize, 32);
		}

		if (Entry.CompressionMethodIndex == 0)
		{
			// Uncompressed: single block of raw data.
			Entry.Size = InEntry.Blocks[0].Bytes.Num();
			if (Entry.UncompressedSize == 0)
			{
				Entry.UncompressedSize = Entry.Size;
			}
		}
		else
		{
			// Compressed: lay out blocks relative to the entry start, packed
			// back-to-back (the engine's block alignment is 1 for unencrypted
			// paks - V5+ RelativeChunkOffsets).
			int64 Cursor = HeaderSize;
			Entry.Size = 0;
			for (int32 I = 0; I < InEntry.Blocks.Num(); ++I)
			{
				FPakCompressedBlock& Block = Entry.CompressionBlocks[I];
				Block.CompressedStart = Cursor;
				Block.CompressedEnd = Cursor + InEntry.Blocks[I].Bytes.Num();
				Cursor = Block.CompressedEnd;
				Entry.Size += InEntry.Blocks[I].Bytes.Num();
			}
		}

		ComputeEntryHash(Entry, InEntry.Blocks, Entry.Hash);

		// Serialize header + blocks and write at the aligned offset.
		TArray64<uint8> Payload;
		SerializePakEntry(Payload, Entry, Info.Version);
		if (Entry.CompressionMethodIndex == 0)
		{
			Payload.Append(InEntry.Blocks[0].Bytes);
		}
		else
		{
			// Write each block back-to-back (no inter-block padding - the
			// engine's compressed-block alignment is 1 for unencrypted paks).
			for (int32 I = 0; I < InEntry.Blocks.Num(); ++I)
			{
				Payload.Append(InEntry.Blocks[I].Bytes);
			}
		}

		IFileHandle* Handle = static_cast<IFileHandle*>(WriteHandle);
		if (!Handle->Seek(Entry.Offset) || !Handle->Write(Payload.GetData(), Payload.Num()))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakWriter: failed appending '%s'"), *InEntry.Path);
			return false;
		}

		if (HoleIdx == INDEX_NONE)
		{
			FileSize = Entry.Offset + Payload.Num();
			BytesAdded += Payload.Num();
		}
		// Hole-placed: the EOF cursor and BytesAdded stay unchanged - the
		// bytes went into recorded dead space, the file did not grow.

		// Track the entry for the directory rebuild.
		FPakReader::FPakEntryLocation Loc;
		Loc.Path = InEntry.Path;
		Loc.Entry = Entry;
		Files.Add(Loc);

		OutEntry = Entry;
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("FPakWriter: appended '%s' (offset %lld, %lld bytes)"),
			*InEntry.Path, Entry.Offset, Entry.Size);
		return true;
	}

	bool FPakWriter::Commit()
	{
		if (!WriteHandle)
		{
			return false;
		}

		// --- Build the V10+ primary index ------------------------------------
		// The engine requires at least one secondary index (PathHashIndex or
		// FullDirectoryIndex) on V10+ paks, so Commit() writes a FullDirectoryIndex
		// blob (directory tree -> entry offset) right after the primary index.
		// Entries are referenced from the directory tree by negative offsets
		// into the non-encoded entries list ((-offset)-1), so EncodedPakEntries
		// stays empty. Layout: [primary index][FullDirectoryIndex blob][footer].
		TArray64<uint8> Index;
		WriteFString(Index, MountPoint);
		WriteLE<int32>(Index, Files.Num());          // NumEntries
		WriteLE<uint64>(Index, 0);                   // PathHashSeed (unused without path hash index)
		WriteLE<int32>(Index, 0);                    // bHasPathHashIndex
		WriteLE<int32>(Index, 1);                    // bHasFullDirectoryIndex (offset filled below)

		// Reserve the (offset,size,hash) triple; we need the blob offset which
		// depends on the final index size.
		const int64 DirRefPos = Index.Num();
		WriteLE<int64>(Index, 0);
		WriteLE<int64>(Index, 0);
		Index.AddZeroed(20); // hash placeholder

		WriteLE<uint32>(Index, 0);                   // EncodedPakEntries (empty TArray<uint8>)
		WriteLE<int32>(Index, Files.Num());          // FilesNum
		for (const FPakReader::FPakEntryLocation& Loc : Files)
		{
			SerializePakEntry(Index, Loc.Entry, Info.Version);
		}

		// --- Build the FullDirectoryIndex blob ------------------------------
		TArray64<uint8> DirIndex;
		{
			// Group files by directory. Directory names carry a trailing '/'
			// like the engine's, root files live in "/".
			struct FDirGroup { FString Name; TArray<const FPakReader::FPakEntryLocation*> Files; };
			TArray<FDirGroup> Groups;
			TMap<FString, int32> GroupIndex;
			for (int32 I = 0; I < Files.Num(); ++I)
			{
				const FPakReader::FPakEntryLocation& Loc = Files[I];
				FString Dir = FPaths::GetPath(Loc.Path);
				FString Name;
				if (Dir.IsEmpty())
				{
					Dir = TEXT("/");
					Name = Loc.Path;
				}
				else
				{
					Dir += TEXT("/"); // trailing slash, matches the engine
					Name = FPaths::GetCleanFilename(Loc.Path);
				}
				int32* G = GroupIndex.Find(Dir);
				if (!G)
				{
					GroupIndex.Add(Dir, Groups.Num());
					Groups.AddDefaulted();
					Groups.Last().Name = Dir;
					G = &GroupIndex.FindChecked(Dir);
				}
				Groups[*G].Files.Add(&Loc);
			}

			WriteLE<uint32>(DirIndex, Groups.Num());
			for (const FDirGroup& Group : Groups)
			{
				WriteFString(DirIndex, Group.Name);
				WriteLE<uint32>(DirIndex, Group.Files.Num());
				for (const FPakReader::FPakEntryLocation* Loc : Group.Files)
				{
					// File name uses FUtf8String serialization (length without
					// null terminator), matching the engine.
					const FTCHARToUTF8 Utf8(*FPaths::GetCleanFilename(Loc->Path));
					WriteLE<int32>(DirIndex, Utf8.Length());
					DirIndex.Append((const uint8*)Utf8.Get(), Utf8.Length());
					// Negative offset: index into the non-encoded entries list.
					int32 FileIndex = 0;
					for (int32 J = 0; J < Files.Num(); ++J)
					{
						if (&Files[J] == Loc) { FileIndex = J; break; }
					}
					WriteLE<int32>(DirIndex, -(FileIndex + 1));
				}
			}
		}

		FSHA1 DirHasher;
		DirHasher.Update(DirIndex.GetData(), DirIndex.Num());
		DirHasher.Final();
		uint8 DirIndexHash[20];
		DirHasher.GetHash(DirIndexHash);

		// --- Resolve the blob offset and patch the primary index -------------
		const int64 DirBlobOffset = FileSize + Index.Num();
		const int64 DirBlobSize = DirIndex.Num();
		uint8* DirRef = Index.GetData() + DirRefPos;
		FMemory::Memcpy(DirRef, &DirBlobOffset, 8);      // offset (absolute)
		FMemory::Memcpy(DirRef + 8, &DirBlobSize, 8);    // size
		FMemory::Memcpy(DirRef + 16, DirIndexHash, 20);  // hash

		FSHA1 IndexHasher;
		IndexHasher.Update(Index.GetData(), Index.Num());
		IndexHasher.Final();
		uint8 IndexHash[20];
		IndexHasher.GetHash(IndexHash);

		// --- Build the footer ------------------------------------------------
		TArray64<uint8> Footer;
		// EncryptionKeyGuid (16B, zeroed)
		Footer.AddZeroed(16);
		WriteLE<uint8>(Footer, Info.bEncryptedIndex ? 1 : 0); // bEncryptedIndex
		WriteLE<uint32>(Footer, PakFileMagic);
		WriteLE<uint32>(Footer, Info.Version);
		WriteLE<int64>(Footer, FileSize);            // IndexOffset
		WriteLE<int64>(Footer, Index.Num());         // IndexSize
		Footer.Append(IndexHash, 20);
		// CompressionMethods: 5 x 32-byte ANSI slots; slot 0 holds the first
		// non-None in-memory method (in-memory index 0 is always NAME_None).
		{
			uint8 Methods[5 * 32] = { 0 };
			for (int32 I = 1; I < Info.CompressionMethods.Num() && (I - 1) < 5; ++I)
			{
				const FName& Method = Info.CompressionMethods[I];
				if (Method != NAME_None)
				{
					const FTCHARToUTF8 Utf8(*Method.ToString());
					FMemory::Memcpy(&Methods[(I - 1) * 32], Utf8.Get(), FMath::Min(Utf8.Length(), 31));
				}
			}
			Footer.Append(Methods, sizeof(Methods));
		}

		// --- Append index + FullDirectoryIndex blob + footer at end of file --
		IFileHandle* Handle = static_cast<IFileHandle*>(WriteHandle);
		if (!Handle->Seek(FileSize) ||
			!Handle->Write(Index.GetData(), Index.Num()) ||
			!Handle->Write(DirIndex.GetData(), DirIndex.Num()) ||
			!Handle->Write(Footer.GetData(), Footer.Num()))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakWriter: failed writing index/footer"));
			return false;
		}
		const int64 Total = Index.Num() + DirIndex.Num() + Footer.Num();
		BytesAdded += Total;
		FileSize += Total;

		UE_LOG(LogUnrealGamePatcher, Display, TEXT("FPakWriter: committed index (%lld) + dir index (%lld) + footer; pak is %lld bytes"),
			Index.Num(), DirIndex.Num(), FileSize);
		return true;
	}
}
