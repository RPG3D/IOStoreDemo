// Copyright Epic Games, Inc. All Rights Reserved.
// -cli updatetest —— 客户端更新主流程（更新管线）
// 验证点：空目录全新装（全量 fetch）；复跑 no-op（信任模式零读取）；
//         §1b 安装锁互斥（L-M5 修复钉）；check-only 只读纯度；
//         删除文件清理；损坏文件修复收敛；跨版本 chunk 复用
//         （纯复用 + 部分复用）。

#include "Tests/TestFramework.h"

namespace UnrealGamePatcher::Tests
{
	int32 RunUpdateTest(const TCHAR* /*CmdLine*/)
	{

		// Client update acceptance (task 5.1): full fetch on empty install,
		// pak integrity cross-check, no-op update, check-only purity, removed
		// file deletion, damaged-file repair, and cross-version chunk reuse
		// (pure reuse for an added file, partial reuse + fetch for a changed
		// file). Works in Saved/updatetest and leaves no artifacts.
		bool bAllPassed = true;
		auto Check = MakeCheck(bAllPassed);

		using namespace UnrealGamePatcher;

		const FString WorkDir = GetTestWorkDir(TEXT("updatetest"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

		const FString Cdn = FPaths::Combine(WorkDir, TEXT("cdn"));
		const FString Install = FPaths::Combine(WorkDir, TEXT("install"));
		PF.CreateDirectoryTree(*Install);

		FIndexManifest Index;
		Check(BuildFixtureCdn(WorkDir, Cdn, Index), TEXT("updatetest: ingest staged fixture"));
		const FString Version = Index.ContentVersion;
		Check(Version == TEXT("1.0.0"), TEXT("index version is 1.0.0"));

		// --- 1. Full update on an empty install ----------------------------
		FUpdateStats S1;
		Check(FUpdateOrchestrator::Update(Cdn, Version, Install, true, S1), TEXT("full update on empty install succeeds"));
		Check(S1.NumFilesAdded == 4 && S1.NumFilesSame == 0, TEXT("full update: 4 added, 0 same"));
		Check(S1.NumChunksFetched == Index.Chunks.Num(), TEXT("full update: every chunk fetched (no local state)"));
		Check(S1.NumChunksReused == 0, TEXT("full update: nothing reused"));
		Check(PF.FileExists(*(FPaths::Combine(Install, TEXT(".ugp"), TEXT("index.json")))), TEXT("local state file written"));

		// --- 1b. Install lock (fixplan L-M5): one writer per install dir ----
		{
			Paths::FInstallLock Lock;
			Check(Lock.Acquire(Install), TEXT("install lock: acquire succeeds"));
			{
				Paths::FInstallLock Second;
				Check(!Second.Acquire(Install), TEXT("install lock: second acquire on same dir fails"));
				const FString Other = FPaths::Combine(WorkDir, TEXT("install-other"));
				Check(Second.Acquire(Other), TEXT("install lock: different dir acquires"));
				Second.Release();
			}
			Lock.Release();
			Check(Lock.Acquire(Install), TEXT("install lock: re-acquire after release succeeds"));
			Lock.Release();
		}



		// Every installed file is hash-identical to the manifest.
		{
			bool bAllMatch = true;
			int64 ExpectedBytes = 0;
			for (const TPair<FString, FIndexFileEntry>& Pair : Index.Files)
			{
				FString Sha1;
				int64 Size = 0;
				ExpectedBytes += Pair.Value.Size;
				if (!FLocalTruthExtractor::HashWholeFile(FPaths::Combine(Install, Pair.Key), Sha1, Size) ||
					!Sha1.Equals(Pair.Value.SHA1, ESearchCase::IgnoreCase) || Size != Pair.Value.Size)
				{
					bAllMatch = false;
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("updatetest: '%s' hash/size mismatch (disk %s/%lld, manifest %s/%lld)"),
						*Pair.Key, *Sha1, Size, *Pair.Value.SHA1, Pair.Value.Size);
				}
			}
			Check(bAllMatch, TEXT("all installed files hash-identical to the remote manifest"));
			Check(S1.BytesWritten == ExpectedBytes, TEXT("full update wrote the full remote byte count"));
		}

		// --- 2. Updated pak is logically equivalent (repak cross-check) ----
		{
			FPakReader Reader;
			const FString PakPath = FPaths::Combine(Install, TEXT("pakchunk1-Windows.pak"));
			const bool bPakOk = Reader.Open(PakPath) && Reader.ReadIndex();
			Check(bPakOk, TEXT("updated pak opens and indexes"));
			bool bEntriesOk = bPakOk && Reader.GetFiles().Num() == 2;
			if (bPakOk)
			{
				for (const FPakReader::FPakEntryLocation& Loc : Reader.GetFiles())
				{
					uint8 Stored[20] = { 0 }, Computed[20] = { 0 };
					const bool bStored = Reader.ReadPayloadHash(Loc.Entry, Stored);
					if (!Reader.ComputePayloadHash(Loc.Entry, Computed))
					{
						bEntriesOk = false;
						break;
					}
					if (bStored && FMemory::Memcmp(Stored, Computed, 20) != 0)
					{
						bEntriesOk = false;
						break;
					}
				}
			}
			Check(bEntriesOk, TEXT("updated pak: built-in SHA1 matches payload for every entry"));
		}

		// --- 3. No-op update -----------------------------------------------
		FUpdateStats S3;
		Check(FUpdateOrchestrator::Update(Cdn, Version, Install, true, S3), TEXT("no-op update succeeds"));
		Check(S3.NumFilesSame == 4 && S3.NumFilesChanged == 0 && S3.NumFilesAdded == 0 && S3.NumFilesRemoved == 0,
			TEXT("no-op update: all files same"));
		Check(S3.NumChunksFetched == 0 && S3.NumChunksReused == 0 && S3.BytesWritten == 0,
			TEXT("no-op update: zero fetches, zero writes"));

		// --- 4. Check-only mode writes nothing -----------------------------
		{
			FString StateBefore;
			FFileHelper::LoadFileToString(StateBefore, *(FPaths::Combine(Install, TEXT(".ugp"), TEXT("index.json"))));

			FString Stray = FPaths::Combine(Install, TEXT("stray.txt"));
			FFileHelper::SaveStringToFile(TEXT("not part of the CDN"), *Stray);

			FUpdateStats S4;
			Check(FUpdateOrchestrator::Update(Cdn, Version, Install, false, S4), TEXT("check-only update succeeds"));
			Check(S4.NumFilesSame == 4 && S4.NumFilesRemoved == 1, TEXT("check-only reports the stray file as removed"));
			Check(S4.BytesWritten == 0 && S4.NumChunksFetched == 0, TEXT("check-only: zero fetches, zero writes"));
			Check(PF.FileExists(*Stray), TEXT("check-only leaves the stray file on disk"));

			FString StateAfter;
			FFileHelper::LoadFileToString(StateAfter, *(FPaths::Combine(Install, TEXT(".ugp"), TEXT("index.json"))));
			Check(StateBefore == StateAfter, TEXT("check-only leaves the local state untouched"));
			PF.DeleteFile(*Stray);
		}

		// --- 5. Removed files are deleted ----------------------------------
		{
			const FString Stray = FPaths::Combine(Install, TEXT("stray.txt"));
			FFileHelper::SaveStringToFile(TEXT("not part of the CDN"), *Stray);
			FUpdateStats S5;
			Check(FUpdateOrchestrator::Update(Cdn, Version, Install, true, S5), TEXT("apply update with stray file succeeds"));
			Check(S5.NumFilesRemoved == 1, TEXT("apply: stray file classified removed"));
			Check(!PF.FileExists(*Stray), TEXT("apply: stray file deleted"));
			Check(PF.FileExists(*(FPaths::Combine(Install, TEXT(".ugp"), TEXT("index.json")))), TEXT("state survives the update"));
		}

		// --- 5b. Locally missing file is re-fetched by a trust update -------
		{
			// Trust mode diffs manifests (no disk content reads) but still
			// heals NAME drift: a deleted file is re-fetched whole.
			const FString Victim = FPaths::Combine(Install, TEXT("boot.txt"));
			Check(PF.DeleteFile(*Victim), TEXT("heal: boot.txt deleted locally"));

			FUpdateStats SHeal;
			Check(FUpdateOrchestrator::Update(Cdn, Version, Install, true, SHeal), TEXT("heal: trust update succeeds"));
			Check(SHeal.NumFilesAdded == 1 && SHeal.NumChunksFetched >= 1,
				TEXT("heal: missing file re-fetched"));
			{
				// Restored file matches the remote manifest hash/size.
				const FIndexFileEntry& Readme = Index.Files[TEXT("boot.txt")];
				FString Sha1;
				int64 Size = 0;
				const bool bRestored = FLocalTruthExtractor::HashWholeFile(Victim, Sha1, Size) &&
					Size == Readme.Size && Sha1.Equals(Readme.SHA1, ESearchCase::IgnoreCase);
				Check(bRestored, TEXT("heal: restored file matches the manifest"));
			}
		}

		// --- 5c. Corrupt sidecar self-heals (full-extraction fallback) -----
		{
			// The sidecar is load-bearing for trust mode; losing it must
			// degrade to the full-extraction fallback (containers parsed,
			// plain files hashed - local work only, zero network), then be
			// rewritten at commit.
			const FString SidecarPath = FPaths::Combine(Install, TEXT(".ugp"), TEXT("index.json"));
			FFileHelper::SaveStringToFile(TEXT("{{{ definitely not json"), *SidecarPath);

			FUpdateStats SCorrupt;
			Check(FUpdateOrchestrator::Update(Cdn, Version, Install, true, SCorrupt), TEXT("sidecar-corrupt: trust update succeeds"));
			Check(SCorrupt.NumChunksFetched == 0 && SCorrupt.NumFilesSame == 4,
				TEXT("sidecar-corrupt: fallback extraction classifies all-Same with zero fetches"));
			FIndexManifest Reloaded;
			Check(FIndexManifestFile::Load(SidecarPath, Reloaded), TEXT("sidecar-corrupt: sidecar rewritten and loadable"));
			Check(Reloaded.ContentVersion == Version, TEXT("sidecar-corrupt: rewritten sidecar points at the installed version"));
		}

		// --- 5d. Plain-file size drift is re-fetched by trust mode ---------
		{
			const FString BootPath = FPaths::Combine(Install, TEXT("boot.txt"));
			const FIndexFileEntry& Boot = Index.Files[TEXT("boot.txt")];
			FFileHelper::SaveStringToFile(TEXT("shorter"), *BootPath); // size drift
			Check(IFileManager::Get().FileSize(*BootPath) != Boot.Size, TEXT("size-drift: boot.txt size drifted"));

			FUpdateStats SDrift;
			Check(FUpdateOrchestrator::Update(Cdn, Version, Install, true, SDrift), TEXT("size-drift: trust update succeeds"));
			// The CLI stats split Added/Changed by on-disk existence: boot.txt
			// still exists (drifted), so the re-fetch counts as Changed.
			Check(SDrift.NumFilesAdded + SDrift.NumFilesChanged == 1 && SDrift.NumChunksFetched >= 1,
				TEXT("size-drift: drifted plain file re-fetched"));
			{
				FString Sha1;
				int64 Size = 0;
				const bool bRestored = FLocalTruthExtractor::HashWholeFile(BootPath, Sha1, Size) &&
					Size == Boot.Size && Sha1.Equals(Boot.SHA1, ESearchCase::IgnoreCase);
				Check(bRestored, TEXT("size-drift: restored file matches the manifest"));
			}
		}

		// --- 5e. Missing container is rebuilt by trust mode ----------------
		{
			const FString UcasPath = FPaths::Combine(Install, TEXT("pakchunk1-Windows.ucas"));
			const FString UtocPath = FPaths::Combine(Install, TEXT("pakchunk1-Windows.utoc"));
			Check(PF.DeleteFile(*UcasPath), TEXT("container-missing: ucas deleted"));

			FUpdateStats SMissing;
			Check(FUpdateOrchestrator::Update(Cdn, Version, Install, true, SMissing), TEXT("container-missing: trust update succeeds"));
			Check(SMissing.NumFilesAdded == 1 && SMissing.NumChunksFetched == Index.Files[TEXT("pakchunk1-Windows.ucas")].Parts.Num(),
				TEXT("container-missing: whole ucas re-fetched from its parts"));
			Check(IFileManager::Get().FileSize(*UcasPath) == Index.Files[TEXT("pakchunk1-Windows.ucas")].Size,
				TEXT("container-missing: rebuilt ucas has the full size"));

			// The rebuilt container must be logically intact (entry hashes).
			bool bLogicallyEqual = false;
			{
				FIndexManifest Truth;
				if (FLocalTruthExtractor::ExtractInstall(Install, Truth))
				{
					FDiffResult CheckDiff;
					FDiffAnalyzer::DiffFilesVirtualDisk(Truth.Files, Index.Files, CheckDiff.Files);
					bLogicallyEqual = true;
					for (const FDiffEntry& E : CheckDiff.Files)
					{
						if (E.Id.EndsWith(TEXT(".pak")) || E.Id.EndsWith(TEXT(".ucas")))
						{
							bLogicallyEqual &= (E.Kind == EChangeKind::Same);
						}
					}
				}
			}
			Check(bLogicallyEqual, TEXT("container-missing: rebuilt containers are logically equal to the manifest"));
			(void)UtocPath;
		}

		// --- 5f. VerifyEveryDays forces a verify run ------------------------
		{
			// Backdate LastVerifiedAt past the interval, then update with
			// VerifyEveryDays=1: the time trigger must flip this trust update
			// into verify mode and refresh the timestamp.
			const FString StatePath = UnrealGamePatcher::Paths::GetStatePath(Install);
			FPatcherState Stale;
			Check(FPatcherStateFile::Load(StatePath, Stale), TEXT("verifydays: state loads"));
			Stale.LastVerifiedAt = TEXT("2020-01-01T00:00:00.000Z");
			Check(FPatcherStateFile::Save(StatePath, Stale), TEXT("verifydays: stale timestamp written"));

			FUpdateOptions Options;
			Options.VerifyEveryDays = 1;
			FUpdateStats SDays;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, Version, Install, true, Options, SDays),
				TEXT("verifydays: update with a stale verification succeeds"));

			FPatcherState Fresh;
			Check(FPatcherStateFile::Load(StatePath, Fresh), TEXT("verifydays: state reloads"));
			Check(Fresh.LastVerifiedAt.StartsWith(TEXT("2026-")) || Fresh.LastVerifiedAt > Stale.LastVerifiedAt,
				TEXT("verifydays: LastVerifiedAt refreshed by the verify run"));
		}

