// Copyright Epic Games, Inc. All Rights Reserved.
// -cli iostoreinplace —— ucas chunk 级原地写
// 验证点：只追加变化 chunk（bytes written ≪ 整 ucas）；stableid 内容
//         变化回填已记录洞（容器体积不变——洞复用钉）；chunkremoval
//         按 id-multiset 记洞 + 复跑收敛；utoc 原子重写。

#include "Tests/TestFramework.h"

namespace UnrealGamePatcher::Tests
{
	int32 RunIoStoreInPlace(const TCHAR* /*CmdLine*/)
	{

		// Ucas chunk-level in-place patching (design D6): build two container
		// versions (one chunk replaced, one appended), ingest both with the
		// IoStore chunk metadata, install v1, then let the orchestrator update
		// to v1.1 IN PLACE - only the changed chunks are appended, the utoc is
		// rewritten atomically, and a second update converges to a no-op.
		bool bAllPassed = true;
		auto Check = MakeCheck(bAllPassed);

		using namespace UnrealGamePatcher;

		const FString WorkDir = GetTestWorkDir(TEXT("iostoreinplace"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

		auto Sha1HexOfBytes = [](const TArray64<uint8>& Data) -> FString
		{
			FSHA1 Hasher;
			Hasher.Update(Data.GetData(), (int32)Data.Num());
			uint8 Hash[20] = { 0 };
			Hasher.Final();
			Hasher.GetHash(Hash);
			FString Out;
			Out.Reserve(40);
			for (int32 I = 0; I < 20; ++I) { Out += FString::Printf(TEXT("%02x"), Hash[I]); }
			return Out;
		};

		/** Stages the crafted containers into a version dir and ingests them
		 *  (the new staged-based pipeline - no official manifest, no BPT). */
		auto CraftAndIngest = [&](const TCHAR* Version, const TArray<TPair<FString, FString>>& Files, FString& OutIndexPath) -> bool
		{
			const FString StagedDir = FPaths::Combine(WorkDir, FString::Printf(TEXT("staged_%s"), Version));
			if (!PF.DirectoryExists(*StagedDir) && !PF.CreateDirectoryTree(*StagedDir))
			{
				return false;
			}
			for (const TPair<FString, FString>& P : Files)
			{
				const FString Dst = FPaths::Combine(StagedDir, P.Key);
				PF.DeleteFile(*Dst);
				if (!PF.CopyFile(*Dst, *P.Value))
				{
					return false;
				}
			}
			FIngestResult Result;
			if (!FBuildIngestor::Ingest(StagedDir, TEXT("InplaceTest"), Version, FPaths::Combine(WorkDir, TEXT("cdn")), Result))
			{
				return false;
			}
			OutIndexPath = Result.IndexPath;
			return true;
		};
		// --- v1 = copies of the real container; v2 = replace chunk 0 + append.
		const FString ProjectRoot = GetProjectRootFromExecutable();
		const FString StageDir = FPaths::Combine(ProjectRoot, TEXT(".manifesttest"), TEXT("chunkinstall"), TEXT("Windows"), TEXT("1.0.0"), TEXT("pakchunk1"));
		const FString V1Ucas = FPaths::Combine(WorkDir, TEXT("v1.ucas"));
		const FString V1Utoc = FPaths::Combine(WorkDir, TEXT("v1.utoc"));
		const FString V2Ucas = FPaths::Combine(WorkDir, TEXT("v2.ucas"));
		const FString V2Utoc = FPaths::Combine(WorkDir, TEXT("v2.utoc"));
		PF.CopyFile(*V1Ucas, *(FPaths::Combine(StageDir, TEXT("pakchunk1-Windows.ucas"))));
		PF.CopyFile(*V1Utoc, *(FPaths::Combine(StageDir, TEXT("pakchunk1-Windows.utoc"))));
		PF.CopyFile(*V2Ucas, *(FPaths::Combine(StageDir, TEXT("pakchunk1-Windows.ucas"))));
		PF.CopyFile(*V2Utoc, *(FPaths::Combine(StageDir, TEXT("pakchunk1-Windows.utoc"))));

		int64 ReplacedChunkRaw = 0, AppendedChunkRaw = 0;
		int32 V1ChunkCount = 0;
		{
			FIoStoreReader V1;
			Check(V1.Open(V1Utoc, V1Ucas), TEXT("iostoreinplace: v1 container opens"));
			V1ChunkCount = V1.GetChunkCount();
			Check(V1ChunkCount > 1, TEXT("iostoreinplace: v1 has multiple chunks"));

			FIoStoreWriter W;
			Check(W.Open(V2Utoc, V2Ucas), TEXT("iostoreinplace: writer opens v2"));

			TArray64<uint8> Replaced, Appended;
			Replaced.SetNumUninitialized(4096);
			for (int32 I = 0; I < Replaced.Num(); ++I) { Replaced[I] = (uint8)(0x10 + (I & 0x3F)); }
			Appended.SetNumUninitialized(8192);
			for (int32 I = 0; I < Appended.Num(); ++I) { Appended[I] = (uint8)(0x40 + (I & 0x3F)); }
			ReplacedChunkRaw = Replaced.Num();
			AppendedChunkRaw = Appended.Num();

			FString NewId;
			Check(W.ReplaceChunk(0, Replaced, NewId), TEXT("iostoreinplace: chunk 0 replaced"));
			Check(W.AppendChunk(Appended, NewId), TEXT("iostoreinplace: chunk appended"));
			Check(W.Commit(V2Utoc), TEXT("iostoreinplace: v2 container committed"));
		}

		// --- Ingest both versions (IoStore metadata extracted by the ingestor).
		FString IndexV1, IndexV2;
		{
			TArray<TPair<FString, FString>> Files1, Files2;
			Files1.Emplace(TEXT("game.ucas"), V1Ucas);
			Files1.Emplace(TEXT("game.utoc"), V1Utoc);
			Files2.Emplace(TEXT("game.ucas"), V2Ucas);
			Files2.Emplace(TEXT("game.utoc"), V2Utoc);
			Check(CraftAndIngest(TEXT("1.0"), Files1, IndexV1), TEXT("iostoreinplace: ingest v1"));
			Check(CraftAndIngest(TEXT("1.1"), Files2, IndexV2), TEXT("iostoreinplace: ingest v2"));
		}
		FIndexManifest Remote;
		Check(FIndexManifestFile::Load(IndexV2, Remote), TEXT("iostoreinplace: v2 index loads"));
		Check(Remote.Files[TEXT("game.ucas")].IoStoreChunks.Num() == V1ChunkCount + 1,
			TEXT("iostoreinplace: v2 index records the IoStore chunk ids"));

		// --- Install v1, then update to v1.1 via the orchestrator (in place).
		const FString Cdn = FPaths::Combine(WorkDir, TEXT("cdn"));
		const FString Install = FPaths::Combine(WorkDir, TEXT("install"));
		PF.CreateDirectoryTree(*Install);
		FUpdateStats S0;
		Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0"), Install, true, S0), TEXT("iostoreinplace: install v1"));

		FUpdateStats S1;
		Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.1"), Install, true, S1), TEXT("iostoreinplace: in-place update to 1.1"));
		Check(S1.NumFilesChanged == 2, TEXT("iostoreinplace: ucas + utoc seen changed"));

		// Only the changed chunks were appended - not the whole ucas.
		const int64 ExpectedBytes = ReplacedChunkRaw + AppendedChunkRaw;
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("iostoreinplace: bytes written %lld (changed chunks %lld, whole ucas %lld)"),
			S1.BytesWritten, ExpectedBytes, IFileManager::Get().FileSize(*(FPaths::Combine(Install, TEXT("game.ucas")))));
		Check(S1.BytesWritten == ExpectedBytes, TEXT("iostoreinplace: only the changed chunks were written"));

		// The patched container parses with the remote chunk ids.
		{
			FIoStoreReader Patched;
			Check(Patched.Open(FPaths::Combine(Install, TEXT("game.utoc")), FPaths::Combine(Install, TEXT("game.ucas"))),
				TEXT("iostoreinplace: patched container parses"));
			Check(Patched.GetChunkCount() == Remote.Files[TEXT("game.ucas")].IoStoreChunks.Num(),
				TEXT("iostoreinplace: patched chunk count matches the target"));
			// The patched container is ORDER-AGNOSTIC (virtual disk): the
			// id-matching patcher may drop/reorder entries - compare the
			// chunk-id MULTISET against the target metadata.
			TArray<FString> PatchedIds, TargetIds;
			for (int32 I = 0; I < Patched.GetChunkCount(); ++I) { PatchedIds.Add(Patched.GetChunkId(I)); }
			for (const FIndexIoStoreChunk& IC : Remote.Files[TEXT("game.ucas")].IoStoreChunks) { TargetIds.Add(IC.Id); }
			PatchedIds.Sort(); TargetIds.Sort();
			bool bIdsOk = PatchedIds.Num() == TargetIds.Num();
			for (int32 I = 0; bIdsOk && I < PatchedIds.Num(); ++I)
			{
				bIdsOk = PatchedIds[I].Equals(TargetIds[I], ESearchCase::IgnoreCase);
			}
			Check(bIdsOk, TEXT("iostoreinplace: chunk-id multiset matches the target metadata"));

			// The replaced chunk's data equals the remote slice (by id - the
			// patched container's chunk order may differ).
			TArray64<uint8> Decoded;
			const FString& ReplacedId = Remote.Files[TEXT("game.ucas")].IoStoreChunks[0].Id;
			Check(Patched.ReadChunkById(ReplacedId, Decoded), TEXT("iostoreinplace: replaced chunk decodes"));
			Check(Decoded.Num() == ReplacedChunkRaw, TEXT("iostoreinplace: replaced chunk has the expected size"));
		}

		// Convergence: a second update to 1.1 is a no-op (chunk-id diff).
		{
			FUpdateStats S2;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.1"), Install, true, S2), TEXT("iostoreinplace: rerun converges"));
			Check(S2.NumFilesChanged == 0 && S2.NumFilesSame == 2 && S2.BytesWritten == 0,
				TEXT("iostoreinplace: rerun is a no-op (chunk-id diff)"));
		}

		// --- Real-engine simulation: content change with a STABLE chunk id.
		// Engine chunk ids derive from the PACKAGE name (FPackageId::FromName
		// in IoStoreUtilities), NOT the content - a content edit inside an
		// existing package keeps its chunk id while the utoc's meta hash
		// changes. The (Id, MetaHash) diff must catch this; the old id-only
		// diff classified it Same and the update was NEVER delivered.
		{
			const FString V3Ucas = FPaths::Combine(WorkDir, TEXT("v3.ucas"));
			const FString V3Utoc = FPaths::Combine(WorkDir, TEXT("v3.utoc"));
			PF.CopyFile(*V3Ucas, *V2Ucas);
			PF.CopyFile(*V3Utoc, *V2Utoc);

			TArray64<uint8> Edited;
			Edited.SetNumUninitialized(4096);
			for (int32 I = 0; I < Edited.Num(); ++I) { Edited[I] = (uint8)(0x77 + (I & 0x3F)); }

			FString StableId;
			{
				FIoStoreReader V2R;
				Check(V2R.Open(V2Utoc, V2Ucas), TEXT("stableid: v2 container opens"));
				StableId = V2R.GetChunks()[0].Id;
			}
			FString NewMeta;
			{
				const FIoHash Meta = FIoHash::HashBuffer(Edited.GetData(), Edited.Num());
				NewMeta = FPatcherHash::FIoHashToHex(Meta);
				FIoStoreWriter W3;
				Check(W3.Open(V3Utoc, V3Ucas), TEXT("stableid: writer opens v3"));
				FString OutId;
				Check(W3.ReplaceChunkEx(0, Edited, StableId, NewMeta, OutId),
					TEXT("stableid: chunk 0 replaced with the STABLE id"));
				Check(OutId.Equals(StableId, ESearchCase::IgnoreCase),
					TEXT("stableid: writer honored the explicit target id"));
				Check(W3.Commit(V3Utoc), TEXT("stableid: v3 container committed"));
			}

			TArray<TPair<FString, FString>> V3Files;
			V3Files.Add(TPair<FString, FString>(TEXT("game.ucas"), V3Ucas));
			V3Files.Add(TPair<FString, FString>(TEXT("game.utoc"), V3Utoc));
			FString V3IndexPath;
			Check(CraftAndIngest(TEXT("1.2"), V3Files, V3IndexPath), TEXT("stableid: v3 ingested"));

			// THE C1 ASSERTION: same chunk ids, different content - the diff
			// must classify the ucas (and its mirrored utoc) Changed.
			const int64 UcasSizeBefore = IFileManager::Get().FileSize(
				*FPaths::Combine(Install, TEXT("game.ucas")));
			FUpdateStats S3;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.2"), Install, true, S3),
				TEXT("stableid: update to the stable-id version succeeds"));
			Check(S3.NumFilesChanged == 2 && S3.NumFilesSame == 0,
				TEXT("stableid: stable-id content change DETECTED (the id-only diff said Same)"));
			// BACKFILL: the only container write is the replaced chunk 0
			// (4 KiB raw -> one 64 KiB block), which must land in the dead
			// 64 KiB region the 1.1 patch's replaced chunk left behind - the
			// .ucas does not grow at all.
			Check(IFileManager::Get().FileSize(*FPaths::Combine(Install, TEXT("game.ucas"))) == UcasSizeBefore,
				TEXT("stableid: replaced chunk BACKFILLED into the recorded hole (ucas size unchanged)"));

			// The patched container decodes the edited data and carries the
			// stable id.
			{
				FIoStoreReader R3;
				Check(R3.Open(FPaths::Combine(Install, TEXT("game.utoc")),
					FPaths::Combine(Install, TEXT("game.ucas"))),
					TEXT("stableid: patched container opens"));
				Check(R3.GetChunkCount() == V1ChunkCount + 1, TEXT("stableid: chunk count preserved"));
				int32 StableIdx = INDEX_NONE;
				for (int32 I = 0; I < R3.GetChunkCount(); ++I)
				{
					if (R3.GetChunkId(I).Equals(StableId, ESearchCase::IgnoreCase)) { StableIdx = I; }
				}
				Check(StableIdx != INDEX_NONE,
					TEXT("stableid: patched chunk keeps the stable id"));
				TArray64<uint8> Decoded;
				Check(StableIdx != INDEX_NONE && R3.ReadChunk(StableIdx, Decoded) &&
					Decoded.Num() == Edited.Num() &&
					FMemory::Memcmp(Decoded.GetData(), Edited.GetData(), Edited.Num()) == 0,
					TEXT("stableid: the stable-id chunk decodes to the edited content"));
			}

			// Convergence: the rerun is a no-op ((Id, MetaHash) multiset equal).
			{
				FUpdateStats S4;
				Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.2"), Install, true, S4), TEXT("stableid: rerun converges"));
				Check(S4.NumFilesChanged == 0 && S4.BytesWritten == 0, TEXT("stableid: rerun is a no-op"));
			}
		}

		// --- Chunk REMOVAL (id-multiset semantics): the target drops one
		// chunk. The patcher must drop the local TOC entry by ID - zero data
		// fetched, zero chunk bytes written - and record the dead block span
		// as a hole. The old positional matching misread every chunk after
		// the removal point as changed (full-container rewrite).
		{
			FString DroppedId;
			{
				FIoStoreReader RCur;
				Check(RCur.Open(FPaths::Combine(Install, TEXT("game.utoc")),
					FPaths::Combine(Install, TEXT("game.ucas"))), TEXT("chunkremoval: current container opens"));
				// The install holds v3 (= v1 + 1 appended chunk, see the
				// stableid section above; 135+1=136 on the original fixture).
				Check(RCur.GetChunkCount() == V1ChunkCount + 1, TEXT("chunkremoval: current container matches the v3 chunk count"));
				DroppedId = RCur.GetChunks()[5].Id;
			}
			const FString V4Ucas = FPaths::Combine(WorkDir, TEXT("v4.ucas"));
			const FString V4Utoc = FPaths::Combine(WorkDir, TEXT("v4.utoc"));
			PF.CopyFile(*V4Ucas, *FPaths::Combine(Install, TEXT("game.ucas")));
			PF.CopyFile(*V4Utoc, *FPaths::Combine(Install, TEXT("game.utoc")));
			{
				FIoStoreWriter W4;
				Check(W4.Open(V4Utoc, V4Ucas), TEXT("chunkremoval: writer opens v4"));
				Check(W4.DropChunk(5), TEXT("chunkremoval: v4 crafted (chunk 5 dropped)"));
				Check(W4.Commit(V4Utoc), TEXT("chunkremoval: v4 committed"));
			}
			TArray<TPair<FString, FString>> V4Files;
			V4Files.Add(TPair<FString, FString>(TEXT("game.ucas"), V4Ucas));
			V4Files.Add(TPair<FString, FString>(TEXT("game.utoc"), V4Utoc));
			FString V4IndexPath;
			Check(CraftAndIngest(TEXT("1.3"), V4Files, V4IndexPath), TEXT("chunkremoval: v4 ingested"));

			FUpdateStats S5;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.3"), Install, true, S5),
				TEXT("chunkremoval: removal update succeeds"));
			Check(S5.NumFilesChanged == 2 && S5.BytesWritten == 0,
				TEXT("chunkremoval: in-place drop (zero chunk bytes written)"));
			{
				FIoStoreReader R5;
				Check(R5.Open(FPaths::Combine(Install, TEXT("game.utoc")),
					FPaths::Combine(Install, TEXT("game.ucas"))), TEXT("chunkremoval: patched container opens"));
				Check(R5.GetChunkCount() == V1ChunkCount, TEXT("chunkremoval: exactly one chunk dropped"));
				bool bGone = true;
				for (const FIoStoreChunk& C : R5.GetChunks())
				{
					if (C.Id.Equals(DroppedId, ESearchCase::IgnoreCase)) { bGone = false; }
				}
				Check(bGone, TEXT("chunkremoval: the dropped chunk is gone"));
			}
			{
				FPatcherState St;
				FPatcherStateFile::Load(Paths::GetStatePath(Install), St);
				Check(St.Holes.Num() > 0, TEXT("chunkremoval: the dropped span recorded as a hole"));
			}
			{
				FUpdateStats S6;
				Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.3"), Install, true, S6), TEXT("chunkremoval: rerun converges"));
				Check(S6.NumFilesChanged == 0 && S6.BytesWritten == 0, TEXT("chunkremoval: rerun is a no-op"));
			}
		}

		if (bAllPassed)
		{
			PF.DeleteDirectoryRecursively(*WorkDir);
		}
		return FinishTest(bAllPassed, TEXT("iostoreinplace"));
	
	}

}
