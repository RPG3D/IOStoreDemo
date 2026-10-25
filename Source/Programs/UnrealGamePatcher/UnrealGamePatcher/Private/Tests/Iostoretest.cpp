// Copyright Epic Games, Inc. All Rights Reserved.
// -cli iostoretest —— IoStore 读取器（格式层）
// 验证点：utoc 头/目录/偏移解析；ucas chunk 定位与（Oodle）解码；
//         meta hash 校验。

#include "Tests/TestFramework.h"

namespace UnrealGamePatcher::Tests
{
	int32 RunIoStoreTest(const TCHAR* /*CmdLine*/)
	{

		// IoStore reader acceptance (task 7.1): parse the real utoc/ucas,
		// verify structural integrity (block coverage, chunk block ranges)
		// and decode a selection of chunks (raw + compressed).
		bool bAllPassed = true;
		auto Check = MakeCheck(bAllPassed);

		using namespace UnrealGamePatcher;

		const FString ProjectRoot = FPaths::GetPath(FPaths::GetPath(FPaths::GetPath(FPlatformProcess::BaseDir())));
		const FString StageDir = FPaths::Combine(ProjectRoot, TEXT(".manifesttest"), TEXT("chunkinstall"), TEXT("Windows"), TEXT("1.0.0"), TEXT("pakchunk1"));
		const FString UtocPath = FPaths::Combine(StageDir, TEXT("pakchunk1-Windows.utoc"));
		const FString UcasPath = FPaths::Combine(StageDir, TEXT("pakchunk1-Windows.ucas"));
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		Check(PF.FileExists(*UtocPath) && PF.FileExists(*UcasPath), TEXT("iostore: real utoc/ucas present"));

		FIoStoreReader Reader;
		Check(Reader.Open(UtocPath, UcasPath), TEXT("iostore: TOC parses"));
		Check(Reader.GetChunkCount() > 0 && Reader.GetBlockCount() > 0, TEXT("iostore: has chunks and blocks"));

		// Structural: every chunk's block range must be in bounds and the
		// compressed blocks must cover the ucas file exactly.
		{
			const int64 UcasSize = IFileManager::Get().FileSize(*UcasPath);
			bool bAllInRange = true;
			int64 MaxBlockEnd = 0;
			for (const FIoStoreBlock& B : Reader.GetBlocks())
			{
				MaxBlockEnd = FMath::Max(MaxBlockEnd, B.FileOffset + B.CompressedSize);
				if (B.FileOffset < 0 || B.CompressedSize <= 0)
				{
					bAllInRange = false;
				}
			}
			for (const FIoStoreChunk& C : Reader.GetChunks())
			{
				if (C.FirstBlockIndex < 0 || C.FirstBlockIndex + C.NumBlocks > Reader.GetBlockCount())
				{
					bAllInRange = false;
				}
			}
			Check(bAllInRange, TEXT("iostore: every chunk's block range in bounds"));
			Check(MaxBlockEnd == UcasSize, TEXT("iostore: compression blocks cover the ucas exactly"));
		}

		// Decode chunks: every chunk must decode to exactly its raw size.
		{
			bool bAllDecoded = true;
			int32 RawCount = 0, CompressedCount = 0;
			for (int32 I = 0; I < Reader.GetChunkCount(); ++I)
			{
				TArray64<uint8> Data;
				if (!Reader.ReadChunk(I, Data) || Data.Num() != Reader.GetChunks()[I].RawSize)
				{
					bAllDecoded = false;
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("iostore: chunk %d decode failed"), I);
					break;
				}
				if (Reader.GetChunks()[I].bIsCompressed) { ++CompressedCount; } else { ++RawCount; }
			}
			Check(bAllDecoded, TEXT("iostore: every chunk decodes to its raw size"));
			Check(CompressedCount > 0 && RawCount > 0, TEXT("iostore: mix of raw and compressed chunks decoded"));

			// Chunk-by-id lookup roundtrip.
			const FString FirstId = Reader.GetChunkId(0);
			TArray64<uint8> ById;
			Check(!FirstId.IsEmpty() && FirstId.Len() == 24 && Reader.ReadChunkById(FirstId, ById),
				TEXT("iostore: chunk-id lookup works"));
		}

		// Determinism: re-open gives identical counts.
		{
			FIoStoreReader Second;
			Check(Second.Open(UtocPath, UcasPath) && Second.GetChunkCount() == Reader.GetChunkCount(),
				TEXT("iostore: re-parse is deterministic"));
		}

		// --- Writer + diff (tasks 7.2/7.3) ---------------------------------
		// Copy the container, replace one chunk with new bytes, append a new
		// chunk, then verify: unchanged chunks are byte-identical AND their
		// block offsets were not moved (zero-movement guarantee); the replaced
		// and appended chunks decode to their new bytes.
		const FString WorkDir = GetTestWorkDir(TEXT("iostoretest"));
		ResetWorkDir(WorkDir);
		const FString WorkUcas = FPaths::Combine(WorkDir, TEXT("container.ucas"));
		const FString WorkUtoc = FPaths::Combine(WorkDir, TEXT("container.utoc"));
		{
			PF.CopyFile(*WorkUcas, *UcasPath);
			PF.CopyFile(*WorkUtoc, *UtocPath);
		}