		// --- 6. Damaged file is repaired -----------------------------------
		{
			const FString UcasPath = FPaths::Combine(Install, TEXT("pakchunk1-Windows.ucas"));
			{
				// IPlatformFile has no read-write handles; corrupt via
				// load/flip/save (size is preserved).
				TArray64<uint8> Bytes;
				if (FFileHelper::LoadFileToArray(Bytes, *UcasPath))
				{
					Bytes[Bytes.Num() / 2] ^= 0xFF;
					FFileHelper::SaveArrayToFile(Bytes, *UcasPath);
				}
			}

			// Damage detection lives in VERIFY/repair mode (trust mode accepts a
			// same-size corruption - the periodic verify / manual repair catches
			// it). The damaged ucas AND its sibling utoc are rebuilt.
			TSet<FString> RepairChunks;
			for (const FIndexFileChunkRef& Part : Index.Files[TEXT("pakchunk1-Windows.ucas")].Parts)
			{
				RepairChunks.Add(Part.Chunk);
			}
			for (const FIndexFileChunkRef& Part : Index.Files[TEXT("pakchunk1-Windows.utoc")].Parts)
			{
				RepairChunks.Add(Part.Chunk);
			}

			FUpdateStats S6;
			Check(FUpdateOrchestrator::Update(Cdn, Version, Install, true, S6, /*bRepairOnly=*/true),
				TEXT("damaged-file repair succeeds"));
			Check(S6.NumFilesChanged == 2 && S6.NumFilesSame == 2,
				TEXT("repair: damaged ucas + sibling utoc rebuilt, pak same"));

			FString Sha1;
			int64 Size = 0;
			FLocalTruthExtractor::HashWholeFile(UcasPath, Sha1, Size);
			Check(Sha1.Equals(Index.Files[TEXT("pakchunk1-Windows.ucas")].SHA1, ESearchCase::IgnoreCase),
				TEXT("repair: ucas hash restored to the manifest value"));
			Check(S6.NumChunksFetched == RepairChunks.Num(), TEXT("repair: fetched exactly the rebuilt files' chunks"));
			Check(S6.BytesWritten == Index.Files[TEXT("pakchunk1-Windows.ucas")].Size + Index.Files[TEXT("pakchunk1-Windows.utoc")].Size,
				TEXT("repair: wrote exactly the rebuilt files' sizes"));
		}

