// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealGamePatcherCLI.h"
#include "UnrealGamePatcherVersion.h"
#include "Misc/Parse.h"
#include "Misc/CommandLine.h"
#include "UnrealGamePatcherLog.h"
#include "Hash/FPatcherHashSelfTest.h"
#include "Formats/Pak/FPakReader.h"
#include "Formats/Pak/FPakWriter.h"
#include "Formats/Pak/FPakRecovery.h"
#include "State/FPatcherState.h"
#include "Formats/Index/FIndexManifest.h"
#include "Formats/JsonIo.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Formats/Diff/FDiffAnalyzer.h"
#include "Formats/Diff/FLocalTruthExtractor.h"
#include "FBuildIngestor.h"
#include "FBuildDiffer.h"
#include "Formats/Chunk/FChunkDataLoader.h"
#include "Formats/IoStore/FIoStoreReader.h"
#include "Formats/IoStore/FIoStoreWriter.h"
#include "Hash/FPatcherHash.h"
#include "IO/IoHash.h"
#include "Update/FUpdateOrchestrator.h"
#include "Update/FInPlacePakPatcher.h"
#include "Download/FDownloadManager.h"
#include "Download/FHttpChunkDownloader.h"
#include "Download/FDownloadExecutor.h"
#include "Download/FHttpRangeRequest.h"
#include "HttpModule.h"
#include "UnrealGamePatcherPaths.h"
#include "Windows/WindowsHWrapper.h"
// Windows.h defines ReplaceFile as a macro (-> ReplaceFileW), which collides
// with the EApplyOp::ReplaceFile member used by the transaction tests.
#ifdef ReplaceFile
#undef ReplaceFile
#endif
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Compression.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "SecureHash.h"
#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogUnrealGamePatcherCLI, Log, All);

namespace
{
	constexpr int32 ExitSuccess = 0;
	constexpr int32 ExitFailure = 1;
	constexpr int32 ExitUsage = 2;

	// The positional command token: the first token that does not start
	// with '-' (switches are read by the handlers via FParse directly).
	FString GetCommandToken(const TCHAR* CommandLine)
	{
		FString Token;
		const TCHAR* Rest = CommandLine;
		while (FParse::Token(Rest, Token, false))
		{
			if (!Token.StartsWith(TEXT("-")))
			{
				return Token;
			}
		}
		return FString();
	}

	/**
	 * External CDN server for the network tests: the patcher itself contains
	 * NO HTTP server (design decision) - tests spawn Scripts/cdn_server.py
	 * (python, Range-capable, with runtime fault control via /__ctl endpoints)
	 * and hit a real network stack. Faults are armed through the control
	 * endpoint, so the server runs for the whole test session.
	 */
	struct FCdnServerProc
	{
		FProcHandle Proc;
		FString ScriptPath;
		FString LogPath;
		int32 Port = 0;

		bool Start(const FString& InScriptPath, const FString& Root, const FString& InLogPath,
			const TCHAR* ProbeRelPath = TEXT("index/1.0.0/UnrealGamePatcher.index.json"))
		{
			ScriptPath = InScriptPath;
			LogPath = InLogPath;
			Port = 18080 + (FPlatformTime::Cycles() % 200);
			Proc = FPlatformProcess::CreateProc(
				TEXT("python"),
				*FString::Printf(TEXT("\"%s\" --root \"%s\" --port %d --log \"%s\""), *ScriptPath, *Root, Port, *LogPath),
				false, true, false, nullptr, 0, nullptr, nullptr);
			if (!Proc.IsValid())
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FCdnServerProc: cannot start python (is it on PATH?)"));
				return false;
			}
			// Wait for READY by probing over the real transport. /__ctl/stats
			// works for any fixture (the downloadtest index needs one with an
			// index/ dir - benchtest passes the control endpoint instead).
			UnrealGamePatcher::FHttpChunkDownloader Probe;
			const FString ProbeUrl = FString::Printf(TEXT("http://127.0.0.1:%d/%s"), Port, ProbeRelPath);
			for (int32 I = 0; I < 50; ++I)
			{
				TArray64<uint8> Bytes;
				if (Probe.DownloadRange(ProbeUrl, 0, -1, Bytes) && Bytes.Num() > 0)
				{
					return true;
				}
				FPlatformProcess::Sleep(0.1);
			}
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FCdnServerProc: CDN server did not become ready (see %s)"), *LogPath);
			return false;
		}

		void Stop()
		{
			if (Proc.IsValid())
			{
				FPlatformProcess::TerminateProc(Proc, true);
				FPlatformProcess::CloseProc(Proc);
			}
		}

		/** Aggregated request stats, read from the server's /__ctl/stats
		 *  control endpoint (no shared-file reads). */
		struct FCdnStats
		{
			int32 Files = 0;
			int32 Ranges = 0;
			int32 Errors = 0;
			int32 Peak = 0;
			int64 MaxRangeBytes = 0;
		};

