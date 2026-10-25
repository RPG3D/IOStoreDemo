// Copyright Epic Games, Inc. All Rights Reserved.
// -cli e2etest / -cli perftest —— 端到端管线 + 比率测量（共一个实现）
// e2etest 验证点：真 staged 输出 ingest → 空目录 update → 产物引擎可
//         加载（pak 挂载逐条目 hash 一致、utoc 可解析、全文件 hash 一致）。
// perftest 验证点：增量更新下载/写入字节相对全量的比率（增量语义的
//         量化哨兵）。

#include "Tests/TestFramework.h"

namespace UnrealGamePatcher::Tests
{
	int32 RunE2ePerfTest(const TCHAR* CmdLine)
	{

		// 9.1 e2etest: the full pipeline - ingest a real packaging output into
		// the CDN, update an empty install, verify the result is engine-loadable
		// (pak opens with matching entry hashes, utoc parses, every file hash-
		// matches the manifest).
		// 9.3 perftest: measures the incremental download/write delta vs the
		// full install and prints the ratio (mechanical-disk savings).
		const FString Command = GetCommandToken(CmdLine);
		const bool bPerf = Command == TEXT("perftest");

		bool bAllPassed = true;
		auto Check = MakeCheck(bAllPassed);

		using namespace UnrealGamePatcher;

		const FString WorkDir = GetTestWorkDir(*Command);
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		const FString Cdn = FPaths::Combine(WorkDir, TEXT("cdn"));
		const FString Install = FPaths::Combine(WorkDir, TEXT("install"));
		PF.CreateDirectoryTree(*Install);

		// --- Pipeline: ingest -> update -> verify ---------------------------
		FIndexManifest Index;
		Check(BuildFixtureCdn(WorkDir, Cdn, Index), TEXT("e2e: ingest staged fixture"));

		int64 FullFetchBytes = 0, FullWriteBytes = 0, FullInstallBytes = 0;
		{
			FUpdateStats S;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.0"), Install, true, S), TEXT("e2e: client update succeeds"));
			FullFetchBytes = S.BytesFetched;
			FullWriteBytes = S.BytesWritten;
			for (const TPair<FString, FIndexFileEntry>& Pair : Index.Files)
			{
				FullInstallBytes += Pair.Value.Size;
			}
			Check(S.NumFilesAdded == 4 && S.NumChunksFetched == Index.Chunks.Num(), TEXT("e2e: full install fetched every chunk"));
		}

		// Engine-loadability of the installed containers. Whole-file hash
		// equality is pinned by updatetest section 1 and the entry-hash
		// cross-check by section 2 (same fixture) - here only the unique
		// value is asserted, once.
		{
			FPakReader Reader;
			Check(Reader.Open(FPaths::Combine(Install, TEXT("pakchunk1-Windows.pak"))) && Reader.ReadIndex(),
				TEXT("e2e: updated pak loads (engine-format)"));

			// The updated IoStore container parses.
			FIoStoreReader Io;
			const FString Utoc = FPaths::Combine(Install, TEXT("pakchunk1-Windows.utoc"));
			const FString Ucas = FPaths::Combine(Install, TEXT("pakchunk1-Windows.ucas"));
			Check(Io.Open(Utoc, Ucas) && Io.GetChunkCount() > 0, TEXT("e2e: updated utoc/ucas parses (engine-format)"));
		}

		// --- Incremental delta (9.3) ----------------------------------------
		// Simulate a real version bump: 1.0.1 adds a small readme.txt (built
		// from an unchanged chunk slice - pure reuse, zero download), then
		// 1.0.2 grows it with a fresh chunk slice (exactly one chunk fetched).
		// The measured delta must be a small fraction of the full install.
		int64 DeltaFetchBytes = 0, DeltaWriteBytes = 0;
		{
			const FIndexFileEntry& Utoc = Index.Files[TEXT("pakchunk1-Windows.utoc")];
			const FIndexFileChunkRef& UtocPart = Utoc.Parts[0];
			const FString ChunkGuid = UtocPart.Chunk;
			const int64 PartOffset = UtocPart.Offset, PartSize = UtocPart.Size;

			// The utoc slice = the utoc file content; readme.txt starts as that.
			TArray64<uint8> UtocBytes;
			FFileHelper::LoadFileToArray(UtocBytes, *(FPaths::Combine(Install, TEXT("pakchunk1-Windows.utoc"))));

			// The chunk's full raw data (for building the 1.0.2 expected bytes).
			const FIndexChunkEntry& Chunk = Index.Chunks[ChunkGuid];
			TArray64<uint8> ChunkData;
			Check(FChunkDataLoader::Load(FPaths::Combine(Cdn, Chunk.File), Chunk, ChunkData),
				TEXT("perf: chunk data loads"));

			auto MakeVersion = [&](const TCHAR* V, const TArray64<uint8>& Content, const TArray<FIndexFileChunkRef>& Parts)
			{
				FIndexManifest M = Index;
				M.ContentVersion = V;
				FIndexFileEntry Readme;
				Readme.Size = Content.Num();
				Readme.SHA1 = Sha1OfBytes(Content);
				Readme.Parts = Parts;
				M.Files.Add(TEXT("readme.txt"), Readme);
				const FString Dir = FPaths::Combine(Cdn, TEXT("index"), V);
				PF.CreateDirectoryTree(*Dir);
				Check(FIndexManifestFile::Save(FPaths::Combine(Dir, TEXT("UnrealGamePatcher.index.json")), M),
					*FString::Printf(TEXT("perf: crafted %s index"), V));
			};

			// 1.0.1: readme.txt = the utoc slice (reused locally, 0 fetches).
			{
				TArray<FIndexFileChunkRef> Parts;
				FIndexFileChunkRef P;
				P.Chunk = ChunkGuid; P.Offset = PartOffset; P.Size = PartSize;
				Parts.Add(P);
				MakeVersion(TEXT("1.0.1"), UtocBytes, Parts);
			}
			FUpdateStats S101;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.1"), Install, true, S101), TEXT("perf: update to 1.0.1"));
			Check(S101.NumFilesAdded == 1 && S101.NumChunksFetched == 0, TEXT("perf: added file served from local chunks (0 downloads)"));

			// 1.0.2: readme grows by a fresh slice of the same chunk -> one fetch.
			{
				// No chunk staging: the window is GLOBAL on the CDN already.

				// Fresh slice (16, 1024) - fits the small utoc chunk window (the
				// new chunker does not zero-pad to 1 MiB).
				const int64 FreshOffset = 16;
				const int64 FreshSize = 1024;
				TArray64<uint8> Content;
				Content.SetNumUninitialized(UtocBytes.Num() + FreshSize);
				FMemory::Memcpy(Content.GetData(), UtocBytes.GetData(), UtocBytes.Num());
				FMemory::Memcpy(Content.GetData() + UtocBytes.Num(), ChunkData.GetData() + FreshOffset, FreshSize);

				TArray<FIndexFileChunkRef> Parts;
				FIndexFileChunkRef P1, P2;
				P1.Chunk = ChunkGuid; P1.Offset = PartOffset; P1.Size = PartSize;
				P2.Chunk = ChunkGuid; P2.Offset = FreshOffset; P2.Size = FreshSize;
				Parts.Add(P1);
				Parts.Add(P2);
				MakeVersion(TEXT("1.0.2"), Content, Parts);
			}
			FUpdateStats S102;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.2"), Install, true, S102), TEXT("perf: update to 1.0.2"));
			Check(S102.NumFilesChanged == 1 && S102.NumChunksFetched == 1, TEXT("perf: one chunk fetched for the changed file"));
			DeltaFetchBytes = S102.BytesFetched;
			DeltaWriteBytes = S102.BytesWritten;
		}

		if (bPerf)
		{
			const double FetchRatio = FullFetchBytes > 0 ? (100.0 * (double)DeltaFetchBytes / (double)FullFetchBytes) : 0.0;
			const double WriteRatio = FullWriteBytes > 0 ? (100.0 * (double)DeltaWriteBytes / (double)FullWriteBytes) : 0.0;
			UE_LOG(LogUnrealGamePatcher, Display,
				TEXT("perftest: full install fetch %lld bytes / write %lld bytes; delta fetch %lld bytes (%.1f%%), delta write %lld bytes (%.1f%%)"),
				FullFetchBytes, FullWriteBytes, DeltaFetchBytes, FetchRatio, DeltaWriteBytes, WriteRatio);
			UE_LOG(LogUnrealGamePatcher, Display,
				TEXT("perftest: install size %lld bytes"), FullInstallBytes);
			// A single-file change must not re-fetch/write the whole install.
			Check(DeltaFetchBytes < FullFetchBytes / 10 && DeltaWriteBytes < FullWriteBytes / 10,
				TEXT("perftest: delta stays far below the full re-fetch (documented ~0.1% scale)"));
		}

		PF.DeleteDirectoryRecursively(*WorkDir);
		return FinishTest(bAllPassed, *Command);
	
	}

}