		// --- 7. Added file built purely from reused local chunks -----------
		// Craft version 1.0.1: same content plus a new readme.txt assembled
		// from the utoc chunk slice (identical part -> deterministic reuse).
		FString ReadmeGuid;
		int64 ReadmeOffset = 0, ReadmeSize = 0;
		TArray64<uint8> ReadmeExpected;
		{
			const FIndexFileEntry& Utoc = Index.Files[TEXT("pakchunk1-Windows.utoc")];
			const FIndexFileChunkRef& Part = Utoc.Parts[0];
			ReadmeGuid = Part.Chunk;
			ReadmeOffset = Part.Offset;
			ReadmeSize = Part.Size;

			const FIndexChunkEntry& Chunk = Index.Chunks[ReadmeGuid];
			TArray64<uint8> ChunkData;
			if (!FChunkDataLoader::Load(FPaths::Combine(Cdn, Chunk.File), Chunk, ChunkData))
			{
				return ExitFailure;
			}
			ReadmeExpected.SetNumUninitialized(ReadmeSize);
			FMemory::Memcpy(ReadmeExpected.GetData(), ChunkData.GetData() + ReadmeOffset, ReadmeSize);

			FIndexManifest V101 = Index;
			V101.ContentVersion = TEXT("1.0.1");
			FIndexFileEntry Readme;
			Readme.SHA1 = Sha1OfBytes(ReadmeExpected);
			Readme.Size = ReadmeSize;
			FIndexFileChunkRef RPart;
			RPart.Chunk = ReadmeGuid;
			RPart.Offset = ReadmeOffset;
			RPart.Size = ReadmeSize;
			Readme.Parts.Add(RPart);
			V101.Files.Add(TEXT("readme.txt"), Readme);

			const FString V101Dir = FPaths::Combine(Cdn, TEXT("index"), TEXT("1.0.1"));
			PF.CreateDirectoryTree(*V101Dir);
			Check(FIndexManifestFile::Save(FPaths::Combine(V101Dir, TEXT("UnrealGamePatcher.index.json")), V101),
				TEXT("crafted 1.0.1 index saved"));
		}

