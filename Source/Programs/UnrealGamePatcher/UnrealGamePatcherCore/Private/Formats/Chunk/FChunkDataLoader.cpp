// Copyright Epic Games, Inc. All Rights Reserved.

#include "Formats/Chunk/FChunkDataLoader.h"
#include "Hash/FPatcherHash.h"
#include "Stats/FIOStats.h"
#include "UnrealGamePatcherLog.h"
#include "UnrealGamePatcherPaths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace UnrealGamePatcher
{
	namespace
	{
	bool ReadAllBytes(const FString& Path, TArray64<uint8>& OutBytes)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		IFileHandle* Handle = PF.OpenRead(*Path);
		if (!Handle)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FChunkDataLoader: cannot open '%s'"), *Path);
			return false;
		}
		const int64 Size = Handle->Size();
		FIOStats::Get().AddDiskRead(Size);
		OutBytes.SetNumUninitialized(Size);
			const bool bOk = Size == 0 || Handle->Read(OutBytes.GetData(), Size);
			delete Handle;
			if (!bOk)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FChunkDataLoader: read failed on '%s'"), *Path);
			}
			return bOk;
		}

		void WriteU64(TArray64<uint8>& Out, uint64 V)
		{
			for (int32 I = 0; I < 8; ++I)
			{
				Out.Add((uint8)(V & 0xFF));
				V >>= 8;
			}
		}

		uint64 ReadU64(const uint8* P)
		{
			uint64 V = 0;
			for (int32 I = 0; I < 8; ++I)
			{
				V |= (uint64(P[I]) << (8 * I));
			}
			return V;
		}
	}

	bool FChunkDataLoader::Write(const FString& ChunkFilePath, const TArray64<uint8>& Uncompressed, FString& OutSha1Hex, uint8& OutStoredAs)
	{
		return Write(ChunkFilePath, Uncompressed.GetData(), Uncompressed.Num(), OutSha1Hex, OutStoredAs);
	}

	bool FChunkDataLoader::Write(const FString& ChunkFilePath, const uint8* Uncompressed, int64 UncompressedSize, FString& OutSha1Hex, uint8& OutStoredAs)
	{
		// Content address = SHA1 of the uncompressed window.
		FSHA1 Hasher;
		Hasher.Update(Uncompressed, (int32)UncompressedSize);
		uint8 Hash[20] = { 0 };
		Hasher.Final();
		Hasher.GetHash(Hash);
		OutSha1Hex = FPatcherHash::BytesToHex(Hash, 20);

	// Try Oodle; keep it only when it saves at least MinOodleSavePercent of
	// the window (compressed < 90%), otherwise store the window raw.
	TArray64<uint8> Stored;
	OutStoredAs = StoredAsRaw;
	{
		int64 Bound = 0;
		if (FCompression::CompressMemoryBound(NAME_Oodle, Bound, UncompressedSize))
		{
			TArray64<uint8> Compressed;
			Compressed.SetNumUninitialized(Bound);
			int64 CompressedSize = Bound;
			if (FCompression::CompressMemory(NAME_Oodle, Compressed.GetData(), CompressedSize,
				Uncompressed, UncompressedSize) &&
				CompressedSize > 0 &&
				CompressedSize * 100 < UncompressedSize * (100 - MinOodleSavePercent))
			{
				FIOStats::Get().AddCompress(UncompressedSize, CompressedSize);
				Compressed.SetNum(CompressedSize, EAllowShrinking::No);
				Stored = MoveTemp(Compressed);
				OutStoredAs = StoredAsOodle;
			}
		}
		if (OutStoredAs == StoredAsRaw)
		{
			Stored.SetNumUninitialized(UncompressedSize);
			FMemory::Memcpy(Stored.GetData(), Uncompressed, UncompressedSize);
		}
	}

	// [8B magic][8B usz][8B csz][1B storedAs][20B sha1][data]
	TArray64<uint8> Out;
	Out.Reserve(HeaderSize + Stored.Num());
	for (int32 I = 0; I < 8; ++I) { Out.Add((uint8)FChunkDataLoader::Magic[I]); }
	WriteU64(Out, (uint64)UncompressedSize);
	WriteU64(Out, (uint64)Stored.Num());
	Out.Add(OutStoredAs);
	for (int32 I = 0; I < 20; ++I) { Out.Add(Hash[I]); }
	Out.Append(Stored);

	// Atomic publish (fixplan L-M3): the content-addressed target may be a
	// cross-version chunk shared with an already-published index - a torn
	// direct write would corrupt it for every existing client. Write to a
	// temp name and swap in; readers never see a partial file.
	const FString TempPath = ChunkFilePath + TEXT(".tmp");
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (PF.FileExists(*TempPath) && !PF.DeleteFile(*TempPath))
	{
		UE_LOG(LogUnrealGamePatcher, Error, TEXT("FChunkDataLoader: cannot remove stale temp '%s'"), *TempPath);
		return false;
	}
	if (!FFileHelper::SaveArrayToFile(Out, *TempPath))
	{
		UE_LOG(LogUnrealGamePatcher, Error, TEXT("FChunkDataLoader: failed writing '%s'"), *TempPath);
		return false;
	}
	if (!Paths::AtomicReplaceFile(TempPath, ChunkFilePath))
	{
		UE_LOG(LogUnrealGamePatcher, Error, TEXT("FChunkDataLoader: cannot publish '%s'"), *ChunkFilePath);
		return false;
	}
	FIOStats::Get().AddDiskWrite(Out.Num());
	return true;
}

	bool FChunkDataLoader::Load(const FString& ChunkFilePath, const FIndexChunkEntry& Entry, TArray64<uint8>& OutData)
	{
		// Count ATTEMPTS (not successes): a patcher that reads windows the
		// prefetch skipped shows up as attempts against missing cache files
		// even though zero of them succeed (the H1 signature).
		FIOStats::Get().AddChunkLoad();
		TArray64<uint8> Raw;
		if (!ReadAllBytes(ChunkFilePath, Raw))
		{
			return false;
		}
		if (Raw.Num() < HeaderSize)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FChunkDataLoader: '%s' too small for a chunk header"), *ChunkFilePath);
			return false;
		}

		const uint8* P = Raw.GetData();
		for (int32 I = 0; I < 8; ++I)
		{
			if (P[I] != (uint8)FChunkDataLoader::Magic[I])
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FChunkDataLoader: bad magic in '%s'"), *ChunkFilePath);
				return false;
			}
		}
		const int64 UncompressedSize = (int64)ReadU64(P + 8);
		const int64 CompressedSize = (int64)ReadU64(P + 16);
		const uint8 StoredAs = P[24];
		const uint8* StoredHash = P + 25;

		if (UncompressedSize <= 0 || UncompressedSize > (64 << 20) ||
			CompressedSize < 0 || CompressedSize != (int64)Raw.Num() - HeaderSize)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FChunkDataLoader: '%s' implausible sizes (usz %lld, csz %lld)"),
				*ChunkFilePath, UncompressedSize, CompressedSize);
			return false;
		}
		const uint8* Data = P + HeaderSize;

		// Unpack: raw or Oodle (both flags are supported - the chunker writes
		// Oodle when it shrinks the window, raw otherwise).
		TArray64<uint8> Uncompressed;
		if (StoredAs == StoredAsRaw)
		{
			if (CompressedSize != UncompressedSize)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FChunkDataLoader: '%s' raw size %lld != uncompressed size %lld"),
					*ChunkFilePath, CompressedSize, UncompressedSize);
				return false;
			}
			Uncompressed.SetNumUninitialized(UncompressedSize);
			if (UncompressedSize > 0)
			{
				FMemory::Memcpy(Uncompressed.GetData(), Data, UncompressedSize);
			}
		}
	else if (StoredAs == StoredAsOodle)
	{
		Uncompressed.SetNumUninitialized(UncompressedSize);
		if (!FCompression::UncompressMemory(NAME_Oodle, Uncompressed.GetData(), UncompressedSize,
			const_cast<uint8*>(Data), CompressedSize))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FChunkDataLoader: Oodle decompress failed for '%s'"), *ChunkFilePath);
			return false;
		}
		FIOStats::Get().AddDecompress(CompressedSize, UncompressedSize);
	}
		else
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FChunkDataLoader: '%s' has unknown StoredAs %d"), *ChunkFilePath, StoredAs);
			return false;
		}

	// Verify the content SHA1 against the manifest entry (the chunk key).
	FSHA1 Hasher;
	Hasher.Update(Uncompressed.GetData(), (int32)Uncompressed.Num());
	FIOStats::Get().AddHashed(Uncompressed.Num());
		uint8 Hash[20] = { 0 };
		Hasher.Final();
		Hasher.GetHash(Hash);
		if (FMemory::Memcmp(Hash, StoredHash, 20) != 0 ||
			!FPatcherHash::BytesToHex(Hash, 20).Equals(Entry.SHA1, ESearchCase::IgnoreCase))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FChunkDataLoader: '%s' SHA1 mismatch (manifest %s)"),
				*ChunkFilePath, *Entry.SHA1);
			return false;
		}

		OutData = MoveTemp(Uncompressed);
		return true;
	}
}
