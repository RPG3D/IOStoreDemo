// Copyright Epic Games, Inc. All Rights Reserved.
// -cli difftest —— 差异分析 + 本地真相提取 + 容器虚拟磁盘分类（差异层）
// 验证点：Added/Changed/Removed/Same 分类；安装目录真提取（含纯文件
//         整文件哈希）；pak/ucas 容器按虚拟磁盘模型逐条目比对。

#include "Tests/TestFramework.h"

namespace UnrealGamePatcher::Tests
{
	int32 RunDiffTest(const TCHAR* /*CmdLine*/)
	{

		// Local truth + diff analyzer acceptance (task 3.3): hand-crafted
		// manifests classify Same/Changed/Added/Removed correctly, the local
		// truth extractor parses a real pak, and the end-to-end diff over an
		// extracted local install matches the hand-crafted expectation.
		bool bAllPassed = true;
		const FString WorkDir = GetTestWorkDir(TEXT("difftest"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

		auto Check = MakeCheck(bAllPassed);

		using namespace UnrealGamePatcher;

		auto KindOf = [](const TArray<FDiffEntry>& Entries, const FString& Id) -> EChangeKind
		{
			for (const FDiffEntry& E : Entries)
			{
				if (E.Id == Id) { return E.Kind; }
			}
			return (EChangeKind)-1; // not found sentinel (casts back != any valid kind)
		};

		// --- 1. Hand-crafted manifests: all four classifications ----------
		{
			TMap<FString, FIndexFileEntry> Local, Remote;
			Local.Add(TEXT("same.txt"), FIndexFileEntry{ TEXT("1111111111111111111111111111111111111111"), 10 });
			Local.Add(TEXT("changed.txt"), FIndexFileEntry{ TEXT("2222222222222222222222222222222222222222"), 20 });
			Local.Add(TEXT("removed.txt"), FIndexFileEntry{ TEXT("3333333333333333333333333333333333333333"), 30 });
			Remote.Add(TEXT("same.txt"), FIndexFileEntry{ TEXT("1111111111111111111111111111111111111111"), 10 });
			Remote.Add(TEXT("changed.txt"), FIndexFileEntry{ TEXT("4444444444444444444444444444444444444444"), 25 });
			Remote.Add(TEXT("added.txt"), FIndexFileEntry{ TEXT("5555555555555555555555555555555555555555"), 50 });

			TArray<FDiffEntry> Diff;
			FDiffAnalyzer::DiffFiles(Local, Remote, Diff);

			Check(Diff.Num() == 4, TEXT("diff covers union of both sides"));
			Check(KindOf(Diff, TEXT("same.txt")) == EChangeKind::Same, TEXT("same id + same sha1 => Same"));
			Check(KindOf(Diff, TEXT("changed.txt")) == EChangeKind::Changed, TEXT("same id + different sha1 => Changed"));
			Check(KindOf(Diff, TEXT("added.txt")) == EChangeKind::Added, TEXT("remote-only => Added"));
			Check(KindOf(Diff, TEXT("removed.txt")) == EChangeKind::Removed, TEXT("local-only => Removed"));

			const FDiffEntry* Changed = nullptr;
			for (const FDiffEntry& E : Diff) { if (E.Id == TEXT("changed.txt")) { Changed = &E; } }
			Check(Changed && Changed->LocalSha1 == TEXT("2222222222222222222222222222222222222222") &&
				Changed->RemoteSha1 == TEXT("4444444444444444444444444444444444444444") &&
				Changed->LocalSize == 20 && Changed->RemoteSize == 25,
				TEXT("diff entry carries both sides' hash/size"));
		}

		// --- 2. Edge cases: identical, empty, single-sided ------------------
		{
			TMap<FString, FIndexFileEntry> A, B;
			A.Add(TEXT("x.bin"), FIndexFileEntry{ TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 7 });
			A.Add(TEXT("y.bin"), FIndexFileEntry{ TEXT("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"), 8 });
			B = A;

			TArray<FDiffEntry> Diff;
			FDiffAnalyzer::DiffFiles(A, B, Diff);
			Check(Diff.Num() == 2 && Diff[0].Kind == EChangeKind::Same && Diff[1].Kind == EChangeKind::Same,
				TEXT("identical manifests => all Same"));

			FIndexManifest LocalM, RemoteM;
			LocalM.AppId = RemoteM.AppId = TEXT("IOStoreDemo");
			LocalM.ContentVersion = RemoteM.ContentVersion = TEXT("1.0.0");

			FDiffResult EmptyDiff = FDiffAnalyzer::Diff(LocalM, RemoteM);
			Check(EmptyDiff.Files.Num() == 0 && EmptyDiff.Chunks.Num() == 0 &&
				EmptyDiff.SameCount() == 0 && EmptyDiff.ChangedCount() == 0,
				TEXT("two empty manifests => empty diff"));

			RemoteM.Files.Add(TEXT("new.bin"), FIndexFileEntry{ TEXT("cccccccccccccccccccccccccccccccccccccccc"), 9 });
			FDiffResult AllAdded = FDiffAnalyzer::Diff(LocalM, RemoteM);
			Check(AllAdded.Files.Num() == 1 && AllAdded.Files[0].Kind == EChangeKind::Added &&
				AllAdded.AddedCount() == 1 && AllAdded.SameCount() == 0,
				TEXT("empty local => everything Added"));

			FDiffResult AllRemoved = FDiffAnalyzer::Diff(RemoteM, LocalM);
			Check(AllRemoved.Files.Num() == 1 && AllRemoved.Files[0].Kind == EChangeKind::Removed &&
				AllRemoved.RemovedCount() == 1,
				TEXT("empty remote => everything Removed"));
		}

		// --- 3. Chunk-level diff (same id skip / new id / missing id) ------
		{
			TMap<FString, FIndexChunkEntry> Local, Remote;
			Local.Add(TEXT("AAAA...1"), FIndexChunkEntry{ TEXT("AAAA...1"), 100, TEXT("chunks/AAAA...1.chunk") });
			Local.Add(TEXT("BBBB...2"), FIndexChunkEntry{ TEXT("BBBB...2"), 200, TEXT("chunks/BBBB...2.chunk") });
			Remote.Add(TEXT("AAAA...1"), FIndexChunkEntry{ TEXT("AAAA...1"), 100, TEXT("chunks/AAAA...1.chunk") }); // same id+sha => skip
			Remote.Add(TEXT("CCCC...3"), FIndexChunkEntry{ TEXT("CCCC...3"), 300, TEXT("chunks/CCCC...3.chunk") });

			TArray<FDiffEntry> Diff;
			FDiffAnalyzer::DiffChunks(Local, Remote, Diff);
			Check(Diff.Num() == 3 &&
				KindOf(Diff, TEXT("AAAA...1")) == EChangeKind::Same &&
				KindOf(Diff, TEXT("BBBB...2")) == EChangeKind::Removed &&
				KindOf(Diff, TEXT("CCCC...3")) == EChangeKind::Added,
				TEXT("chunk diff: same id skipped, missing/added detected"));
		}

		// --- 4. Local truth extraction from a real pak ----------------------
		FIndexManifest LocalTruth;
		{
			// FPlatformMisc::ProjectDir() points into the engine's Program dir
			// when running as a Program, so locate the project root via the
			// executable: <Project>/Binaries/Win64/ -> three GetPath hops (the
			// first only strips the trailing slash BaseDir carries).
		const FString ProjectRoot = FPaths::GetPath(FPaths::GetPath(FPaths::GetPath(FPlatformProcess::BaseDir())));
			const FString SrcPak = FPaths::Combine(ProjectRoot, TEXT(".manifesttest"), TEXT("chunkinstall"),
				TEXT("Windows"), TEXT("1.0.0"), TEXT("pakchunk1"), TEXT("pakchunk1-Windows.pak"));
			Check(PF.FileExists(*SrcPak), TEXT("source pak from task 3.1 exists"));

			const FString PakDir = FPaths::Combine(WorkDir, TEXT("paks"));
			PF.CreateDirectory(*PakDir);
			const FString DstPak = FPaths::Combine(PakDir, TEXT("pakchunk1-Windows.pak"));
			Check(IFileManager::Get().Copy(*DstPak, *SrcPak) == ECopyResult::COPY_OK, TEXT("copy pak into test dir"));

			// A plain sidecar file next to the pak: whole-file hash path.
			const FString PlainPath = FPaths::Combine(PakDir, TEXT("plainfile.txt"));
			Check(FFileHelper::SaveStringToFile(TEXT("plain payload for whole-file hashing"), *PlainPath), TEXT("write plain sidecar file"));

			const int32 ParsedPaks = FLocalTruthExtractor::ExtractPaks(PakDir, LocalTruth);
			Check(ParsedPaks == 1, TEXT("extract: one pak parsed"));
			Check(LocalTruth.Files.Num() >= 1, TEXT("extract: pak entries present"));

			// The pak's entries must carry a 40-hex SHA1 and a positive size.
			bool bAllWellFormed = LocalTruth.Files.Num() > 0;
			for (const TPair<FString, FIndexFileEntry>& Pair : LocalTruth.Files)
			{
				if (Pair.Key == TEXT("plainfile.txt")) { continue; } // sidecar, checked below
				if (Pair.Value.SHA1.Len() != 40 || Pair.Value.Size <= 0)
				{
					bAllWellFormed = false;
					break;
				}
			}
			Check(bAllWellFormed, TEXT("extract: every pak entry has 40-hex sha1 + size"));

			// Verify one entry's hash independently: re-open the pak, dump its
			// first file's data through FPakReader and hash the payload header.
			{
				FPakReader Reader;
				Check(Reader.Open(DstPak) && Reader.ReadIndex(), TEXT("re-open pak for hash cross-check"));
				if (Reader.GetFiles().Num() > 0)
				{
					const FPakReader::FPakEntryLocation& Loc = Reader.GetFiles()[0];
					uint8 Hash[20] = { 0 };
					if (Reader.ReadPayloadHash(Loc.Entry, Hash))
					{
						FString Hex;
						for (int32 I = 0; I < 20; ++I) { Hex += FString::Printf(TEXT("%02x"), Hash[I]); }
						const bool bMatch = LocalTruth.Files.Contains(Loc.Path) &&
							LocalTruth.Files[Loc.Path].SHA1 == Hex;
						Check(bMatch, TEXT("extract: entry sha1 matches independent payload hash"));
					}
					else
					{
						Check(false, TEXT("extract: entry sha1 matches independent payload hash"));
					}
				}
			}

			// Whole-file truth via ExtractAll (pak + plain sidecar together).
			FIndexManifest AllTruth;
			Check(FLocalTruthExtractor::ExtractAll(PakDir, AllTruth), TEXT("ExtractAll succeeds"));
			// Cross-checked against an external sha1sum of the same 36 bytes.
			int32 PlainFound = AllTruth.Files.FilterByPredicate([](const TPair<FString, FIndexFileEntry>& P)
			{
				return P.Key == TEXT("plainfile.txt") && P.Value.Size == 36 &&
					P.Value.SHA1 == TEXT("24bfd809541cadbd27d0f1d54b31d3bdaa41390c");
			}).Num();
			Check(PlainFound == 1, TEXT("ExtractAll: plain file whole-file hashed"));
			int32 PakEntries = 0;
			for (const TPair<FString, FIndexFileEntry>& P : AllTruth.Files)
			{
				if (P.Key != TEXT("plainfile.txt") && P.Key != TEXT("pakchunk1-Windows.pak")) { ++PakEntries; }
			}
			Check(PakEntries == LocalTruth.Files.Num(), TEXT("ExtractAll: pak entries preserved"));

			// The pak file itself is also a whole-file entry.
			const FIndexFileEntry* PakSelf = AllTruth.Files.Find(TEXT("pakchunk1-Windows.pak"));
			Check(PakSelf && PakSelf->Size > 0 && PakSelf->SHA1.Len() == 40,
				TEXT("ExtractAll: the pak itself whole-file hashed"));

			// Missing directory fails cleanly.
			Check(FLocalTruthExtractor::ExtractAll(FPaths::Combine(WorkDir, TEXT("nope")), AllTruth) == false,
				TEXT("extract: missing directory fails"));
		}

		// --- 5. End-to-end: local truth vs a hand-crafted remote ------------
		{
			FIndexManifest Remote;
			Remote.AppId = TEXT("IOStoreDemo");
			Remote.ContentVersion = TEXT("1.0.1");

			// Pick two real local entries to drive Same / Changed; every other
			// local entry (if any) becomes Removed, zz_remote_only.txt is Added.
			TArray<FString> LocalIds;
			LocalTruth.Files.GetKeys(LocalIds);
			LocalIds.Sort();
			Check(LocalIds.Num() >= 2, TEXT("end-to-end: local truth has >= 2 entries"));

			if (LocalIds.Num() >= 2)
			{
				const FString& SameId = LocalIds[0];
				const FString& ChangedId = LocalIds[1];

				Remote.Files.Add(SameId, LocalTruth.Files[SameId]); // identical => Same

				FIndexFileEntry ChangedEntry = LocalTruth.Files[ChangedId];
				ChangedEntry.SHA1 = ChangedEntry.SHA1 == TEXT("0000000000000000000000000000000000000000")
					? TEXT("1111111111111111111111111111111111111111")
					: TEXT("0000000000000000000000000000000000000000"); // guaranteed different
				ChangedEntry.Size += 1;
				Remote.Files.Add(ChangedId, ChangedEntry); // different hash => Changed

				Remote.Files.Add(TEXT("zz_remote_only.txt"),
					FIndexFileEntry{ TEXT("9999999999999999999999999999999999999999"), 123 }); // Added

				const FDiffResult Result = FDiffAnalyzer::Diff(LocalTruth, Remote);

				Check(KindOf(Result.Files, SameId) == EChangeKind::Same, TEXT("end-to-end: unchanged file skipped"));
				Check(KindOf(Result.Files, ChangedId) == EChangeKind::Changed, TEXT("end-to-end: changed file detected"));
				Check(KindOf(Result.Files, TEXT("zz_remote_only.txt")) == EChangeKind::Added,
					TEXT("end-to-end: remote-only file detected Added"));

				// Conservation: every entry on both sides is classified exactly once.
				Check(Result.SameCount() + Result.ChangedCount() + Result.AddedCount() + Result.RemovedCount()
					== Result.Files.Num() + Result.Chunks.Num(),
					TEXT("end-to-end: every entry classified exactly once"));
				Check(Result.SameCount() >= 1 && Result.AddedCount() >= 1 && Result.ChangedCount() == 1,
					TEXT("end-to-end: summary counts consistent"));

				// Symmetry: diffing the other way round flips Added/Removed for
				// the remote-only item (and Same/Changed stay put).
				const FDiffResult Reverse = FDiffAnalyzer::Diff(Remote, LocalTruth);
				Check(KindOf(Reverse.Files, TEXT("zz_remote_only.txt")) == EChangeKind::Removed,
					TEXT("end-to-end: reverse diff flips Added to Removed"));
				Check(KindOf(Reverse.Files, SameId) == EChangeKind::Same &&
					KindOf(Reverse.Files, ChangedId) == EChangeKind::Changed,
					TEXT("end-to-end: reverse diff keeps Same/Changed"));
			}
		}

		// --- 6. Container virtual-disk classification (inner hashes) --------
		{
			using namespace UnrealGamePatcher;

			// Two paks with the SAME entries in DIFFERENT physical order must
			// classify Same (the container is a virtual disk - the whole-file
			// hash differs but the entries are the truth).
			FIndexFileEntry PakA, PakB;
			PakA.SHA1 = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
			PakB.SHA1 = TEXT("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
			PakA.Size = PakB.Size = 1000;
			{
				FIndexPakEntry E1, E2;
				E1.Path = TEXT("/data.bin"); E1.SHA1 = TEXT("1111111111111111111111111111111111111111");
				E2.Path = TEXT("/meta.bin"); E2.SHA1 = TEXT("2222222222222222222222222222222222222222");
				PakA.PakEntries = { E1, E2 };
				PakB.PakEntries = { E2, E1 }; // same entries, REVERSED order
			}
			{
				FIndexManifest L, R;
				L.Files.Add(TEXT("game.pak"), PakA);
				R.Files.Add(TEXT("game.pak"), PakB);
				TArray<FDiffEntry> Out;
				FDiffAnalyzer::DiffFilesVirtualDisk(L.Files, R.Files, Out);
				Check(Out.Num() == 1 && Out[0].Kind == EChangeKind::Same,
					TEXT("vd: same pak entries in different order are Same (whole-file hash ignored)"));
			}
			// A changed entry flips the container to Changed.
			PakB.PakEntries[1].SHA1 = TEXT("3333333333333333333333333333333333333333");
			{
				FIndexManifest L, R;
				L.Files.Add(TEXT("game.pak"), PakA);
				R.Files.Add(TEXT("game.pak"), PakB);
				TArray<FDiffEntry> Out;
				FDiffAnalyzer::DiffFilesVirtualDisk(L.Files, R.Files, Out);
				Check(Out.Num() == 1 && Out[0].Kind == EChangeKind::Changed,
					TEXT("vd: a changed pak entry flips the container to Changed"));
			}
			// C2 regression: a REMOVED pak entry (local-only leftover) must
			// flip the container to Changed - the old remote-subset-only
			// comparison classified it Same and the stale asset stayed
			// mounted forever (no update/check/repair converged it).
			{
				FIndexFileEntry PakTarget = PakA;
				PakTarget.PakEntries.SetNum(1); // remote has only the first entry
				FIndexManifest L, R;
				L.Files.Add(TEXT("game.pak"), PakA);
				R.Files.Add(TEXT("game.pak"), PakTarget);
				TArray<FDiffEntry> Out;
				FDiffAnalyzer::DiffFilesVirtualDisk(L.Files, R.Files, Out);
				Check(Out.Num() == 1 && Out[0].Kind == EChangeKind::Changed,
					TEXT("vd: a REMOVED pak entry flips the container to Changed (C2)"));
			}

			// Two ucas with the SAME chunk-id multiset in different order -> Same.
			FIndexFileEntry UcasA, UcasB;
			UcasA.SHA1 = TEXT("cccccccccccccccccccccccccccccccccccccccc");
			UcasB.SHA1 = TEXT("dddddddddddddddddddddddddddddddddddddddd");
			UcasA.Size = UcasB.Size = 5000;
			{
				FIndexIoStoreChunk C1, C2, C3;
				C1.Id = TEXT("aaaaaaaaaaaa01"); C2.Id = TEXT("bbbbbbbbbbbb01"); C3.Id = TEXT("cccccccccccc01");
				UcasA.IoStoreChunks = { C1, C2, C3 };
				UcasB.IoStoreChunks = { C3, C1, C2 }; // same set, different order
			}
			{
				FIndexManifest L, R;
				L.Files.Add(TEXT("game.ucas"), UcasA);
				R.Files.Add(TEXT("game.ucas"), UcasB);
				TArray<FDiffEntry> Out;
				FDiffAnalyzer::DiffFilesVirtualDisk(L.Files, R.Files, Out);
				Check(Out.Num() == 1 && Out[0].Kind == EChangeKind::Same,
					TEXT("vd: same ucas chunk-id set in different order is Same"));
			}
			// A different chunk id flips it to Changed.
			UcasB.IoStoreChunks[0].Id = TEXT("dddddddddddd01");
			{
				FIndexManifest L, R;
				L.Files.Add(TEXT("game.ucas"), UcasA);
				R.Files.Add(TEXT("game.ucas"), UcasB);
				TArray<FDiffEntry> Out;
				FDiffAnalyzer::DiffFilesVirtualDisk(L.Files, R.Files, Out);
				Check(Out.Num() == 1 && Out[0].Kind == EChangeKind::Changed,
					TEXT("vd: a different ucas chunk id flips the container to Changed"));
			}

			// C1 regression: engine chunk ids are PACKAGE-derived (FPackageId::
			// FromName), so a content edit inside an existing package keeps its
			// id while the utoc's meta hash changes. The (Id, MetaHash)
			// comparison must detect it - the id-only comparison classified it
			// Same and the update was NEVER delivered.
			{
				FIndexFileEntry UcasL = UcasA, UcasR = UcasA; // same ids, same order
				for (FIndexIoStoreChunk& C : UcasL.IoStoreChunks) { C.MetaHash = TEXT("1111111111111111111111111111111111111111"); }
				for (FIndexIoStoreChunk& C : UcasR.IoStoreChunks) { C.MetaHash = TEXT("1111111111111111111111111111111111111111"); }
				UcasR.IoStoreChunks[1].MetaHash = TEXT("2222222222222222222222222222222222222222"); // content edited, id unchanged
				{
					FIndexManifest L, R;
					L.Files.Add(TEXT("game.ucas"), UcasL);
					R.Files.Add(TEXT("game.ucas"), UcasR);
					TArray<FDiffEntry> Out;
					FDiffAnalyzer::DiffFilesVirtualDisk(L.Files, R.Files, Out);
					Check(Out.Num() == 1 && Out[0].Kind == EChangeKind::Changed,
						TEXT("vd: same ids + changed meta hash DETECT the content edit (C1)"));
				}
				// Legacy manifests (no meta hashes recorded) keep the id-only
				// behavior - the mixed comparison would force one-time rebuilds.
				{
					FIndexManifest L, R;
					L.Files.Add(TEXT("game.ucas"), UcasA);
					R.Files.Add(TEXT("game.ucas"), UcasA);
					TArray<FDiffEntry> Out;
					FDiffAnalyzer::DiffFilesVirtualDisk(L.Files, R.Files, Out);
					Check(Out.Num() == 1 && Out[0].Kind == EChangeKind::Same,
						TEXT("vd: manifests without meta hashes keep id-only semantics"));
				}
			}

			// The .utoc mirrors its sibling .ucas (container metadata).
			FIndexFileEntry UtocA, UtocB;
			UtocA.SHA1 = TEXT("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
			UtocB.SHA1 = TEXT("ffffffffffffffffffffffffffffffffffffffff");
			UtocA.Size = UtocB.Size = 100;
			{
				FIndexManifest L, R;
				L.Files.Add(TEXT("game.ucas"), UcasA); // original (intact) ucas
				R.Files.Add(TEXT("game.ucas"), UcasB); // changed chunk id -> ucas Changed
				L.Files.Add(TEXT("game.utoc"), UtocA);
				R.Files.Add(TEXT("game.utoc"), UtocB);
				TArray<FDiffEntry> Out;
				FDiffAnalyzer::DiffFilesVirtualDisk(L.Files, R.Files, Out);
				Check(KindOf(Out, TEXT("game.ucas")) == EChangeKind::Changed &&
					KindOf(Out, TEXT("game.utoc")) == EChangeKind::Changed,
					TEXT("vd: utoc mirrors a Changed ucas"));
			}
			// When the ucas is Same, the utoc is Same too (its own bytes may
			// legitimately differ after an in-place rewrite).
			{
				FIndexManifest L, R;
				L.Files.Add(TEXT("game.ucas"), UcasA);
				R.Files.Add(TEXT("game.ucas"), UcasA);
				L.Files.Add(TEXT("game.utoc"), UtocA);
				R.Files.Add(TEXT("game.utoc"), UtocB); // different bytes, same semantics
				TArray<FDiffEntry> Out;
				FDiffAnalyzer::DiffFilesVirtualDisk(L.Files, R.Files, Out);
				Check(KindOf(Out, TEXT("game.ucas")) == EChangeKind::Same &&
					KindOf(Out, TEXT("game.utoc")) == EChangeKind::Same,
					TEXT("vd: utoc mirrors a Same ucas (byte layout ignored)"));
			}
		}

		// Clean up the work dir.
		PF.DeleteDirectoryRecursively(*WorkDir);

		return FinishTest(bAllPassed, TEXT("difftest"));
	
	}

}