		FUpdateStats S7;
		Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.1"), Install, true, S7), TEXT("update to 1.0.1 succeeds"));
		Check(S7.NumFilesAdded == 1 && S7.NumFilesSame == 4, TEXT("1.0.1: readme added, rest same"));
		Check(S7.NumChunksFetched == 0, TEXT("1.0.1: nothing fetched (chunk file not even staged)"));
		Check(S7.NumChunksReused == 1 && S7.BytesReused == ReadmeSize, TEXT("1.0.1: one chunk reused"));
		{
			TArray64<uint8> OnDisk;
			const bool bRead = FFileHelper::LoadFileToArray(OnDisk, *(FPaths::Combine(Install, TEXT("readme.txt"))));
			Check(bRead && OnDisk.Num() == ReadmeExpected.Num() &&
				FMemory::Memcmp(OnDisk.GetData(), ReadmeExpected.GetData(), OnDisk.Num()) == 0,
				TEXT("1.0.1: readme.txt bytes equal the reused chunk slice"));
		}

		// --- 8. Changed file with partial reuse + partial fetch -------------
		// Craft 1.0.2: readme.txt grows by a second part from the same chunk
		// at a slice that no local file carries -> fetched from CDN.
		{
			// Any slice of the chunk not present in any local file.
			FIndexManifest V102Index;
			FString StatePath;
			{
				// The sidecar is the committed remote index verbatim - the
				// crafted next version must carry the same container entry
				// metadata (the in-place patchers consume it).
				StatePath = FPaths::Combine(Install, TEXT(".ugp"), TEXT("index.json"));
				Check(FIndexManifestFile::Load(StatePath, V102Index), TEXT("1.0.2: local state loads"));
				{
					// Invariant: sidecar == remote index of the installed version.
					FIndexManifest RemoteOfInstalled;
					Check(FIndexManifestFile::Load(FPaths::Combine(Cdn, TEXT("index"), V102Index.ContentVersion, TEXT("UnrealGamePatcher.index.json")), RemoteOfInstalled),
						TEXT("1.0.2: remote index of the installed version loads"));
					Check(V102Index.Files.Num() == RemoteOfInstalled.Files.Num() &&
						V102Index.Chunks.Num() == RemoteOfInstalled.Chunks.Num() &&
						V102Index.ContentVersion == RemoteOfInstalled.ContentVersion,
						TEXT("sidecar mirrors the committed remote index"));
				}
			}

			TArray64<uint8> ChunkData;
			{
				const FIndexChunkEntry& Chunk = Index.Chunks[ReadmeGuid];
				if (!FChunkDataLoader::Load(FPaths::Combine(Cdn, Chunk.File), Chunk, ChunkData))
				{
					return ExitFailure;
				}
			}

			// Collect every (offset,size) slice of ReadmeGuid already on disk.
			TSet<FString> KnownSlices;
			for (const TPair<FString, FIndexFileEntry>& Pair : V102Index.Files)
			{
				for (const FIndexFileChunkRef& P : Pair.Value.Parts)
				{
					if (P.Chunk == ReadmeGuid)
					{
						KnownSlices.Add(FString::Printf(TEXT("%lld/%lld"), P.Offset, P.Size));
					}
				}
			}

			int64 Off2 = 0, Size2 = 0;
			const int64 ChunkSize = (int64)ChunkData.Num();
			for (int64 O = 16; O + 16 < ChunkSize; O += 16)
			{
				const int64 Sz = ChunkSize / 2;
				if (!KnownSlices.Contains(FString::Printf(TEXT("%lld/%lld"), O, Sz)))
				{
					Off2 = O;
					Size2 = Sz;
					break;
				}
			}
			Check(Off2 != 0 && Size2 > 0, TEXT("found a chunk slice not present locally"));

			TArray64<uint8> Expected2;
			Expected2.SetNumUninitialized(ReadmeSize + Size2);
			FMemory::Memcpy(Expected2.GetData(), ReadmeExpected.GetData(), ReadmeSize);
			FMemory::Memcpy(Expected2.GetData() + ReadmeSize, ChunkData.GetData() + Off2, Size2);

			FIndexManifest V102 = V102Index;
			V102.ContentVersion = TEXT("1.0.2");
			FIndexFileEntry Readme = V102.Files[TEXT("readme.txt")];
			Readme.Parts.Reset();
			Readme.Size = 0;

			FIndexFileChunkRef P1, P2;
			P1.Chunk = ReadmeGuid; P1.Offset = ReadmeOffset; P1.Size = ReadmeSize;
			P2.Chunk = ReadmeGuid; P2.Offset = Off2; P2.Size = Size2;
			Readme.Parts.Add(P1);
			Readme.Parts.Add(P2);
			Readme.Size = ReadmeSize + Size2;
			Readme.SHA1 = Sha1OfBytes(Expected2);
			V102.Files[TEXT("readme.txt")] = Readme;

			const FString V102Dir = FPaths::Combine(Cdn, TEXT("index"), TEXT("1.0.2"));
			PF.CreateDirectoryTree(*V102Dir);
			// No chunk staging needed: chunks are GLOBAL, the reused/fetched
			// window is already on the CDN from the 1.0.0 ingest.
			Check(FIndexManifestFile::Save(FPaths::Combine(V102Dir, TEXT("UnrealGamePatcher.index.json")), V102),
				TEXT("crafted 1.0.2 index saved"));

			FUpdateStats S8;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.2"), Install, true, S8), TEXT("update to 1.0.2 succeeds"));
			Check(S8.NumFilesChanged == 1 && S8.NumFilesSame == 4, TEXT("1.0.2: readme changed, rest same"));
			Check(S8.NumChunksReused == 1, TEXT("1.0.2: first part reused from local"));
			Check(S8.NumChunksFetched == 1, TEXT("1.0.2: second part fetched once"));
			{
				TArray64<uint8> OnDisk;
				const bool bRead = FFileHelper::LoadFileToArray(OnDisk, *(FPaths::Combine(Install, TEXT("readme.txt"))));
				Check(bRead && OnDisk.Num() == Expected2.Num() &&
					FMemory::Memcmp(OnDisk.GetData(), Expected2.GetData(), OnDisk.Num()) == 0,
					TEXT("1.0.2: readme.txt bytes equal reused + fetched slices"));
			}

			// The local state now points at 1.0.2 and knows readme's new parts.
			FIndexManifest FinalState;
			if (FIndexManifestFile::Load(StatePath, FinalState))
			{
				Check(FinalState.ContentVersion == TEXT("1.0.2") && FinalState.Files.Contains(TEXT("readme.txt")),
					TEXT("state refreshed to 1.0.2 with the new file"));
			}
			else
			{
				Check(false, TEXT("state refreshed to 1.0.2 with the new file"));
			}
		}