		FCdnStats Stats() const
		{
			FCdnStats S;
			UnrealGamePatcher::FHttpChunkDownloader Probe;
			TArray64<uint8> Bytes;
			const FString Url = FString::Printf(TEXT("http://127.0.0.1:%d/__ctl/stats"), Port);
			if (!Probe.DownloadRange(Url, 0, -1, Bytes))
			{
				return S;
			}
			FString Text = FString(UTF8_TO_TCHAR((const ANSICHAR*)Bytes.GetData()));
			auto Grab = [&Text](const TCHAR* Key) -> int64
			{
				const int32 P = Text.Find(Key, ESearchCase::CaseSensitive);
				if (P == INDEX_NONE)
				{
					return 0;
				}
				const int32 V = Text.Find(TEXT(" "), ESearchCase::CaseSensitive, ESearchDir::FromStart, P + FCString::Strlen(Key));
				return (int64)FCString::Atoi64(*(Text.Mid(P + FCString::Strlen(Key), V == INDEX_NONE ? MAX_int32 : V - (P + FCString::Strlen(Key)))));
			};
			S.Files = (int32)Grab(TEXT("file="));
			S.Ranges = (int32)Grab(TEXT("ranges="));
			S.Errors = (int32)Grab(TEXT("errors="));
			S.Peak = (int32)Grab(TEXT("peak="));
			S.MaxRangeBytes = Grab(TEXT("maxrange="));
			return S;
		}
	};

	void PrintUsage()
	{
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("Usage: UnrealGamePatcher -cli <command> [options]"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("Commands:"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  --version                 Print version and exit"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  selftest                  Run hash wrapper self tests"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  statetest                 Roundtrip + corruption tests for the sidecar state file"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  indextest                 Roundtrip + tamper/corruption tests for the extension manifest"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  difftest                  Local truth extraction + diff classification tests"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  buildtest                 Ingest determinism + hash consistency tests"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  diffbuild                 Build-side diff + version coexistence tests"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  updatetest                Client update flow tests (fetch / no-op / repair / reuse)"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  maintest                  check / compact / repair maintenance tests"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  downloadtest              HTTP download tests (range / resume / concurrency / retry / custom backend)"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  benchtest                 Download executor throughput (windowed vs single) + memory peak"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  iostoretest               IoStore reader tests (utoc parse / chunk decode)"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  iostoredump -utoc=<f> -ucas=<f>  Dump the utoc chunk list"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  e2etest                   End-to-end pipeline test (ingest -> update -> engine-load check)"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  pakinplace                Pak entry-level in-place patch test (no whole-file rewrite)"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  iostoreinplace            Ucas chunk-level in-place patch test (append changed chunks, rewrite utoc)"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  perftest                  Incremental vs full download/write measurement"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  diff -base=<i1> -target=<i2> -out=<dir> [-targetcdn=<cdn>] [-nocopy]  Build-side: delta between two versions"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  build -staged=<dir> -app=<name> -version=<v> -out=<cdn root>  Chunk a staged build + write the index"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  pakdump -pak=<path>       Parse a pak, verify built-in SHA1 per file"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  pakpatch -pak=<src> -out=<dst> -ops=<file>  Apply incremental edits to a pak (test harness for FPakWriter)"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  update -cdn=<file:///...|http(s)://...|dir> -install=<dir> [-version=<v>] [-check] [-injectfail=N] [-crashafter=N]  Update install dir to a CDN version (no -version = latest.json)"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  check -cdn=<root> -version=<v> -install=<dir>  Report diff + hole/bloat without writing"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  compact -cdn=<root> -version=<v> -install=<dir>  Rebuild hole-bearing files to reclaim dead weight"));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("  repair -cdn=<root> -version=<v> -install=<dir>  Rebuild only damaged (hash-mismatched) files"));
	}

	// ---- Shared test scaffolding (used by every -cli test command) -------

	/** Returns a Check(b, what) lambda bound to bAllPassed. */
	auto MakeCheck(bool& bAllPassed)
	{
		return [&bAllPassed](bool b, const TCHAR* What)
		{
			if (b) { UE_LOG(LogUnrealGamePatcher, Display, TEXT("[PASS] %s"), What); }
			else { UE_LOG(LogUnrealGamePatcher, Error, TEXT("[FAIL] %s"), What); }
			bAllPassed &= b;
		};
	}

	/** Absolute path of the per-test work dir under Saved/. */
	FString GetTestWorkDir(const TCHAR* Name)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformMisc::ProjectDir(), TEXT("Saved"), Name));
	}

	/** Delete (if any) and recreate WorkDir. */
	void ResetWorkDir(const FString& WorkDir)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (PF.DirectoryExists(*WorkDir))
		{
			PF.DeleteDirectoryRecursively(*WorkDir);
		}
		PF.CreateDirectoryTree(*WorkDir);
	}

	/** Log the per-test summary and return the process exit code. */
	int32 FinishTest(bool bAllPassed, const TCHAR* Name)
	{
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("%s: %s"), Name, bAllPassed ? TEXT("all checks passed") : TEXT("FAILURES DETECTED"));
		return bAllPassed ? ExitSuccess : ExitFailure;
	}

	/** SHA1 (lowercase hex) of an in-memory byte buffer. FSHA1::Update takes
	 *  an int32 count, so buffers > 2 GiB are fed in chunks (same guard as the
	 *  orchestrator's Sha1HexOf). */
	FString Sha1OfBytes(const TArray64<uint8>& Data)
	{
		FSHA1 Hasher;
		static constexpr int64 HashChunk = 1 << 30; // 1 GiB per Update call
		int64 Offset = 0;
		while (Offset < Data.Num())
		{
			const int64 N = FMath::Min<int64>(HashChunk, Data.Num() - Offset);
			Hasher.Update(Data.GetData() + Offset, (int32)N);
			Offset += N;
		}
		uint8 Hash[20] = { 0 };
		Hasher.Final();
		Hasher.GetHash(Hash);
		return UnrealGamePatcher::FPatcherHash::BytesToHex(Hash, 20);
	}

	/** Builds a small staged fixture + ingests it into Cdn, loading the
	 *  resulting index. The fixture is the real small containers (pakchunk1:
	 *  pak/ucas/utoc) plus a plain file - the same input the new build command
	 *  consumes. Returns false on any failure. */
	bool BuildFixtureCdn(
		const FString& WorkDir,
		const FString& Cdn,
		UnrealGamePatcher::FIndexManifest& OutIndex)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		const FString ProjectRoot = FPaths::GetPath(FPaths::GetPath(FPaths::GetPath(FPlatformProcess::BaseDir())));
		const FString StagedSrc = FPaths::Combine(ProjectRoot, TEXT(".manifesttest"), TEXT("chunkinstall"),
			TEXT("Windows"), TEXT("1.0.0"), TEXT("pakchunk1"));
		const FString StagedDir = FPaths::Combine(WorkDir, TEXT("staged"));
		if (!PF.DirectoryExists(*StagedDir) && !PF.CreateDirectoryTree(*StagedDir))
		{
			return false;
		}
		for (const TCHAR* Name : { TEXT("pakchunk1-Windows.pak"), TEXT("pakchunk1-Windows.ucas"), TEXT("pakchunk1-Windows.utoc") })
		{
			if (!PF.CopyFile(*(FPaths::Combine(StagedDir, Name)), *(FPaths::Combine(StagedSrc, Name))))
			{
				return false;
			}
		}
		FFileHelper::SaveStringToFile(TEXT("plain file content"), *(FPaths::Combine(StagedDir, TEXT("boot.txt"))));

		UnrealGamePatcher::FIngestResult Ingest;
		if (!UnrealGamePatcher::FBuildIngestor::Ingest(StagedDir, TEXT("IOStoreDemo"), TEXT("1.0.0"), Cdn, Ingest))
		{
			return false;
		}
		return UnrealGamePatcher::FIndexManifestFile::Load(Ingest.IndexPath, OutIndex);
	}

	/** Verifies a container file is LOGICALLY equivalent to the remote metadata
	 *  (virtual-disk model: per-entry / per-chunk hashes). The whole-file hash
	 *  legitimately differs after an in-place patch - per-entry equality is the
	 *  correct post-repair assertion. Verify-mode truth re-reads the container
	 *  and recomputes payload hashes, so this also proves the damage is gone. */
	bool CheckContainerLogicallyEqual(
		const UnrealGamePatcher::FIndexManifest& Remote,
		const FString& InstallDir,
		const FString& FileId)
	{
		UnrealGamePatcher::FIndexManifest Truth;
		if (!UnrealGamePatcher::FLocalTruthExtractor::ExtractInstall(InstallDir, Truth, nullptr, /*bVerify=*/true))
		{
			return false;
		}
		UnrealGamePatcher::FDiffResult D;
		UnrealGamePatcher::FDiffAnalyzer::DiffFilesVirtualDisk(Truth.Files, Remote.Files, D.Files);
		for (const UnrealGamePatcher::FDiffEntry& E : D.Files)
		{
			if (E.Id == FileId)
			{
				return E.Kind == UnrealGamePatcher::EChangeKind::Same;
			}
		}
		return false;
	}

	// ---- Per-command handlers (split out of RunUnrealGamePatcherCLI) -------

	static int32 RunSelftest(const TCHAR* /*CmdLine*/)
	{

		return UnrealGamePatcher::FPatcherHashSelfTest::Run() ? ExitSuccess : ExitFailure;
	
	}


	static int32 RunStateTest(const TCHAR* /*CmdLine*/)
	{

		// Sidecar state acceptance (task 2.3): write/read roundtrip plus
		// corruption detection. Works on a temp dir, leaves no artifacts.
		bool bAllPassed = true;
		const FString WorkDir = GetTestWorkDir(TEXT("statetest"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		const FString StatePath = UnrealGamePatcher::Paths::GetStatePath(WorkDir);

		auto Check = MakeCheck(bAllPassed);

		// --- 1. Roundtrip of a full state (holes + pending transaction) ----
		{
			UnrealGamePatcher::FPatcherState State;
			UnrealGamePatcher::FHole Hole0;
			Hole0.File = TEXT("Content/Paks/game.pak");
			Hole0.Offset = 5300306;
			Hole0.Size = 586;
			State.Holes.Add(Hole0);
			State.Holes.Add({ 5301964, 441 });
			State.Transaction.Status = UnrealGamePatcher::ETransactionStatus::Pending;
			State.Transaction.ContentVersion = TEXT("v123");
			State.LastVerifiedAt = TEXT("2026-09-06T00:00:00.000Z");
			UnrealGamePatcher::FTransactionTarget OverwriteTarget;
			OverwriteTarget.File = TEXT("Content/Paks/game.pak");
			OverwriteTarget.Op = UnrealGamePatcher::EApplyOp::Overwrite;
			OverwriteTarget.Offset = 5000188;
			OverwriteTarget.Size = 300000;
			OverwriteTarget.OriginalHash = TEXT("c9393344fbc1cf3d495f87e9f8d5d18b90597b1a");
			OverwriteTarget.NewHash = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
			OverwriteTarget.BackupPath = TEXT("Content/Paks/.backup/game.pak.0.bak");
			OverwriteTarget.bPreExisted = true;
			State.Transaction.Targets.Add(OverwriteTarget);
			UnrealGamePatcher::FTransactionTarget AppendTarget;
			AppendTarget.File = TEXT("Content/Paks/game.pak");
			AppendTarget.Op = UnrealGamePatcher::EApplyOp::Append;
			AppendTarget.Offset = 5300896;
			AppendTarget.Size = 840;
			AppendTarget.OriginalHash = TEXT("");
			AppendTarget.NewHash = TEXT("1b106f5a3c11df0c6fe11796f4e7cfb62420ac29");
			State.Transaction.Targets.Add(AppendTarget);

			Check(UnrealGamePatcher::FPatcherStateFile::Save(StatePath, State), TEXT("save full state"));

			UnrealGamePatcher::FPatcherState Loaded;
			Check(UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Loaded), TEXT("load full state"));
			Check(Loaded.Holes.Num() == 2 &&
				Loaded.Holes[0].Offset == 5300306 && Loaded.Holes[0].Size == 586 &&
				Loaded.Holes[1].Offset == 5301964 && Loaded.Holes[1].Size == 441,
				TEXT("holes roundtrip"));
			Check(Loaded.Transaction.Status == UnrealGamePatcher::ETransactionStatus::Pending &&
				Loaded.Transaction.ContentVersion == TEXT("v123"),
				TEXT("txn status/version roundtrip"));
			Check(Loaded.Transaction.Targets.Num() == 2 &&
				Loaded.Transaction.Targets[0].File == TEXT("Content/Paks/game.pak") &&
				Loaded.Transaction.Targets[0].Op == UnrealGamePatcher::EApplyOp::Overwrite &&
				Loaded.Transaction.Targets[0].Offset == 5000188 &&
				Loaded.Transaction.Targets[0].Size == 300000 &&
				Loaded.Transaction.Targets[0].OriginalHash == TEXT("c9393344fbc1cf3d495f87e9f8d5d18b90597b1a") &&
				Loaded.Transaction.Targets[0].BackupPath == TEXT("Content/Paks/.backup/game.pak.0.bak"),
				TEXT("overwrite target roundtrip"));
			Check(Loaded.Transaction.Targets[1].Op == UnrealGamePatcher::EApplyOp::Append &&
				Loaded.Transaction.Targets[1].Offset == 5300896 &&
				Loaded.Transaction.Targets[1].NewHash == TEXT("1b106f5a3c11df0c6fe11796f4e7cfb62420ac29"),
				TEXT("append target roundtrip"));
			// Field-level roundtrips the original section skipped:
			// LastVerifiedAt (drives the VerifyEveryDays trigger), the hole's
			// file id, bPreExisted (decides restore vs remove on rollback),
			// and the committed/rolled-back statuses + replace/delete ops.
			Check(Loaded.Holes[0].File == TEXT("Content/Paks/game.pak"), TEXT("hole file id roundtrip"));
			Check(Loaded.Transaction.Targets[0].bPreExisted, TEXT("bPreExisted roundtrip"));
			Check(Loaded.Transaction.Targets[1].bPreExisted == false, TEXT("bPreExisted=false roundtrip"));
			Check(!Loaded.LastVerifiedAt.IsEmpty(), TEXT("LastVerifiedAt roundtrip (present)"));

			// committed + replace-file/delete targets.
			UnrealGamePatcher::FPatcherState Committed;
			Committed.Transaction.Status = UnrealGamePatcher::ETransactionStatus::Committed;
			Committed.Transaction.ContentVersion = TEXT("v124");
			UnrealGamePatcher::FTransactionTarget ReplaceTarget;
			ReplaceTarget.File = TEXT("readme.txt");
			ReplaceTarget.Op = UnrealGamePatcher::EApplyOp::ReplaceFile;
			ReplaceTarget.bPreExisted = true;
			Committed.Transaction.Targets.Add(ReplaceTarget);
			UnrealGamePatcher::FTransactionTarget DeleteTarget;
			DeleteTarget.File = TEXT("old.ini");
			DeleteTarget.Op = UnrealGamePatcher::EApplyOp::Delete;
			DeleteTarget.bPreExisted = true;
			Committed.Transaction.Targets.Add(DeleteTarget);
			Check(UnrealGamePatcher::FPatcherStateFile::Save(StatePath, Committed), TEXT("save committed state"));
			UnrealGamePatcher::FPatcherState LoadedCommitted;
			Check(UnrealGamePatcher::FPatcherStateFile::Load(StatePath, LoadedCommitted), TEXT("load committed state"));
			Check(LoadedCommitted.Transaction.Status == UnrealGamePatcher::ETransactionStatus::Committed &&
				LoadedCommitted.Transaction.Targets.Num() == 2 &&
				LoadedCommitted.Transaction.Targets[0].Op == UnrealGamePatcher::EApplyOp::ReplaceFile &&
				LoadedCommitted.Transaction.Targets[1].Op == UnrealGamePatcher::EApplyOp::Delete,
				TEXT("committed status + replace/delete ops roundtrip"));

			// rolled-back status + LastVerifiedAt value roundtrip.
			UnrealGamePatcher::FPatcherState RolledBack;
			RolledBack.Transaction.Status = UnrealGamePatcher::ETransactionStatus::RolledBack;
			RolledBack.LastVerifiedAt = TEXT("2026-01-02T03:04:05.000Z");
			Check(UnrealGamePatcher::FPatcherStateFile::Save(StatePath, RolledBack), TEXT("save rolled-back state"));
			UnrealGamePatcher::FPatcherState LoadedRolled;
			Check(UnrealGamePatcher::FPatcherStateFile::Load(StatePath, LoadedRolled), TEXT("load rolled-back state"));
			Check(LoadedRolled.Transaction.Status == UnrealGamePatcher::ETransactionStatus::RolledBack &&
				LoadedRolled.LastVerifiedAt == TEXT("2026-01-02T03:04:05.000Z"),
				TEXT("rolled-back status + LastVerifiedAt value roundtrip"));
		}

		// --- 2. Empty state roundtrip --------------------------------------
		{
			UnrealGamePatcher::FPatcherState Empty;
			Check(UnrealGamePatcher::FPatcherStateFile::Save(StatePath, Empty), TEXT("save empty state"));
			UnrealGamePatcher::FPatcherState Loaded;
			Check(UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Loaded), TEXT("load empty state"));
			Check(Loaded.Holes.Num() == 0 &&
				Loaded.Transaction.Status == UnrealGamePatcher::ETransactionStatus::None &&
				Loaded.Transaction.Targets.Num() == 0,
				TEXT("empty state roundtrip"));
		}

		// --- 3. Corruption detection ---------------------------------------
		{
			// 3a. Not JSON at all.
			Check(FFileHelper::SaveStringToFile(TEXT("{ not valid json"), *StatePath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write corrupt (bad json)"));
			UnrealGamePatcher::FPatcherState Dummy;
			Check(!UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Dummy), TEXT("reject bad json"));

			// 3b. Valid JSON, wrong schema (format missing).
			Check(FFileHelper::SaveStringToFile(TEXT("{\"holes\":[]}"), *StatePath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write wrong schema"));
			Check(!UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Dummy), TEXT("reject missing format"));

			// 3c. Valid JSON, wrong format name.
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"SomeOther.state\",\"version\":1,\"holes\":[]}"), *StatePath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write wrong format"));
			Check(!UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Dummy), TEXT("reject wrong format"));

			// 3d. Wrong version number.
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"UnrealGamePatcher.state\",\"version\":99,\"holes\":[]}"), *StatePath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write wrong version"));
			Check(!UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Dummy), TEXT("reject wrong version"));

			// 3e. Truncated JSON (cut mid-string).
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"UnrealGamePatcher.state\",\"version\":1,\"holes\":[{\"offset\":5"), *StatePath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write truncated json"));
			Check(!UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Dummy), TEXT("reject truncated json"));

			// 3f. Missing file.
			PF.DeleteFile(*StatePath);
			Check(!UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Dummy), TEXT("reject missing file"));

			// 3g. Valid file, corrupt transaction (bad target type).
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"UnrealGamePatcher.state\",\"version\":1,\"holes\":[],\"transaction\":{\"status\":\"pending\",\"targets\":[42]}}"),
				*StatePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write corrupt txn target"));
			Check(!UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Dummy), TEXT("reject corrupt txn target"));
		}

		// Clean up the work dir.
		PF.DeleteDirectoryRecursively(*WorkDir);

		return FinishTest(bAllPassed, TEXT("statetest"));
	
	}


	static int32 RunIndexTest(const TCHAR* /*CmdLine*/)
	{

		// Extension manifest acceptance (task 3.2): roundtrip consistency,
		// deterministic output, and tamper/corruption detection via the
		// embedded XXH3-64 self-check. Works on a temp dir, leaves nothing.
		bool bAllPassed = true;
		const FString WorkDir = GetTestWorkDir(TEXT("indextest"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		const FString IndexPath = FPaths::Combine(WorkDir, TEXT("UnrealGamePatcher.index.json"));

		auto Check = MakeCheck(bAllPassed);

		// Sample data mirrors a real ingest output (the staged fixture's
		// pakchunk1-Windows.pak, 2 entries / 939 bytes).
		UnrealGamePatcher::FIndexManifest Sample;
		Sample.AppId = TEXT("IOStoreDemo");
		Sample.ContentVersion = TEXT("1.0.0");
		Sample.BaseVersion = TEXT("0.9.0");
		Sample.Files.Add(TEXT("pakchunk1-Windows.pak"),
			UnrealGamePatcher::FIndexFileEntry{ TEXT("5a9f1eb0c1fc6268b90ee71f32ce5f7ed9c2c986"), 939 });
		Sample.Files.Add(TEXT("pakchunk1-Windows.ucas"),
			UnrealGamePatcher::FIndexFileEntry{ TEXT("293049e4b3aa98b1814d9f3f18ac3e7aff606436"), 10348176 });
		Sample.Files.Add(TEXT("pakchunk1-Windows.utoc"),
			UnrealGamePatcher::FIndexFileEntry{ TEXT("4c9af782ffcbfe5e524fc6bef14194e6705008d2"), 19405 });
		Sample.Chunks.Add(TEXT("C5C97C8A3F1F1B2F5BB35C0C46A380340408FB2A"),
			UnrealGamePatcher::FIndexChunkEntry{ TEXT("C5C97C8A3F1F1B2F5BB35C0C46A380340408FB2A"), 1048638, TEXT("chunks/C5C97C8A3F1F1B2F5BB35C0C46A380340408FB2A.chunk") });
		Sample.Chunks.Add(TEXT("0206B8FC805A30FECA3BFC686F06E87835134D75"),
			UnrealGamePatcher::FIndexChunkEntry{ TEXT("0206B8FC805A30FECA3BFC686F06E87835134D75"), 895382, TEXT("chunks/0206B8FC805A30FECA3BFC686F06E87835134D75.chunk") });

		// --- 1. Full roundtrip (files + chunks + baseVersion) ----
		{
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("save full manifest"));

			UnrealGamePatcher::FIndexManifest Loaded;
			Check(UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Loaded), TEXT("load full manifest"));
			Check(Loaded.AppId == TEXT("IOStoreDemo") &&
				Loaded.ContentVersion == TEXT("1.0.0") &&
				Loaded.BaseVersion == TEXT("0.9.0"),
				TEXT("header fields roundtrip"));
			Check(Loaded.Files.Num() == 3 &&
				Loaded.Files[TEXT("pakchunk1-Windows.pak")].SHA1 == TEXT("5a9f1eb0c1fc6268b90ee71f32ce5f7ed9c2c986") &&
				Loaded.Files[TEXT("pakchunk1-Windows.pak")].Size == 939 &&
				Loaded.Files[TEXT("pakchunk1-Windows.ucas")].Size == 10348176,
				TEXT("file entries roundtrip"));
			Check(Loaded.Chunks.Num() == 2 &&
				Loaded.Chunks[TEXT("0206B8FC805A30FECA3BFC686F06E87835134D75")].SHA1 == TEXT("0206B8FC805A30FECA3BFC686F06E87835134D75") &&
				Loaded.Chunks[TEXT("0206B8FC805A30FECA3BFC686F06E87835134D75")].Size == 895382 &&
				Loaded.Chunks[TEXT("0206B8FC805A30FECA3BFC686F06E87835134D75")].File == TEXT("chunks/0206B8FC805A30FECA3BFC686F06E87835134D75.chunk"),
				TEXT("chunk entries roundtrip"));
		}

		// --- 2. Empty manifest roundtrip -----------------------------------
		{
			UnrealGamePatcher::FIndexManifest Empty;
			Empty.AppId = TEXT("IOStoreDemo");
			Empty.ContentVersion = TEXT("0.0.1");
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Empty), TEXT("save empty manifest"));
			UnrealGamePatcher::FIndexManifest Loaded;
			Check(UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Loaded), TEXT("load empty manifest"));
			Check(Loaded.Files.Num() == 0 && Loaded.Chunks.Num() == 0,
				TEXT("empty manifest roundtrip"));
		}

		// --- 2b. Container metadata + parts + verifyOnUpdate roundtrip -----
		{
			// The v2 optional per-file fields (parts / pak entries / iostore
			// chunk tables) and the per-version verifyOnUpdate flag are
			// load-bearing (chunk reuse + in-place patching + forced verify)
			// - roundtrip them explicitly.
			UnrealGamePatcher::FIndexManifest Rich;
			Rich.AppId = TEXT("IOStoreDemo");
			Rich.ContentVersion = TEXT("0.0.2");
			Rich.bVerifyOnUpdate = true;

			UnrealGamePatcher::FIndexFileEntry Pak;
			Pak.Size = 1000;
			Pak.SHA1 = TEXT("1111111111111111111111111111111111111111");
			UnrealGamePatcher::FIndexPakEntry PE;
			PE.Path = TEXT("Game/Content/file.uasset");
			PE.OffsetInFile = 176;
			PE.StoredSize = 800;
			PE.UncompressedSize = 800;
			PE.SHA1 = TEXT("2222222222222222222222222222222222222222");
			Pak.PakEntries.Add(PE);

			UnrealGamePatcher::FIndexFileEntry Ucas;
			Ucas.Size = 2000;
			Ucas.SHA1 = TEXT("3333333333333333333333333333333333333333");
			UnrealGamePatcher::FIndexIoStoreChunk IC;
			IC.Id = TEXT("AAAABBBBCCCCDDDDEEEEFFFF");
			IC.RawOffset = 176;
			IC.RawSize = 900;
			Ucas.IoStoreChunks.Add(IC);
			UnrealGamePatcher::FIndexFileChunkRef Part;
			Part.Chunk = TEXT("0206B8FC805A30FECA3BFC686F06E87835134D75");
			Part.Offset = 0;
			Part.Size = 500;
			Ucas.Parts.Add(Part);

			Rich.Files.Add(TEXT("game.pak"), Pak);
			Rich.Files.Add(TEXT("game.ucas"), Ucas);

			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Rich), TEXT("save rich manifest"));
			UnrealGamePatcher::FIndexManifest Loaded;
			Check(UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Loaded), TEXT("load rich manifest"));
			Check(Loaded.bVerifyOnUpdate, TEXT("verifyOnUpdate roundtrip"));
			Check(Loaded.Files.Contains(TEXT("game.pak")) && Loaded.Files[TEXT("game.pak")].PakEntries.Num() == 1 &&
				Loaded.Files[TEXT("game.pak")].PakEntries[0].SHA1 == PE.SHA1 &&
				Loaded.Files[TEXT("game.pak")].PakEntries[0].Path == PE.Path,
				TEXT("pak entries roundtrip"));
			Check(Loaded.Files.Contains(TEXT("game.ucas")) && Loaded.Files[TEXT("game.ucas")].IoStoreChunks.Num() == 1 &&
				Loaded.Files[TEXT("game.ucas")].IoStoreChunks[0].Id == IC.Id &&
				Loaded.Files[TEXT("game.ucas")].Parts.Num() == 1 &&
				Loaded.Files[TEXT("game.ucas")].Parts[0].Chunk == Part.Chunk,
				TEXT("ioChunks + parts roundtrip"));

			// Malformed container metadata must be rejected (strict schema).
			FString RichJson;
			Check(FFileHelper::LoadFileToString(RichJson, *IndexPath), TEXT("read rich manifest json"));
			FString BadJson = RichJson.Replace(TEXT("\"storedSize\":"), TEXT("\"storedSizeX\":"));
			Check(FFileHelper::SaveStringToFile(BadJson, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM),
				TEXT("write malformed entries"));
			UnrealGamePatcher::FIndexManifest Bad;
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Bad), TEXT("reject malformed pak entries"));
		}

		// --- 3. Deterministic output (two saves are byte-identical) --------
		{
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("save full manifest (2nd time)"));
			FString Bytes1, Bytes2;
			Check(FFileHelper::LoadFileToString(Bytes1, *IndexPath), TEXT("read 2nd save"));
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("save full manifest (3rd time)"));
			Check(FFileHelper::LoadFileToString(Bytes2, *IndexPath), TEXT("read 3rd save"));
			Check(Bytes1 == Bytes2, TEXT("deterministic serialization (same bytes across saves)"));
		}

		// --- 4. Tamper detection (self-check catches every modification) ---
		{
			// 4a. Flip one char of a file sha1 (locate by the unique value, so
			//     the test is robust to JSON field ordering).
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("save clean manifest"));
			FString Json;
			Check(FFileHelper::LoadFileToString(Json, *IndexPath), TEXT("read clean manifest"));
			const FString Sha1Val = TEXT("5a9f1eb0c1fc6268b90ee71f32ce5f7ed9c2c986");
			const int32 Sha1Pos = Json.Find(Sha1Val);
			Check(Sha1Pos != INDEX_NONE, TEXT("locate file sha1 value"));
			Json[Sha1Pos + 39] = (Json[Sha1Pos + 39] == TEXT('6')) ? TEXT('7') : TEXT('6');
			Check(FFileHelper::SaveStringToFile(Json, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write tampered sha1"));
			UnrealGamePatcher::FIndexManifest Dummy;
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject tampered file sha1"));

			// 4b. Change contentVersion value (unique substring "1.0.0").
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("re-save clean manifest"));
			Check(FFileHelper::LoadFileToString(Json, *IndexPath), TEXT("re-read clean manifest"));
			Json = Json.Replace(TEXT("1.0.0"), TEXT("1.0.1"));
			Check(FFileHelper::SaveStringToFile(Json, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write tampered contentVersion"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject tampered contentVersion"));

			// 4c. Change a chunk size number (unique value).
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("re-save clean manifest"));
			Check(FFileHelper::LoadFileToString(Json, *IndexPath), TEXT("re-read clean manifest"));
			const int32 SzPos = Json.Find(TEXT("895382"));
			Check(SzPos != INDEX_NONE, TEXT("locate chunk size value"));
			Json = Json.Left(SzPos) + TEXT("895383") + Json.RightChop(SzPos + 6);
			Check(FFileHelper::SaveStringToFile(Json, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write tampered chunk size"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject tampered chunk size"));

			// 4d. Strip the selfCheck field entirely (locate by key, then drop
			//     the field plus its neighbouring comma).
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("re-save clean manifest"));
			Check(FFileHelper::LoadFileToString(Json, *IndexPath), TEXT("re-read clean manifest"));
			const int32 ScPos = Json.Find(TEXT("\"selfCheck\""));
			Check(ScPos != INDEX_NONE, TEXT("locate selfCheck key"));
			const int32 ValStart = Json.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, ScPos + 11);  // opening quote of the value
			Check(ValStart != INDEX_NONE, TEXT("locate selfCheck value start"));
			const int32 ValEnd = Json.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, ValStart + 1);  // closing quote of the value
			Check(ValEnd != INDEX_NONE, TEXT("locate selfCheck value end"));
			int32 FieldStart = ScPos;
			if (FieldStart > 0 && Json[FieldStart - 1] == TEXT(',')) { FieldStart -= 1; }
			int32 FieldEnd = ValEnd + 1;
			if (FieldEnd < Json.Len() && Json[FieldEnd] == TEXT(',')) { FieldEnd += 1; }
			Json = Json.Left(FieldStart) + Json.RightChop(FieldEnd);
			Check(FFileHelper::SaveStringToFile(Json, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write manifest without selfCheck"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject missing selfCheck"));
		}

		// --- 4e. Legacy-layout fallback (a pre-condensed serializer embeds
		//        the hash over its own byte layout, e.g. pretty-printed) ----
		{
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("save clean manifest (legacy case)"));
			FString Json;
			Check(FFileHelper::LoadFileToString(Json, *IndexPath), TEXT("read clean manifest (legacy case)"));
			TSharedPtr<FJsonObject> Root;
			{
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
				Check(FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid(), TEXT("parse clean manifest (legacy case)"));
			}
			// Re-serialize without selfCheck under a pretty writer: the byte
			// layout a legacy publisher could have produced (LINE_TERMINATOR
			// + tabs). The hash goes over exactly these bytes.
			Root->Values.Remove(TEXT("selfCheck"));
			FString PrettyBody;
			{
				const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&PrettyBody);
				Check(FJsonSerializer::Serialize(Root.ToSharedRef(), Writer), TEXT("serialize pretty body (legacy case)"));
			}
			Root->SetStringField(TEXT("selfCheck"), UnrealGamePatcher::JsonSelfCheckHex(PrettyBody));
			FString PrettyFull;
			{
				const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&PrettyFull);
				Check(FJsonSerializer::Serialize(Root.ToSharedRef(), Writer), TEXT("serialize pretty full (legacy case)"));
			}
			Check(FFileHelper::SaveStringToFile(PrettyFull, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write legacy-layout manifest"));
			// Canonical re-serialization cannot reproduce the pretty bytes;
			// the raw-body fallback must accept the file.
			UnrealGamePatcher::FIndexManifest LegacyLoaded;
			Check(UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, LegacyLoaded), TEXT("load legacy-layout manifest (raw-body fallback)"));
			Check(LegacyLoaded.Files.Num() == Sample.Files.Num() &&
				LegacyLoaded.Chunks.Num() == Sample.Chunks.Num(),
				TEXT("legacy manifest data intact"));

			// Tampering a legacy-layout file must still be caught: the raw
			// hash over the modified bytes no longer matches.
			Check(FFileHelper::LoadFileToString(Json, *IndexPath), TEXT("re-read legacy manifest"));
			Json = Json.Replace(TEXT("1.0.0"), TEXT("1.0.1"));
			Check(FFileHelper::SaveStringToFile(Json, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write tampered legacy manifest"));
			UnrealGamePatcher::FIndexManifest LegacyDummy;
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, LegacyDummy), TEXT("reject tampered legacy-layout manifest"));
		}

		// --- 5. Corruption / schema detection ------------------------------
		{
			UnrealGamePatcher::FIndexManifest Dummy;
			// 5a. Not JSON at all.
			Check(FFileHelper::SaveStringToFile(TEXT("{ not valid json"), *IndexPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write corrupt (bad json)"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject bad json"));

			// 5b. Wrong format name.
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"Other.index\",\"version\":1}"), *IndexPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write wrong format"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject wrong format"));

			// 5c. Wrong version number.
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"UnrealGamePatcher.index\",\"version\":99}"), *IndexPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write wrong version"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject wrong version"));

			// 5d. Missing required appId.
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"UnrealGamePatcher.index\",\"version\":1,\"contentVersion\":\"1\",\"files\":{},\"chunks\":{}}"),
				*IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write missing appId"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject missing appId"));

			// 5e. (updateStrategy removed from the format. A hand-crafted
			// "legacy" file cannot pass the self-check anyway - the embedded
			// hash binds the exact body, and re-serialization drops unknown
			// fields - so there is no meaningful compat case: v2 manifests
			// written by the current publisher simply lack the field, which
			// section 1 covers.)

			// 5f. File entry with non-hex sha1.
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"UnrealGamePatcher.index\",\"version\":1,\"appId\":\"IOStoreDemo\",\"contentVersion\":\"1\",\"files\":{\"a.bin\":{\"sha1\":\"nothex\",\"size\":5}},\"chunks\":{}}"),
				*IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write bad file sha1"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject non-hex file sha1"));

			// 5g. Missing file.
			PF.DeleteFile(*IndexPath);
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject missing file"));
		}

		// Clean up the work dir.
		PF.DeleteDirectoryRecursively(*WorkDir);

		return FinishTest(bAllPassed, TEXT("indextest"));
	
	}


	static int32 RunDiffTest(const TCHAR* /*CmdLine*/)
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


	static int32 RunBuild(const TCHAR* CmdLine)
	{

		// Build-side ingest (self-designed): staged build directory -> CDN
		// layout (1 MiB chunks + deterministic extension manifest).
		FString StagedDir, AppId, Version, OutCdn;
		FParse::Value(CmdLine, TEXT("-staged="), StagedDir);
		FParse::Value(CmdLine, TEXT("-app="), AppId);
		FParse::Value(CmdLine, TEXT("-version="), Version);
		FParse::Value(CmdLine, TEXT("-out="), OutCdn);
		if (StagedDir.IsEmpty() || AppId.IsEmpty() || Version.IsEmpty() || OutCdn.IsEmpty())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("build requires -staged=<staged build dir> -app=<name> -version=<v> -out=<cdn root>"));
			return ExitUsage;
		}

		UnrealGamePatcher::FIngestResult Result;
		if (!UnrealGamePatcher::FBuildIngestor::Ingest(StagedDir, AppId, Version, OutCdn, Result))
		{
			return ExitFailure;
		}
		// Publish the latest-version announcement LAST (chunks + index already
		// on the CDN): a client polling index/latest.json only ever sees a
		// complete version. Advisory file - the index itself still self-checks.
		UnrealGamePatcher::FLatestVersion Latest;
		Latest.Version = Version;
		if (!UnrealGamePatcher::FLatestVersionFile::Save(UnrealGamePatcher::FLatestVersionFile::GetPath(OutCdn), Latest))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("build: failed writing latest.json"));
			return ExitFailure;
		}
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("build: latest.json -> %s"), *UnrealGamePatcher::FLatestVersionFile::GetPath(OutCdn));
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("build: %d files, %d chunks, %lld chunk bytes -> %s"),
			Result.NumFiles, Result.NumChunks, Result.NumChunkBytesWritten, *Result.IndexPath);
		return ExitSuccess;
	
	}


	static int32 RunBuildTest(const TCHAR* /*CmdLine*/)
	{

		// Ingest acceptance (task 4.1): determinism (two ingests of the same
		// input produce byte-identical manifests), file hashes equal to the
		// staged files on disk, chunk files present with the recorded sizes,
		// and idempotent re-ingest into the same directory.
		bool bAllPassed = true;
		auto Check = MakeCheck(bAllPassed);

		using namespace UnrealGamePatcher;

		const FString WorkDir = GetTestWorkDir(TEXT("buildtest"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

		// Staged fixture: the real small containers + a plain file.
		const FString ProjectRoot = FPaths::GetPath(FPaths::GetPath(FPaths::GetPath(FPlatformProcess::BaseDir())));
		const FString StagedSrc = FPaths::Combine(ProjectRoot, TEXT(".manifesttest"), TEXT("chunkinstall"),
			TEXT("Windows"), TEXT("1.0.0"), TEXT("pakchunk1"));
		const FString StagedDir = FPaths::Combine(WorkDir, TEXT("staged"));
		PF.CreateDirectoryTree(*StagedDir);
		{
			for (const TCHAR* Name : { TEXT("pakchunk1-Windows.pak"), TEXT("pakchunk1-Windows.ucas"), TEXT("pakchunk1-Windows.utoc") })
			{
				Check(PF.CopyFile(*(FPaths::Combine(StagedDir, Name)), *(FPaths::Combine(StagedSrc, Name))),
					*FString::Printf(TEXT("copy staged %s"), Name));
			}
			Check(FFileHelper::SaveStringToFile(TEXT("plain file content"), *(FPaths::Combine(StagedDir, TEXT("boot.txt")))), TEXT("write staged plain file"));
		}

		// --- 1. Ingest twice into two different CDN roots -------------------
		FIngestResult R1, R2;
		const FString CdnA = FPaths::Combine(WorkDir, TEXT("cdnA"));
		const FString CdnB = FPaths::Combine(WorkDir, TEXT("cdnB"));
		Check(FBuildIngestor::Ingest(StagedDir, TEXT("IOStoreDemo"), TEXT("1.0.0"), CdnA, R1), TEXT("ingest #1 succeeds"));
		Check(FBuildIngestor::Ingest(StagedDir, TEXT("IOStoreDemo"), TEXT("1.0.0"), CdnB, R2), TEXT("ingest #2 succeeds"));
		Check(R1.NumFiles == 4 && R1.NumChunks > 0, TEXT("ingest: 4 files, chunks produced"));

		// --- 2. Determinism: byte-identical index files ---------------------
		{
			FString BytesA, BytesB;
			const bool bReadBoth = FFileHelper::LoadFileToString(BytesA, *R1.IndexPath) &&
				FFileHelper::LoadFileToString(BytesB, *R2.IndexPath);
			Check(bReadBoth && BytesA == BytesB, TEXT("deterministic: two ingests byte-identical"));
		}

		// --- 3. Index loads; hashes match the staged files; container
		// metadata extracted directly from the staged containers -------------
		{
			FIndexManifest Index;
			Check(FIndexManifestFile::Load(R1.IndexPath, Index), TEXT("ingested index loads + passes self-check"));
			Check(Index.Chunks.Num() == R1.NumChunks, TEXT("index chunk count matches result"));

			const FIndexFileEntry* Pak = Index.Files.Find(TEXT("pakchunk1-Windows.pak"));
			Check(Pak && Pak->PakEntries.Num() == 2, TEXT("pak entry metadata extracted (2 entries)"));
			const FIndexFileEntry* Ucas = Index.Files.Find(TEXT("pakchunk1-Windows.ucas"));
			Check(Ucas && Ucas->IoStoreChunks.Num() > 0, TEXT("ucas IoStore metadata extracted"));

			// Whole-file SHA1 == staged files on disk.
			bool bDiskMatch = true;
			for (const TPair<FString, FIndexFileEntry>& Pair : Index.Files)
			{
				const FString Src = FPaths::Combine(StagedDir, Pair.Key);
				FString Sha;
				int64 Size = 0;
				if (!FLocalTruthExtractor::HashWholeFile(Src, Sha, Size) || Sha != Pair.Value.SHA1 || Size != Pair.Value.Size)
				{
					bDiskMatch = false;
					break;
				}
			}
			Check(bDiskMatch, TEXT("extension manifest file hashes == staged files on disk"));
		}

		// --- 4. Chunk files present in the CDN layout with recorded sizes ---
		{
			FIndexManifest Index;
			FIndexManifestFile::Load(R1.IndexPath, Index);
			bool bAllChunks = Index.Chunks.Num() > 0;
			for (const TPair<FString, FIndexChunkEntry>& Pair : Index.Chunks)
			{
				const FIndexChunkEntry& E = Pair.Value;
				if (E.File.IsEmpty())
				{
					bAllChunks = false;
					break;
				}
				const FString ChunkPath = FPaths::Combine(CdnA, E.File);
				const int64 Size = IFileManager::Get().FileSize(*ChunkPath);
				if (Size != E.Size)
				{
					bAllChunks = false;
					break;
				}
			}
			Check(bAllChunks, TEXT("all chunk files staged with recorded sizes"));
		}

		// --- 5. Idempotent re-ingest into the same CDN root -----------------
		{
			FIngestResult R3;
			Check(FBuildIngestor::Ingest(StagedDir, TEXT("IOStoreDemo"), TEXT("1.0.0"), CdnA, R3), TEXT("re-ingest into same CDN succeeds"));
			FString BytesA, BytesB;
			const bool bReadBoth = FFileHelper::LoadFileToString(BytesA, *R1.IndexPath) &&
				FFileHelper::LoadFileToString(BytesB, *R3.IndexPath);
			Check(bReadBoth && BytesA == BytesB, TEXT("re-ingest leaves index byte-identical"));
		}

		// --- 6. C6 regression: an EMPTY staged dir must be refused -----------
		// Publishing a zero-file index classifies EVERY file of every client
		// install as Removed - the next update deletes the whole install
		// (live-reproduced during the code review).
		{
			const FString EmptyStaged = FPaths::Combine(WorkDir, TEXT("empty_staged"));
			PF.CreateDirectoryTree(*EmptyStaged);
			FIngestResult REmpty;
			Check(!FBuildIngestor::Ingest(EmptyStaged, TEXT("IOStoreDemo"), TEXT("9.9.9"), FPaths::Combine(WorkDir, TEXT("empty_cdn")), REmpty),
				TEXT("buildtest: EMPTY staged dir is refused (install-wipe guard)"));
			// A trailing path separator must not corrupt file ids either
			// (RightChop(Dir.Len()+1) ate the filename's first character).
			const FString SepStaged = StagedDir + TEXT("/");
			FIngestResult RSep;
			Check(FBuildIngestor::Ingest(SepStaged, TEXT("IOStoreDemo"), TEXT("9.9.9"), FPaths::Combine(WorkDir, TEXT("sep_cdn")), RSep),
				TEXT("buildtest: trailing-separator -staged= ingests"));
			FIndexManifest SepIndex;
			bool bSepOk = FIndexManifestFile::Load(RSep.IndexPath, SepIndex);
			if (bSepOk)
			{
				for (const TPair<FString, FIndexFileEntry>& Pair : SepIndex.Files)
				{
					// No id may start with a separator or a stray fragment.
					if (Pair.Key.StartsWith(TEXT("/")) || Pair.Key.StartsWith(TEXT("\\")) ||
						Pair.Key.StartsWith(TEXT(".")))
					{
						bSepOk = false;
					}
				}
			}
			Check(bSepOk && SepIndex.Files.Num() > 0, TEXT("buildtest: trailing-separator ids are clean relative paths"));
		}

		// --- 7. CDC shift regression: content-defined boundaries survive
		// insertions. A 1-byte insert at offset 0 must re-chunk only the
		// first window - the Gear state re-synchronizes (it depends on the
		// last 64 bytes only), so every following cut point lands at the
		// same content position and all later chunks keep their identity.
		// Fixed-size windows would shift wholesale and re-chunk everything.
		{
			const FString BigStaged = FPaths::Combine(WorkDir, TEXT("big_staged"));
			PF.CreateDirectoryTree(*BigStaged);
			const int64 BigSize = 12 << 20; // 12 MB - spans many CDC chunks
			TArray64<uint8> Big;
			Big.SetNumUninitialized(BigSize);
			uint64 Seed = 0x123456789ABCDEFull;
			for (int64 I = 0; I < BigSize; ++I)
			{
				Seed = Seed * 6364136223846793005ull + 1442695040888963407ull;
				Big[I] = (uint8)(Seed >> 33);
			}
			Check(FFileHelper::SaveArrayToFile(Big, *(FPaths::Combine(BigStaged, TEXT("big.bin")))), TEXT("cdcshift: write big file v1"));

			FIngestResult RV1;
			const FString CdnCdc1 = FPaths::Combine(WorkDir, TEXT("cdncdc1"));
			Check(FBuildIngestor::Ingest(BigStaged, TEXT("IOStoreDemo"), TEXT("1.0.0"), CdnCdc1, RV1), TEXT("cdcshift: ingest v1"));

			// v2: insert one byte at offset 0.
			TArray64<uint8> Shifted;
			Shifted.SetNumUninitialized(BigSize + 1);
			Shifted[0] = 0x5A;
			FMemory::Memcpy(Shifted.GetData() + 1, Big.GetData(), BigSize);
			Check(FFileHelper::SaveArrayToFile(Shifted, *(FPaths::Combine(BigStaged, TEXT("big.bin")))), TEXT("cdcshift: write big file v2 (1 byte inserted at 0)"));

			FIngestResult RV2;
			const FString CdnCdc2 = FPaths::Combine(WorkDir, TEXT("cdncdc2"));
			Check(FBuildIngestor::Ingest(BigStaged, TEXT("IOStoreDemo"), TEXT("1.0.1"), CdnCdc2, RV2), TEXT("cdcshift: ingest v2"));

			FIndexManifest I1, I2;
			Check(FIndexManifestFile::Load(RV1.IndexPath, I1) && FIndexManifestFile::Load(RV2.IndexPath, I2), TEXT("cdcshift: both indexes load"));
			int32 Shared = 0;
			for (const TPair<FString, FIndexChunkEntry>& P : I2.Chunks)
			{
				if (I1.Chunks.Contains(P.Key))
				{
					++Shared;
				}
			}
			const int32 NewChunks = I2.Chunks.Num() - Shared;
			Check(NewChunks <= 2,
				*FString::Printf(TEXT("cdcshift: 1-byte insert adds <= 2 chunks (got %d new of %d)"), NewChunks, I2.Chunks.Num()));
			Check((double)Shared >= 0.9 * (double)I2.Chunks.Num(),
				*FString::Printf(TEXT("cdcshift: >= 90%% of v2 chunks shared with v1 (%d/%d)"), Shared, I2.Chunks.Num()));
		}

		PF.DeleteDirectoryRecursively(*WorkDir);

		return FinishTest(bAllPassed, TEXT("buildtest"));
	
	}


	static int32 RunDiff(const TCHAR* CmdLine)
	{

		// Build-side diff (tasks 4.2/4.3): two extension manifests -> delta
		// manifest + incremental chunk directory.
		FString BaseIndex, TargetIndex, TargetCdn, OutDir;
		const bool bNoCopy = FParse::Param(CmdLine, TEXT("nocopy"));
		FParse::Value(CmdLine, TEXT("-base="), BaseIndex);
		FParse::Value(CmdLine, TEXT("-target="), TargetIndex);
		FParse::Value(CmdLine, TEXT("-targetcdn="), TargetCdn);
		FParse::Value(CmdLine, TEXT("-out="), OutDir);
		if (BaseIndex.IsEmpty() || TargetIndex.IsEmpty() || OutDir.IsEmpty())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("diff requires -base=<index.json> -target=<index.json> -out=<dir> [-targetcdn=<cdn root>] [-nocopy]"));
			return ExitUsage;
		}

		UnrealGamePatcher::FDiffOutput Out;
		if (!UnrealGamePatcher::FBuildDiffer::DiffIndexFiles(BaseIndex, TargetIndex, TargetCdn, OutDir, !bNoCopy, Out))
		{
			return ExitFailure;
		}
		return ExitSuccess;
	
	}


	static int32 RunDiffBuild(const TCHAR* /*CmdLine*/)
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
			Check(ViaUnion.Num() == Direct.Num(), TEXT("skip-version delta == union of step deltas"));
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


	static int32 RunPakDump(const TCHAR* CmdLine)
	{

		// Diagnostics: parse a pak and verify every entry's built-in SHA1 hash
		// against the re-read (and decompressed) payload. Used as the
		// acceptance harness for task 2.1.
		FString PakPath;
		FParse::Value(CmdLine, TEXT("-pak="), PakPath);
		if (PakPath.IsEmpty())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("pakdump requires -pak=<path>"));
			return ExitUsage;
		}

		UnrealGamePatcher::FPakReader Reader;
		if (!Reader.Open(PakPath))
		{
			return ExitFailure;
		}
		if (!Reader.ReadIndex())
		{
			return ExitFailure;
		}

		int32 Verified = 0;
		int32 Failed = 0;
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("Pak %s (version %d, mount '%s'): %d files"),
			*PakPath, Reader.GetInfo().Version, *Reader.GetMountPoint(), Reader.GetFiles().Num());
		for (const UnrealGamePatcher::FPakReader::FPakEntryLocation& Loc : Reader.GetFiles())
		{
			// Read (and decompress) the payload. For plain entries this doubles
			// as the "data readable" acceptance; for compressed entries it proves
			// decompression works and yields the same bytes as the source.
			TArray64<uint8> Data;
			const bool bRead = Reader.ReadEntryData(Loc.Entry, Data);

			// The built-in per-file hash stored in the payload header is the
			// SHA1 of the bytes written to the pak (see ComputePayloadHash):
			//   - uncompressed entries: the original data
			//   - compressed entries: the compressed block payloads with the
			//     32-byte inter-block alignment padding stripped
			// Compare the built-in hash against that stored-payload hash.
			uint8 BuiltIn[20] = { 0 };
			const bool bGotBuiltIn = Reader.ReadPayloadHash(Loc.Entry, BuiltIn);
			uint8 PayloadSha[20] = { 0 };
			const bool bComputed = Reader.ComputePayloadHash(Loc.Entry, PayloadSha);
			const bool bHashMatch = bRead && bGotBuiltIn && bComputed &&
				FMemory::Memcmp(PayloadSha, BuiltIn, 20) == 0;

			TStringBuilder<160> Hex;
			Hex << TEXT("payload=");
			for (int32 I = 0; I < 20; ++I)
			{
				Hex += FString::Printf(TEXT("%02x"), PayloadSha[I]);
			}
			if (bHashMatch) { ++Verified; }
			else { ++Failed; }

			UE_LOG(LogUnrealGamePatcher, Display, TEXT("  %-48s off=%8lld size=%8lld unc=%8lld method=%d blocks=%d read=%s hash=%s %s"),
				*Loc.Path, Loc.Entry.Offset, Loc.Entry.Size, Loc.Entry.UncompressedSize,
				Loc.Entry.CompressionMethodIndex, Loc.Entry.CompressionBlocks.Num(),
				bRead ? TEXT("ok") : TEXT("FAIL"), *Hex, bHashMatch ? TEXT("MATCH") : TEXT("MISMATCH"));
		}
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("pakdump: %d/%d entries verified (built-in SHA1 matches payload)."),
			Verified, Verified + Failed);
		return Failed == 0 ? ExitSuccess : ExitFailure;
	
	}


	static int32 RunPakPatch(const TCHAR* CmdLine)
	{

		// Test harness for FPakWriter: apply a set of in-place overwrites and
		// end appends described by a tab-separated ops file, then commit the
		// rebuilt directory/footer. Ops file lines:
		//   O <pak-path>      <source-file>              in-place overwrite (same stored size, uncompressed)
		//   A <pak-path>      <source-file>              append uncompressed entry
		//   C <pak-path>      <compressed-file> <method> <raw-size>   append compressed entry (single block)
		FString PakPath, OutPath, OpsPath;
		FParse::Value(CmdLine, TEXT("-pak="), PakPath);
		FParse::Value(CmdLine, TEXT("-out="), OutPath);
		FParse::Value(CmdLine, TEXT("-ops="), OpsPath);
		if (PakPath.IsEmpty() || OutPath.IsEmpty() || OpsPath.IsEmpty())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("pakpatch requires -pak=<src> -out=<dst> -ops=<file>"));
			return ExitUsage;
		}

		// Work on a copy so the source pak stays untouched. IFileManager::Copy
		// is unavailable in this Program context, so use the platform file API.
		if (!OutPath.Equals(PakPath, ESearchCase::IgnoreCase))
		{
			IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
			if (PF.FileExists(*OutPath))
			{
				PF.DeleteFile(*OutPath);
			}
			if (!PF.CopyFile(*OutPath, *PakPath))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("pakpatch: failed to copy '%s' to '%s'"), *PakPath, *OutPath);
				return ExitFailure;
			}
		}

		UnrealGamePatcher::FPakWriter Writer;
		if (!Writer.Open(OutPath))
		{
			return ExitFailure;
		}

		FString OpsText;
		if (!FFileHelper::LoadFileToString(OpsText, *OpsPath))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("pakpatch: cannot read ops file '%s'"), *OpsPath);
			return ExitFailure;
		}

		int32 OpsApplied = 0;
		TArray<FString> Lines;
		OpsText.ParseIntoArrayLines(Lines, /*bCullEmpty=*/true);
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("#"))) continue;
			TArray<FString> Fields;
			Line.ParseIntoArray(Fields, TEXT("\t"), true);
			if (Fields.Num() < 3)
			{
				// A malformed line must not be silently dropped - the ops file
				// is a patch spec; skipping one entry corrupts the result.
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("pakpatch: malformed ops line (need >= 3 tab fields): '%s'"), *Line);
				return ExitFailure;
			}

			const FString& Op = Fields[0];
			const FString& PakEntryPath = Fields[1];
			const FString& SrcFile = Fields[2];

			if (Op == TEXT("O"))
			{
				// CoW v2: "O" no longer overwrites payload in place (the writer
				// has no overwrite path at all). The new generation is appended
				// - dead ranges first, EOF fallback - which is size-agnostic.
				const UnrealGamePatcher::FPakReader::FPakEntryLocation* Existing = Writer.FindEntry(PakEntryPath);
				if (!Existing)
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("pakpatch: cannot overwrite '%s': not in pak"), *PakEntryPath);
					return ExitFailure;
				}
				TArray64<uint8> Data;
				if (!FFileHelper::LoadFileToArray(Data, *SrcFile))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("pakpatch: cannot read '%s'"), *SrcFile);
					return ExitFailure;
				}
				UnrealGamePatcher::FPakWriter::FEntryToWrite W;
				W.Path = PakEntryPath;
				W.CompressionMethodIndex = 0;
				W.UncompressedSize = Data.Num();
				W.Blocks.AddDefaulted();
				W.Blocks[0].Bytes = MoveTemp(Data);
				UnrealGamePatcher::FPakEntry OutEntry;
				if (!Writer.AppendEntry(W, OutEntry))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("pakpatch: cannot re-append '%s'"), *PakEntryPath);
					return ExitFailure;
				}
				++OpsApplied;
			}
			else if (Op == TEXT("A") || Op == TEXT("C"))
			{
				TArray64<uint8> Data;
				if (!FFileHelper::LoadFileToArray(Data, *SrcFile))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("pakpatch: cannot read '%s'"), *SrcFile);
					return ExitFailure;
				}
				UnrealGamePatcher::FPakWriter::FEntryToWrite W;
				W.Path = PakEntryPath;
				W.Blocks.AddDefaulted();
				if (Op == TEXT("A"))
				{
					W.CompressionMethodIndex = 0;
					W.UncompressedSize = Data.Num();
					W.Blocks[0].Bytes = MoveTemp(Data);
				}
				else
				{
					// Compressed single block; resolve the method index.
					if (Fields.Num() < 5)
					{
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("pakpatch: compressed append needs <method> and <raw-size>"));
						return ExitFailure;
					}
					const FString MethodName = Fields[3];
					W.UncompressedSize = FCString::Atoi64(*Fields[4]);
					W.CompressionBlockSize = W.UncompressedSize; // single block
					W.CompressionMethodIndex = 0;
					for (uint32 I = 0; I < 8; ++I)
					{
						FName M = Writer.GetInfo().GetCompressionMethod(I);
						if (M != NAME_None && M.ToString() == MethodName) { W.CompressionMethodIndex = I; break; }
					}
					if (W.CompressionMethodIndex == 0)
					{
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("pakpatch: compression method '%s' not in pak"), *MethodName);
						return ExitFailure;
					}
					W.Blocks[0].Bytes = MoveTemp(Data);
				}
				UnrealGamePatcher::FPakEntry OutEntry;
				if (!Writer.AppendEntry(W, OutEntry))
				{
					return ExitFailure;
				}
				++OpsApplied;
			}
			else
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("pakpatch: unknown op '%s' (expected O/A/C)"), *Op);
				return ExitFailure;
			}
		}

		if (!Writer.Commit())
		{
			return ExitFailure;
		}

		UE_LOG(LogUnrealGamePatcher, Display, TEXT("pakpatch: applied %d appends, wrote %lld added bytes to '%s'"),
			OpsApplied, Writer.GetBytesAdded(), *OutPath);
		return ExitSuccess;
	
	}


	static int32 RunUpdate(const TCHAR* CmdLine)
	{

		// Client update (task 5.1/5.2): bring InstallDir to the CDN version.
		// -cdn is ONE base: http(s)://... (everything fetched over HTTP) or a
		// local directory (bare path or file://...). -version= is an optional
		// lock; omitted, the target resolves from the CDN's latest.json.
		FString CdnRoot, Version, InstallDir;
		bool bCheckOnly = false;
		int32 InjectFail = 0, CrashAfter = 0;
		FParse::Value(CmdLine, TEXT("-cdn="), CdnRoot);
		FParse::Value(CmdLine, TEXT("-version="), Version);
		FParse::Value(CmdLine, TEXT("-install="), InstallDir);
		FParse::Value(CmdLine, TEXT("-injectfail="), InjectFail);
		FParse::Value(CmdLine, TEXT("-crashafter="), CrashAfter);
		// Documented flag (usage + README): report the diff without writing.
		// It was advertised but never parsed - passing it ran a full apply.
		bCheckOnly = FParse::Param(CmdLine, TEXT("check"));
		if (CdnRoot.IsEmpty() || InstallDir.IsEmpty())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("update requires -cdn=<file:///...|http(s)://...|local dir> -install=<dir> [-version=<v>] [-check] [-injectfail=N] [-crashafter=N]"));
			return ExitUsage;
		}

		UnrealGamePatcher::FUpdateOrchestrator::TestFailAfterWrites = InjectFail;
		UnrealGamePatcher::FUpdateOrchestrator::TestCrashAfterWrites = CrashAfter;
		UnrealGamePatcher::FUpdateStats Stats;
		UnrealGamePatcher::EPatcherError LastError = UnrealGamePatcher::EPatcherError::Ok;
		UnrealGamePatcher::FUpdateOptions Options;
		// Bind the unified event stream: stages/progress are logged, the
		// terminal error code becomes the process exit code (scripts can
		// branch on it; `if errorlevel 1` still catches any failure).
		Options.Events = [&LastError](const UnrealGamePatcher::FUpdateEvent& E)
		{
			if (E.bIsError)
			{
				LastError = E.Error;
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("update failed: [code %d] %s"), (int32)E.Error, *E.Message);
			}
			else
			{
				UE_LOG(LogUnrealGamePatcher, Display, TEXT("[stage %d] %s (%d/%d)"),
					(int32)E.Phase, *E.Message, E.Current, E.Total);
			}
		};
		const bool bCdnIsHttp = CdnRoot.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase) ||
			CdnRoot.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase);
		// Function scope on purpose: Options.Downloader is a raw pointer the
		// orchestrator (and ResolveLatest below) keeps using after this block -
		// a block-scoped downloader here would dangle (use-after-destroy).
		UnrealGamePatcher::FHttpChunkDownloader Downloader;
		if (bCdnIsHttp)
		{
			// HTTP CDN: latest.json, the version index AND the chunks all come
			// over HTTP. Force-load FHttpModule on the game thread first (see
			// downloadtest).
			FHttpModule::Get();
			Options.Downloader = &Downloader;
			Options.BaseUrl = CdnRoot;
		}
		if (Version.IsEmpty())
		{
			// No -version= lock: resolve the target from the CDN's fixed
			// announcement file index/latest.json (same base as everything else).
			if (!UnrealGamePatcher::FUpdateOrchestrator::ResolveLatest(CdnRoot, Options, Version))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("update: -version= not given and no latest.json on the CDN"));
				return ExitUsage;
			}
			UE_LOG(LogUnrealGamePatcher, Display, TEXT("update: resolved latest version %s"), *Version);
		}
		const bool bOk = UnrealGamePatcher::FUpdateOrchestrator::UpdateWithOptions(
			CdnRoot, Version, InstallDir, !bCheckOnly, Options, Stats);
		UnrealGamePatcher::FUpdateOrchestrator::TestFailAfterWrites = 0;
		UnrealGamePatcher::FUpdateOrchestrator::TestCrashAfterWrites = 0;
		if (!bOk)
		{
			return LastError != UnrealGamePatcher::EPatcherError::Ok ? (int32)LastError : ExitFailure;
		}
		UE_LOG(LogUnrealGamePatcher, Display,
			TEXT("update %s: files %d same / %d changed / %d added / %d removed; %d chunks fetched (%lld bytes), %d reused (%lld bytes), %lld bytes written%s"),
			*Version, Stats.NumFilesSame, Stats.NumFilesChanged, Stats.NumFilesAdded, Stats.NumFilesRemoved,
			Stats.NumChunksFetched, Stats.BytesFetched, Stats.NumChunksReused, Stats.BytesReused, Stats.BytesWritten,
			bCheckOnly ? TEXT(" (check only)") : TEXT(""));
		return ExitSuccess;
	
	}


	static int32 RunCheckCompactRepair(const TCHAR* CmdLine)
	{

		// Maintenance commands (tasks 5.4/5.5). check reports the diff and
		// the sidecar's hole/bloat state without writing; compact rebuilds
		// hole-bearing files from CDN chunks (temp + atomic, reclaims dead
		// weight); repair rebuilds only hash-mismatched files.
		const FString Command = GetCommandToken(CmdLine);
		FString CdnRoot, Version, InstallDir;
		FParse::Value(CmdLine, TEXT("-cdn="), CdnRoot);
		FParse::Value(CmdLine, TEXT("-version="), Version);
		FParse::Value(CmdLine, TEXT("-install="), InstallDir);
		if (CdnRoot.IsEmpty() || Version.IsEmpty() || InstallDir.IsEmpty())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("%s requires -cdn=<cdn root> -version=<version> -install=<install dir>"), *Command);
			return ExitUsage;
		}

		// HTTP(S) CDN: wire the transport so index AND chunk fetches go over
		// the network (same wiring as RunUpdate). Function scope on purpose:
		// Options.Downloader is a raw pointer the orchestrator keeps using.
		UnrealGamePatcher::FUpdateOptions Options;
		UnrealGamePatcher::FHttpChunkDownloader Downloader;
		if (CdnRoot.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase) ||
			CdnRoot.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
		{
			FHttpModule::Get();
			Options.Downloader = &Downloader;
			Options.BaseUrl = CdnRoot;
		}

		IPlatformFile& MPF = FPlatformFileManager::Get().GetPlatformFile();

		if (Command == TEXT("check"))
		{
			UnrealGamePatcher::FUpdateStats Stats;
			if (!UnrealGamePatcher::FUpdateOrchestrator::UpdateWithOptions(CdnRoot, Version, InstallDir, /*bApply=*/false, Options, Stats))
			{
				return ExitFailure;
			}

			// Hole / bloat report from the sidecar (task 5.5 thresholds).
			UnrealGamePatcher::FPatcherState State;
			if (!UnrealGamePatcher::FPatcherStateFile::Load(UnrealGamePatcher::Paths::GetStatePath(InstallDir), State))
			{
				// A corrupt/unreadable sidecar must not silently read as "no
				// holes" - the bloat report would be a lie.
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("check: cannot load sidecar state in '%s'"), *InstallDir);
				return ExitFailure;
			}
			int64 HoleBytes = 0;
			int64 InstallBytes = 0;
			{
				UnrealGamePatcher::FIndexManifest Truth;
				if (!UnrealGamePatcher::FLocalTruthExtractor::ExtractInstall(InstallDir, Truth))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("check: cannot extract install truth from '%s'"), *InstallDir);
					return ExitFailure;
				}
				for (const TPair<FString, UnrealGamePatcher::FIndexFileEntry>& Pair : Truth.Files)
				{
					InstallBytes += Pair.Value.Size;
				}
			}
			for (const UnrealGamePatcher::FHole& Hole : State.Holes)
			{
				HoleBytes += Hole.Size;
			}
			const double BloatPct = InstallBytes > 0 ? (100.0 * (double)HoleBytes / (double)InstallBytes) : 0.0;
			const bool bOverThreshold = HoleBytes > (512ll << 20) || BloatPct > 10.0;

			UE_LOG(LogUnrealGamePatcher, Display,
				TEXT("check %s: files %d same / %d changed / %d added / %d removed; %d chunks fetched estimate (%lld bytes)"),
				*Version, Stats.NumFilesSame, Stats.NumFilesChanged, Stats.NumFilesAdded, Stats.NumFilesRemoved,
				Stats.NumChunksFetched, Stats.BytesFetched);
			UE_LOG(LogUnrealGamePatcher, Display,
				TEXT("check %s: holes %d (%lld bytes of %lld installed = %.2f%%)%s"),
				*Version, State.Holes.Num(), HoleBytes, InstallBytes, BloatPct,
				bOverThreshold ? TEXT(" - OVER THRESHOLD (>10%% or >512MB), run compact") : TEXT(""));
			if (State.Transaction.Status == UnrealGamePatcher::ETransactionStatus::Pending)
			{
				UE_LOG(LogUnrealGamePatcher, Warning,
					TEXT("check: interrupted transaction (%s) found - run update to recover"),
					*State.Transaction.ContentVersion);
			}
			return ExitSuccess;
		}

		if (Command == TEXT("compact"))
		{
			UnrealGamePatcher::FPatcherState State;
			const FString StatePath = UnrealGamePatcher::Paths::GetStatePath(InstallDir);
			if (!UnrealGamePatcher::FPatcherStateFile::Load(StatePath, State))
			{
				// Same lie as check: a corrupt sidecar would read as "no holes".
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("compact: cannot load sidecar state in '%s'"), *InstallDir);
				return ExitFailure;
			}

			TSet<FString> HoleFiles;
			for (const UnrealGamePatcher::FHole& Hole : State.Holes)
			{
				if (!Hole.File.IsEmpty())
				{
					HoleFiles.Add(Hole.File);
				}
			}
			if (HoleFiles.Num() == 0)
			{
				UE_LOG(LogUnrealGamePatcher, Display, TEXT("compact: no holes to reclaim in '%s'"), *InstallDir);
				return ExitSuccess;
			}

			TArray<FString> Files = HoleFiles.Array();
			Files.Sort();

			// Compact rebuilds straight from the CDN chunk DIRECTORY - an
			// HTTP(S) CDN has no local dir to read; route through repair
			// instead (same per-file rebuild, driven over the transport).
			if (Options.Downloader != nullptr)
			{
				UE_LOG(LogUnrealGamePatcher, Error,
					TEXT("compact: requires a local CDN directory (file:// or bare path); for an HTTP(S) CDN use repair"));
				return ExitUsage;
			}

			UnrealGamePatcher::FUpdateStats Stats;
			const int32 Rebuilt = UnrealGamePatcher::FUpdateOrchestrator::Compact(CdnRoot, Version, InstallDir, Files, Stats);
			if (Rebuilt < 0)
			{
				return ExitFailure;
			}

			// Drop the holes of every rebuilt file and persist.
			State.Holes.RemoveAll([&](const UnrealGamePatcher::FHole& Hole)
			{
				return !Hole.File.IsEmpty() && HoleFiles.Contains(Hole.File);
			});
			if (!UnrealGamePatcher::FPatcherStateFile::Save(StatePath, State))
			{
				return ExitFailure;
			}

			UE_LOG(LogUnrealGamePatcher, Display,
				TEXT("compact: rebuilt %d/%d hole-bearing files (%lld bytes written, %lld fetched), %d holes remain"),
				Rebuilt, Files.Num(), Stats.BytesWritten, Stats.BytesFetched, State.Holes.Num());
			return ExitSuccess;
		}

		// repair
		{
			UnrealGamePatcher::FUpdateStats Stats;
			Options.bRepairOnly = true;
			if (!UnrealGamePatcher::FUpdateOrchestrator::UpdateWithOptions(CdnRoot, Version, InstallDir, /*bApply=*/true, Options, Stats))
			{
				return ExitFailure;
			}
			UE_LOG(LogUnrealGamePatcher, Display,
				TEXT("repair %s: %d files repaired (%lld bytes written, %lld fetched), %d files already intact"),
				*Version, Stats.NumFilesChanged, Stats.BytesWritten, Stats.BytesFetched, Stats.NumFilesSame);
			return ExitSuccess;
		}
	
	}


	static int32 RunUpdateTest(const TCHAR* /*CmdLine*/)
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


	static int32 RunMainTest(const TCHAR* /*CmdLine*/)
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


	static int32 RunDownloadTest(const TCHAR* /*CmdLine*/)
	{

		// Download layer acceptance (tasks 6.1-6.6): the real HTTP backend
		// against an in-process HTTP server with Range support and fault
		// injection. Covers basic fetch + verify, cache reuse, byte-range
		// resume, concurrency, retry/limit with no corrupt bytes landing,
		// end-to-end network update, a custom downloader backend driving the
		// full update flow, and occupied-file handling with rollback.
		bool bAllPassed = true;
		auto Check = MakeCheck(bAllPassed);

		using namespace UnrealGamePatcher;

		const FString WorkDir = GetTestWorkDir(TEXT("downloadtest"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

		const FString Cdn = FPaths::Combine(WorkDir, TEXT("cdn"));
		const FString Install = FPaths::Combine(WorkDir, TEXT("install"));
		const FString Cache = FPaths::Combine(WorkDir, TEXT("cache"));
		PF.CreateDirectoryTree(*Install);
		PF.CreateDirectoryTree(*Cache);

		FIndexManifest Index;
		Check(BuildFixtureCdn(WorkDir, Cdn, Index), TEXT("downloadtest: ingest staged fixture"));

		// Serve the CDN over HTTP with the EXTERNAL python server (the patcher
		// contains no HTTP server; Scripts/cdn_server.py provides it).
		const FString ProjectRoot = FPaths::GetPath(FPaths::GetPath(FPaths::GetPath(FPlatformProcess::BaseDir())));
		const FString CdnScript = FPaths::Combine(ProjectRoot, TEXT("Source"), TEXT("Programs"), TEXT("UnrealGamePatcher"), TEXT("Scripts"), TEXT("cdn_server.py"));
		FCdnServerProc CdnServer;
		Check(CdnServer.Start(CdnScript, Cdn, FPaths::Combine(WorkDir, TEXT("cdn.log"))),
			TEXT("downloadtest: external CDN server started"));
		const FString BaseUrl = FString::Printf(TEXT("http://127.0.0.1:%d"), CdnServer.Port);

		// Force-load the HTTP module on the game thread before any callback
		// thread touches it (FHttpModule::Get() asserts off the game thread
		// on first use).
		FHttpModule::Get();
		FHttpChunkDownloader Http;
		FDownloadManager Manager(Http, /*MaxConcurrent=*/8, /*MaxRetries=*/3);

		// Single-request probe on the game thread: smoke-tests the transport
		// before the executor batches below settle on the HttpThread.
		{
			const FString FirstFile = Index.Chunks.CreateConstIterator().Value().File;
			const FString ProbeUrl = FString::Printf(TEXT("%s/%s"), *BaseUrl, *FirstFile);
			TArray64<uint8> Probe;
			const bool bProbe = Http.DownloadRange(ProbeUrl, 0, -1, Probe);
			Check(bProbe && Probe.Num() > 0, TEXT("A-probe: game-thread HTTP request succeeds"));
			if (bProbe)
			{
				Check(Probe.Num() == Index.Chunks.CreateConstIterator().Value().Size, TEXT("A-probe: probe returned the full chunk file"));
			}

		}

		auto MakeChunkRequests = [&](const FString& CacheDir, TArray<FDownloadRequest>& OutRequests)
		{
			OutRequests.Reset();
			for (const TPair<FString, FIndexChunkEntry>& Pair : Index.Chunks)
			{
				FDownloadRequest Req;
				Req.Id = Pair.Key;
				Req.Url = FString::Printf(TEXT("%s/%s"), *BaseUrl, *Pair.Value.File);
				Req.Size = Pair.Value.Size;
				Req.DestFile = FPaths::Combine(CacheDir, Pair.Value.File);
				OutRequests.Add(Req);
			}
		};

		// --- A. Basic download + verify + cache reuse (6.5/6.1) -------------
		{
			// The fixture's chunk count is content-defined (CDC), not a fixed
			// 1 MiB window count - assert relative to the index, never a pin.
			const int32 NumFixtureChunks = Index.Chunks.Num();
			TArray<FDownloadRequest> Requests;
			MakeChunkRequests(Cache, Requests);
			FDownloadStats Stats;
			TArray<FString> Failed;
			Check(Manager.DownloadAll(Requests, Stats, Failed) && Stats.SkippedExisting == 0,
				*FString::Printf(TEXT("A: all %d chunks downloaded over HTTP"), NumFixtureChunks));
			Check(CdnServer.Stats().Peak >= 2, TEXT("A: server saw overlapping connections (real concurrency)"));

			bool bAllCached = true;
			for (const FDownloadRequest& Req : Requests)
			{
				if (!PF.FileExists(*Req.DestFile) || IFileManager::Get().FileSize(*Req.DestFile) != Req.Size)
				{
					bAllCached = false;
				}
			}
			Check(bAllCached, TEXT("A: every chunk cached with the right size"));

			// Every cached chunk file passes the loader (content verified).
			bool bLoadOk = true;
			for (const TPair<FString, FIndexChunkEntry>& Pair : Index.Chunks)
			{
				TArray64<uint8> Data;
				if (!FChunkDataLoader::Load(FPaths::Combine(Cache, Pair.Value.File), Pair.Value, Data))
				{
					bLoadOk = false;
				}
			}
			Check(bLoadOk, TEXT("A: every downloaded chunk passes content verification"));

			const int32 ReqAfterFirst = CdnServer.Stats().Files;

			// A second pass must be served entirely from cache.
			FDownloadStats Stats2;
			TArray<FString> Failed2;
			Check(Manager.DownloadAll(Requests, Stats2, Failed2), TEXT("A: second download pass succeeds"));
			Check(Stats2.SkippedExisting == NumFixtureChunks && Stats2.BytesDownloaded == 0,
				TEXT("A: second pass served from cache (0 bytes over the wire)"));
			Check(CdnServer.Stats().Files == ReqAfterFirst, TEXT("A: no new HTTP requests on cache hit"));
		}

		// --- B. Journal bitmap resume of an interrupted file (6.1) ----------
		{
			// Simulate an interrupted download exactly the way the executor
			// leaves it: a preallocated staging file with the first two 4MB
			// segments on disk + a journal bitmap marking them done. The file
			// is 9MB, so the missing tail is one 1MB segment - only that may
			// be re-fetched, over a 206 Range request.
			const int64 BigTotal = 9ll << 20;
			const int64 SegSize = 4ll << 20;
			const FString BigName = TEXT("chunks/resume-tail-big.chunk");
			const FString Dest = FPaths::Combine(Cache, BigName);
			const FString Staging = Dest + TEXT(".download");
			const FString Journal = Dest + TEXT(".journal");

			TArray64<uint8> Whole;
			Whole.SetNumUninitialized(BigTotal);
			{
				uint8* P = Whole.GetData();
				for (int64 I = 0; I < BigTotal; ++I)
				{
					P[I] = (uint8)(I * 31 + (I >> 13));
				}
			}
			Check(FFileHelper::SaveArrayToFile(Whole, *FPaths::Combine(Cdn, BigName)), TEXT("B: synthetic 9MB chunk staged on the CDN"));

			{
				IFileHandle* H = PF.OpenWrite(*Staging);
				Check(H != nullptr, TEXT("B: staging file opened"));
				if (H)
				{
					H->Seek(BigTotal - 1); // preallocate to the full size
					uint8 Zero = 0;
					H->Write(&Zero, 1);
					H->Seek(0); // first two segments are on disk
					H->Write(Whole.GetData(), (int32)(SegSize * 2));
					delete H;
				}
			}
			{
				const FString Text = FString::Printf(TEXT("UGPJRN1 %lld %lld\nSEG 0\nSEG 1\n"), SegSize, BigTotal);
				Check(FFileHelper::SaveStringToFile(Text, *Journal, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("B: journal written"));
			}

			TArray<FDownloadRequest> Requests;
			FDownloadRequest Big;
			Big.Id = TEXT("resume-tail-big");
			Big.Url = FString::Printf(TEXT("%s/%s"), *BaseUrl, *BigName);
			Big.Size = BigTotal;
			Big.DestFile = Dest;
			Requests.Add(Big);

			FDownloadStats Stats;
			TArray<FString> Failed;
			Check(Manager.DownloadAll(Requests, Stats, Failed), TEXT("B: resume download succeeds"));

			const int64 FinalSize = IFileManager::Get().FileSize(*Dest);
			Check(FinalSize == BigTotal, TEXT("B: resumed file has the full size"));
			bool bBResumeOk = false;
			{
				// The promoted file must match the CDN copy byte for byte.
				TArray64<uint8> Final;
				bBResumeOk = FFileHelper::LoadFileToArray(Final, *Dest) && Final.Num() == BigTotal &&
					FMemory::Memcmp(Final.GetData(), Whole.GetData(), BigTotal) == 0;
			}
			Check(bBResumeOk, TEXT("B: resumed content matches the CDN copy"));
			Check(!PF.FileExists(*Staging) && !PF.FileExists(*Journal), TEXT("B: staged file promoted (no .download/.journal left)"));

			// The server must have served a 206 Range request for the tail.
			{
				const FCdnServerProc::FCdnStats BStats = CdnServer.Stats();
				Check(BStats.Ranges > 0, TEXT("B: resume used a 206 Range request"));
				Check(BStats.MaxRangeBytes == BigTotal - SegSize * 2, TEXT("B: only the missing tail was re-fetched"));
			}
		}

		// --- D. Retry with cap + no corrupt data on disk (6.3) --------------
		{
			const FString CacheD = FPaths::Combine(WorkDir, TEXT("cacheD"));
			PF.CreateDirectoryTree(*CacheD);
			TArray<FDownloadRequest> Requests;
			MakeChunkRequests(CacheD, Requests);

			// Arm a one-shot 500 on the next request via the control endpoint,
			// then the batch must retry and succeed.
			{
				TArray64<uint8> Ctrl;
				Http.DownloadRange(FString::Printf(TEXT("%s/__ctl/fail_next"), *BaseUrl), 0, -1, Ctrl);
			}
			FDownloadStats Stats;
			TArray<FString> Failed;
			FDownloadManager ManagerD(Http, /*MaxConcurrent=*/1, /*MaxRetries=*/3);
			Check(ManagerD.DownloadAll(Requests, Stats, Failed), TEXT("D: batch succeeds after a retried 500"));
			Check(Stats.Retries >= 1 && CdnServer.Stats().Errors > 0, TEXT("D: a transport retry was issued for a 500"));

			// Permanent failure: a 404 for one chunk fails the whole batch,
			// reports the id, and leaves no DestFile.
			FString Guid;
			for (const auto& BadPair : Index.Chunks) { Guid = BadPair.Key; break; }
			const FIndexChunkEntry& Chunk = Index.Chunks[Guid];
			FDownloadRequest Bad;
			Bad.Id = Guid;
			Bad.Url = FString::Printf(TEXT("%s/chunks/missing_%s.chunk"), *BaseUrl, *Guid);
			Bad.Size = Chunk.Size;
			Bad.DestFile = FPaths::Combine(CacheD, TEXT("missing.chunk"));
			TArray<FDownloadRequest> BadBatch = Requests;
			BadBatch.Add(Bad);

			FDownloadStats StatsBad;
			TArray<FString> FailedBad;
			FDownloadManager ManagerBad(Http, /*MaxConcurrent=*/1, /*MaxRetries=*/1);
			const bool bBatchFailed = !ManagerBad.DownloadAll(BadBatch, StatsBad, FailedBad);
			Check(bBatchFailed, TEXT("D: batch fails when a chunk 404s past the retry cap"));
			Check(FailedBad.Num() == 1 && FailedBad[0] == Guid, TEXT("D: the failed chunk id is reported"));
			Check(!PF.FileExists(*Bad.DestFile), TEXT("D: no corrupt file landed on disk"));

			// Manager precheck refusals: a negative and an over-cap declared
			// size are refused BEFORE any transfer (zero HTTP requests).
			const int32 FilesBeforePrecheck = CdnServer.Stats().Files;
			TArray<FDownloadRequest> Insane;
			FDownloadRequest Negative = Bad;
			Negative.Id = TEXT("D-negative");
			Negative.Size = -5;
			Insane.Add(Negative);
			FDownloadRequest Huge = Bad;
			Huge.Id = TEXT("D-huge");
			Huge.Size = (256ll << 20) + 1; // one byte over the transport cap
			Insane.Add(Huge);
			FDownloadStats StatsInsane;
			TArray<FString> FailedInsane;
			Check(!ManagerBad.DownloadAll(Insane, StatsInsane, FailedInsane), TEXT("D: insane sizes refused"));
			Check(FailedInsane.Num() == 2, TEXT("D: both insane sizes reported"));
			Check(CdnServer.Stats().Files == FilesBeforePrecheck, TEXT("D: insane sizes made zero HTTP requests"));

			// Mid-body truncation fault: a short response is a retryable
			// transport failure - the batch still succeeds, nothing corrupt
			// lands. (truncate_next arms exactly one response.)
			{
				TArray64<uint8> Ctrl;
				Http.DownloadRange(FString::Printf(TEXT("%s/__ctl/truncate_next?bytes=100"), *BaseUrl), 0, -1, Ctrl);
			}
			// A FRESH cache dir forces real HTTP requests (cacheD is fully
			// warm by now - the truncation must hit a live transfer).
			const FString CacheDTrunc = FPaths::Combine(WorkDir, TEXT("cacheD-trunc"));
			PF.CreateDirectoryTree(*CacheDTrunc);
			TArray<FDownloadRequest> TruncRequests;
			MakeChunkRequests(CacheDTrunc, TruncRequests);
			FDownloadStats StatsTrunc;
			TArray<FString> FailedTrunc;
			FDownloadManager ManagerTrunc(Http, /*MaxConcurrent=*/1, /*MaxRetries=*/3);
			Check(ManagerTrunc.DownloadAll(TruncRequests, StatsTrunc, FailedTrunc), TEXT("D: batch succeeds after a truncated body"));
			Check(StatsTrunc.Retries >= 1, TEXT("D: a truncated body was retried"));
		}

		// --- E. End-to-end network update + interruption (6.1) --------------
		{
			FUpdateOptions Options;
			Options.Downloader = &Http;
			Options.BaseUrl = BaseUrl;
			Options.ChunkCacheDir = FPaths::Combine(WorkDir, TEXT("installcache"));

			FUpdateStats S1;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.0.0"), Install, true, Options, S1),
				TEXT("E: full network update succeeds"));
			Check(S1.NumFilesAdded == 4, TEXT("E: 4 files installed"));
			Check(S1.NumChunksFetched == Index.Chunks.Num(), TEXT("E: every chunk downloaded over HTTP"));

			bool bAllMatch = true;
			for (const TPair<FString, FIndexFileEntry>& Pair : Index.Files)
			{
				FString Sha1;
				int64 Size = 0;
				if (!FLocalTruthExtractor::HashWholeFile(FPaths::Combine(Install, Pair.Key), Sha1, Size) ||
					!Sha1.Equals(Pair.Value.SHA1, ESearchCase::IgnoreCase))
				{
					bAllMatch = false;
				}
			}
			Check(bAllMatch, TEXT("E: installed files hash-identical to the manifest"));

			// No-op rerun: everything served from the chunk cache.
			const int32 ReqBefore = CdnServer.Stats().Files;
			FUpdateStats S2;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.0.0"), Install, true, Options, S2),
				TEXT("E: no-op rerun succeeds"));
			Check(S2.NumFilesSame == 4 && S2.NumChunksFetched == 0, TEXT("E: no-op rerun fetches nothing"));
			Check(CdnServer.Stats().Files == ReqBefore, TEXT("E: rerun hit the cache, not the network"));

			// Interruption: drop one chunk from the cache and damage a file.
			// The rerun must re-fetch exactly that one chunk.
			// Drop one of the UCAS's chunks (the damaged container is rebuilt,
			// so exactly that one chunk is re-downloaded).
			FString DropGuid;
			for (const FIndexFileChunkRef& DropPart : Index.Files[TEXT("pakchunk1-Windows.ucas")].Parts) { DropGuid = DropPart.Chunk; break; }
			const FIndexChunkEntry& DropChunk = Index.Chunks[DropGuid];
			PF.DeleteFile(*(FPaths::Combine(Options.ChunkCacheDir, DropChunk.File)));

			{
				const FString UcasPath = FPaths::Combine(Install, TEXT("pakchunk1-Windows.ucas"));
				TArray64<uint8> Bytes;
				FFileHelper::LoadFileToArray(Bytes, *UcasPath);
				Bytes[Bytes.Num() / 2] ^= 0xFF;
				FFileHelper::SaveArrayToFile(Bytes, *UcasPath);
			}

			// Same-size damage is only caught by verify/repair runs (trust
			// model) - force verify for this interruption+repair scenario.
			Options.bForceVerify = true;
			const int32 ReqBeforeRepair = CdnServer.Stats().Files;
			FUpdateStats S3;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.0.0"), Install, true, Options, S3),
				TEXT("E: repair over network succeeds"));
			Check(S3.NumFilesChanged == 2, TEXT("E: damaged ucas + sibling utoc rebuilt"));
			// The download cache is a TEMP store (no double data on the client),
			// so a later run re-fetches what the repair rebuilds over HTTP.
			Check(CdnServer.Stats().Files > ReqBeforeRepair, TEXT("E: repair re-downloaded the rebuilt files over HTTP"));
			{
				FString Sha1;
				int64 Size = 0;
				FLocalTruthExtractor::HashWholeFile(FPaths::Combine(Install, TEXT("pakchunk1-Windows.ucas")), Sha1, Size);
				Check(Sha1.Equals(Index.Files[TEXT("pakchunk1-Windows.ucas")].SHA1, ESearchCase::IgnoreCase),
					TEXT("E: damaged file repaired to the manifest hash"));
			}

			// --- Network container delta (optimistic-prefetch regression): a
			// small change to a CONTAINER over HTTP must fetch only the
			// changed entry's covering window. The old download phase pulled
			// the target's ENTIRE part set for any Changed file - a 4-byte
			// container edit cost the whole 10GB over the wire.
			Options.bForceVerify = false; // trust mode: exercises the optimistic path
			// Own install dir + cache: this scenario must not disturb the E
			// install that the G group continues from.
			const FString InstallND = FPaths::Combine(WorkDir, TEXT("installnetdelta"));
			Options.ChunkCacheDir = FPaths::Combine(WorkDir, TEXT("netdeltacache"));
			PF.CreateDirectoryTree(*InstallND);
			const FString NDPak = FPaths::Combine(InstallND, TEXT("pakchunk1-Windows.pak"));
			FUpdateStats SInstND;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.0.0"), InstallND, true, Options, SInstND),
				TEXT("netdelta: baseline install over HTTP succeeds"));

			const FString V2Pak = FPaths::Combine(WorkDir, TEXT("netdelta_v2.pak"));
			PF.CopyFile(*V2Pak, *NDPak);
			{
				FPakWriter W;
				Check(W.Open(V2Pak), TEXT("netdelta: writer opens"));
				FPakWriter::FEntryToWrite Extra;
				Extra.Path = TEXT("Content/netdelta.txt");
				FPakWriter::FStoredBlock Block;
				const ANSICHAR* Text = "network-delta";
				Block.Bytes.SetNumUninitialized(13);
				FMemory::Memcpy(Block.Bytes.GetData(), Text, 13);
				Extra.Blocks.Add(MoveTemp(Block));
				FPakEntry OutEntry;
				Check(W.AppendEntry(Extra, OutEntry) && W.Commit(), TEXT("netdelta: v2 pak crafted"));
			}
			{
				// The staged 1.1.0 dir carries ALL FOUR files (the modified pak
				// + unchanged ucas/utoc/boot) - a one-file manifest would be a
				// shrink release that deletes the rest of the install.
				const FString Staged = FPaths::Combine(WorkDir, TEXT("netdelta_staged"));
				PF.CreateDirectoryTree(*Staged);
				PF.CopyFile(*FPaths::Combine(Staged, TEXT("pakchunk1-Windows.pak")), *V2Pak);
				PF.CopyFile(*FPaths::Combine(Staged, TEXT("pakchunk1-Windows.ucas")), *FPaths::Combine(InstallND, TEXT("pakchunk1-Windows.ucas")));
				PF.CopyFile(*FPaths::Combine(Staged, TEXT("pakchunk1-Windows.utoc")), *FPaths::Combine(InstallND, TEXT("pakchunk1-Windows.utoc")));
				PF.CopyFile(*FPaths::Combine(Staged, TEXT("boot.txt")), *FPaths::Combine(InstallND, TEXT("boot.txt")));
				FIngestResult R;
				Check(FBuildIngestor::Ingest(Staged, TEXT("DownloadTest"), TEXT("1.1.0"), Cdn, R),
					TEXT("netdelta: 1.1.0 ingested into the HTTP-served CDN"));
			}
			const int32 ReqBeforeDelta = CdnServer.Stats().Files;
			FUpdateStats SDelta;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.1.0"), InstallND, true, Options, SDelta),
				TEXT("netdelta: container delta update over HTTP succeeds"));
			Check(SDelta.NumFilesChanged == 1 && SDelta.NumFilesSame == 3,
				TEXT("netdelta: exactly the pak changed, the rest Same"));
			// THE ASSERTION: one changed entry -> exactly ONE window chunk
			// fetched over HTTP (the whole-container prefetch would have
			// fetched the pak's full part set).
			Check(SDelta.NumChunksFetched == 1,
				TEXT("netdelta: ONLY the changed entry's window was fetched"));
			Check(CdnServer.Stats().Files - ReqBeforeDelta <= 3,
				TEXT("netdelta: server saw ~1 index + 1 chunk request"));
			{
				FPakReader R;
				Check(R.Open(NDPak) && R.ReadIndex(), TEXT("netdelta: patched pak opens"));
				bool bOk = false;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path.Contains(TEXT("netdelta.txt")))
					{
						TArray64<uint8> Payload;
						bOk = R.ReadEntryData(L.Entry, Payload) && Payload.Num() == 13 &&
							FMemory::Memcmp(Payload.GetData(), "network-delta", 13) == 0;
					}
				}
				Check(bOk, TEXT("netdelta: appended entry decodes"));
			}
			{
				FUpdateStats SD2;
				Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.1.0"), InstallND, true, Options, SD2),
					TEXT("netdelta: rerun converges"));
				Check(SD2.NumFilesSame == 4 && SD2.NumChunksFetched == 0, TEXT("netdelta: rerun is a no-op"));
			}
		}

		// --- Network pak delta at REALISTIC scale (review 2026-10-09 H1
		// regression). The netdelta scenario above runs on the 939B fixture
		// pak, which is a SINGLE CDC window: same and changed entries share
		// that one chunk, so the fetch-before-same-check bug was invisible
		// there (the same entries' "fetch" hit the cached window and the CoW
		// patch looked healthy). This scenario crafts a multi-window pak and
		// pins the CoW contract with I/O stats:
		//   (a) a changed 100KB entry of a ~4.3MB pak must be written CoW
		//       (append + index/footer), never a whole-container rewrite;
		//   (b) the chunk store must be read ONLY for the changed entry's
		//       covering windows (the old order fetched every SAME entry's
		//       bytes - in network mode those windows are deliberately not
		//       prefetched, so the patch aborted and silently fell back to a
		//       full-container download + rebuild).
		{
			const FString InstallBig = FPaths::Combine(WorkDir, TEXT("installbigpak"));
			// Own options (E's went out of scope with its block): HTTP-served
			// CDN, trust mode - the same shape the netdelta scenario used.
			FUpdateOptions BigOptions;
			BigOptions.Downloader = &Http;
			BigOptions.BaseUrl = BaseUrl;
			BigOptions.ChunkCacheDir = FPaths::Combine(WorkDir, TEXT("bigpakcache"));
			const FString BigPakName = TEXT("pakchunk1-Windows.pak");

			// Deterministic pseudo-random bytes (xorshift64) - random content
			// is what makes FastCDC split the pak into several windows (a
			// structured pattern could fail to cut at all).
			auto MakeRandomBytes = [](int64 Size, uint64 Seed)
			{
				TArray64<uint8> Bytes;
				Bytes.SetNumUninitialized(Size);
				uint64 S = Seed ? Seed : 0x9E3779B97F4A7C15ull;
				for (int64 I = 0; I < Size; ++I)
				{
					S ^= S << 13; S ^= S >> 7; S ^= S << 17;
					Bytes[(int32)I] = (uint8)(S >> 32);
				}
				return Bytes;
			};
			auto AddBigEntry = [&MakeRandomBytes](FPakWriter& W, const TCHAR* Path, int64 Size, uint64 Seed) -> bool
			{
				FPakWriter::FEntryToWrite Item;
				Item.Path = Path;
				FPakWriter::FStoredBlock Block;
				Block.Bytes = MakeRandomBytes(Size, Seed);
				Item.Blocks.Add(MoveTemp(Block));
				FPakEntry Out;
				return W.AppendEntry(Item, Out);
			};

			// v1: seed from the installed 939B fixture pak (2 entries) + the
			// 100KB entry v2 will change + two 2MB entries that stay identical.
			const FString BigV1 = FPaths::Combine(WorkDir, TEXT("bigpak_v1.pak"));
			PF.CopyFile(*BigV1, *FPaths::Combine(Install, BigPakName));
			{
				FPakWriter W;
				Check(W.Open(BigV1), TEXT("bigpak: v1 writer opens"));
				Check(AddBigEntry(W, TEXT("Content/big_a.bin"), 100 << 10, 1), TEXT("bigpak: v1 entry a"));
				Check(AddBigEntry(W, TEXT("Content/big_b.bin"), 2 << 20, 2), TEXT("bigpak: v1 entry b"));
				Check(AddBigEntry(W, TEXT("Content/big_c.bin"), 2 << 20, 3), TEXT("bigpak: v1 entry c"));
				Check(W.Commit(), TEXT("bigpak: v1 committed"));
			}

			const FString StagedV1 = FPaths::Combine(WorkDir, TEXT("bigpak_staged_v1"));
			PF.CreateDirectoryTree(*StagedV1);
			PF.CopyFile(*FPaths::Combine(StagedV1, BigPakName), *BigV1);
			FIngestResult RBig1;
			Check(FBuildIngestor::Ingest(StagedV1, TEXT("DownloadTest"), TEXT("1.2.0"), Cdn, RBig1),
				TEXT("bigpak: 1.2.0 ingested"));

			FUpdateStats SBig1;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.2.0"), InstallBig, true, BigOptions, SBig1),
				TEXT("bigpak: baseline install over HTTP"));
			Check(SBig1.NumFilesAdded == 1, TEXT("bigpak: single-file install"));

			// v2: 'big_a' re-appended with DIFFERENT content (the CoW replace-
			// generation path, same path); b/c and the two seed entries stay
			// byte-identical - they are the SAME majority the buggy order
			// would have tried to fetch.
			const FString BigV2 = FPaths::Combine(WorkDir, TEXT("bigpak_v2.pak"));
			PF.CopyFile(*BigV2, *BigV1);
			{
				FPakWriter W;
				Check(W.Open(BigV2), TEXT("bigpak: v2 writer opens"));
				Check(AddBigEntry(W, TEXT("Content/big_a.bin"), 100 << 10, 0xDEADBEEFull), TEXT("bigpak: v2 changed entry"));
				Check(W.Commit(), TEXT("bigpak: v2 committed"));
			}
			const FString StagedV2 = FPaths::Combine(WorkDir, TEXT("bigpak_staged_v2"));
			PF.CreateDirectoryTree(*StagedV2);
			PF.CopyFile(*FPaths::Combine(StagedV2, BigPakName), *BigV2);
			FIngestResult RBig2;
			Check(FBuildIngestor::Ingest(StagedV2, TEXT("DownloadTest"), TEXT("1.3.0"), Cdn, RBig2),
				TEXT("bigpak: 1.3.0 ingested"));

			FUpdateStats SBig2;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.3.0"), InstallBig, true, BigOptions, SBig2),
				TEXT("bigpak: container delta over HTTP succeeds"));
			Check(SBig2.NumFilesChanged == 1 && SBig2.NumFilesSame == 0, TEXT("bigpak: exactly the pak changed"));
			Check(SBig2.BytesWritten < (256 << 10),
				TEXT("bigpak: CoW wrote only the changed entry + index/footer (no whole-container rewrite)"));
			Check(SBig2.IOStats.ChunkLoads <= 3,
				TEXT("bigpak: chunk-store loads limited to the changed entry's covering windows"));
			{
				FIndexManifest BigIdx2;
				Check(FIndexManifestFile::Load(
					FPaths::Combine(Cdn, TEXT("index"), TEXT("1.3.0"), TEXT("UnrealGamePatcher.index.json")), BigIdx2),
					TEXT("bigpak: 1.3.0 index loads"));
				Check(CheckContainerLogicallyEqual(BigIdx2, InstallBig, BigPakName),
					TEXT("bigpak: patched pak logically equals 1.3.0"));
			}
			{
				FUpdateStats SBig3;
				Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.3.0"), InstallBig, true, BigOptions, SBig3),
					TEXT("bigpak: rerun converges"));
				Check(SBig3.NumFilesSame == 1 && SBig3.BytesWritten == 0 && SBig3.NumChunksFetched == 0,
					TEXT("bigpak: rerun is a no-op"));
			}
		}

		// --- F. Custom downloader backend drives the full update (6.6) -----
		{
			// A file-backed backend: ignores the transport, maps the URL path
			// onto a local CDN dir. The update flow must be identical.
			struct FFileChunkDownloader : public IChunkDownloader
			{
				FString Root;
				bool DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& OutData) override
				{
					// Extract the path after the scheme://authority part.
					FString Rest = Url;
					const int32 Scheme = Rest.Find(TEXT("://"), ESearchCase::CaseSensitive);
					if (Scheme != INDEX_NONE)
					{
						const int32 PathStart = Rest.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Scheme + 3);
						if (PathStart != INDEX_NONE)
						{
							Rest = Rest.RightChop(PathStart + 1);
						}
					}
					const FString Path = FPaths::Combine(Root, Rest);
					TArray64<uint8> Whole;
					if (!FFileHelper::LoadFileToArray(Whole, *Path))
					{
						return false;
					}
					if (Offset > 0 || (Size >= 0 && Size != Whole.Num()))
					{
						const int64 End = Size < 0 ? Whole.Num() : FMath::Min<int64>(Offset + Size, Whole.Num());
						OutData.SetNumUninitialized(End - Offset);
						FMemory::Memcpy(OutData.GetData(), Whole.GetData() + Offset, End - Offset);
					}
					else
					{
						OutData = MoveTemp(Whole);
					}
					return true;
				}
			};

			FFileChunkDownloader Custom;
			Custom.Root = Cdn;

			const FString Install2 = FPaths::Combine(WorkDir, TEXT("install2"));
			PF.CreateDirectoryTree(*Install2);
			FUpdateOptions Options;
			Options.Downloader = &Custom;
			Options.BaseUrl = TEXT("custom://cdn");
			Options.ChunkCacheDir = FPaths::Combine(WorkDir, TEXT("customcache"));

			FUpdateStats SF;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.0.0"), Install2, true, Options, SF),
				TEXT("F: custom backend drives the full update"));
			Check(SF.NumFilesAdded == 4, TEXT("F: 4 files installed via the custom backend"));
			{
				bool bAllMatch = true;
				for (const TPair<FString, FIndexFileEntry>& Pair : Index.Files)
				{
					FString Sha1;
					int64 Size = 0;
					if (!FLocalTruthExtractor::HashWholeFile(FPaths::Combine(Install2, Pair.Key), Sha1, Size) ||
						!Sha1.Equals(Pair.Value.SHA1, ESearchCase::IgnoreCase))
					{
						bAllMatch = false;
					}
				}
				Check(bAllMatch, TEXT("F: custom-backend install is hash-identical to the manifest"));
			}
		}

		// --- G. Occupied file: write fails, rollback, retry (6.4) -----------
		{
			const FString PakPath = FPaths::Combine(Install, TEXT("pakchunk1-Windows.pak"));

			// Corrupt a byte INSIDE the first entry's payload data (verify mode
			// recomputes per-entry payload hashes), then hold the pak open with
			// no sharing (simulating a running game locking its binaries).
			FString CorruptedSha1;
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
				FSHA1 H;
				H.Update(Bytes.GetData(), (int32)Bytes.Num());
				uint8 Hash[20] = { 0 };
				H.Final();
				H.GetHash(Hash);
				for (int32 I = 0; I < 20; ++I) { CorruptedSha1 += FString::Printf(TEXT("%02x"), Hash[I]); }
			}
			// Lock the file for reads only (share read, no write): the update
			// can analyze it but cannot replace it, so the write fails mid-
			// apply and the transaction must roll back.
			HANDLE Lock = CreateFileW(*PakPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			Check(Lock != INVALID_HANDLE_VALUE, TEXT("G: file locked (read-only share)"));

			// Same-size corruption needs a verify run to be detected at all.
			FUpdateOptions VerifyOptions;
			VerifyOptions.bForceVerify = true;
			// Capture the terminal failure event: the unified error code must
			// classify the locked target as WriteLocked (not a generic failure).
			UnrealGamePatcher::EPatcherError FailCode = UnrealGamePatcher::EPatcherError::Ok;
			VerifyOptions.Events = [&FailCode](const UnrealGamePatcher::FUpdateEvent& E)
			{
				if (E.bIsError) { FailCode = E.Error; }
			};
			FUpdateStats SFail;
			const bool bFailed = !FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.0.0"), Install, true, VerifyOptions, SFail);
			Check(bFailed, TEXT("G: update fails while the file is locked"));
			Check(FailCode == UnrealGamePatcher::EPatcherError::WriteLocked,
				TEXT("G: failure classified as WriteLocked (unified error code)"));
			{
				FPatcherState State;
				FPatcherStateFile::Load(Paths::GetStatePath(Install), State);
				Check(State.Transaction.Status == ETransactionStatus::RolledBack, TEXT("G: transaction rolled back"));
			}
			// Unlock first: the locked file cannot even be read, and the
			// whole point is that a failed update left it untouched.
			CloseHandle(Lock);
			{
				// The file must be byte-identical to before the update
				// (still the corrupted bytes, not the manifest's).
				FString Sha1;
				int64 Size = 0;
				FLocalTruthExtractor::HashWholeFile(PakPath, Sha1, Size);
				Check(Sha1 == CorruptedSha1, TEXT("G: locked file unchanged after failed update"));
			}

			// Unlocked: the same verify update now repairs the file.
			FUpdateStats SOk;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.0.0"), Install, true, VerifyOptions, SOk),
				TEXT("G: update succeeds after unlock"));
			{
				// Virtual-disk model: logically equivalent, not byte-identical.
				Check(CheckContainerLogicallyEqual(Index, Install, TEXT("pakchunk1-Windows.pak")),
					TEXT("G: pak repaired after unlock (per-entry hashes match remote)"));
			}
		}

		// --- H. Executor-level cases: segmentation / resume-only-missing /
		//     fairness / probe fallback (unified-downloader tasks 4.2-4.5) --
		{
			const int64 BigTotal = 9ll << 20; // the 9MB synthetic from group B
			const int64 SegSize = 4ll << 20;
			const FString BigName = TEXT("chunks/resume-tail-big.chunk");
			const FString BigUrl = FString::Printf(TEXT("%s/%s"), *BaseUrl, *BigName);

			// A recording fake backend: file-backed delivery with request
			// bookkeeping (offsets, sizes, count) - deterministic, no network.
			struct FRecordingBackend : public IChunkDownloader
			{
				FString Root;
				FCriticalSection StatsLock;
				TArray<int64> Offsets;
				TArray<int64> Sizes;
				int32 WholeFileGets = 0;

				bool DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& OutData) override
				{
					{
						FScopeLock Scope(&StatsLock);
						if (Offset == 0) { WholeFileGets += 1; }
						Offsets.Add(Offset);
						Sizes.Add(Size);
					}
					FString Rest = Url;
					const int32 Scheme = Rest.Find(TEXT("://"), ESearchCase::CaseSensitive);
					if (Scheme != INDEX_NONE)
					{
						const int32 PathStart = Rest.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Scheme + 3);
						if (PathStart != INDEX_NONE)
						{
							Rest = Rest.RightChop(PathStart + 1);
						}
					}
					TArray64<uint8> Whole;
					if (!FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Root, Rest)))
					{
						return false;
					}
					const int64 End = Size < 0 ? Whole.Num() : FMath::Min<int64>(Offset + Size, Whole.Num());
					OutData.SetNumUninitialized(End - Offset);
					FMemory::Memcpy(OutData.GetData(), Whole.GetData() + Offset, End - Offset);
					return true;
				}
			};

			// --- H1: multi-segment fetch of a >4MB file (4.2) ---------------
			{
				const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH1"), TEXT("big.chunk"));
				FRecordingBackend Backend;
				Backend.Root = Cdn;
				FDownloadExecutor Executor(Backend);

				std::atomic<bool> bDone{ false };
				FDownloadJob Job;
				Job.Id = TEXT("H1");
				Job.Url = BigUrl;
				Job.DestPath = Dest;
				Job.ExpectedSize = BigTotal;
				Executor.Submit(MoveTemp(Job), [&bDone](const FDownloadResult&) { bDone.store(true); });
				// The file backend settles inline - by the time Submit returns
				// the job has already run to completion.
				Check(bDone.load(), TEXT("H1: big-file job completed"));

				const bool bOkFile = IFileManager::Get().FileSize(*Dest) == BigTotal;
				Check(bOkFile, TEXT("H1: big file downloaded with the full size"));
				int32 SegmentCount = 0;
				int64 MaxSeg = 0;
				{
					FScopeLock Scope(&Backend.StatsLock);
					SegmentCount = Backend.Offsets.Num();
					for (const int64 S : Backend.Sizes) { MaxSeg = FMath::Max(MaxSeg, S); }
				}
				Check(SegmentCount == 3, TEXT("H1: 9MB file split into 3 segments (4MB quantum)"));
				Check(MaxSeg <= SegSize, TEXT("H1: no segment exceeds the 4MB quantum"));
				if (bOkFile)
				{
					TArray64<uint8> Final;
					TArray64<uint8> Whole;
					if (FFileHelper::LoadFileToArray(Final, *Dest) &&
						FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Cdn, BigName)) &&
						FMemory::Memcmp(Final.GetData(), Whole.GetData(), BigTotal) == 0)
					{
						Check(true, TEXT("H1: big-file content matches the CDN copy"));
					}
					else
					{
						Check(false, TEXT("H1: big-file content matches the CDN copy"));
					}
				}
			}

			// --- H2: journal resume fetches ONLY the missing segments (4.3) -
			{
				const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH2"), TEXT("big.chunk"));
				PF.CreateDirectoryTree(*FPaths::GetPath(Dest));
				const FString Staging = Dest + TEXT(".download");
				const FString Journal = Dest + TEXT(".journal");

				TArray64<uint8> Whole;
				FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Cdn, BigName));

				// Crash simulation: segment 0 is on disk + journaled; segments
				// 1-2 are missing. The restart must fetch exactly those two.
				bool bStaged = false;
				{
					IFileHandle* H = PF.OpenWrite(*Staging);
					if (H)
					{
						H->Seek(BigTotal - 1);
						uint8 Zero = 0;
						H->Write(&Zero, 1);
						H->Seek(0);
						H->Write(Whole.GetData(), (int32)SegSize);
						delete H;
						bStaged = true;
					}
				}
				Check(bStaged, TEXT("H2: staging file written"));
				const FString Text = FString::Printf(TEXT("UGPJRN1 %lld %lld\nSEG 0\n"), SegSize, BigTotal);
				Check(FFileHelper::SaveStringToFile(Text, *Journal, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM),
					TEXT("H2: journal written"));

				FRecordingBackend Backend;
				Backend.Root = Cdn;
				FDownloadExecutor Executor(Backend);

				FDownloadJob Job;
				Job.Id = TEXT("H2");
				Job.Url = BigUrl;
				Job.DestPath = Dest;
				Job.ExpectedSize = BigTotal;
				Executor.Submit(MoveTemp(Job), [](const FDownloadResult&) {});

				bool bOnlyMissing = false;
				int64 MaxRange = 0;
				{
					FScopeLock Scope(&Backend.StatsLock);
					bOnlyMissing = Backend.Offsets.Num() == 2 &&
						Backend.Offsets[0] == SegSize && Backend.Offsets[1] == SegSize * 2;
					for (const int64 S : Backend.Sizes) { MaxRange = FMath::Max(MaxRange, S); }
					if (!bOnlyMissing)
					{
						FString Off;
						for (const int64 O : Backend.Offsets) { Off += FString::Printf(TEXT(" %lld"), O); }
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("H2 offsets:%s"), *Off);
					}
				}
				Check(bOnlyMissing, TEXT("H2: only segments 1-2 were re-fetched (journal replay honored)"));
				Check(MaxRange <= SegSize, TEXT("H2: refetched segments stay within the quantum"));
				const bool bFinalOk = IFileManager::Get().FileSize(*Dest) == BigTotal &&
					!PF.FileExists(*Staging) && !PF.FileExists(*Journal);
				Check(bFinalOk, TEXT("H2: resumed file promoted (full size, no journal left)"));
				if (bFinalOk)
				{
					TArray64<uint8> Final;
					if (FFileHelper::LoadFileToArray(Final, *Dest) &&
						FMemory::Memcmp(Final.GetData(), Whole.GetData(), BigTotal) == 0)
					{
						Check(true, TEXT("H2: resumed content matches the CDN copy"));
					}
					else
					{
						Check(false, TEXT("H2: resumed content matches the CDN copy"));
					}
				}
			}

			// --- H2b: journal resume from a NATURALLY-produced journal (C5
			// regression). H2 hand-crafts the journal - which masked the bug
			// where a fresh download NEVER wrote the UGPJRN1 header (only
			// JournalReset in the verify-retry path did), so real crash-resume
			// always replay-rejected and re-downloaded everything. Here the
			// journal is produced by the executor itself: a stalling backend
			// holds segments 1-2 in flight, segment 0 settles (written +
			// journaled), then the executor is destroyed - the exact on-disk
			// state a killed process leaves behind.
			{
				const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH2b"), TEXT("big.chunk"));
				PF.CreateDirectoryTree(*FPaths::GetPath(Dest));
				const FString Staging = Dest + TEXT(".download");
				const FString Journal = Dest + TEXT(".journal");
				TArray64<uint8> Whole;
				FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Cdn, BigName));

				struct FStallingBackend : public IChunkDownloader
				{
					FString Root;
					FCriticalSection Lock;
					struct FPending
					{
						int64 Offset;
						TFunction<void(bool, const TArray64<uint8>&, const FString&)> OnDone;
					};
					TArray<FPending> Pending;

					virtual bool DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& OutData) override
					{
						return false; // unused (jobs carry known sizes)
					}
					virtual FCancelHandle DownloadRangeAsync(const FString& Url, int64 Offset, int64 Size,
						TFunction<void(bool, const TArray64<uint8>&, const FString&)>&& OnDone) override
					{
						FScopeLock Scope(&Lock);
						Pending.Add({ Offset, MoveTemp(OnDone) });
						return MakeShared<int32>(0);
					}
					bool DeliverOne(int32 Index, const TArray64<uint8>& Whole, int64 Total)
					{
						TFunction<void(bool, const TArray64<uint8>&, const FString&)> Cb;
						int64 Offset = -1;
						{
							FScopeLock Scope(&Lock);
							if (!Pending.IsValidIndex(Index)) { return false; }
							Offset = Pending[Index].Offset;
							Cb = MoveTemp(Pending[Index].OnDone);
							Pending[Index].OnDone = nullptr;
						}
						if (!Cb) { return false; }
						const int64 Sz = FMath::Min<int64>(Total - Offset, 4 << 20);
						TArray64<uint8> Data;
						Data.SetNumUninitialized(Sz);
						FMemory::Memcpy(Data.GetData(), Whole.GetData() + Offset, Sz);
						Cb(true, Data, FString());
						return true;
					}
				};

				{
					FStallingBackend Stall;
					Stall.Root = Cdn;
					{
						FDownloadExecutor Executor(Stall);
						FDownloadJob Job;
						Job.Id = TEXT("H2b");
						Job.Url = BigUrl;
						Job.DestPath = Dest;
						Job.ExpectedSize = BigTotal;
						Executor.Submit(MoveTemp(Job), [](const FDownloadResult&) {});
						// Three segments issued and pending; deliver segment 0
						// only, then destroy the executor mid-flight.
						Check(Stall.DeliverOne(0, Whole, BigTotal), TEXT("H2b: segment 0 delivered"));
						Check(Stall.Pending.Num() == 3, TEXT("H2b: three segments were in flight"));
					} // executor destroyed: journal + staging MUST survive (no Finish ran)
					Check(PF.FileExists(*Journal), TEXT("H2b: journal survived the interruption"));
					Check(PF.FileExists(*Staging), TEXT("H2b: staging survived the interruption"));
					// THE C5 ASSERTION: the journal must carry the production
					// header (a fresh download never wrote it before the fix).
					FString JournalText;
					Check(FFileHelper::LoadFileToString(JournalText, *Journal) &&
						JournalText.StartsWith(TEXT("UGPJRN1")),
						TEXT("H2b: the natural journal carries the UGPJRN1 header"));
					Check(JournalText.Contains(TEXT("SEG 0")), TEXT("H2b: segment 0 is journaled"));
				} // stalling backend dies WITHOUT firing segments 1-2 (no cleanup ran)

				// Restart: only the missing segments may be re-fetched.
				FRecordingBackend Backend;
				Backend.Root = Cdn;
				FDownloadExecutor Executor(Backend);
				FDownloadJob Job;
				Job.Id = TEXT("H2b-restart");
				Job.Url = BigUrl;
				Job.DestPath = Dest;
				Job.ExpectedSize = BigTotal;
				Executor.Submit(MoveTemp(Job), [](const FDownloadResult&) {});
				bool bOnlyMissing = false;
				{
					FScopeLock Scope(&Backend.StatsLock);
					bOnlyMissing = Backend.Offsets.Num() == 2 &&
						Backend.Offsets[0] == SegSize && Backend.Offsets[1] == SegSize * 2;
					if (!bOnlyMissing)
					{
						FString Off;
						for (const int64 O : Backend.Offsets) { Off += FString::Printf(TEXT(" %lld"), O); }
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("H2b restart offsets:%s"), *Off);
					}
				}
				Check(bOnlyMissing, TEXT("H2b: restart fetched ONLY the missing segments (natural journal replayed)"));
				const bool bPromoted = IFileManager::Get().FileSize(*Dest) == BigTotal &&
					!PF.FileExists(*Staging) && !PF.FileExists(*Journal);
				Check(bPromoted, TEXT("H2b: resumed file promoted"));
				if (bPromoted)
				{
					TArray64<uint8> Final;
					Check(FFileHelper::LoadFileToArray(Final, *Dest) &&
						FMemory::Memcmp(Final.GetData(), Whole.GetData(), BigTotal) == 0,
						TEXT("H2b: resumed content matches the CDN copy"));
				}
			}

			// --- H3: fairness - later small jobs finish before the big (4.4) -
			{
				// Real HTTP backend: completion order depends on actual transfer
				// timing. The big 9MB job is submitted first, three small
				// manifest chunks after - the finishing-priority + fair-share
				// rules must let the small ones finish first.
				const FString CacheH3 = FPaths::Combine(WorkDir, TEXT("cacheH3"));
				PF.CreateDirectoryTree(*CacheH3);

				FCriticalSection OrderLock;
				TArray<FString> FinishOrder;
				FDownloadExecutor Executor(Http);
				std::atomic<int32> Remaining{ 4 };
				FEvent* H3Done = FPlatformProcess::GetSynchEventFromPool(true);
				auto Track = [&OrderLock, &FinishOrder, &Remaining, H3Done](const FDownloadResult& R)
				{
					{
						FScopeLock Scope(&OrderLock);
						FinishOrder.Add(R.Id);
					}
					if (Remaining.fetch_sub(1) == 1)
					{
						H3Done->Trigger();
					}
				};

				FDownloadJob Big;
				Big.Id = TEXT("H3-big");
				Big.Url = BigUrl;
				Big.DestPath = FPaths::Combine(CacheH3, TEXT("big.chunk"));
				Big.ExpectedSize = BigTotal;
				Executor.Submit(MoveTemp(Big), Track);

				int32 SmallIdx = 0;
				for (const TPair<FString, FIndexChunkEntry>& Pair : Index.Chunks)
				{
					if (++SmallIdx > 3) { break; }
					FDownloadJob Small;
					Small.Id = Pair.Key;
					Small.Url = FString::Printf(TEXT("%s/%s"), *BaseUrl, *Pair.Value.File);
					Small.DestPath = FPaths::Combine(CacheH3, Pair.Value.File);
					Small.ExpectedSize = Pair.Value.Size;
					Executor.Submit(MoveTemp(Small), Track);
				}

				const bool bAllDone = H3Done->Wait(60 * 1000);
				FPlatformProcess::ReturnSynchEventToPool(H3Done);
				Check(bAllDone, TEXT("H3: all 4 jobs completed"));

				bool bSmallsFirst = false;
				{
					FScopeLock Scope(&OrderLock);
					const int32 BigPos = FinishOrder.Find(TEXT("H3-big"));
					int32 SmallCount = 0;
					for (int32 I = 0; I < BigPos; ++I)
					{
						if (FinishOrder[I] != TEXT("H3-big")) { ++SmallCount; }
					}
					bSmallsFirst = BigPos >= 0 && SmallCount >= 2;
				}
				Check(bSmallsFirst, TEXT("H3: small jobs finish before the big one (fair share + finishing priority)"));
			}

			// --- H4a: lied manifest vs transport cap (4.5) ------------------
			{
				// The manifest declares 512KB but the server serves the full
				// 9MB body: the transport cap (lowered for the test) must
				// cancel at header time and nothing may land on disk.
				const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH4"), TEXT("big.chunk"));
				const int64 SavedCap = FHttpRangeRequest::MaxResponseBytes;
				FHttpRangeRequest::MaxResponseBytes = 1ll << 20;

				TArray<FDownloadRequest> Requests;
				FDownloadRequest Lied;
				Lied.Id = TEXT("H4-cap");
				Lied.Url = BigUrl;
				Lied.Size = 512ll << 10;
				Lied.DestFile = Dest;
				Requests.Add(Lied);

				FDownloadStats StatsH4;
				TArray<FString> FailedH4;
				const bool bBatchOk = Manager.DownloadAll(Requests, StatsH4, FailedH4);
				FHttpRangeRequest::MaxResponseBytes = SavedCap;

				Check(!bBatchOk && FailedH4.Num() == 1 && FailedH4[0] == TEXT("H4-cap"),
					TEXT("H4a: lied-size transfer refused (whole-body fetch over the cap)"));
				Check(!PF.FileExists(*Dest) && !PF.FileExists(*(Dest + TEXT(".download"))) &&
					!PF.FileExists(*(Dest + TEXT(".journal"))),
					TEXT("H4a: nothing landed on disk"));
			}

			// --- H4b: no-Range server falls back to a single whole-file GET (4.5)
			{
				const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH4b"), TEXT("big.chunk"));

				struct FNoRangeBackend : public FRecordingBackend
				{
					void ProbeSizeAsync(const FString& Url, TFunction<void(int64, bool)>&& OnDone) override
					{
						// A server that strips Range: probe gets the total from
						// the plain 200 but reports Range as unsupported.
						TArray64<uint8> Whole;
						FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Root, TEXT("chunks"), TEXT("resume-tail-big.chunk")));
						OnDone(Whole.Num(), false);
					}
				};

				FNoRangeBackend Backend;
				Backend.Root = Cdn;
				FDownloadExecutor Executor(Backend);

				FDownloadJob Job;
				Job.Id = TEXT("H4b");
				Job.Url = BigUrl;
				Job.DestPath = Dest;
				Job.ExpectedSize = -1; // unknown: the executor must probe first
				Executor.Submit(MoveTemp(Job), [](const FDownloadResult&) {});

				bool bSingleWholeGet = false;
				{
					FScopeLock Scope(&Backend.StatsLock);
					bSingleWholeGet = Backend.WholeFileGets == 1 && Backend.Offsets.Num() == 1;
				}
				Check(bSingleWholeGet, TEXT("H4b: no-Range server -> exactly one whole-file GET (no segmentation)"));

				TArray64<uint8> Whole;
				TArray64<uint8> Final;
				const bool bContentOk = FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Cdn, BigName)) &&
					FFileHelper::LoadFileToArray(Final, *Dest) &&
					Final.Num() == Whole.Num() &&
					FMemory::Memcmp(Final.GetData(), Whole.GetData(), Whole.Num()) == 0;
				Check(bContentOk, TEXT("H4b: whole-file fallback content matches the CDN copy"));
			}

			// --- H5: job cancellation (executor Cancel, untested until now) --
			{
				// A DEFERRED backend: requests are queued and settle only on
				// Flush() - the only way to interleave Cancel with in-flight
				// segments against a synchronous test backend.
				struct FDeferredBackend : public IChunkDownloader
				{
					struct FPending
					{
						FString Url;
						int64 Offset = 0;
						int64 Size = 0;
						TFunction<void(bool, const TArray64<uint8>&, const FString&)> OnDone;
						bool bCanceled = false;
					};
					TArray<TSharedPtr<FPending>> Pending;

					bool DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& OutData) override
					{
						bool bOk = false;
						DownloadRangeAsync(Url, Offset, Size, [&bOk, &OutData](bool bSuccess, const TArray64<uint8>& Data, const FString&)
						{
							bOk = bSuccess;
							if (bSuccess) { OutData = Data; }
						});
						return bOk;
					}

					FCancelHandle DownloadRangeAsync(const FString& Url, int64 Offset, int64 Size,
						TFunction<void(bool bOk, const TArray64<uint8>& Data, const FString& Error)>&& OnDone) override
					{
						TSharedPtr<FPending> P = MakeShared<FPending>();
						P->Url = Url;
						P->Offset = Offset;
						P->Size = Size;
						P->OnDone = MoveTemp(OnDone);
						Pending.Add(P);
						return P;
					}

					void CancelDownload(const FCancelHandle& Handle) override
					{
						for (const TSharedPtr<FPending>& P : Pending)
						{
							if (P.Get() == Handle.Get())
							{
								P->bCanceled = true;
							}
						}
					}

					void Flush(const FString& Root)
					{
						for (const TSharedPtr<FPending>& P : Pending)
						{
							if (P->bCanceled)
							{
								P->OnDone(false, TArray64<uint8>(), TEXT("canceled by test"));
								continue;
							}
							TArray64<uint8> Whole;
							// Slice from the CDN file by absolute offset.
							FString Rel = P->Url;
							const int32 Scheme = Rel.Find(TEXT("://"), ESearchCase::CaseSensitive);
							if (Scheme != INDEX_NONE)
							{
								const int32 PathStart = Rel.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Scheme + 3);
								if (PathStart != INDEX_NONE)
								{
									Rel = Rel.RightChop(PathStart + 1);
								}
							}
							const bool bOk = FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Root, Rel));
							TArray64<uint8> Out;
							if (bOk)
							{
								const int64 End = P->Size < 0 ? Whole.Num() : FMath::Min<int64>(P->Offset + P->Size, Whole.Num());
								if (P->Offset >= 0 && End > P->Offset && End <= Whole.Num())
								{
									Out.SetNumUninitialized(End - P->Offset);
									FMemory::Memcpy(Out.GetData(), Whole.GetData() + P->Offset, End - P->Offset);
								}
							}
							P->OnDone(bOk && Out.Num() > 0, Out, bOk ? FString() : TEXT("slice failed"));
						}
						Pending.Reset();
					}
				};

				// Case a: cancel while segments are in flight -> exactly one
				// failure result, staging/journal cleaned up.
				{
					const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH5"), TEXT("big.chunk"));
					FDeferredBackend Backend;
					FDownloadExecutor Executor(Backend);

					std::atomic<int32> DoneCount{ 0 };
					std::atomic<bool> bCancelReason{ false };
					FDownloadJob Job;
					Job.Id = TEXT("H5-cancel");
					Job.Url = BigUrl;
					Job.DestPath = Dest;
					Job.ExpectedSize = BigTotal;
					const int64 Handle = Executor.Submit(MoveTemp(Job), [&DoneCount, &bCancelReason](const FDownloadResult& R)
					{
						DoneCount.fetch_add(1);
						if (!R.bSuccess && R.Error.Contains(TEXT("cancel")))
						{
							bCancelReason.store(true);
						}
					});
					Check(Backend.Pending.Num() > 0, TEXT("H5: segments issued before the cancel"));
					Executor.Cancel(Handle);
					// Review H2: the cancel must reach the TRANSPORT - the
					// executor has to store the backend's cancel handle and
					// hand it to CancelDownload. With the dropped-return-value
					// bug this stayed bookkeeping-only and every pending
					// request kept transferring to completion.
					{
						int32 NumTransportCanceled = 0;
						for (const TSharedPtr<FDeferredBackend::FPending>& P : Backend.Pending)
						{
							if (P->bCanceled)
							{
								++NumTransportCanceled;
							}
						}
						Check(NumTransportCanceled > 0, TEXT("H5: the in-flight transport request received CancelDownload"));
					}
					Backend.Flush(Cdn);

					Check(DoneCount.load() == 1, TEXT("H5: OnDone fired exactly once after cancel"));
					Check(bCancelReason.load(), TEXT("H5: canceled job fails with a cancel reason"));
					Check(!PF.FileExists(*Dest) && !PF.FileExists(*(Dest + TEXT(".download"))) &&
						!PF.FileExists(*(Dest + TEXT(".journal"))),
						TEXT("H5: canceled job leaves no sidecars"));
				}

				// Case b: cancel AFTER completion is a guarded no-op.
				{
					const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH5b"), TEXT("big.chunk"));
					FRecordingBackend Backend;
					Backend.Root = Cdn;
					FDownloadExecutor Executor(Backend);

					std::atomic<int32> DoneCount{ 0 };
					FDownloadJob Job;
					Job.Id = TEXT("H5-done");
					Job.Url = BigUrl;
					Job.DestPath = Dest;
					Job.ExpectedSize = BigTotal;
					const int64 Handle = Executor.Submit(MoveTemp(Job), [&DoneCount](const FDownloadResult&) { DoneCount.fetch_add(1); });
					const int32 AfterDone = DoneCount.load();
					Executor.Cancel(Handle);
					Check(DoneCount.load() == AfterDone, TEXT("H5: cancel after completion is a no-op"));
				}
			}

			// --- H6: whole-file SHA1 verify path (ExpectedSha1, untested) -----
			{
				// Success: correct SHA1 promotes.
				{
					const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH6"), TEXT("big.chunk"));
					FRecordingBackend Backend;
					Backend.Root = Cdn;
					FDownloadExecutor Executor(Backend);

					TArray64<uint8> Whole;
					FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Cdn, BigName));
					FSHA1 Hasher;
					Hasher.Update(Whole.GetData(), (int32)Whole.Num());
					Hasher.Final();
					uint8 Digest[20];
					Hasher.GetHash(Digest);

					std::atomic<bool> bOk{ false };
					FDownloadJob Job;
					Job.Id = TEXT("H6-ok");
					Job.Url = BigUrl;
					Job.DestPath = Dest;
					Job.ExpectedSize = BigTotal;
					Job.ExpectedSha1 = FPatcherHash::BytesToHex(Digest, 20);
					Executor.Submit(MoveTemp(Job), [&bOk](const FDownloadResult& R) { bOk.store(R.bSuccess); });
					Check(bOk.load(), TEXT("H6: correct ExpectedSha1 promotes"));
				}

				// Failure: wrong SHA1 on a correct-size file triggers the
				// verify-failure re-download loop (all segments reissued every
				// round: 3 segments x (1 + MaxRetries) rounds) and terminal
				// failure with no sidecars.
				{
					const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH6bad"), TEXT("big.chunk"));
					FRecordingBackend Backend;
					Backend.Root = Cdn;
					FDownloadExecutor Executor(Backend);

					std::atomic<bool> bOk{ true };
					FDownloadJob Job;
					Job.Id = TEXT("H6-bad");
					Job.Url = BigUrl;
					Job.DestPath = Dest;
					Job.ExpectedSize = BigTotal;
					Job.ExpectedSha1 = TEXT("0000000000000000000000000000000000000000");
					Executor.Submit(MoveTemp(Job), [&bOk](const FDownloadResult& R) { bOk.store(R.bSuccess); });

					int32 SegmentRequests = 0;
					{
						FScopeLock Scope(&Backend.StatsLock);
						SegmentRequests = Backend.Offsets.Num();
					}
					Check(!bOk.load(), TEXT("H6: wrong ExpectedSha1 fails the job"));
					Check(SegmentRequests == 3 * 4, TEXT("H6: verify failure re-downloads all segments every round (3 segs x 4 rounds)"));
					Check(!PF.FileExists(*(Dest + TEXT(".download"))) && !PF.FileExists(*(Dest + TEXT(".journal"))),
						TEXT("H6: failed verify leaves no sidecars"));
				}
			}

			// --- H7: journal resume with a NON-CONTIGUOUS hole (only the
			//     middle segment journaled done -> segments 0 and 2 refetch) -
			{
				const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH7"), TEXT("big.chunk"));
				PF.CreateDirectoryTree(*FPaths::GetPath(Dest));
				const FString Staging = Dest + TEXT(".download");
				const FString Journal = Dest + TEXT(".journal");

				TArray64<uint8> Whole;
				FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Cdn, BigName));
				{
					IFileHandle* H = PF.OpenWrite(*Staging);
					if (H)
					{
						H->Seek(BigTotal - 1);
						uint8 Zero = 0;
						H->Write(&Zero, 1);
						H->Seek(SegSize); // middle segment bytes on disk
						H->Write(Whole.GetData() + SegSize, (int32)SegSize);
						delete H;
					}
					const FString Text = FString::Printf(TEXT("UGPJRN1 %lld %lld\nSEG 1\n"), SegSize, BigTotal);
					Check(FFileHelper::SaveStringToFile(Text, *Journal, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM),
						TEXT("H7: journal written (only SEG 1 done)"));
				}

				FRecordingBackend Backend;
				Backend.Root = Cdn;
				FDownloadExecutor Executor(Backend);

				FDownloadJob Job;
				Job.Id = TEXT("H7");
				Job.Url = BigUrl;
				Job.DestPath = Dest;
				Job.ExpectedSize = BigTotal;
				Executor.Submit(MoveTemp(Job), [](const FDownloadResult&) {});

				bool bHolesOnly = false;
				{
					FScopeLock Scope(&Backend.StatsLock);
					bHolesOnly = Backend.Offsets.Num() == 2 &&
						Backend.Offsets[0] == 0 && Backend.Offsets[1] == SegSize * 2;
				}
				Check(bHolesOnly, TEXT("H7: non-contiguous journal refetches exactly segments 0 and 2"));
				{
					TArray64<uint8> Final;
					const bool bOk = IFileManager::Get().FileSize(*Dest) == BigTotal &&
						FFileHelper::LoadFileToArray(Final, *Dest) &&
						FMemory::Memcmp(Final.GetData(), Whole.GetData(), BigTotal) == 0;
					Check(bOk, TEXT("H7: assembled content matches the CDN copy"));
				}
			}

			// --- H8: probe SUCCESS drives a segmented download; real-HTTP
			//     segment 0 uses an exact Range (bytes=0-(n-1), 206) ----------
			{
				const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH8"), TEXT("big.chunk"));

				// (a) Recording backend with a working probe: unknown size ->
				// probed total -> segmented fetch (the bRangeOk==true mainline).
				{
					struct FProbingBackend : public FRecordingBackend
					{
						void ProbeSizeAsync(const FString& Url, TFunction<void(int64, bool)>&& OnDone) override
						{
							TArray64<uint8> Whole;
							FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Root, TEXT("chunks"), TEXT("resume-tail-big.chunk")));
							OnDone(Whole.Num(), true);
						}
					};
					FProbingBackend Backend;
					Backend.Root = Cdn;
					FDownloadExecutor Executor(Backend);

					FDownloadJob Job;
					Job.Id = TEXT("H8a");
					Job.Url = BigUrl;
					Job.DestPath = Dest;
					Job.ExpectedSize = -1; // unknown: probe first
					Executor.Submit(MoveTemp(Job), [](const FDownloadResult&) {});

					bool bSegmentedFromProbe = false;
					{
						FScopeLock Scope(&Backend.StatsLock);
						bSegmentedFromProbe = Backend.Offsets.Num() == 3 &&
							Backend.Offsets[0] == 0 && Backend.Offsets[1] == SegSize && Backend.Offsets[2] == SegSize * 2;
					}
					Check(bSegmentedFromProbe, TEXT("H8a: successful probe drives the full segmented fetch"));
				}

				// (b) Real HTTP: every segment of a fresh multi-segment fetch -
				// INCLUDING segment 0 - goes out as an exact Range (206).
				{
					const FString Dest2 = FPaths::Combine(WorkDir, TEXT("cacheH8b"), TEXT("big.chunk"));
					const int32 FilesBefore = CdnServer.Stats().Files;
					const int32 RangesBefore = CdnServer.Stats().Ranges;

					FDownloadManager ManagerH8(Http, /*MaxConcurrent=*/8, /*MaxRetries=*/3);
					TArray<FDownloadRequest> H8Requests;
					FDownloadRequest H8;
					H8.Id = TEXT("H8b");
					H8.Url = BigUrl;
					H8.Size = BigTotal;
					H8.DestFile = Dest2;
					H8Requests.Add(H8);
					FDownloadStats H8Stats;
					TArray<FString> H8Failed;
					Check(ManagerH8.DownloadAll(H8Requests, H8Stats, H8Failed), TEXT("H8b: multi-segment fetch over real HTTP succeeds"));

					const int32 FilesDelta = CdnServer.Stats().Files - FilesBefore;
					const int32 RangesDelta = CdnServer.Stats().Ranges - RangesBefore;
					Check(FilesDelta == 3 && RangesDelta == 3,
						TEXT("H8b: all 3 segments (incl. segment 0) used exact 206 Range requests"));
				}

				// (c) Real FHttpRangeRequest::ProbeSizeAsync against the live
				// server: total + Range-support verdict, body never read.
				{
					std::atomic<int64> ProbedTotal{ -1 };
					std::atomic<bool> ProbedRange{ false };
					FEvent* ProbeDone = FPlatformProcess::GetSynchEventFromPool(true);
					Http.ProbeSizeAsync(BigUrl, [ProbeDone, &ProbedTotal, &ProbedRange](int64 TotalSize, bool bRangeOk)
					{
						ProbedTotal.store(TotalSize);
						ProbedRange.store(bRangeOk);
						ProbeDone->Trigger();
					});
					Check(ProbeDone->Wait(60 * 1000), TEXT("H8c: probe completes"));
					FPlatformProcess::ReturnSynchEventToPool(ProbeDone);
					Check(ProbedTotal.load() == BigTotal && ProbedRange.load(),
						TEXT("H8c: real probe returns the total and Range support"));
				}
			}
		}

		CdnServer.Stop();

		return FinishTest(bAllPassed, TEXT("downloadtest"));
	}

	// benchtest (unified-downloader 5.2/5.3): throughput of the windowed
	// executor vs a single connection, and the process memory peak at full
	// window. Runs against the external CDN server with a per-connection
	// rate cap so bandwidth - not loopback latency - is the bottleneck.
	static int32 RunBenchTest(const TCHAR* /*CmdLine*/)
	{
		bool bAllPassed = true;
		auto Check = MakeCheck(bAllPassed);

		using namespace UnrealGamePatcher;

		const FString WorkDir = GetTestWorkDir(TEXT("benchtest"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		const FString Cdn = FPaths::Combine(WorkDir, TEXT("cdn"));
		PF.CreateDirectoryTree(*Cdn);

		// Synthetic payloads, written incrementally so the process never
		// holds a whole payload in memory (the memory bench below measures
		// the DOWNLOAD buffers, not the generator).
		const int64 SegSize = 4ll << 20;
		const int64 TputTotal = 16ll << 20; // 16MB = 4 segments (throughput)
		const int64 MemTotal = 96ll << 20;  // 96MB = 24 segments (memory peak)
		auto GenFile = [&PF](const FString& Path, int64 Total)
		{
			TArray<uint8> Piece;
			Piece.SetNumUninitialized(1 << 20);
			for (int32 I = 0; I < Piece.Num(); ++I)
			{
				Piece[I] = (uint8)(I * 31 + (I >> 13));
			}
			IFileHandle* H = PF.OpenWrite(*Path);
			if (!H)
			{
				return false;
			}
			int64 Left = Total;
			int64 Salt = 0;
			while (Left > 0)
			{
				const int32 N = (int32)FMath::Min<int64>(Left, Piece.Num());
				Piece[0] = (uint8)(Salt++); // vary content per MB
				H->Write(Piece.GetData(), N);
				Left -= N;
			}
			delete H;
			return true;
		};
		const FString TputName = TEXT("chunks/bench-tput.bin");
		const FString MemName = TEXT("chunks/bench-mem.bin");
		PF.CreateDirectoryTree(*FPaths::Combine(Cdn, TEXT("chunks")));
		Check(GenFile(*FPaths::Combine(Cdn, TputName), TputTotal), TEXT("bench: 16MB payload generated"));
		Check(GenFile(*FPaths::Combine(Cdn, MemName), MemTotal), TEXT("bench: 96MB payload generated"));

		const FString ProjectRoot = FPaths::GetPath(FPaths::GetPath(FPaths::GetPath(FPlatformProcess::BaseDir())));
		const FString CdnScript = FPaths::Combine(ProjectRoot, TEXT("Source"), TEXT("Programs"), TEXT("UnrealGamePatcher"), TEXT("Scripts"), TEXT("cdn_server.py"));
		FCdnServerProc CdnServer;
		Check(CdnServer.Start(CdnScript, Cdn, FPaths::Combine(WorkDir, TEXT("cdn.log")), TEXT("__ctl/stats")),
			TEXT("bench: external CDN server started"));
		const FString BaseUrl = FString::Printf(TEXT("http://127.0.0.1:%d"), CdnServer.Port);
		FHttpModule::Get();

		FHttpChunkDownloader Http;
		auto ArmThrottle = [&Http, &BaseUrl](int32 Kbps)
		{
			TArray64<uint8> Ctrl;
			Http.DownloadRange(FString::Printf(TEXT("%s/__ctl/throttle?kbps=%d"), *BaseUrl, Kbps), 0, -1, Ctrl);
		};

		// --- A. Throughput: windowed executor vs a single connection -------
		{
			// 1 MB/s per connection: a single connection needs ~16s, four
			// in-flight segments should land near ~4s if concurrency is real.
			ArmThrottle(1024);
			const FString Url = FString::Printf(TEXT("%s/%s"), *BaseUrl, *TputName);

			double SingleSec = 0.0;
			{
				TArray64<uint8> Out;
				const double T0 = FPlatformTime::Seconds();
				Http.DownloadRange(Url, 0, -1, Out);
				SingleSec = FPlatformTime::Seconds() - T0;
				Check(Out.Num() == TputTotal, TEXT("bench: single-connection fetch complete"));
			}

			double WindowedSec = 0.0;
			{
				const FString Dest = FPaths::Combine(WorkDir, TEXT("tput.bin"));
				FDownloadExecutorConfig Cfg;
				Cfg.Window = 8; // 4MB quantum -> 4 segments, all in flight
				FDownloadExecutor Executor(Http, Cfg);
				FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
				FDownloadJob Job;
				Job.Id = TEXT("bench-tput");
				Job.Url = Url;
				Job.DestPath = Dest;
				Job.ExpectedSize = TputTotal;
				Executor.Submit(MoveTemp(Job), [Done](const FDownloadResult&) { Done->Trigger(); });
				const double T0 = FPlatformTime::Seconds();
				Check(Done->Wait(300 * 1000), TEXT("bench: windowed fetch complete"));
				WindowedSec = FPlatformTime::Seconds() - T0;
				FPlatformProcess::ReturnSynchEventToPool(Done);
				Check(IFileManager::Get().FileSize(*Dest) == TputTotal, TEXT("bench: windowed output has the full size"));
			}

			const double SingleMbps = (TputTotal / (1024.0 * 1024.0)) / SingleSec;
			const double WindowedMbps = (TputTotal / (1024.0 * 1024.0)) / WindowedSec;
			UE_LOG(LogUnrealGamePatcher, Display,
				TEXT("[BENCH] throughput: single-connection %.1fs (%.2f MB/s) | windowed %.1fs (%.2f MB/s) | speedup x%.2f"),
				SingleSec, SingleMbps, WindowedSec, WindowedMbps, SingleSec / WindowedSec);
			Check(WindowedSec < SingleSec, TEXT("bench: windowed throughput beats a single connection"));
		}

		// --- B. Memory peak at full window (96MB payload / 24 segments) -----
		{
			// 8 MB/s per connection with 24 in-flight segments streaming
			// concurrently: the full 96MB window cycles continuously, so the
			// ~12s total run is enough for the process peak to reflect the
			// download buffers.
			ArmThrottle(8192);
			const FString Url = FString::Printf(TEXT("%s/%s"), *BaseUrl, *MemName);
			const FString Dest = FPaths::Combine(WorkDir, TEXT("mem.bin"));

			FPlatformMemory::GetStats(); // refresh
			const uint64 PeakBefore = FPlatformMemory::GetStats().PeakUsedPhysical;

			FDownloadExecutorConfig Cfg;
			Cfg.Window = 24;
			FDownloadExecutor Executor(Http, Cfg);
			FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
			FDownloadJob Job;
			Job.Id = TEXT("bench-mem");
			Job.Url = Url;
			Job.DestPath = Dest;
			Job.ExpectedSize = MemTotal;
			Executor.Submit(MoveTemp(Job), [Done](const FDownloadResult&) { Done->Trigger(); });
			Check(Done->Wait(300 * 1000), TEXT("bench: full-window fetch complete"));
			FPlatformProcess::ReturnSynchEventToPool(Done);
			Check(IFileManager::Get().FileSize(*Dest) == MemTotal, TEXT("bench: memory-run output has the full size"));

			const uint64 PeakAfter = FPlatformMemory::GetStats().PeakUsedPhysical;
			const double DeltaMB = (double)(PeakAfter - PeakBefore) / (1024.0 * 1024.0);
			UE_LOG(LogUnrealGamePatcher, Display,
				TEXT("[BENCH] memory: process peak delta %.1f MB with window 24 x 4MB (data budget 96MB, engine buffer growth ~2x)"),
				DeltaMB);
			// The DATA budget is window x segment (96MB); the engine's curl
			// response TArray grows x1.5 with a realloc copy spike, so the
			// process peak lands at ~2x the data budget - a constant factor,
			// the window stays the linear knob.
			Check(DeltaMB < 96.0 * 2.5, TEXT("bench: memory peak within 2.5x of the data budget"));
		}

		CdnServer.Stop();

		return FinishTest(bAllPassed, TEXT("benchtest"));
	}


	static int32 RunIoStoreTest(const TCHAR* /*CmdLine*/)
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


	static int32 RunIoStoreDump(const TCHAR* CmdLine)
	{

		FString UtocPath, UcasPath;
		FParse::Value(CmdLine, TEXT("-utoc="), UtocPath);
		FParse::Value(CmdLine, TEXT("-ucas="), UcasPath);
		if (UtocPath.IsEmpty() || UcasPath.IsEmpty())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("iostoredump requires -utoc=<file.utoc> -ucas=<file.ucas>"));
			return ExitUsage;
		}

		UnrealGamePatcher::FIoStoreReader Reader;
		if (!Reader.Open(UtocPath, UcasPath))
		{
			return ExitFailure;
		}
		for (int32 I = 0; I < Reader.GetChunkCount(); ++I)
		{
			const UnrealGamePatcher::FIoStoreChunk& C = Reader.GetChunks()[I];
			UE_LOG(LogUnrealGamePatcher, Display, TEXT("chunk %4d: %s off=%lld size=%lld hash=%s blocks=%d..%d%s"),
				I, *C.Id, C.RawOffset, C.RawSize, *C.Hash,
				C.FirstBlockIndex, C.FirstBlockIndex + C.NumBlocks - 1,
				C.bIsCompressed ? TEXT(" [compressed]") : TEXT(""));
		}
		return ExitSuccess;
	
	}


	static int32 RunE2ePerfTest(const TCHAR* CmdLine)
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


	static int32 RunPakInPlace(const TCHAR* /*CmdLine*/)
	{

		// In-place pak entry-level patching (the differentiator vs whole-file
		// rewrite): build two pak versions (one entry changed in place, one
		// appended), ingest both through the real pipeline (with entry
		// metadata), install v1, then patch the pak in place and verify ONLY
		// the changed entries were written and the result is byte-exact.
		bool bAllPassed = true;
		auto Check = MakeCheck(bAllPassed);

		using namespace UnrealGamePatcher;

		const FString WorkDir = GetTestWorkDir(TEXT("pakinplace"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

		// --- Helpers ---------------------------------------------------------
		// The packed byte encoding is LSB-first for numbers (the manifest's
		// uint32/uint64 values), normal order for hashes.
		auto PackU32 = [](uint32 V) -> FString
		{
			return FString::Printf(TEXT("%03d%03d%03d%03d"), V & 0xFF, (V >> 8) & 0xFF, (V >> 16) & 0xFF, (V >> 24) & 0xFF);
		};
		auto PackU64 = [](uint64 V) -> FString
		{
			FString Out;
			for (int32 I = 0; I < 8; ++I) { Out += FString::Printf(TEXT("%03d"), (V >> (I * 8)) & 0xFF); }
			return Out;
		};
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

		/** Stages the crafted container into a version dir and ingests it
		 *  (the new staged-based pipeline - no official manifest, no BPT). */
		auto CraftAndIngest = [&](const TCHAR* Version, const FString& PakPath, const FString& LogicalName, FString& OutIndexPath) -> bool
		{
			const FString StagedDir = FPaths::Combine(WorkDir, FString::Printf(TEXT("staged_%s"), Version));
			if (!PF.DirectoryExists(*StagedDir) && !PF.CreateDirectoryTree(*StagedDir))
			{
				return false;
			}
			const FString Dst = FPaths::Combine(StagedDir, LogicalName);
			PF.DeleteFile(*Dst);
			if (!PF.CopyFile(*Dst, *PakPath))
			{
				return false;
			}
			FIngestResult Result;
			if (!FBuildIngestor::Ingest(StagedDir, TEXT("InplaceTest"), Version, FPaths::Combine(WorkDir, TEXT("cdn")), Result))
			{
				return false;
			}
			OutIndexPath = Result.IndexPath;
			return true;
		};

		// --- 1. v1 = the real staged pak; v2 = re-append entry[0] (CoW) + append.
		const FString ProjectRoot = FPaths::GetPath(FPaths::GetPath(FPaths::GetPath(FPlatformProcess::BaseDir())));
		const FString SrcPak = FPaths::Combine(ProjectRoot, TEXT(".manifesttest"), TEXT("chunkinstall"), TEXT("Windows"), TEXT("1.0.0"), TEXT("pakchunk1"), TEXT("pakchunk1-Windows.pak"));
		Check(PF.FileExists(*SrcPak), TEXT("inplace: source pak present"));
		const FString V1Pak = FPaths::Combine(WorkDir, TEXT("v1.pak"));
		const FString V2Pak = FPaths::Combine(WorkDir, TEXT("v2.pak"));
		PF.CopyFile(*V1Pak, *SrcPak);
		PF.CopyFile(*V2Pak, *SrcPak);

		int64 Entry0Size = 0;
		FString Entry0Path;
		TArray64<uint8> ReappendBytes;
		{
			// Read the entry info first; the reader must be destroyed before
			// the writer opens the file for writing (handle share flags).
			FPakReader R;
			Check(R.Open(V2Pak) && R.ReadIndex(), TEXT("inplace: v2 copy opens"));
			Check(R.GetFiles().Num() >= 1, TEXT("inplace: source pak has entries"));
			Entry0Path = R.GetFiles()[0].Path;
			Entry0Size = R.GetFiles()[0].Entry.Size;
		}
		{
			FPakWriter W;
			Check(W.Open(V2Pak), TEXT("inplace: writer opens v2"));
			ReappendBytes.SetNumUninitialized(Entry0Size);
			for (int32 I = 0; I < ReappendBytes.Num(); ++I) { ReappendBytes[I] = (uint8)(0xA0 + (I & 0x0F)); }
			// CoW v2: the changed entry is re-appended (never overwritten in
			// place); the writer computes the fresh built-in hash.
			FPakWriter::FEntryToWrite Replaced;
			Replaced.Path = Entry0Path;
			Replaced.CompressionMethodIndex = 0;
			Replaced.UncompressedSize = ReappendBytes.Num();
			FPakWriter::FStoredBlock ReplacedBlock;
			ReplacedBlock.Bytes = ReappendBytes; // copy: the test asserts on these bytes later
			Replaced.Blocks.Add(MoveTemp(ReplacedBlock));
			FPakEntry ReplacedOut;
			Check(W.AppendEntry(Replaced, ReplacedOut), TEXT("inplace: v2 re-appends entry[0] (CoW)"));
			FPakWriter::FEntryToWrite Appended;
			Appended.Path = TEXT("patched.txt");
			Appended.CompressionMethodIndex = 0;
			Appended.UncompressedSize = 15;
			FPakWriter::FStoredBlock Block;
			const char* Text = "in-place-patched";
			Block.Bytes.SetNumUninitialized(15);
			FMemory::Memcpy(Block.Bytes.GetData(), Text, 15);
			Appended.Blocks.Add(MoveTemp(Block));
			FPakEntry OutEntry;
			Check(W.AppendEntry(Appended, OutEntry), TEXT("inplace: v2 appends patched.txt"));
			Check(W.Commit(), TEXT("inplace: v2 committed"));
			// The commit pads to a 512-byte sector boundary and writes the
			// footer as one single-sector (<=221B) write - the crash-model
			// requirement. File size == FooterEnd, so the size itself pins it.
			Check(IFileManager::Get().FileSize(*V2Pak) % 512 == 0,
				TEXT("inplace: committed pak is 512-sector aligned (single-sector footer write)"));
		}

		// --- 2. Ingest both versions (entry metadata extracted by the ingestor).
		FString IndexV1, IndexV2;
		Check(CraftAndIngest(TEXT("1.0"), V1Pak, TEXT("game.pak"), IndexV1), TEXT("inplace: ingest v1"));
		Check(CraftAndIngest(TEXT("1.1"), V2Pak, TEXT("game.pak"), IndexV2), TEXT("inplace: ingest v2"));
		FIndexManifest Remote;
		Check(FIndexManifestFile::Load(IndexV2, Remote), TEXT("inplace: v2 index loads"));

		// The ingest must have recorded pak entry metadata.
		Check(Remote.Files[TEXT("game.pak")].PakEntries.Num() == 3, TEXT("inplace: v2 index has 3 pak entry records"));
		{
			bool bHasPatched = false;
			for (const FIndexPakEntry& PE : Remote.Files[TEXT("game.pak")].PakEntries)
			{
				if (PE.Path == TEXT("/patched.txt") || PE.Path == TEXT("patched.txt")) { bHasPatched = true; }
			}
			Check(bHasPatched, TEXT("inplace: appended entry recorded"));
		}

		// --- 3. Install v1 (whole-file path), then patch to v1.1 in place.
		const FString Cdn = FPaths::Combine(WorkDir, TEXT("cdn"));
		const FString Install = FPaths::Combine(WorkDir, TEXT("install"));
		PF.CreateDirectoryTree(*Install);
		FUpdateStats S0;
		Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0"), Install, true, S0), TEXT("inplace: install v1"));

		FInPlacePakStats PStats;
		const FString InstalledPak = FPaths::Combine(Install, TEXT("game.pak"));
		Check(FInPlacePakPatcher::Patch(Remote, Cdn, TEXT("game.pak"), InstalledPak, PStats),
			TEXT("inplace: in-place patch succeeds"));
		Check(PStats.NumSame == 1 && PStats.NumAppended == 2,
			TEXT("inplace: 1 same (zero movement), 2 appended (CoW: changed entry re-append + new file)"));

		// The exact byte count is pinned below by the orchestrator-path run
		// (BytesWritten == 91); keep the ratio as log-only context.
		{
			const int64 PakSize = IFileManager::Get().FileSize(*InstalledPak);
			UE_LOG(LogUnrealGamePatcher, Display, TEXT("inplace: pak size %lld, bytes written %lld (%.1f%%)"),
				PakSize, PStats.BytesWritten, PakSize > 0 ? (100.0 * (double)PStats.BytesWritten / (double)PakSize) : 0.0);
		}

		// Whole-file hash must equal the v2 manifest.
		{
			FString Sha1;
			int64 Size = 0;
			FLocalTruthExtractor::HashWholeFile(InstalledPak, Sha1, Size);
			Check(Sha1.Equals(Remote.Files[TEXT("game.pak")].SHA1, ESearchCase::IgnoreCase),
				TEXT("inplace: patched pak is byte-exact vs the v2 manifest"));
		}

		// Re-open: 3 entries, the overwritten one has the new bytes.
		{
			FPakReader R;
			Check(R.Open(InstalledPak) && R.ReadIndex(), TEXT("inplace: patched pak re-opens"));
			Check(R.GetFiles().Num() == 3, TEXT("inplace: patched pak has 3 entries"));
			bool bReappendOk = false, bAppendOk = false;
			for (const FPakReader::FPakEntryLocation& Loc : R.GetFiles())
			{
				if (Loc.Path == Entry0Path)
				{
					TArray64<uint8> Payload;
					if (R.ReadEntryData(Loc.Entry, Payload) && Payload.Num() == ReappendBytes.Num() &&
						FMemory::Memcmp(Payload.GetData(), ReappendBytes.GetData(), Payload.Num()) == 0)
					{
						bReappendOk = true;
					}
				}
				if (Loc.Path == TEXT("/patched.txt") || Loc.Path == TEXT("patched.txt"))
				{
					TArray64<uint8> Payload;
					if (R.ReadEntryData(Loc.Entry, Payload) && Payload.Num() == 15 &&
						FMemory::Memcmp(Payload.GetData(), "in-place-patched", 15) == 0)
					{
						bAppendOk = true;
					}
				}
			}
			Check(bReappendOk, TEXT("inplace: re-appended (CoW) entry has the new bytes"));
			Check(bAppendOk, TEXT("inplace: appended entry present with the expected bytes"));
		}


		// --- 4b. The UPDATE FLOW itself must use the in-place path ----------
		// Reset the install to v1, then run a normal orchestrator update to
		// 1.1: the diff sees game.pak changed and patches it in place instead
		// of rewriting the whole file.
		{
			PF.CopyFile(*InstalledPak, *V1Pak); // back to v1
			FUpdateStats SOrch;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.1"), Install, true, SOrch),
				TEXT("inplace: orchestrator update to 1.1"));
			Check(SOrch.NumFilesChanged == 1, TEXT("inplace: update flow sees exactly the pak changed"));
			Check(SOrch.BytesWritten == 91, TEXT("inplace: update flow wrote only the changed entries (91 bytes, not the whole pak)"));
			{
				FString Sha1;
				int64 Size = 0;
				FLocalTruthExtractor::HashWholeFile(InstalledPak, Sha1, Size);
				Check(Sha1.Equals(Remote.Files[TEXT("game.pak")].SHA1, ESearchCase::IgnoreCase),
					TEXT("inplace: update-flow result is byte-exact vs the target"));
				// The patcher commits through FPakWriter, so the sector-alignment
				// invariant holds for orchestrator-produced containers too.
				Check(IFileManager::Get().FileSize(*InstalledPak) % 512 == 0,
					TEXT("inplace: orchestrator-patched pak is 512-sector aligned"));
			}
		}

		// The untouched entry is byte-identical to v1's (zero movement).
		{
			FPakReader A, B;
			A.Open(V1Pak); A.ReadIndex();
			B.Open(InstalledPak); B.ReadIndex();
			const FString SecondPath = A.GetFiles()[1].Path;
			TArray64<uint8> OldBytes, NewBytes;
			A.ReadEntryData(A.GetFiles()[1].Entry, OldBytes);
			const FPakReader::FPakEntryLocation* NewLoc = nullptr;
			for (const FPakReader::FPakEntryLocation& L : B.GetFiles())
			{
				if (L.Path == SecondPath) { NewLoc = &L; break; }
			}
			B.ReadEntryData(NewLoc->Entry, NewBytes);
			Check(NewLoc && OldBytes.Num() == NewBytes.Num() &&
				FMemory::Memcmp(OldBytes.GetData(), NewBytes.GetData(), OldBytes.Num()) == 0,
				TEXT("inplace: untouched entry byte-identical (zero movement)"));
		}

		// --- 4c. Backfill: the next update places new entries INTO the dead
		// ranges the previous patch left behind (superseded entry data + the
		// old index/footer) instead of growing the file. CoW v2 recomputes the
		// holes from the committed container, so the previous patch recorded
		// nothing - the dead ranges are visible by construction.
		{
			// v3 = v2 + one more small appended entry.
			const FString V3Pak = FPaths::Combine(WorkDir, TEXT("v3.pak"));
			PF.CopyFile(*V3Pak, *V2Pak);
			{
				FPakWriter W;
				Check(W.Open(V3Pak), TEXT("backfill: v3 writer opens"));
				FPakWriter::FEntryToWrite Extra;
				Extra.Path = TEXT("backfilled.txt");
				FPakWriter::FStoredBlock Block;
				const ANSICHAR* Text = "backfilled!!";
				Block.Bytes.SetNumUninitialized(12);
				FMemory::Memcpy(Block.Bytes.GetData(), Text, 12);
				Extra.Blocks.Add(MoveTemp(Block));
				FPakEntry OutEntry;
				Check(W.AppendEntry(Extra, OutEntry) && W.Commit(), TEXT("backfill: v3 committed"));
			}
			FString IndexV3;
			Check(CraftAndIngest(TEXT("1.2"), V3Pak, TEXT("game.pak"), IndexV3), TEXT("backfill: ingest v3"));

			const int64 SizeBefore = IFileManager::Get().FileSize(*InstalledPak);
			FUpdateStats SBackfill;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.2"), Install, true, SBackfill),
				TEXT("backfill: update to 1.2 succeeds"));
			Check(SBackfill.NumFilesChanged == 1, TEXT("backfill: exactly the pak changed"));

			// The new entry must sit BELOW the pre-update EOF - physical proof
			// it was placed into a recorded dead range, not appended at EOF.
			{
				FPakReader R;
				Check(R.Open(InstalledPak) && R.ReadIndex(), TEXT("backfill: patched pak opens"));
				const FPakReader::FPakEntryLocation* Loc = nullptr;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path == TEXT("backfilled.txt") || L.Path == TEXT("/backfilled.txt")) { Loc = &L; }
				}
				Check(Loc != nullptr, TEXT("backfill: new entry present"));
				if (Loc)
				{
					Check(Loc->Entry.Offset + Loc->Entry.Size + 64 <= SizeBefore,
						TEXT("backfill: the new entry was placed INSIDE a recorded hole"));
					TArray64<uint8> Payload;
					Check(R.ReadEntryData(Loc->Entry, Payload) && Payload.Num() == 12 &&
						FMemory::Memcmp(Payload.GetData(), "backfilled!!", 12) == 0,
						TEXT("backfill: placed entry decodes correctly"));
				}
			}

			// Convergence: the rerun is a no-op.
			FUpdateStats SBackfill2;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.2"), Install, true, SBackfill2),
				TEXT("backfill: rerun converges"));
			Check(SBackfill2.NumFilesChanged == 0 && SBackfill2.BytesWritten == 0,
				TEXT("backfill: rerun is a no-op"));
		}

		// --- 5. Crash recovery (CoW v2, ROADMAP §5.2): the recovery scanner
		// is the WAL replay - truncate the torn tail back to the last valid
		// footer (magic + version + geometry + index SHA1, not magic alone),
		// then converge by replay.
		{
			auto AppendRaw = [&](const FString& Path, int64 Num, uint8 Fill) -> bool
			{
				IFileHandle* H = PF.OpenWrite(*Path, true, true);
				if (!H) { return false; }
				H->SeekFromEnd(0);
				TArray64<uint8> Garbage;
				Garbage.SetNumUninitialized(Num);
				FMemory::Memset(Garbage.GetData(), Fill, Num);
				const bool bWrote = H->Write(Garbage.GetData(), Num);
				delete H;
				return bWrote;
			};
			auto CorruptTail = [&](const FString& Path, int64 Num) -> bool
			{
				IFileHandle* H = PF.OpenWrite(*Path, true, true);
				if (!H) { return false; }
				H->SeekFromEnd(-Num);
				TArray64<uint8> Garbage;
				Garbage.SetNumUninitialized(Num);
				FMemory::Memset(Garbage.GetData(), 0xBB, Num);
				const bool bWrote = H->Write(Garbage.GetData(), Num);
				delete H;
				return bWrote;
			};

			const int64 V1Size = IFileManager::Get().FileSize(*V1Pak);
			const FString V1Sha = FPatcherHash::FileSHA1Hex(V1Pak);

			// 5a. Committed pak: Clean fast path, validated footer returned.
			const FString RecClean = FPaths::Combine(WorkDir, TEXT("rec_clean.pak"));
			PF.CopyFile(*RecClean, *V2Pak);
			{
				const FPakRecovery::FOutcome O = FPakRecovery::ScanAndRepair(RecClean);
				Check(O.Result == FPakRecovery::EResult::Clean, TEXT("recovery: committed pak scans Clean"));
				Check(O.Info.Version >= 10 && O.Info.IndexOffset > 0, TEXT("recovery: Clean carries the validated footer"));
			}

			// 5b. Torn tail (crash before the footer commit write): the v1
			// footer below the garbage is still valid - truncate to it.
			const FString RecTail = FPaths::Combine(WorkDir, TEXT("rec_tail.pak"));
			PF.CopyFile(*RecTail, *V1Pak);
			Check(AppendRaw(RecTail, 5000, 0xA5), TEXT("recovery: torn tail appended"));
			{
				const FPakRecovery::FOutcome O = FPakRecovery::ScanAndRepair(RecTail);
				Check(O.Result == FPakRecovery::EResult::Truncated && O.TruncatedToBytes == V1Size,
					TEXT("recovery: torn tail truncated to the last committed footer"));
				Check(FPatcherHash::FileSHA1Hex(RecTail) == V1Sha,
					TEXT("recovery: truncation restores the byte-exact v1"));
			}

			// 5b2. Multi-window scan: a >64KB pak whose torn tail spans several
			// 64KB scan windows. Every other fixture here is tiny, so only this
			// case exercises the backward window-walk loop - it must cross the
			// window boundary and find the committed footer in an interior
			// window.
			const FString BigPak = FPaths::Combine(WorkDir, TEXT("rec_window.pak"));
			PF.CopyFile(*BigPak, *V2Pak);
			{
				FPakWriter W;
				Check(W.Open(BigPak), TEXT("recovery: multi-window writer opens"));
				FPakWriter::FEntryToWrite Big;
				Big.Path = TEXT("bigblob.bin");
				Big.CompressionMethodIndex = 0;
				Big.UncompressedSize = 200ll * 1024;
				FPakWriter::FStoredBlock BigBlock;
				BigBlock.Bytes.SetNumUninitialized(200ll * 1024);
				for (int64 I = 0; I < BigBlock.Bytes.Num(); ++I)
				{
					BigBlock.Bytes[(int32)I] = (uint8)((I * 131 + (I >> 7)) & 0xFF);
				}
				Big.Blocks.Add(MoveTemp(BigBlock));
				FPakEntry BigOut;
				Check(W.AppendEntry(Big, BigOut) && W.Commit(), TEXT("recovery: 200KB entry committed"));
			}
			// Hash AFTER the writer handle is closed: the writer holds WRITE
			// access with FILE_SHARE_READ only, and OpenRead (which does not
			// share WRITE by default) then fails with a sharing violation -
			// FileSHA1Hex would silently return an empty string.
			const int64 BigCommitted = IFileManager::Get().FileSize(*BigPak);
			const FString BigSha = FPatcherHash::FileSHA1Hex(BigPak);
			Check(BigCommitted > 64 * 1024, TEXT("recovery: multi-window fixture exceeds one scan window"));
			Check(!BigSha.IsEmpty(), TEXT("recovery: fixture hash captured"));
			Check(AppendRaw(BigPak, 150 * 1024, 0xA5), TEXT("recovery: 150KB torn tail appended"));
			{
				const FPakRecovery::FOutcome O = FPakRecovery::ScanAndRepair(BigPak);
				Check(O.Result == FPakRecovery::EResult::Truncated && O.TruncatedToBytes == BigCommitted,
					TEXT("recovery: multi-window walk truncates to the committed footer"));
				Check(FPatcherHash::FileSHA1Hex(BigPak) == BigSha,
					TEXT("recovery: multi-window truncation is byte-exact"));
			}

			// 5c. Torn footer (crash during the sector-atomic footer write):
			// the v2 footer is unreadable, the v1 footer below still is.
			const FString RecFooter = FPaths::Combine(WorkDir, TEXT("rec_footer.pak"));
			PF.CopyFile(*RecFooter, *V2Pak);
			Check(CorruptTail(RecFooter, 256), TEXT("recovery: footer corrupted"));
			{
				const FPakRecovery::FOutcome O = FPakRecovery::ScanAndRepair(RecFooter);
				Check(O.Result == FPakRecovery::EResult::Truncated && O.TruncatedToBytes == V1Size,
					TEXT("recovery: torn footer falls back to the previous generation"));
				Check(FPatcherHash::FileSHA1Hex(RecFooter) == V1Sha,
					TEXT("recovery: previous generation is byte-exact"));
			}

			// 5d. A footer-SHAPED byte string inside an entry payload must not
			// be mistaken for a footer: magic + version + plausible geometry
			// all present, only the index SHA1 gives it away.
			const FString RecFake = FPaths::Combine(WorkDir, TEXT("rec_fake.pak"));
			PF.CopyFile(*RecFake, *V1Pak);
			{
				TArray64<uint8> FakeFooter;
				auto Le32 = [&FakeFooter](uint32 V) { FakeFooter.Append((const uint8*)&V, 4); };
				auto Le64 = [&FakeFooter](uint64 V) { FakeFooter.Append((const uint8*)&V, 8); };
				FakeFooter.AddZeroed(16);  // EncryptionKeyGuid
				FakeFooter.Add(0);         // bEncryptedIndex
				Le32(PakFileMagic);        // magic
				Le32(12);                  // version
				Le64(0);                   // IndexOffset (claims the file start)
				Le64(1024);                // IndexSize
				FakeFooter.AddZeroed(20);  // IndexHash (will NOT match)
				FakeFooter.AddZeroed(160); // CompressionMethods

				FPakWriter W;
				Check(W.Open(RecFake), TEXT("recovery: fake-footer pak opens"));
				FPakWriter::FEntryToWrite FE;
				FE.Path = TEXT("fakefooter.bin");
				FPakWriter::FStoredBlock FB;
				FB.Bytes = FakeFooter;
				FB.Bytes.Append(FakeFooter); // payload carries 3 copies of the fake
				FB.Bytes.Append(FakeFooter);
				FE.UncompressedSize = FB.Bytes.Num();
				FE.Blocks.Add(MoveTemp(FB));
				FPakEntry Out;
				Check(W.AppendEntry(FE, Out) && W.Commit(), TEXT("recovery: fake-footer entry committed"));
			}
			Check(CorruptTail(RecFake, 256), TEXT("recovery: real footer corrupted"));
			{
				const FPakRecovery::FOutcome O = FPakRecovery::ScanAndRepair(RecFake);
				Check(O.Result == FPakRecovery::EResult::Truncated && O.TruncatedToBytes == V1Size,
					TEXT("recovery: footer-shaped payload bytes are rejected (index SHA1 is the filter)"));
				Check(FPatcherHash::FileSHA1Hex(RecFake) == V1Sha,
					TEXT("recovery: the real previous generation survives"));
			}

			// 5e. No valid footer anywhere: Unrecoverable (the orchestrator
			// deletes such files so the diff re-fetches them whole).
			const FString RecDead = FPaths::Combine(WorkDir, TEXT("rec_dead.pak"));
			Check(AppendRaw(RecDead, 8192, 0x00), TEXT("recovery: garbage file written"));
			{
				const FPakRecovery::FOutcome O = FPakRecovery::ScanAndRepair(RecDead);
				Check(O.Result == FPakRecovery::EResult::Unrecoverable, TEXT("recovery: garbage file is Unrecoverable"));
			}

			// 5f. End to end: a torn tail on the INSTALLED pak is repaired by
			// the update's startup scan, and the run converges with zero
			// writes to the identical committed bytes.
			const FString InstalledV3Sha = FPatcherHash::FileSHA1Hex(InstalledPak);
			Check(AppendRaw(InstalledPak, 3000, 0xC3), TEXT("recovery: torn tail appended to the installed pak"));
			FUpdateStats SRec;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.2"), Install, true, SRec),
				TEXT("recovery: update runs after the torn tail"));
			Check(SRec.NumFilesChanged == 0 && SRec.BytesWritten == 0,
				TEXT("recovery: replay converges with zero writes"));
			Check(FPatcherHash::FileSHA1Hex(InstalledPak) == InstalledV3Sha,
				TEXT("recovery: the installed pak is byte-identical to the committed v3"));
		}

		// --- Compressed multi-block entry: write / read / hash / ingest / update
		// (the real-engine-format blind spot that let the fpak block-alignment
		// bug ship: every other fixture pak is uncompressed single-block).
		{
			const int64 BlockSize = 64ll << 10; // 64 KiB (engine default quantum)
			const int32 SliceCount = 3;
			const int64 SliceBytes = BlockSize;
			const int64 TotalBytes = SliceBytes * SliceCount;

			// Deterministic pseudo-random content (3 independent 64KiB slices).
			TArray64<uint8> ContentA, ContentB;
			ContentA.SetNumUninitialized(TotalBytes);
			ContentB.SetNumUninitialized(TotalBytes);
			for (int32 S = 0; S < SliceCount; ++S)
			{
				for (int64 I = 0; I < SliceBytes; ++I)
				{
					const int64 G = S * SliceBytes + I;
					ContentA[G] = (uint8)((I * 31 + S * 17 + (I >> 9)) & 0xFF);
					ContentB[G] = (uint8)((I * 13 + S * 29 + (I >> 8) + 0x5A) & 0xFF);
				}
			}

			auto CompressSlices = [](const TArray64<uint8>& Content, int64 SliceBytes, TArray<TArray64<uint8>>& OutBlocks) -> bool
			{
				const int32 Count = (int32)(Content.Num() / SliceBytes);
				for (int32 S = 0; S < Count; ++S)
				{
					const uint8* Src = Content.GetData() + (int64)S * SliceBytes;
					TArray64<uint8> Dst;
					Dst.SetNumUninitialized(SliceBytes + 4096); // compressed can exceed raw
					int32 CompressedSize = Dst.Num();
					if (!FCompression::CompressMemory((FName)TEXT("Oodle"), Dst.GetData(), CompressedSize, Src, (int32)SliceBytes))
					{
						return false;
					}
					Dst.SetNum(CompressedSize);
					OutBlocks.Add(MoveTemp(Dst));
				}
				return true;
			};

			TArray<TArray64<uint8>> BlocksA, BlocksB;
			Check(CompressSlices(ContentA, SliceBytes, BlocksA) && BlocksA.Num() == SliceCount, TEXT("zcomp: v1 content compressed into 3 Oodle blocks"));
			Check(CompressSlices(ContentB, SliceBytes, BlocksB) && BlocksB.Num() == SliceCount, TEXT("zcomp: v2 content compressed into 3 Oodle blocks"));

			// Write v1: based on the installed pak (inherits the engine Oodle
			// method table in its footer), one compressed multi-block entry.
			const FString ZPakV1 = FPaths::Combine(WorkDir, TEXT("zcomp_v1.pak"));
			const FString ZPakV2 = FPaths::Combine(WorkDir, TEXT("zcomp_v2.pak"));
			PF.CopyFile(*ZPakV1, *InstalledPak);
			int32 OodleIndex = INDEX_NONE;
			{
				FPakWriter W;
				Check(W.Open(ZPakV1), TEXT("zcomp: writer opens"));
				FPakWriter::FEntryToWrite ZEntry;
				ZEntry.Path = TEXT("Content/ZCompressed.bin");
				// Register Oodle into the footer method table (a pak that never
				// compressed anything has an empty table - a real gap this test
				// exposed; RegisterCompressionMethod fills it on first use).
				OodleIndex = W.RegisterCompressionMethod(TEXT("Oodle"));
				Check(OodleIndex > 0, TEXT("zcomp: Oodle registered in the method table"));
				ZEntry.CompressionMethodIndex = (uint32)OodleIndex;
				ZEntry.UncompressedSize = TotalBytes;
				ZEntry.CompressionBlockSize = (uint32)SliceBytes;
				for (int32 S = 0; S < SliceCount; ++S)
				{
					FPakWriter::FStoredBlock B;
					B.Bytes = BlocksA[S];
					ZEntry.Blocks.Add(MoveTemp(B));
				}
				FPakEntry Out;
				Check(W.AppendEntry(ZEntry, Out) && W.Commit(), TEXT("zcomp: compressed multi-block entry appended"));
				Check(Out.CompressionBlocks.Num() == SliceCount, TEXT("zcomp: entry stored as 3 blocks"));
			}
			// v2: same base, but the entry carries the B content blocks (the
			// stored size differs between versions - the real update case).
			PF.CopyFile(*ZPakV2, *InstalledPak);
			{
				FPakWriter W;
				Check(W.Open(ZPakV2), TEXT("zcomp: v2 writer opens"));
				FPakWriter::FEntryToWrite ZEntry;
				ZEntry.Path = TEXT("Content/ZCompressed.bin");
				const int32 OodleIndexV2 = W.RegisterCompressionMethod(TEXT("Oodle"));
				Check(OodleIndexV2 > 0, TEXT("zcomp: v2 Oodle registered"));
				ZEntry.CompressionMethodIndex = (uint32)OodleIndexV2;
				ZEntry.UncompressedSize = TotalBytes;
				ZEntry.CompressionBlockSize = (uint32)SliceBytes;
				for (int32 S = 0; S < SliceCount; ++S)
				{
					FPakWriter::FStoredBlock B;
					B.Bytes = BlocksB[S];
					ZEntry.Blocks.Add(MoveTemp(B));
				}
				FPakEntry Out;
				Check(W.AppendEntry(ZEntry, Out) && W.Commit(), TEXT("zcomp: v2 compressed entry appended"));
			}

			// Read back: decompressed bytes identical, payload hash consistent.
			{
				FPakReader R;
				Check(R.Open(ZPakV1) && R.ReadIndex(), TEXT("zcomp: v1 re-opens"));
				const FPakReader::FPakEntryLocation* Loc = nullptr;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path.EndsWith(TEXT("ZCompressed.bin"))) { Loc = &L; }
				}
				Check(Loc != nullptr, TEXT("zcomp: compressed entry found"));
				if (Loc)
				{
					Check(Loc->Entry.CompressionMethodIndex > 0 && Loc->Entry.CompressionBlocks.Num() == SliceCount,
						TEXT("zcomp: entry is Oodle multi-block on disk"));
					TArray64<uint8> Data;
					Check(R.ReadEntryData(Loc->Entry, Data) && Data.Num() == TotalBytes &&
						FMemory::Memcmp(Data.GetData(), ContentA.GetData(), TotalBytes) == 0,
						TEXT("zcomp: decompressed bytes identical to the source"));
					uint8 Computed[20] = { 0 };
					Check(R.ComputePayloadHash(Loc->Entry, Computed), TEXT("zcomp: payload hash computes"));
					uint8 Stored[20] = { 0 };
					Check(R.ReadPayloadHash(Loc->Entry, Stored) && FMemory::Memcmp(Stored, Computed, 20) == 0,
						TEXT("zcomp: built-in hash == computed payload hash"));
				}
			}

			// Ingest both versions and run the UPDATE flow: the compressed
			// entry's stored size changes between versions, so the update
			// appends a raw re-store (the production main path) and the final
			// container decodes to the v2 content.
			FIngestResult ZIngestResult;
			auto IngestZ = [&](const TCHAR* Ver, const FString& PakFile) -> bool
			{
				const FString StagedDir = FPaths::Combine(WorkDir, FString::Printf(TEXT("zstaged_%s"), Ver));
				if (!PF.DirectoryExists(*StagedDir) && !PF.CreateDirectoryTree(*StagedDir))
				{
					return false;
				}
				const FString Dst = FPaths::Combine(StagedDir, TEXT("game.pak"));
				PF.DeleteFile(*Dst);
				return PF.CopyFile(*Dst, *PakFile) &&
					FBuildIngestor::Ingest(StagedDir, TEXT("InplaceTest"), Ver, FPaths::Combine(WorkDir, TEXT("cdn")), ZIngestResult);
			};
			Check(IngestZ(TEXT("1.0"), ZPakV1), TEXT("zcomp: v1 ingested"));
			Check(IngestZ(TEXT("1.1"), ZPakV2), TEXT("zcomp: v2 ingested"));

			const FString ZInstall = FPaths::Combine(WorkDir, TEXT("zinstall"));
			PF.CreateDirectoryTree(*ZInstall);
			FUpdateStats ZS1;
			Check(FUpdateOrchestrator::Update(FPaths::Combine(WorkDir, TEXT("cdn")), TEXT("1.0"), ZInstall, true, ZS1),
						TEXT("zcomp: fresh install of the compressed container succeeds"));
			{
				TArray64<uint8> Data;
				FPakReader R;
				const FString ZPak = FPaths::Combine(ZInstall, TEXT("game.pak"));
				Check(R.Open(ZPak) && R.ReadIndex(), TEXT("zcomp: installed compressed container opens"));
				const FPakReader::FPakEntryLocation* Loc = nullptr;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path.EndsWith(TEXT("ZCompressed.bin"))) { Loc = &L; }
				}
				Check(Loc != nullptr && R.ReadEntryData(Loc->Entry, Data) && Data.Num() == TotalBytes &&
					FMemory::Memcmp(Data.GetData(), ContentA.GetData(), TotalBytes) == 0,
					TEXT("zcomp: installed content decodes to v1 bytes"));
			}

			FUpdateStats ZS2;
			Check(FUpdateOrchestrator::Update(FPaths::Combine(WorkDir, TEXT("cdn")), TEXT("1.1"), ZInstall, true, ZS2),
						TEXT("zcomp: update to v2 (stored-size change -> raw re-store) succeeds"));
			{
				TArray64<uint8> Data;
				FPakReader R;
				const FString ZPak = FPaths::Combine(ZInstall, TEXT("game.pak"));
				Check(R.Open(ZPak) && R.ReadIndex(), TEXT("zcomp: updated container opens"));
				const FPakReader::FPakEntryLocation* Loc = nullptr;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path.EndsWith(TEXT("ZCompressed.bin"))) { Loc = &L; }
				}
				Check(Loc != nullptr && R.ReadEntryData(Loc->Entry, Data) && Data.Num() == TotalBytes &&
					FMemory::Memcmp(Data.GetData(), ContentB.GetData(), TotalBytes) == 0,
					TEXT("zcomp: updated content decodes to v2 bytes"));
			}

			// --- NEW compressed entry (remote-only): it can be neither
			// appended raw (method-0 append would hand the engine compressed
			// bytes as content - the append path has no block table) nor
			// overwritten. The patcher must bail to the whole-file rebuild,
			// which reproduces the real compressed layout. Before this bail
			// existed, the new compressed entry was appended as method-0
			// garbage that passed every per-entry hash check.
			const FString ZPakV3 = FPaths::Combine(WorkDir, TEXT("zcomp_v3.pak"));
			PF.CopyFile(*ZPakV3, *ZPakV2);
			{
				FPakWriter W;
				Check(W.Open(ZPakV3), TEXT("zcomp: v3 writer opens"));
				FPakWriter::FEntryToWrite ZEntry;
				ZEntry.Path = TEXT("Content/ZAdded.bin");
				const int32 OodleIndexV3 = W.RegisterCompressionMethod(TEXT("Oodle"));
				Check(OodleIndexV3 > 0, TEXT("zcomp: v3 Oodle registered"));
				ZEntry.CompressionMethodIndex = (uint32)OodleIndexV3;
				ZEntry.UncompressedSize = TotalBytes;
				ZEntry.CompressionBlockSize = (uint32)SliceBytes;
				for (int32 S = 0; S < SliceCount; ++S)
				{
					FPakWriter::FStoredBlock B;
					B.Bytes = BlocksB[S];
					ZEntry.Blocks.Add(MoveTemp(B));
				}
				FPakEntry Out;
				Check(W.AppendEntry(ZEntry, Out) && W.Commit(), TEXT("zcomp: v3 adds a NEW compressed entry"));
			}
			Check(IngestZ(TEXT("1.2"), ZPakV3), TEXT("zcomp: v3 ingested"));
			{
				FUpdateStats ZS3;
				Check(FUpdateOrchestrator::Update(FPaths::Combine(WorkDir, TEXT("cdn")), TEXT("1.2"), ZInstall, true, ZS3),
					TEXT("zcomp: update with a NEW compressed entry succeeds (in-place bailed to rebuild)"));
				Check(ZS3.NumFilesChanged == 1, TEXT("zcomp: the pak went through the rebuild path"));
				TArray64<uint8> Data;
				FPakReader R;
				const FString ZPak = FPaths::Combine(ZInstall, TEXT("game.pak"));
				Check(R.Open(ZPak) && R.ReadIndex(), TEXT("zcomp: v3 container opens"));
				const FPakReader::FPakEntryLocation* Loc = nullptr;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path.EndsWith(TEXT("ZAdded.bin"))) { Loc = &L; }
				}
				Check(Loc != nullptr && Loc->Entry.CompressionMethodIndex > 0 &&
					R.ReadEntryData(Loc->Entry, Data) && Data.Num() == TotalBytes &&
					FMemory::Memcmp(Data.GetData(), ContentB.GetData(), TotalBytes) == 0,
					TEXT("zcomp: the added entry is COMPRESSED on disk and decodes correctly"));
			}
		}

		// --- 6. Fail-after-pak-patch (ROADMAP §5.3): the footer flip is the
		// commit point - once it lands, the container IS the new version even
		// if the update fails afterwards. The orchestrator must not back up or
		// roll back the .pak (the container is its own WAL): no .pak backup on
		// disk, the patched container survives the rollback intact, and the
		// rerun converges without re-writing container bytes.
		FString IndexV13;
		FIndexManifest Remote13;
		{
			auto ReadStatus = [&]() -> ETransactionStatus
			{
				FPatcherState S;
				if (!FPatcherStateFile::Load(Paths::GetStatePath(Install), S))
				{
					return ETransactionStatus::None;
				}
				return S.Transaction.Status;
			};

			// 1.3 = v2 + backfilled.txt (so the entry set is a superset of the
			// installed 1.2 - no deletions) + one more appended entry. A
			// version that REMOVES a pak entry takes the rebuild path instead
			// (section 8); here the in-place CoW path is the one under test.
			const FString V4Pak = FPaths::Combine(WorkDir, TEXT("v4.pak"));
			PF.CopyFile(*V4Pak, *V2Pak);
			{
				FPakWriter W;
				Check(W.Open(V4Pak), TEXT("failpak: v4 writer opens"));
				FPakWriter::FEntryToWrite Backfill;
				Backfill.Path = TEXT("backfilled.txt");
				Backfill.CompressionMethodIndex = 0;
				Backfill.UncompressedSize = 12;
				FPakWriter::FStoredBlock BackfillBlock;
				BackfillBlock.Bytes.SetNumUninitialized(12);
				FMemory::Memcpy(BackfillBlock.Bytes.GetData(), "backfilled!!", 12);
				Backfill.Blocks.Add(MoveTemp(BackfillBlock));
				FPakEntry BackfillOut;
				Check(W.AppendEntry(Backfill, BackfillOut), TEXT("failpak: backfill entry appended"));
				FPakWriter::FEntryToWrite Extra;
				Extra.Path = TEXT("failtest.txt");
				Extra.CompressionMethodIndex = 0;
				Extra.UncompressedSize = 8;
				FPakWriter::FStoredBlock Block;
				Block.Bytes.SetNumUninitialized(8);
				FMemory::Memcpy(Block.Bytes.GetData(), "failtest", 8);
				Extra.Blocks.Add(MoveTemp(Block));
				FPakEntry Out;
				Check(W.AppendEntry(Extra, Out) && W.Commit(), TEXT("failpak: v4 committed"));
			}
			Check(CraftAndIngest(TEXT("1.3"), V4Pak, TEXT("game.pak"), IndexV13), TEXT("failpak: ingest v4 as 1.3"));
			Check(FIndexManifestFile::Load(IndexV13, Remote13), TEXT("failpak: 1.3 index loads"));

			FUpdateOrchestrator::TestFailAfterWrites = 1;
			FUpdateStats SFail;
			const bool bFailed = !FUpdateOrchestrator::Update(Cdn, TEXT("1.3"), Install, true, SFail);
			FUpdateOrchestrator::TestFailAfterWrites = 0;
			Check(bFailed, TEXT("failpak: injected failure after the pak patch makes update fail"));
			Check(ReadStatus() == ETransactionStatus::RolledBack, TEXT("failpak: sidecar reports rolled-back"));
			{
				// No .pak ever lands in the backup dir: the container is its
				// own rollback mechanism (footer commit + recovery scanner).
				bool bPakBackup = false;
				const FString BackupDir = FPaths::Combine(Install, TEXT(".ugp"), TEXT("backup"));
				TArray<FString> BackupFiles;
				PF.FindFilesRecursively(BackupFiles, *BackupDir, TEXT("*.pak"));
				if (PF.DirectoryExists(*BackupDir))
				{
					bPakBackup = BackupFiles.Num() > 0;
				}
				Check(!bPakBackup, TEXT("failpak: the patched pak was never backed up"));
			}
			{
				// The CoW commit survived the rollback: the container on disk
				// already carries the 1.3 entry set and parses cleanly.
				FPakReader R;
				Check(R.Open(InstalledPak) && R.ReadIndex(), TEXT("failpak: patched container survived the rollback"));
				bool bHasNew = false;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path.Contains(TEXT("failtest.txt"))) { bHasNew = true; }
				}
				Check(bHasNew, TEXT("failpak: the committed entry is in the surviving container"));
			}

			// Rerun (as the next launch): the disk container already matches
			// the 1.3 metadata, so the rerun converges with zero writes and
			// the sidecar commits the new version.
			FUpdateStats SRetry;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.3"), Install, true, SRetry), TEXT("failpak: rerun after rollback succeeds"));
			Check(SRetry.BytesWritten == 0, TEXT("failpak: rerun re-writes nothing (container already committed)"));
			Check(CheckContainerLogicallyEqual(Remote13, Install, TEXT("game.pak")), TEXT("failpak: container logically equal to 1.3"));
			Check(ReadStatus() == ETransactionStatus::Committed, TEXT("failpak: sidecar committed after the rerun"));
		}

		// --- 7. Unrecoverable container: no valid footer anywhere, so the
		// scanner reports Unrecoverable, the orchestrator deletes the pak for
		// the whole-file rebuild, and the rebuilt file is byte-identical to
		// the CDN copy.
		{
			// A zero-length file is the degenerate Unrecoverable case (the
			// real-world "container zeroed by a failing disk / crashed writer").
			IFileHandle* Truncate = PF.OpenWrite(*InstalledPak); // CREATE_ALWAYS: 0 bytes
			Check(Truncate != nullptr, TEXT("zeropak: opened for truncation"));
			delete Truncate;
			Check(IFileManager::Get().FileSize(*InstalledPak) == 0, TEXT("zeropak: container is zero bytes"));

			FUpdateStats SZero;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.3"), Install, true, SZero), TEXT("zeropak: update over a zeroed pak succeeds"));
			Check(SZero.NumFilesAdded + SZero.NumFilesChanged == 1, TEXT("zeropak: exactly the pak rebuilt"));
			Check(SZero.BytesWritten == Remote13.Files[TEXT("game.pak")].Size, TEXT("zeropak: the whole container was re-written"));
			Check(FPatcherHash::FileSHA1Hex(InstalledPak) == Remote13.Files[TEXT("game.pak")].SHA1,
				TEXT("zeropak: rebuilt container is byte-identical to the CDN copy"));

			// Convergence: the rerun is a no-op.
			FUpdateStats SZero2;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.3"), Install, true, SZero2), TEXT("zeropak: rerun converges"));
			Check(SZero2.NumFilesChanged == 0 && SZero2.BytesWritten == 0, TEXT("zeropak: rerun is a no-op"));
		}

		// --- 8. Removed pak entry: the CoW append path cannot express a
		// deletion (the re-committed index would still carry the stale
		// entry), so the patcher must refuse and the orchestrator must take
		// the whole-file rebuild - otherwise the container never converges to
		// the target metadata while every update reports success.
		{
			// 1.4 = v2 + backfilled.txt only: vs the installed 1.3 (v2 +
			// backfilled.txt + failtest.txt) the failtest entry is GONE.
			const FString V5Pak = FPaths::Combine(WorkDir, TEXT("v5.pak"));
			PF.CopyFile(*V5Pak, *V2Pak);
			{
				FPakWriter W;
				Check(W.Open(V5Pak), TEXT("entryremove: v5 writer opens"));
				FPakWriter::FEntryToWrite Backfill;
				Backfill.Path = TEXT("backfilled.txt");
				Backfill.CompressionMethodIndex = 0;
				Backfill.UncompressedSize = 12;
				FPakWriter::FStoredBlock BackfillBlock;
				BackfillBlock.Bytes.SetNumUninitialized(12);
				FMemory::Memcpy(BackfillBlock.Bytes.GetData(), "backfilled!!", 12);
				Backfill.Blocks.Add(MoveTemp(BackfillBlock));
				FPakEntry Out;
				Check(W.AppendEntry(Backfill, Out) && W.Commit(), TEXT("entryremove: v5 committed"));
			}
			FString IndexV14;
			Check(CraftAndIngest(TEXT("1.4"), V5Pak, TEXT("game.pak"), IndexV14), TEXT("entryremove: ingest v5 as 1.4"));
			FIndexManifest Remote14;
			Check(FIndexManifestFile::Load(IndexV14, Remote14), TEXT("entryremove: 1.4 index loads"));

			FUpdateStats SRemoveEntry;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.4"), Install, true, SRemoveEntry), TEXT("entryremove: removal update succeeds"));
			Check(SRemoveEntry.NumFilesChanged == 1, TEXT("entryremove: exactly the pak changed"));
			// The rebuild rewrites the whole container; the CoW path would
			// have written only the changed bytes (here: none).
			Check(SRemoveEntry.BytesWritten == Remote14.Files[TEXT("game.pak")].Size,
				TEXT("entryremove: the whole container was rebuilt (CoW cannot remove entries)"));
			Check(CheckContainerLogicallyEqual(Remote14, Install, TEXT("game.pak")),
				TEXT("entryremove: container logically equal to 1.4"));
			{
				FPakReader R;
				Check(R.Open(InstalledPak) && R.ReadIndex(), TEXT("entryremove: container opens"));
				bool bHasRemoved = false;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path.Contains(TEXT("failtest.txt"))) { bHasRemoved = true; }
				}
				Check(!bHasRemoved, TEXT("entryremove: the removed entry is gone from the live index"));
			}

			// Convergence: the rerun is a no-op.
			FUpdateStats SRemoveEntry2;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.4"), Install, true, SRemoveEntry2), TEXT("entryremove: rerun converges"));
			Check(SRemoveEntry2.NumFilesChanged == 0 && SRemoveEntry2.BytesWritten == 0, TEXT("entryremove: rerun is a no-op"));
		}

				if (bAllPassed)
		{
			PF.DeleteDirectoryRecursively(*WorkDir);
		}
		return FinishTest(bAllPassed, TEXT("pakinplace"));
	
	}


	static int32 RunIoStoreInPlace(const TCHAR* /*CmdLine*/)
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
		const FString ProjectRoot = FPaths::GetPath(FPaths::GetPath(FPaths::GetPath(FPlatformProcess::BaseDir())));
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
				Check(RCur.GetChunkCount() == 136, TEXT("chunkremoval: current container has 136 chunks"));
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
				Check(R5.GetChunkCount() == 135, TEXT("chunkremoval: 135 chunks remain"));
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