		const int32 TargetIndex = 1;
		FString NewChunkId, AppendedChunkId;
		TArray64<uint8> ReplacedData, AppendedData;
		{
			// Replaced data: a distinctive, content-addressed blob.
			ReplacedData.SetNumUninitialized(3000);
			for (int32 I = 0; I < ReplacedData.Num(); ++I)
			{
				ReplacedData[I] = (uint8)((I * 7 + 13) & 0xFF);
			}
			AppendedData.SetNumUninitialized(70000); // spans two 64KiB blocks
			for (int32 I = 0; I < AppendedData.Num(); ++I)
			{
				AppendedData[I] = (uint8)((I * 3 + 5) & 0xFF);
			}

			FIoStoreWriter Writer;
			Check(Writer.Open(WorkUtoc, WorkUcas), TEXT("iostore: writer opens the copy"));
			Check(Writer.ReplaceChunk(TargetIndex, ReplacedData, NewChunkId), TEXT("iostore: chunk replaced"));
			Check(Writer.AppendChunk(AppendedData, AppendedChunkId), TEXT("iostore: chunk appended"));
			Check(Writer.Commit(WorkUtoc), TEXT("iostore: rebuilt utoc committed"));
		}

		// Verify the modified container.
		{
			FIoStoreReader Modified;
			Check(Modified.Open(WorkUtoc, WorkUcas), TEXT("iostore: modified container parses"));
			Check(Modified.GetChunkCount() == Reader.GetChunkCount() + 1, TEXT("iostore: one chunk appended"));

			// Unchanged chunks are byte-identical and their block offsets
			// were not moved.
			bool bUnchangedOk = true;
			int32 MovedBlocks = 0;
			for (int32 I = 0; I < Reader.GetChunkCount(); ++I)
			{
				if (I == TargetIndex)
				{
					continue;
				}
				TArray64<uint8> A, B;
				if (!Reader.ReadChunk(I, A) || !Modified.ReadChunk(I, B) ||
					A.Num() != B.Num() || FMemory::Memcmp(A.GetData(), B.GetData(), A.Num()) != 0)
				{
					bUnchangedOk = false;
				}
				for (int32 J = 0; J < Modified.GetChunks()[I].NumBlocks; ++J)
				{
					const FIoStoreBlock& NB = Modified.GetBlocks()[Modified.GetChunks()[I].FirstBlockIndex + J];
					const FIoStoreBlock& OB = Reader.GetBlocks()[Reader.GetChunks()[I].FirstBlockIndex + J];
					if (NB.FileOffset != OB.FileOffset)
					{
						++MovedBlocks;
					}
				}
			}
			Check(bUnchangedOk, TEXT("iostore: unchanged chunks byte-identical after update"));
			Check(MovedBlocks == 0, TEXT("iostore: unchanged blocks zero-moved (same file offsets)"));

			// The replaced chunk decodes to the new bytes with the new id.
			TArray64<uint8> Decoded;
			Check(Modified.ReadChunk(TargetIndex, Decoded), TEXT("iostore: replaced chunk decodes"));
			Check(Decoded.Num() == ReplacedData.Num() &&
				FMemory::Memcmp(Decoded.GetData(), ReplacedData.GetData(), Decoded.Num()) == 0,
				TEXT("iostore: replaced chunk has the new bytes"));
			Check(Modified.GetChunkId(TargetIndex).Equals(NewChunkId, ESearchCase::IgnoreCase),
				TEXT("iostore: replaced chunk carries the content-hash id"));

			// The appended chunk decodes correctly.
			TArray64<uint8> Appended;
			Check(Modified.ReadChunk(Modified.GetChunkCount() - 1, Appended), TEXT("iostore: appended chunk decodes"));
			Check(Appended.Num() == AppendedData.Num() &&
				FMemory::Memcmp(Appended.GetData(), AppendedData.GetData(), Appended.Num()) == 0,
				TEXT("iostore: appended chunk has the expected bytes"));
			Check(Modified.GetChunkId(Modified.GetChunkCount() - 1).Equals(AppendedChunkId, ESearchCase::IgnoreCase),
				TEXT("iostore: appended chunk id is its content hash"));

			// Chunk-id diff (task 7.3): base vs modified by id.
			{
				TSet<FString> BaseIds, ModifiedIds;
				for (const FIoStoreChunk& C : Reader.GetChunks()) { BaseIds.Add(C.Id); }
				for (const FIoStoreChunk& C : Modified.GetChunks()) { ModifiedIds.Add(C.Id); }

				TSet<FString> Same = BaseIds.Intersect(ModifiedIds);
				TSet<FString> Removed = BaseIds.Difference(ModifiedIds);
				TSet<FString> Added = ModifiedIds.Difference(BaseIds);
				// A pure id-set diff reports the replacement as one Removed
				// (old id) + one Added (new id), plus the appended chunk.
				Check(Removed.Num() == 1 && Added.Num() == 2 && Same.Num() == Reader.GetChunkCount() - 1,
					TEXT("iostore: chunk-id diff: 1 removed id, 2 added ids, rest same"));
				Check(Added.Contains(AppendedChunkId), TEXT("iostore: the appended chunk id is the diff Added"));
				Check(!Same.Contains(NewChunkId), TEXT("iostore: the replaced chunk id is no longer Same"));

				// Build-side delta closure (task 7.4): the download payload for
				// the update is exactly the added/changed chunk DATA - a tiny
				// fraction of the 10 MB container.
				int64 DeltaBytes = 0;
				for (const FString& Id : Added)
				{
					TArray64<uint8> ChunkData;
					if (Modified.ReadChunkById(Id, ChunkData))
					{
						DeltaBytes += ChunkData.Num();
					}
				}
				const int64 WholeBytes = IFileManager::Get().FileSize(*WorkUcas);
				Check(DeltaBytes > 0 && DeltaBytes < WholeBytes / 20,
					TEXT("iostore: delta closure fetches only the changed chunks"));
			}
		}
		if (bAllPassed)
		{
			PF.DeleteDirectoryRecursively(*WorkDir);
		}

		return FinishTest(bAllPassed, TEXT("iostoretest"));
	
	}

}