		// --- 9. Transaction rollback on injected failure (task 5.2) ---------
		// Snapshot the current (1.0.2) install.
		{
			const TArray<FString> InstallFiles = {
				TEXT("pakchunk1-Windows.pak"), TEXT("pakchunk1-Windows.ucas"),
				TEXT("pakchunk1-Windows.utoc"), TEXT("readme.txt") };
			TMap<FString, FString> Snapshot;
			for (const FString& F : InstallFiles)
			{
				FString Sha1;
				int64 Size = 0;
				if (FLocalTruthExtractor::HashWholeFile(FPaths::Combine(Install, F), Sha1, Size))
				{
					Snapshot.Add(F, Sha1);
				}
			}
			Check(Snapshot.Num() == 4, TEXT("rollback: snapshot of 4 installed files"));

			auto BytesMatchSnapshot = [&]() -> bool
			{
				bool bOk = true;
				for (const TPair<FString, FString>& Pair : Snapshot)
				{
					FString Sha1;
					int64 Size = 0;
					if (!FLocalTruthExtractor::HashWholeFile(FPaths::Combine(Install, Pair.Key), Sha1, Size) ||
						!Sha1.Equals(Pair.Value, ESearchCase::IgnoreCase))
					{
						bOk = false;
					}
				}
				return bOk;
			};

			auto ReadStateStatus = [&]() -> ETransactionStatus
			{
				FPatcherState S;
				const FString SP = UnrealGamePatcher::Paths::GetStatePath(Install);
				if (!FPatcherStateFile::Load(SP, S))
				{
					return ETransactionStatus::None;
				}
				return S.Transaction.Status;
			};

			// Craft 1.0.3: readme grows by a third slice (a fresh fetch).
			{
				FIndexManifest V102State;
				const FString StatePath = FPaths::Combine(Install, TEXT(".ugp"), TEXT("index.json"));
				Check(FIndexManifestFile::Load(StatePath, V102State), TEXT("rollback: local state loads for crafting 1.0.3"));
				{
				}

				TArray64<uint8> ChunkData;
				{
					const FIndexChunkEntry& Chunk = Index.Chunks[ReadmeGuid];
					if (!FChunkDataLoader::Load(FPaths::Combine(Cdn, Chunk.File), Chunk, ChunkData))
					{
						return ExitFailure;
					}
				}
				const int64 ChunkSize = (int64)ChunkData.Num();

				// Choose Off3/Size3 outside every known slice of the guid.
				TSet<FString> KnownSlices;
				for (const TPair<FString, FIndexFileEntry>& Pair : V102State.Files)
				{
					for (const FIndexFileChunkRef& P : Pair.Value.Parts)
					{
						if (P.Chunk == ReadmeGuid)
						{
							KnownSlices.Add(FString::Printf(TEXT("%lld/%lld"), P.Offset, P.Size));
						}
					}
				}
				int64 Off3 = 0, Size3 = 0;
				for (int64 O = 48; O + 16 < ChunkSize; O += 16)
				{
					const int64 Sz = ChunkSize / 3;
					if (!KnownSlices.Contains(FString::Printf(TEXT("%lld/%lld"), O, Sz)))
					{
						Off3 = O;
						Size3 = Sz;
						break;
					}
				}
				Check(Off3 != 0 && Size3 > 0, TEXT("rollback: found a third slice not present locally"));

				// Expected bytes = current readme bytes + the new slice.
				TArray64<uint8> Expected3;
				{
					TArray64<uint8> Current;
					FFileHelper::LoadFileToArray(Current, *(FPaths::Combine(Install, TEXT("readme.txt"))));
					Expected3.SetNumUninitialized(Current.Num() + Size3);
					FMemory::Memcpy(Expected3.GetData(), Current.GetData(), Current.Num());
					FMemory::Memcpy(Expected3.GetData() + Current.Num(), ChunkData.GetData() + Off3, Size3);
				}

				FIndexManifest V103 = V102State;
				V103.ContentVersion = TEXT("1.0.3");
				FIndexFileEntry Readme = V103.Files[TEXT("readme.txt")];
				FIndexFileChunkRef P3;
				P3.Chunk = ReadmeGuid; P3.Offset = Off3; P3.Size = Size3;
				Readme.Parts.Add(P3);
				Readme.Size += Size3;
				Readme.SHA1 = Sha1OfBytes(Expected3);
				V103.Files[TEXT("readme.txt")] = Readme;

				const FString V103Dir = FPaths::Combine(Cdn, TEXT("index"), TEXT("1.0.3"));
				PF.CreateDirectoryTree(*V103Dir);
				Check(FIndexManifestFile::Save(FPaths::Combine(V103Dir, TEXT("UnrealGamePatcher.index.json")), V103),
					TEXT("crafted 1.0.3 index saved"));

				// 9a. Injected failure after the first write -> rollback.
				FUpdateOrchestrator::TestFailAfterWrites = 1;
				FUpdateStats SFail;
				const bool bFailed = !FUpdateOrchestrator::Update(Cdn, TEXT("1.0.3"), Install, true, SFail);
				FUpdateOrchestrator::TestFailAfterWrites = 0;
				Check(bFailed, TEXT("rollback: injected failure makes update fail"));

				const bool bRestored = BytesMatchSnapshot();
				Check(bRestored, TEXT("rollback: every installed file byte-identical to pre-update"));
				Check(ReadStateStatus() == ETransactionStatus::RolledBack, TEXT("rollback: sidecar reports rolled-back"));
				Check(!PF.DirectoryExists(*(FPaths::Combine(Install, TEXT(".ugp"), TEXT("backup")))), TEXT("rollback: backups cleaned up"));

				// 9b. A retry (no injection) completes and reaches 1.0.3.
				FUpdateStats SRetry;
				Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.3"), Install, true, SRetry), TEXT("rollback: retry after rollback succeeds"));
				{
					TArray64<uint8> OnDisk;
					const bool bRead = FFileHelper::LoadFileToArray(OnDisk, *(FPaths::Combine(Install, TEXT("readme.txt"))));
					Check(bRead && OnDisk.Num() == Expected3.Num() &&
						FMemory::Memcmp(OnDisk.GetData(), Expected3.GetData(), OnDisk.Num()) == 0,
						TEXT("rollback: retry reaches the 1.0.3 readme bytes"));
				}
				Check(ReadStateStatus() == ETransactionStatus::Committed, TEXT("rollback: sidecar reports committed after retry"));

				// 9c. Removed-file rollback: 1.0.4 drops readme; injected
				// failure after the delete must restore it.
				FIndexManifest V104 = V103;
				V104.ContentVersion = TEXT("1.0.4");
				V104.Files.Remove(TEXT("readme.txt"));
				const FString V104Dir = FPaths::Combine(Cdn, TEXT("index"), TEXT("1.0.4"));
				PF.CreateDirectoryTree(*V104Dir);
				Check(FIndexManifestFile::Save(FPaths::Combine(V104Dir, TEXT("UnrealGamePatcher.index.json")), V104),
					TEXT("crafted 1.0.4 index saved"));

				FUpdateOrchestrator::TestFailAfterWrites = 1;
				FUpdateStats SDeleteFail;
				const bool bDeleteFailed = !FUpdateOrchestrator::Update(Cdn, TEXT("1.0.4"), Install, true, SDeleteFail);
				FUpdateOrchestrator::TestFailAfterWrites = 0;
				Check(bDeleteFailed, TEXT("rollback: injected failure on delete makes update fail"));
				{
					TArray64<uint8> OnDisk;
					const bool bRead = FFileHelper::LoadFileToArray(OnDisk, *(FPaths::Combine(Install, TEXT("readme.txt"))));
					Check(bRead && OnDisk.Num() == Expected3.Num() &&
						FMemory::Memcmp(OnDisk.GetData(), Expected3.GetData(), OnDisk.Num()) == 0,
						TEXT("rollback: deleted file restored byte-identically"));
				}
				Check(ReadStateStatus() == ETransactionStatus::RolledBack, TEXT("rollback: delete-failure sidecar reports rolled-back"));

				// 9d. Normal update to 1.0.4 commits the removal.
				FUpdateStats SRemove;
				Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.4"), Install, true, SRemove), TEXT("rollback: normal update to 1.0.4 succeeds"));
				Check(SRemove.NumFilesRemoved == 1, TEXT("rollback: readme classified removed"));
				Check(!PF.FileExists(*(FPaths::Combine(Install, TEXT("readme.txt")))), TEXT("rollback: readme removed on commit"));
				Check(ReadStateStatus() == ETransactionStatus::Committed, TEXT("rollback: removal committed in sidecar"));

				// Leave the install consistent: back to 1.0.3 content.
				FUpdateStats SBack;
				Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.3"), Install, true, SBack), TEXT("rollback: restore 1.0.3 install"));
			}
		}

