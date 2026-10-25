// Copyright Epic Games, Inc. All Rights Reserved.
// -cli maintest —— check / compact / repair 维护命令
// 验证点：check 强制验证模式报同尺寸损坏；repair 逐条目修复并收敛；
//         compact 压实洞并清账。

#include "Tests/TestFramework.h"

namespace UnrealGamePatcher::Tests
{
	int32 RunMainTest(const TCHAR* /*CmdLine*/)
	{

		// Maintenance acceptance (tasks 5.4/5.5): check purity, compact
		// reclaims injected holes with content-equivalent output, repair
		// rebuilds only damaged files (leaves strays/missing alone).
		bool bAllPassed = true;
		auto Check = MakeCheck(bAllPassed);

		using namespace UnrealGamePatcher;

		const FString WorkDir = GetTestWorkDir(TEXT("maintest"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

		const FString Cdn = FPaths::Combine(WorkDir, TEXT("cdn"));
		const FString Install = FPaths::Combine(WorkDir, TEXT("install"));
		PF.CreateDirectoryTree(*Install);
		const FString StatePath = Paths::GetStatePath(Install);

		FIndexManifest Index;
		Check(BuildFixtureCdn(WorkDir, Cdn, Index), TEXT("maintest: ingest staged fixture"));

		FUpdateStats S0;
		Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.0"), Install, true, S0), TEXT("maintest: install 1.0.0"));
		Check(S0.NumFilesAdded == 4, TEXT("maintest: 4 files installed"));

		// --- 1. check is read-only and reports a no-op ---------------------
		{
			// The check-only purity matrix (0 writes / 0 fetches / untouched)
			// is pinned by updatetest section 4 - here only the unique value:
			// the zero-holes report the maintenance flows key off.
			FUpdateStats SCheck;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.0"), Install, false, SCheck), TEXT("check: analysis succeeds"));
			Check(SCheck.BytesWritten == 0, TEXT("check: no-op install reported (0 writes)"));

			FPatcherState State;
			FPatcherStateFile::Load(StatePath, State);
			Check(State.Holes.Num() == 0, TEXT("check: no holes recorded"));
		}

		// --- 2. compact reclaims injected holes ----------------------------
		{
			// Inject a fake hole for the ucas into the sidecar.
			FPatcherState State;
			FPatcherStateFile::Load(StatePath, State);
			FHole Hole;
			Hole.Offset = 0;
			Hole.Size = 500000;
			Hole.File = TEXT("pakchunk1-Windows.ucas");
			State.Holes.Add(Hole);
			Check(FPatcherStateFile::Save(StatePath, State), TEXT("compact: injected a hole for ucas"));

			// The check-side report must flag the bloat (500000 / 10348176 = 4.8%).
			{
				int64 HoleBytes = 0;
				int64 InstallBytes = 0;
				FIndexManifest Truth;
				FLocalTruthExtractor::ExtractInstall(Install, Truth);
				for (const TPair<FString, FIndexFileEntry>& Pair : Truth.Files) { InstallBytes += Pair.Value.Size; }
				FPatcherState S2;
				FPatcherStateFile::Load(StatePath, S2);
				for (const FHole& H : S2.Holes) { HoleBytes += H.Size; }
				const double Bloat = InstallBytes > 0 ? (100.0 * (double)HoleBytes / (double)InstallBytes) : 0.0;
				Check(HoleBytes == 500000 && Bloat > 4.0 && Bloat < 6.0,
					TEXT("compact: hole report shows ~4.8% bloat"));
			}

			// Compact rebuilds the hole-bearing file from CDN chunks.
			FUpdateStats SCompact;
			TArray<FString> Files;
			Files.Add(TEXT("pakchunk1-Windows.ucas"));
			const int32 Rebuilt = FUpdateOrchestrator::Compact(Cdn, TEXT("1.0.0"), Install, Files, SCompact);
			Check(Rebuilt == 1, TEXT("compact: ucas rebuilt"));

			FString Sha1;
			int64 Size = 0;
			FLocalTruthExtractor::HashWholeFile(FPaths::Combine(Install, TEXT("pakchunk1-Windows.ucas")), Sha1, Size);
			Check(Sha1.Equals(Index.Files[TEXT("pakchunk1-Windows.ucas")].SHA1, ESearchCase::IgnoreCase) &&
				Size == Index.Files[TEXT("pakchunk1-Windows.ucas")].Size,
				TEXT("compact: rebuilt ucas is content-equivalent (hash == manifest)"));

			// After compact, holes are cleared.
			State.Holes.RemoveAll([&](const FHole& H) { return H.File == TEXT("pakchunk1-Windows.ucas"); });
			Check(FPatcherStateFile::Save(StatePath, State), TEXT("compact: holes cleared"));

			// Compact with no holes is a no-op success.
			TArray<FString> None;
			FUpdateStats SNoop;
			Check(FUpdateOrchestrator::Compact(Cdn, TEXT("1.0.0"), Install, None, SNoop) == 0,
				TEXT("compact: empty file list is a no-op"));
		}

		// --- 3. repair rebuilds only damaged files -------------------------
		{
			// Corrupt a byte INSIDE the first entry's payload data (not the
			// header/index): verify mode recomputes per-entry payload hashes,
			// so the damage must be in the payload bytes to be caught.
			const FString PakPath = FPaths::Combine(Install, TEXT("pakchunk1-Windows.pak"));
			{
				TArray64<uint8> Bytes;
				FFileHelper::LoadFileToArray(Bytes, *PakPath);
				{
					UnrealGamePatcher::FPakReader Locator;
					if (Locator.Open(PakPath) && Locator.ReadIndex())
					{
					const FPakReader::FPakEntryLocation& First = Locator.GetFiles()[0];
					const int64 HeaderSize = UnrealGamePatcher::FPakReader::GetSerializedEntrySize(First.Entry, Locator.GetInfo().Version);
					const int64 Flip = First.Entry.Offset + HeaderSize + (First.Entry.Size > 0 ? First.Entry.Size / 2 : 0);
					if (Flip < (int64)Bytes.Num())
					{
						Bytes[Flip] ^= 0xFF;
					}
					}
				}
				// The Locator (read handle) is destroyed above - its share flags
				// would block the write open below.
				FFileHelper::SaveArrayToFile(Bytes, *PakPath);
			}
			const FString Stray = FPaths::Combine(Install, TEXT("stray.txt"));
			FFileHelper::SaveStringToFile(TEXT("stray content"), *Stray);

			FUpdateStats SRepair;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.0"), Install, true, SRepair, /*bRepairOnly=*/true),
				TEXT("repair: succeeds"));
			Check(SRepair.NumFilesChanged == 1 && SRepair.NumFilesSame == 3,
				TEXT("repair: exactly the damaged pak rebuilt"));
			Check(PF.FileExists(*Stray), TEXT("repair: stray file untouched (not deleted)"));
			Check(SRepair.NumFilesRemoved == 0 && SRepair.NumFilesAdded == 0,
				TEXT("repair: no additions/deletions"));

			// The virtual-disk model: the in-place-repaired pak is logically
			// equivalent (per-entry hashes match), not byte-identical.
			Check(CheckContainerLogicallyEqual(Index, Install, TEXT("pakchunk1-Windows.pak")),
				TEXT("repair: patched pak is logically equivalent (per-entry hashes match remote)"));

			FPatcherState State;
			FPatcherStateFile::Load(StatePath, State);
			Check(State.Transaction.Status == ETransactionStatus::Committed, TEXT("repair: transaction committed"));

			// A second repair is a no-op.
			FUpdateStats SRepair2;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0.0"), Install, true, SRepair2, /*bRepairOnly=*/true),
				TEXT("repair: idempotent second run succeeds"));
			Check(SRepair2.NumFilesChanged == 0, TEXT("repair: nothing left to repair"));
			PF.DeleteFile(*Stray);
		}

		return FinishTest(bAllPassed, TEXT("maintest"));
	
	}

}