int32 RunUnrealGamePatcherCLI(const TCHAR* CommandLine)
{
	// Engine FParse semantics throughout: the command is the first positional
	// token (switches start with '-'); handlers read their -key=value
	// arguments straight off the raw line (dash-prefixed keys keep the match
	// anchored to the token head - "-cdn=" can never hit "-targetcdn=").
	const FString Command = GetCommandToken(CommandLine);

	if (Command.IsEmpty())
	{
		// "-version" / "--version" as the only argument (Param cannot match
		// the double-dash spelling - it compares whole tokens after one dash).
		bool bVersion = false;
		{
			FString Token;
			const TCHAR* Rest = CommandLine;
			while (FParse::Token(Rest, Token, false))
			{
				if (Token == TEXT("-version") || Token == TEXT("--version"))
				{
					bVersion = true;
					break;
				}
			}
		}
		if (bVersion)
		{
			UE_LOG(LogUnrealGamePatcher, Display, TEXT("UnrealGamePatcher %s"), UnrealGamePatcher::ProgramVersion);
			return ExitSuccess;
		}
		PrintUsage();
		return ExitUsage;
	}

	// Dispatch to the per-command handlers below.
	struct FCommandEntry
	{
		const TCHAR* Name;
		int32 (*Handler)(const TCHAR* CmdLine);
	};
	static const FCommandEntry Commands[] =
	{
		{ TEXT("selftest"), RunSelftest },
		{ TEXT("statetest"), RunStateTest },
		{ TEXT("indextest"), RunIndexTest },
		{ TEXT("difftest"), RunDiffTest },
		{ TEXT("build"), RunBuild },
		{ TEXT("buildtest"), RunBuildTest },
		{ TEXT("diff"), RunDiff },
		{ TEXT("diffbuild"), RunDiffBuild },
		{ TEXT("pakdump"), RunPakDump },
		{ TEXT("pakpatch"), RunPakPatch },
		{ TEXT("update"), RunUpdate },
		{ TEXT("check"), RunCheckCompactRepair },
		{ TEXT("compact"), RunCheckCompactRepair },
		{ TEXT("repair"), RunCheckCompactRepair },
		{ TEXT("updatetest"), RunUpdateTest },
		{ TEXT("maintest"), RunMainTest },
		{ TEXT("downloadtest"), RunDownloadTest },
		{ TEXT("benchtest"), RunBenchTest },
		{ TEXT("iostoretest"), RunIoStoreTest },
		{ TEXT("iostoredump"), RunIoStoreDump },
		{ TEXT("e2etest"), RunE2ePerfTest },
		{ TEXT("perftest"), RunE2ePerfTest },
		{ TEXT("pakinplace"), RunPakInPlace },
		{ TEXT("iostoreinplace"), RunIoStoreInPlace },
	};

	for (const FCommandEntry& C : Commands)
	{
		if (Command == C.Name)
		{
			return C.Handler(CommandLine);
		}
	}

	UE_LOG(LogUnrealGamePatcher, Display, TEXT("Command '%s' is not implemented yet."), *Command);
	PrintUsage();
	return ExitUsage;
}