		// --- 10. Crash recovery (task 5.3) ----------------------------------
		// Simulate a process kill mid-apply: the update stops after one write
		// and leaves the transaction Pending + backups on disk. A rerun must
		// first roll the interrupted transaction back, then apply cleanly.
		{
			// Snapshot the current (1.0.3) install.
			TMap<FString, FString> Snapshot;
			{
				const TArray<FString> InstallFiles = {
					TEXT("pakchunk1-Windows.pak"), TEXT("pakchunk1-Windows.ucas"),
					TEXT("pakchunk1-Windows.utoc"), TEXT("readme.txt") };
				for (const FString& F : InstallFiles)
				{
					FString Sha1;
					int64 Size = 0;
					if (FLocalTruthExtractor::HashWholeFile(FPaths::Combine(Install, F), Sha1, Size))
					{
						Snapshot.Add(F, Sha1);
					}
				}
			}
			Check(Snapshot.Num() == 4, TEXT("crash: snapshot of 4 installed files"));

			auto ReadStateStatus = [&]() -> ETransactionStatus
			{
				FPatcherState S;
				const FString SP = UnrealGamePatcher::Paths::GetStatePath(Install);
				if (!FPatcherStateFile::Load(SP, S))
				{
					return ETransactionStatus::None;
				}
				return S.Transaction.Status;
			};

			// Craft 1.0.5: readme grows by one more slice (needs a fetch).
			TArray64<uint8> Expected5;
			{
				FIndexManifest V103State;
				const FString StatePath = FPaths::Combine(Install, TEXT(".ugp"), TEXT("index.json"));
				Check(FIndexManifestFile::Load(StatePath, V103State), TEXT("crash: local state loads for crafting 1.0.5"));

				TArray64<uint8> ChunkData;
				{
					const FIndexChunkEntry& Chunk = Index.Chunks[ReadmeGuid];
					if (!FChunkDataLoader::Load(FPaths::Combine(Cdn, Chunk.File), Chunk, ChunkData))
					{
						return ExitFailure;
					}
				}
				const int64 ChunkSize = (int64)ChunkData.Num();

				TSet<FString> KnownSlices;
				for (const TPair<FString, FIndexFileEntry>& Pair : V103State.Files)
				{
					for (const FIndexFileChunkRef& P : Pair.Value.Parts)
					{
						if (P.Chunk == ReadmeGuid)
						{
							KnownSlices.Add(FString::Printf(TEXT("%lld/%lld"), P.Offset, P.Size));
						}
					}
				}
				int64 Off5 = 0, Size5 = 0;
				for (int64 O = 96; O + 16 < ChunkSize; O += 16)
				{
					const int64 Sz = ChunkSize / 5;
					if (!KnownSlices.Contains(FString::Printf(TEXT("%lld/%lld"), O, Sz)))
					{
						Off5 = O;
						Size5 = Sz;
						break;
					}
				}
				Check(Off5 != 0 && Size5 > 0, TEXT("crash: found a fifth slice not present locally"));

				TArray64<uint8> Current;
				FFileHelper::LoadFileToArray(Current, *(FPaths::Combine(Install, TEXT("readme.txt"))));
				Expected5.SetNumUninitialized(Current.Num() + Size5);
				FMemory::Memcpy(Expected5.GetData(), Current.GetData(), Current.Num());
				FMemory::Memcpy(Expected5.GetData() + Current.Num(), ChunkData.GetData() + Off5, Size5);

				FIndexManifest V105 = V103State;
				V105.ContentVersion = TEXT("1.0.5");
				FIndexFileEntry Readme = V105.Files[TEXT("readme.txt")];
				FIndexFileChunkRef P5;
				P5.Chunk = ReadmeGuid; P5.Offset = Off5; P5.Size = Size5;
				Readme.Parts.Add(P5);
				Readme.Size += Size5;
				Readme.SHA1 = Sha1OfBytes(Expected5);
				V105.Files[TEXT("readme.txt")] = Readme;

				const FString V105Dir = FPaths::Combine(Cdn, TEXT("index"), TEXT("1.0.5"));
				PF.CreateDirectoryTree(*V105Dir);
				// No chunk staging: the window is GLOBAL on the CDN already.
				Check(FIndexManifestFile::Save(FPaths::Combine(V105Dir, TEXT("UnrealGamePatcher.index.json")), V105),
					TEXT("crafted 1.0.5 index saved"));
			}

			// 10a. "Kill" the process after the first write: transaction stays
			// Pending, backups stay on disk, the new readme is already written.
			FUpdateOrchestrator::TestCrashAfterWrites = 1;
			FUpdateStats SCrash;
			const bool bCrashed = !FUpdateOrchestrator::Update(Cdn, TEXT("1.0.5"), Install, true, SCrash);
			FUpdateOrchestrator::TestCrashAfterWrites = 0;
			Check(bCrashed, TEXT("crash: simulated kill makes update fail"));
			Check(ReadStateStatus() == ETransactionStatus::Pending, TEXT("crash: transaction left pending"));
			Check(PF.DirectoryExists(*(FPaths::Combine(Install, TEXT(".ugp"), TEXT("backup")))), TEXT("crash: backups left on disk"));

			// 10b. Rerun (as the next launch): recovery rolls back the
			// interrupted transaction, then applies 1.0.5 fresh.
			FUpdateStats SRecover;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.5"), Install, true, SRecover), TEXT("crash: rerun after kill succeeds"));
			{
				TArray64<uint8> OnDisk;
				const bool bRead = FFileHelper::LoadFileToArray(OnDisk, *(FPaths::Combine(Install, TEXT("readme.txt"))));
				Check(bRead && OnDisk.Num() == Expected5.Num() &&
					FMemory::Memcmp(OnDisk.GetData(), Expected5.GetData(), OnDisk.Num()) == 0,
					TEXT("crash: rerun converges to the 1.0.5 readme bytes"));
				bool bOthersOk = true;
				for (const TPair<FString, FString>& Pair : Snapshot)
				{
					if (Pair.Key == TEXT("readme.txt")) { continue; }
					FString Sha1;
					int64 Size = 0;
					if (!FLocalTruthExtractor::HashWholeFile(FPaths::Combine(Install, Pair.Key), Sha1, Size) ||
						!Sha1.Equals(Pair.Value, ESearchCase::IgnoreCase))
					{
						bOthersOk = false;
					}
				}
				Check(bOthersOk, TEXT("crash: untouched files keep their pre-crash bytes"));
			}
			Check(ReadStateStatus() == ETransactionStatus::Committed, TEXT("crash: rerun commits cleanly"));
			Check(!PF.DirectoryExists(*(FPaths::Combine(Install, TEXT(".ugp"), TEXT("backup")))), TEXT("crash: backups cleaned after commit"));

			// 10c. Crash recovery with an Added target: kill after the write,
			// then verify a rerun still converges (rollback removed the added
			// file, the fresh apply re-adds it).
			{
				// 1.0.6 adds notes.txt (one part, utoc slice) to the 1.0.5 set.
				FIndexManifest V106;
				Check(FIndexManifestFile::Load(FPaths::Combine(Cdn, TEXT("index"), TEXT("1.0.5"), TEXT("UnrealGamePatcher.index.json")), V106),
					TEXT("crash: 1.0.5 index loads for crafting 1.0.6"));
				V106.ContentVersion = TEXT("1.0.6");
				const FIndexFileEntry& Utoc = V106.Files[TEXT("pakchunk1-Windows.utoc")];
				const FIndexFileChunkRef& Part = Utoc.Parts[0];
				FIndexFileEntry Notes;
				Notes.Size = Part.Size;
				FIndexFileChunkRef NPart;
				NPart.Chunk = Part.Chunk; NPart.Offset = Part.Offset; NPart.Size = Part.Size;
				Notes.Parts.Add(NPart);
				// Expected bytes = the utoc slice = the utoc file content.
				TArray64<uint8> UtocBytes;
				FFileHelper::LoadFileToArray(UtocBytes, *(FPaths::Combine(Install, TEXT("pakchunk1-Windows.utoc"))));
				Notes.SHA1 = Sha1OfBytes(UtocBytes);
				V106.Files.Add(TEXT("notes.txt"), Notes);

				const FString V106Dir = FPaths::Combine(Cdn, TEXT("index"), TEXT("1.0.6"));
				PF.CreateDirectoryTree(*V106Dir);
				// No chunk copy needed: notes reuses the utoc slice, so a
				// recovery that falls back to fetch would fail loudly.
				Check(FIndexManifestFile::Save(FPaths::Combine(V106Dir, TEXT("UnrealGamePatcher.index.json")), V106),
					TEXT("crafted 1.0.6 index saved"));

				FUpdateOrchestrator::TestCrashAfterWrites = 1;
				FUpdateStats SAddCrash;
				const bool bAddCrashed = !FUpdateOrchestrator::Update(Cdn, TEXT("1.0.6"), Install, true, SAddCrash);
				FUpdateOrchestrator::TestCrashAfterWrites = 0;
				Check(bAddCrashed, TEXT("crash: added-file kill makes update fail"));
				Check(ReadStateStatus() == ETransactionStatus::Pending, TEXT("crash: added-file transaction left pending"));

				FUpdateStats SAddRecover;
				Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.6"), Install, true, SAddRecover), TEXT("crash: added-file rerun succeeds"));
				Check(SAddRecover.NumFilesAdded == 1, TEXT("crash: added-file rerun re-adds it"));
				{
					TArray64<uint8> OnDisk;
					const bool bRead = FFileHelper::LoadFileToArray(OnDisk, *(FPaths::Combine(Install, TEXT("notes.txt"))));
					Check(bRead && OnDisk.Num() == UtocBytes.Num() &&
						FMemory::Memcmp(OnDisk.GetData(), UtocBytes.GetData(), OnDisk.Num()) == 0,
						TEXT("crash: added-file rerun has correct notes.txt bytes"));
				}
				Check(ReadStateStatus() == ETransactionStatus::Committed, TEXT("crash: added-file rerun commits"));
			}
		}

