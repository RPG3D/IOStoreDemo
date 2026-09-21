// Copyright Epic Games, Inc. All Rights Reserved.

#include "Formats/Chunk/FChunkAssembler.h"
#include "Formats/Chunk/FChunkDataLoader.h"
#include "Stats/FIOStats.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UnrealGamePatcher
{
	bool FChunkAssembler::AssembleToBytes(
		const FIndexManifest& Remote,
		const FString& ChunkRoot,
		const FString& FileId,
		TArray64<uint8>& OutBytes,
		FChunkCache* Cache)
	{
		const FIndexFileEntry* File = Remote.Files.Find(FileId);
		if (!File || File->Parts.Num() == 0)
		{
			return false;
		}
		return AssembleRangeToBytes(Remote, ChunkRoot, FileId, 0, File->Size, OutBytes, Cache);
	}

	bool FChunkAssembler::AssembleRangeToBytes(
		const FIndexManifest& Remote,
		const FString& ChunkRoot,
		const FString& FileId,
		int64 Offset,
		int64 Size,
		TArray64<uint8>& OutBytes,
		FChunkCache* Cache)
	{
		const FIndexFileEntry* File = Remote.Files.Find(FileId);
		if (!File || Size < 0)
		{
			return false;
		}
		OutBytes.Reset();
		int64 FilePos = 0;
		const int64 RangeEnd = Offset + Size;
		for (const FIndexFileChunkRef& Part : File->Parts)
		{
			const int64 PartEnd = FilePos + Part.Size;
			if (PartEnd <= Offset)
			{
				FilePos = PartEnd;
				continue;
			}
			if (FilePos >= RangeEnd)
			{
				break;
			}
			const int64 SliceStart = FMath::Max<int64>(FilePos, Offset);
			const int64 SliceEnd = FMath::Min<int64>(PartEnd, RangeEnd);
			if (SliceEnd <= SliceStart)
			{
				FilePos = PartEnd;
				continue;
			}

			const FIndexChunkEntry* Chunk = Remote.Chunks.Find(Part.Chunk);
			if (!Chunk || Chunk->File.IsEmpty())
			{
				return false;
			}

			const TArray64<uint8>* ChunkData = Cache ? Cache->Verified.Find(Part.Chunk) : nullptr;
			TArray64<uint8> Owned;
			if (!ChunkData)
			{
				TArray64<uint8> Loaded;
				if (!FChunkDataLoader::Load(FPaths::Combine(ChunkRoot, Chunk->File), *Chunk, Loaded))
				{
					return false;
				}
				if (Cache)
				{
					Cache->Verified.Emplace(Part.Chunk, MoveTemp(Loaded));
					ChunkData = &Cache->Verified.FindChecked(Part.Chunk);
				}
				else
				{
					Owned = MoveTemp(Loaded);
					ChunkData = &Owned;
				}
			}
			const int64 ChunkOff = Part.Offset + (SliceStart - FilePos);
			const int64 Len = SliceEnd - SliceStart;
			if (ChunkOff + Len > (int64)ChunkData->Num())
			{
				return false;
			}
			const int64 Old = OutBytes.Num();
			OutBytes.SetNum(Old + Len, EAllowShrinking::No);
			FMemory::Memcpy(OutBytes.GetData() + Old, ChunkData->GetData() + ChunkOff, Len);
			FilePos = PartEnd;
		}
		return OutBytes.Num() == Size;
	}

	bool FChunkAssembler::AssembleToFile(
		const FIndexManifest& Remote,
		const FString& ChunkRoot,
		const FString& FileId,
		const FString& OutPath,
		FChunkCache* Cache)
	{
		TArray64<uint8> Bytes;
		if (!AssembleToBytes(Remote, ChunkRoot, FileId, Bytes, Cache))
		{
			return false;
		}
		const bool bOk = FFileHelper::SaveArrayToFile(Bytes, *OutPath);
		if (bOk)
		{
			FIOStats::Get().AddDiskWrite(Bytes.Num());
		}
		return bOk;
	}
}
