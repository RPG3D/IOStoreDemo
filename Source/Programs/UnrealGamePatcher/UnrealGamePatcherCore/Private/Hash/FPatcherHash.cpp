// Copyright Epic Games, Inc. All Rights Reserved.

#include "Hash/FPatcherHash.h"
#include "String/BytesToHex.h"
#include "String/HexToBytes.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "SecureHash.h"

namespace UnrealGamePatcher
{
	uint64 FPatcherHash::XXH64(const void* Data, uint64 Size)
	{
		return FXxHash64::HashBuffer(Data, Size).Hash;
	}

	FString FPatcherHash::XXH64ToHex(const void* Data, uint64 Size)
	{
		return XXH64ToHex(FXxHash64::HashBuffer(Data, Size));
	}

	FString FPatcherHash::XXH64ToHex(const FXxHash64& Hash)
	{
		// ToByteArray emits the hash in big-endian order, so the hex form is
		// stable across platforms and matches xxhsum -H2.
		uint8 Bytes[sizeof(uint64)];
		Hash.ToByteArray(Bytes);
		return FString::Printf(TEXT("%02x%02x%02x%02x%02x%02x%02x%02x"),
			Bytes[0], Bytes[1], Bytes[2], Bytes[3], Bytes[4], Bytes[5], Bytes[6], Bytes[7]);
	}

	FXxHash64 FPatcherHash::XXH64FromHex(const FString& Hex)
	{
		uint8 Bytes[sizeof(uint64)] = { 0 };
		UE::String::HexToBytes(Hex, Bytes);
		return FXxHash64::FromByteArray(Bytes);
	}

	FString FPatcherHash::FIoHashToHex(const FIoHash& Hash)
	{
		TStringBuilder<64> Builder;
		UE::String::BytesToHex(Hash.GetBytes(), Builder);
		return Builder.ToString();
	}

	FIoHash FPatcherHash::FIoHashFromHex(const FString& Hex)
	{
		return FIoHash(FWideStringView(Hex));
	}

	FString FPatcherHash::FIoChunkIdToHex(const FIoChunkId& ChunkId)
	{
		return LexToString(ChunkId);
	}

	FIoChunkId FPatcherHash::FIoChunkIdFromHex(const FString& Hex)
	{
		return FIoChunkId::FromHex(FStringView(Hex));
	}

	FString FPatcherHash::BytesToHex(const uint8* Data, int32 Len)
	{
		TStringBuilder<64> Builder;
		UE::String::BytesToHex(TConstArrayView<uint8>(Data, Len), Builder);
		return Builder.ToString();
	}

	bool FPatcherHash::HexToBytes(const FString& Hex, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		const int32 MaxBytes = Hex.Len() / 2;
		OutBytes.SetNumUninitialized(MaxBytes);
		const int32 Written = UE::String::HexToBytes(FWideStringView(Hex), OutBytes.GetData());
		OutBytes.SetNum(Written);
		return Written == MaxBytes;
	}

	FString FPatcherHash::FileSHA1Hex(const FString& Path, int64* OutSize)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		IFileHandle* Handle = PF.OpenRead(*Path);
		if (!Handle)
		{
			if (OutSize) { *OutSize = 0; }
			return FString();
		}
		FSHA1 Hasher;
		const int64 Total = Handle->Size();
		constexpr int64 WindowSize = 1 << 20;
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(WindowSize);
		int64 Remaining = Total;
		while (Remaining > 0)
		{
			const int32 ToRead = (int32)FMath::Min<int64>(Remaining, WindowSize);
			if (!Handle->Read(Buffer.GetData(), ToRead))
			{
				delete Handle;
				if (OutSize) { *OutSize = 0; }
				return FString();
			}
			Hasher.Update(Buffer.GetData(), ToRead);
			Remaining -= ToRead;
		}
		uint8 Hash[20] = { 0 };
		Hasher.Final();
		Hasher.GetHash(Hash);
		delete Handle;
		if (OutSize) { *OutSize = Total; }
		return BytesToHex(Hash, 20);
	}
}