		// --- 11. Trust mode skips hashing: same-size damage is NOT detected --
		// The accepted tradeoff: trust mode accepts a Same chunk-id set without
		// reading the data - the periodic verify / manual repair catches it.
		{
			using namespace UnrealGamePatcher;
			const FString UcasPath = FPaths::Combine(Install, TEXT("pakchunk1-Windows.ucas"));
			{
				TArray64<uint8> Bytes;
				FFileHelper::LoadFileToArray(Bytes, *UcasPath);
				Bytes[Bytes.Num() / 2] ^= 0xFF;
				FFileHelper::SaveArrayToFile(Bytes, *UcasPath);
			}
			FUpdateStats STrust;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.6"), Install, true, STrust), TEXT("trust: update succeeds"));
			Check(STrust.NumFilesChanged == 0 && STrust.NumFilesSame >= 5,
				TEXT("trust: same-size corruption is NOT detected (chunk-id set trusted, no data read)"));
			{
				// Prove the damage is still on disk: the local container's
				// chunk data no longer matches its utoc meta hash (the pure
				// classifier only compares chunk IDs, which are intact - only
				// a verify-mode DATA check would flag this).
				UnrealGamePatcher::FIoStoreReader Io;
				const FString UtocPath = FPaths::Combine(Install, TEXT("pakchunk1-Windows.utoc"));
				bool bDamaged = false;
				if (Io.Open(UtocPath, UcasPath))
				{
					for (int32 I = 0; I < Io.GetChunkCount() && !bDamaged; ++I)
					{
						TArray64<uint8> Data;
						if (!Io.ReadChunk(I, Data))
						{
							bDamaged = true;
							break;
						}
						const FString HashHex = UnrealGamePatcher::FPatcherHash::FIoHashToHex(
							FIoHash::HashBuffer(Data.GetData(), Data.Num()));
						if (!Io.GetChunks()[I].Hash.Equals(HashHex, ESearchCase::IgnoreCase))
						{
							bDamaged = true;
						}
					}
				}
				Check(bDamaged, TEXT("trust: the damage is still on disk (chunk data fails the utoc meta hash)"));
			}
		}

