// Copyright Epic Games, Inc. All Rights Reserved.
// -cli diffbuild —— 发布侧版本 diff / 跳版本 / 防篡改
// 验证点：v1→v2 差异正确性；跨版本（跳版本）diff；delta manifest 篡改拒绝。

#include "Tests/TestFramework.h"

namespace UnrealGamePatcher::Tests
{
	int32 RunDiffBuild(const TCHAR* /*CmdLine*/)
	{

		// Build-side diff acceptance (tasks 4.2 + 4.3): hand-crafted v1/v2/v3
		// manifests, empty diff, delta roundtrip/tamper, chunk staging, and
		// cross-version (v1->v3) diff correctness.
		bool bAllPassed = true;
		auto Check = MakeCheck(bAllPassed);

		using namespace UnrealGamePatcher;

		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		const FString WorkDir = GetTestWorkDir(TEXT("diffbuild"));
		ResetWorkDir(WorkDir);
		const FString Cdn = FPaths::Combine(WorkDir, TEXT("cdn"));

		// Helper: write a chunk in OUR format into the GLOBAL <cdn>/chunks/
		// (content-addressed, 2/2/36 sharded path) and return its entry.
		auto StageChunk = [&PF, &Cdn](const FString& Ver, const FString& Content) -> FIndexChunkEntry
		{
			TArray64<uint8> Bytes;
			{
				const FTCHARToUTF8 Conv(*Content);
				Bytes.SetNumUninitialized(Conv.Length());
				FMemory::Memcpy(Bytes.GetData(), Conv.Get(), Conv.Length());
			}
			const FString ChunkDir = FPaths::Combine(Cdn, TEXT("chunks"));
			if (!PF.DirectoryExists(*ChunkDir)) { PF.CreateDirectoryTree(*ChunkDir); }
			FIndexChunkEntry E;
			uint8 StoredAs = 0;
			if (!FChunkDataLoader::Write(FPaths::Combine(ChunkDir, TEXT(".tmp.chunk")), Bytes, E.SHA1, StoredAs))
			{
				return E;
			}
			E.File = FChunkDataLoader::MakeChunkRelPath(E.SHA1);
			// Sharded parent dir (chunks/<h0><h1>/<h2><h3>) may not exist yet.
			PF.CreateDirectoryTree(*FPaths::GetPath(FPaths::Combine(Cdn, E.File)));
			PF.MoveFile(*(FPaths::Combine(Cdn, E.File)), *(FPaths::Combine(ChunkDir, TEXT(".tmp.chunk"))));
			E.Size = IFileManager::Get().FileSize(*(FPaths::Combine(Cdn, E.File)));
			return E;
		};

		FString NewChunkSha1, FreshChunkSha1; // content addresses of staged chunks

		// --- 1. Hand-crafted v1 -> v2: every classification -----------------
		{
			FIndexManifest V1, V2;
			V1.AppId = V2.AppId = TEXT("TestApp");
			V1.ContentVersion = TEXT("1.0.0");
			V2.ContentVersion = TEXT("2.0.0");

			V1.Files.Add(TEXT("keep.txt"), FIndexFileEntry{ TEXT("1111111111111111111111111111111111111111"), 10 });
			V1.Files.Add(TEXT("change.txt"), FIndexFileEntry{ TEXT("2222222222222222222222222222222222222222"), 20 });
			V1.Files.Add(TEXT("gone.txt"), FIndexFileEntry{ TEXT("3333333333333333333333333333333333333333"), 30 });
			V2.Files.Add(TEXT("keep.txt"), FIndexFileEntry{ TEXT("1111111111111111111111111111111111111111"), 10 });
			V2.Files.Add(TEXT("change.txt"), FIndexFileEntry{ TEXT("4444444444444444444444444444444444444444"), 25 });
			V2.Files.Add(TEXT("new.txt"), FIndexFileEntry{ TEXT("5555555555555555555555555555555555555555"), 50 });

			const FIndexChunkEntry KEEP = StageChunk(TEXT("1.0.0"), TEXT("keep-chunk-data-100-bytes-pad"));
			const FIndexChunkEntry OLD = StageChunk(TEXT("1.0.0"), TEXT("old-chunk"));
			const FIndexChunkEntry NEWC = StageChunk(TEXT("2.0.0"), TEXT("new-chunk-300"));
			NewChunkSha1 = NEWC.SHA1;
			V1.Chunks.Add(KEEP.SHA1, KEEP);
			V1.Chunks.Add(OLD.SHA1, OLD);
			V2.Chunks.Add(KEEP.SHA1, KEEP); // unchanged
			V2.Chunks.Add(NEWC.SHA1, NEWC);

			Check(FIndexManifestFile::Save(FPaths::Combine(Cdn, TEXT("index"), TEXT("1.0.0"), TEXT("UnrealGamePatcher.index.json")), V1), TEXT("save v1 index"));
			Check(FIndexManifestFile::Save(FPaths::Combine(Cdn, TEXT("index"), TEXT("2.0.0"), TEXT("UnrealGamePatcher.index.json")), V2), TEXT("save v2 index"));

			FDiffOutput Out;
			const FString DeltaDir = FPaths::Combine(WorkDir, TEXT("delta_v1_v2"));
			Check(FBuildDiffer::Diff(V1, V2, Cdn, DeltaDir, true, Out), TEXT("diff v1->v2 succeeds"));

			Check(Out.Summary.NumFilesSame == 1 && Out.Summary.NumFilesChanged == 1 &&
				Out.Summary.NumFilesAdded == 1 && Out.Summary.NumFilesRemoved == 1,
				TEXT("v1->v2 file summary: 1 same/changed/added/removed"));
			Check(Out.Summary.NumChunksSame == 1 && Out.Summary.NumChunksAdded == 1 &&
				Out.Summary.NumChunksRemoved == 1 && Out.Summary.NumChunksChanged == 0,
				TEXT("v1->v2 chunk summary: 1 same, 1 added, 1 removed"));
			Check(Out.Summary.DownloadBytes == NEWC.Size, TEXT("v1->v2 download bytes = new chunk only"));

			// Delta content: changed+added files at target state, removed list.
			Check(Out.Delta.Files.Num() == 2 &&
				Out.Delta.Files.Contains(TEXT("change.txt")) && Out.Delta.Files.Contains(TEXT("new.txt")) &&
				!Out.Delta.Files.Contains(TEXT("keep.txt")),
				TEXT("delta files = target state of changed+added"));
			Check(Out.Delta.RemovedFiles.Num() == 1 && Out.Delta.RemovedFiles[0] == TEXT("gone.txt"),
				TEXT("delta removedFiles = gone.txt"));
			Check(Out.Delta.Chunks.Num() == 1 && Out.Delta.Chunks.Contains(NewChunkSha1),
				TEXT("delta chunks = only what client must download"));
			Check(Out.Delta.BaseVersion == TEXT("1.0.0") && Out.Delta.TargetVersion == TEXT("2.0.0"),
				TEXT("delta version chain correct"));

			// Delta file roundtrips and the referenced chunk got staged.
			FDeltaManifest Loaded;
			const FString DeltaPath = FPaths::Combine(DeltaDir, TEXT("UnrealGamePatcher.delta.json"));
			Check(FDeltaManifestFile::Load(DeltaPath, Loaded), TEXT("delta manifest roundtrip + self-check"));
			Check(Loaded.Files.Num() == 2 && Loaded.RemovedFiles.Num() == 1 && Loaded.Chunks.Num() == 1 &&
				Loaded.Chunks[NewChunkSha1].File == FChunkDataLoader::MakeChunkRelPath(NewChunkSha1),
				TEXT("delta manifest fields roundtrip"));
			const FString StagedChunk = FPaths::Combine(DeltaDir, Loaded.Chunks[NewChunkSha1].File);
			Check(PF.FileExists(*StagedChunk), TEXT("referenced chunk staged next to delta"));
		}

		// --- 2. Empty diff: identical manifests -----------------------------
		{
			FIndexManifest A;
			A.AppId = TEXT("TestApp");
			A.ContentVersion = TEXT("2.0.0");
			A.Files.Add(TEXT("keep.txt"), FIndexFileEntry{ TEXT("1111111111111111111111111111111111111111"), 10 });
			A.Chunks.Add(TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
				FIndexChunkEntry{ TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 100, TEXT("chunks/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.chunk") });

			FDiffOutput Out;
			const FString DeltaDir = FPaths::Combine(WorkDir, TEXT("delta_empty"));
			Check(FBuildDiffer::Diff(A, A, Cdn, DeltaDir, true, Out), TEXT("diff identical manifests succeeds"));
			Check(Out.Summary.NumFilesSame == 1 && Out.Summary.NumFilesChanged == 0 &&
				Out.Summary.NumFilesAdded == 0 && Out.Summary.NumFilesRemoved == 0 &&
				Out.Summary.NumChunksSame == 1 && Out.Summary.NumChunksAdded == 0,
				TEXT("empty diff: everything Same, nothing to download"));
			Check(Out.Summary.DownloadBytes == 0 && Out.Delta.Files.Num() == 0 &&
				Out.Delta.Chunks.Num() == 0 && Out.Delta.RemovedFiles.Num() == 0,
				TEXT("empty diff: delta manifest is empty"));
			FDeltaManifest Loaded;
			Check(FDeltaManifestFile::Load(FPaths::Combine(DeltaDir, TEXT("UnrealGamePatcher.delta.json")), Loaded),
				TEXT("empty delta manifest loads"));
		}

		// --- 3. Version coexistence + skip-version diff v1 -> v3 ------------
		{
			// v2 already staged above (GUID_KEEP + GUID_NEW). Build v3 on top:
			// GUID_NEW survives, GUID_FRESH appears, GUID_KEEP disappears.
			FIndexManifest V3;
			V3.AppId = TEXT("TestApp");
			V3.ContentVersion = TEXT("3.0.0");
			V3.Files.Add(TEXT("keep.txt"), FIndexFileEntry{ TEXT("1111111111111111111111111111111111111111"), 10 });
			V3.Files.Add(TEXT("new.txt"), FIndexFileEntry{ TEXT("6666666666666666666666666666666666666666"), 60 }); // changed again in v3

			// Reuse v2's entries for GUID_KEEP/GUID_NEW where appropriate.
			FIndexManifest V2;
			Check(FIndexManifestFile::Load(FPaths::Combine(Cdn, TEXT("index"), TEXT("2.0.0"), TEXT("UnrealGamePatcher.index.json")), V2),
				TEXT("load 2.0.0 index for 3.0.0 crafting"));
			// Reuse v2's GUID_NEW entry as-is: chunks are GLOBAL and
			// content-addressed, so the same key/file is already on the CDN.
			const FIndexChunkEntry& V2New = V2.Chunks[NewChunkSha1];
			FIndexChunkEntry V3New = V2New;
			V3New.File = FChunkDataLoader::MakeChunkRelPath(NewChunkSha1);
			V3.Chunks.Add(NewChunkSha1, V3New); // unchanged from v2
			const FIndexChunkEntry FRESH = StageChunk(TEXT("3.0.0"), TEXT("fresh-chunk-400"));
			FreshChunkSha1 = FRESH.SHA1;
			V3.Chunks.Add(FreshChunkSha1, FRESH);

			Check(FIndexManifestFile::Save(FPaths::Combine(Cdn, TEXT("index"), TEXT("3.0.0"), TEXT("UnrealGamePatcher.index.json")), V3), TEXT("save v3 index"));

			// All three versions coexist in the CDN layout.
			Check(PF.FileExists(*FPaths::Combine(Cdn, TEXT("index"), TEXT("1.0.0"), TEXT("UnrealGamePatcher.index.json"))) &&
				PF.FileExists(*FPaths::Combine(Cdn, TEXT("index"), TEXT("2.0.0"), TEXT("UnrealGamePatcher.index.json"))) &&
				PF.FileExists(*FPaths::Combine(Cdn, TEXT("index"), TEXT("3.0.0"), TEXT("UnrealGamePatcher.index.json"))),
				TEXT("v1/v2/v3 indexes coexist in CDN layout"));

			// Skip-version diff: v1 -> v3 directly.
			FIndexManifest V1;
			FIndexManifestFile::Load(FPaths::Combine(Cdn, TEXT("index"), TEXT("1.0.0"), TEXT("UnrealGamePatcher.index.json")), V1);

			FDiffOutput Out;
			const FString DeltaDir = FPaths::Combine(WorkDir, TEXT("delta_v1_v3"));
			Check(FBuildDiffer::Diff(V1, V3, Cdn, DeltaDir, true, Out), TEXT("diff v1->v3 (skip version) succeeds"));

			// Files: keep.txt same; change.txt removed (not in v3); gone.txt removed;
			// new.txt added (its v3 hash differs from v1's absence).
			Check(Out.Summary.NumFilesSame == 1 && Out.Summary.NumFilesAdded == 1 &&
				Out.Summary.NumFilesRemoved == 2,
				TEXT("v1->v3 file summary correct"));
			// Chunks: GUID_KEEP removed, GUID_OLD removed, GUID_NEW + GUID_FRESH added.
			Check(Out.Summary.NumChunksSame == 0 && Out.Summary.NumChunksAdded == 2 &&
				Out.Summary.NumChunksRemoved == 2,
				TEXT("v1->v3 chunk summary correct"));
			Check(Out.Delta.Chunks.Num() == 2 && Out.Delta.Chunks.Contains(NewChunkSha1) &&
				Out.Delta.Chunks.Contains(FreshChunkSha1),
				TEXT("v1->v3 delta carries all target chunks"));
			Check(Out.Summary.DownloadBytes == V3.Chunks[NewChunkSha1].Size + FRESH.Size, TEXT("v1->v3 download bytes = new+fresh chunks"));

			// And the direct v1->v3 delta equals diffing v1->v2 then v2->v3
			// in coverage terms: chunk set union must match.
			FDiffOutput Out13Direct = Out;
			FDiffOutput Out12, Out23;
			Check(FBuildDiffer::Diff(V1, V2, Cdn, FPaths::Combine(WorkDir, TEXT("delta_v1_v2b")), true, Out12),
				TEXT("union check: v1->v2 diff succeeds"));
			Check(FBuildDiffer::Diff(V2, V3, Cdn, FPaths::Combine(WorkDir, TEXT("delta_v2_v3")), true, Out23),
				TEXT("union check: v2->v3 diff succeeds"));
			TSet<FString> ViaUnion;
			for (const TPair<FString, FIndexChunkEntry>& P : Out12.Delta.Chunks) { ViaUnion.Add(P.Key); }
			for (const TPair<FString, FIndexChunkEntry>& P : Out23.Delta.Chunks) { ViaUnion.Add(P.Key); }
			TSet<FString> Direct;
			for (const TPair<FString, FIndexChunkEntry>& P : Out13Direct.Delta.Chunks) { Direct.Add(P.Key); }
			Check(ViaUnion.Difference(Direct).Num() == 0 && Direct.Difference(ViaUnion).Num() == 0,
				TEXT("skip-version delta == union of step deltas (set equality)"));
		}

		// --- 4. Delta manifest tamper detection -----------------------------
		{
			const FString DeltaPath = FPaths::Combine(WorkDir, TEXT("delta_v1_v2"), TEXT("UnrealGamePatcher.delta.json"));
			FString Json;
			if (FFileHelper::LoadFileToString(Json, *DeltaPath))
			{
				// Flip one hex char of a chunk sha1 value.
				const FString ShaPrefix = NewChunkSha1.Left(6);
				const int32 At = Json.Find(*ShaPrefix, ESearchCase::CaseSensitive, ESearchDir::FromStart);
				Check(At != INDEX_NONE, TEXT("locate chunk sha1 in delta json"));
				if (At != INDEX_NONE)
				{
					FString Tampered = Json;
					Tampered[At] = Json[At] == TEXT('e') ? TEXT('f') : TEXT('e');
					FFileHelper::SaveStringToFile(Tampered, *DeltaPath);
					FDeltaManifest Bad;
					Check(!FDeltaManifestFile::Load(DeltaPath, Bad), TEXT("tampered delta manifest rejected"));
				}
			}
			else
			{
				Check(false, TEXT("read delta json for tamper test"));
			}
		}

		// --- 5. App mismatch refused ---------------------------------------
		{
			FIndexManifest A, B;
			A.AppId = TEXT("AppA"); A.ContentVersion = TEXT("1");
			B.AppId = TEXT("AppB"); B.ContentVersion = TEXT("2");
			FDiffOutput Out;
			Check(!FBuildDiffer::Diff(A, B, Cdn, FPaths::Combine(WorkDir, TEXT("delta_mismatch")), false, Out),
				TEXT("app mismatch refused"));
		}

		PF.DeleteDirectoryRecursively(*WorkDir);

		return FinishTest(bAllPassed, TEXT("diffbuild"));
	
	}

}
