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
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
// Windows.h defines ReplaceFile as a macro (-> ReplaceFileW), which collides
// with the EApplyOp::ReplaceFile member used by the transaction tests.
#ifdef ReplaceFile
#undef ReplaceFile
#endif
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
#include "Tests/TestFramework.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealGamePatcherCLI, Log, All);

namespace
{
	constexpr int32 ExitSuccess = 0;
	constexpr int32 ExitFailure = 1;
	constexpr int32 ExitUsage = 2;

	// Moved to Tests/TestFramework.h; used by the CLI and the suites.
	using UnrealGamePatcher::Tests::GetCommandToken;


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


	// ---- Per-command handlers (split out of RunUnrealGamePatcherCLI) -------

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
		{ TEXT("selftest"), UnrealGamePatcher::Tests::RunSelftest },
		{ TEXT("statetest"), UnrealGamePatcher::Tests::RunStateTest },
		{ TEXT("indextest"), UnrealGamePatcher::Tests::RunIndexTest },
		{ TEXT("difftest"), UnrealGamePatcher::Tests::RunDiffTest },
		{ TEXT("build"), RunBuild },
		{ TEXT("buildtest"), UnrealGamePatcher::Tests::RunBuildTest },
		{ TEXT("diff"), RunDiff },
		{ TEXT("diffbuild"), UnrealGamePatcher::Tests::RunDiffBuild },
		{ TEXT("pakdump"), RunPakDump },
		{ TEXT("pakpatch"), RunPakPatch },
		{ TEXT("update"), RunUpdate },
		{ TEXT("check"), RunCheckCompactRepair },
		{ TEXT("compact"), RunCheckCompactRepair },
		{ TEXT("repair"), RunCheckCompactRepair },
		{ TEXT("updatetest"), UnrealGamePatcher::Tests::RunUpdateTest },
		{ TEXT("maintest"), UnrealGamePatcher::Tests::RunMainTest },
		{ TEXT("downloadtest"), UnrealGamePatcher::Tests::RunDownloadTest },
		{ TEXT("benchtest"), UnrealGamePatcher::Tests::RunBenchTest },
		{ TEXT("iostoretest"), UnrealGamePatcher::Tests::RunIoStoreTest },
		{ TEXT("iostoredump"), RunIoStoreDump },
		{ TEXT("e2etest"), UnrealGamePatcher::Tests::RunE2ePerfTest },
		{ TEXT("perftest"), UnrealGamePatcher::Tests::RunE2ePerfTest },
		{ TEXT("pakinplace"), UnrealGamePatcher::Tests::RunPakInPlace },
		{ TEXT("iostoreinplace"), UnrealGamePatcher::Tests::RunIoStoreInPlace },
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