		// --- 12. verifyOnUpdate flag forces a verify run (publish trigger) ---
		{
			using namespace UnrealGamePatcher;
			// Craft version 1.0.7: identical content, but the manifest carries
			// the per-version verifyOnUpdate flag (set when publishing a big or
			// long-absent update).
			FIndexManifest V107;
			{
				// Source the REMOTE index of the installed version: the local
				// sidecar IS the committed remote index verbatim (container
				// entry metadata included - the manifest diff needs it to
				// match the remote), and the crafted 1.0.7 CDN index must
				// keep it (the in-place patchers consume
				// RemoteFile->PakEntries / IoStoreChunks).
				Check(FIndexManifestFile::Load(FPaths::Combine(Install, TEXT(".ugp"), TEXT("index.json")), V107),
					TEXT("verifyflag: local state loads (verbatim remote index)"));
			}
			V107.ContentVersion = TEXT("1.0.7");
			V107.bVerifyOnUpdate = true;
			const FString V107Dir = FPaths::Combine(Cdn, TEXT("index"), TEXT("1.0.7"));
			PF.CreateDirectoryTree(*V107Dir);
			Check(FIndexManifestFile::Save(FPaths::Combine(V107Dir, TEXT("UnrealGamePatcher.index.json")), V107),
				TEXT("verifyflag: crafted 1.0.7 index with verifyOnUpdate"));
			// No chunk staging: chunks are GLOBAL on the CDN already.

			// The flag flips the update into VERIFY mode: the still-corrupted
			// ucas is detected and repaired together with its sibling utoc.
			FUpdateStats SVerify;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.7"), Install, true, SVerify), TEXT("verifyflag: update to 1.0.7 succeeds"));
			Check(SVerify.NumFilesChanged == 2, TEXT("verifyflag: damaged ucas + sibling utoc rebuilt"));
			Check(CheckContainerLogicallyEqual(Index, Install, TEXT("pakchunk1-Windows.ucas")),
				TEXT("verifyflag: ucas repaired (per-chunk hashes match remote)"));
			{
				FPatcherState St;
				FPatcherStateFile::Load(Paths::GetStatePath(Install), St);
				Check(!St.LastVerifiedAt.IsEmpty(), TEXT("verifyflag: LastVerifiedAt recorded after the verify run"));
			}
		}

		// --- 13. C3 regression: a MISSING utoc must be re-fetched in TRUST
		// mode. The old mirror overrode the utoc's Added with the sibling
		// ucas's Same; the committed sidecar kept claiming the utoc existed,
		// and no update/check/repair ever converged it - the container could
		// not mount while every run reported success. (Verify mode healed it
		// by accident - the missing utoc fails the ucas extraction - which is
		// why the bug survived; 1.0.8 deliberately carries NO verifyOnUpdate.)
		{
			FIndexManifest V108;
			Check(FIndexManifestFile::Load(FPaths::Combine(Install, TEXT(".ugp"), TEXT("index.json")), V108),
				TEXT("missingutoc: local state loads"));
			V108.ContentVersion = TEXT("1.0.8");
			V108.bVerifyOnUpdate = false; // force the TRUST path (the broken one)
			const FString V108Dir = FPaths::Combine(Cdn, TEXT("index"), TEXT("1.0.8"));
			PF.CreateDirectoryTree(*V108Dir);
			Check(FIndexManifestFile::Save(FPaths::Combine(V108Dir, TEXT("UnrealGamePatcher.index.json")), V108),
				TEXT("missingutoc: crafted 1.0.8 index (no verify flag)"));

			const FString UtocPath = FPaths::Combine(Install, TEXT("pakchunk1-Windows.utoc"));
			const FString UcasPath = FPaths::Combine(Install, TEXT("pakchunk1-Windows.ucas"));
			PF.DeleteFile(*UtocPath);
			Check(!PF.FileExists(*UtocPath), TEXT("missingutoc: utoc deleted"));

			FUpdateStats SMissing;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.8"), Install, true, SMissing),
				TEXT("missingutoc: update with a missing utoc succeeds"));
			Check(SMissing.NumFilesChanged == 1 && SMissing.NumFilesAdded == 1,
				TEXT("missingutoc: utoc Added (not mirrored to Same) + sibling ucas forced to repair"));
			Check(PF.FileExists(*UtocPath), TEXT("missingutoc: utoc restored"));
			{
				FIoStoreReader R;
				Check(R.Open(UtocPath, UcasPath) && R.GetChunkCount() > 0,
					TEXT("missingutoc: restored pair parses with chunks"));
			}

			// Convergence: the rerun is a no-op.
			FUpdateStats SMissing2;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.8"), Install, true, SMissing2),
				TEXT("missingutoc: rerun converges"));
			Check(SMissing2.NumFilesChanged == 0 && SMissing2.BytesWritten == 0,
				TEXT("missingutoc: rerun is a no-op"));
		}

		return FinishTest(bAllPassed, TEXT("updatetest"));
	
	}

}
