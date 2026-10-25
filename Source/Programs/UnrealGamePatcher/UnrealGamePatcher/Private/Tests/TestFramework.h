// Copyright Epic Games, Inc. All Rights Reserved.
// Shared scaffolding for the -cli test suites (Docs/TESTCASES.md).
// Moved VERBATIM from UnrealGamePatcherCLI.cpp in the 2026-10-24 split;
// only 'inline' keywords and this namespace were added.

#pragma once

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

namespace UnrealGamePatcher::Tests
{
	constexpr int32 ExitSuccess = 0;
	constexpr int32 ExitFailure = 1;

	// The positional command token: the first token that does not start
	// with '-' (switches are read by the handlers via FParse directly).
	inline FString GetCommandToken(const TCHAR* CommandLine)
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

	/** Returns a Check(b, what) lambda bound to bAllPassed. */
	inline auto MakeCheck(bool& bAllPassed)
	{
		return [&bAllPassed](bool b, const TCHAR* What)
		{
			if (b) { UE_LOG(LogUnrealGamePatcher, Display, TEXT("[PASS] %s"), What); }
			else { UE_LOG(LogUnrealGamePatcher, Error, TEXT("[FAIL] %s"), What); }
			bAllPassed &= b;
		};
	}

	/** Absolute path of the per-test work dir under Saved/. */
	inline FString GetTestWorkDir(const TCHAR* Name)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformMisc::ProjectDir(), TEXT("Saved"), Name));
	}

	/** Delete (if any) and recreate WorkDir. */
	inline void ResetWorkDir(const FString& WorkDir)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (PF.DirectoryExists(*WorkDir))
		{
			PF.DeleteDirectoryRecursively(*WorkDir);
		}
		PF.CreateDirectoryTree(*WorkDir);
	}

	/** Log the per-test summary and return the process exit code. */
	inline int32 FinishTest(bool bAllPassed, const TCHAR* Name)
	{
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("%s: %s"), Name, bAllPassed ? TEXT("all checks passed") : TEXT("FAILURES DETECTED"));
		return bAllPassed ? ExitSuccess : ExitFailure;
	}

	/** SHA1 (lowercase hex) of an in-memory byte buffer. FSHA1::Update takes
	 *  an int32 count, so buffers > 2 GiB are fed in chunks (same guard as the
	 *  orchestrator's Sha1HexOf). */
	inline FString Sha1OfBytes(const TArray64<uint8>& Data)
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

	bool BuildFixtureCdn(const FString& WorkDir, const FString& Cdn, UnrealGamePatcher::FIndexManifest& OutIndex);
	bool CheckContainerLogicallyEqual(const UnrealGamePatcher::FIndexManifest& Remote, const FString& InstallDir, const FString& FileId);

	// Suite entry points (one per -cli test command).
	int32 RunSelftest(const TCHAR* CmdLine);
	int32 RunStateTest(const TCHAR* CmdLine);
	int32 RunIndexTest(const TCHAR* CmdLine);
	int32 RunDiffTest(const TCHAR* CmdLine);
	int32 RunBuildTest(const TCHAR* CmdLine);
	int32 RunDiffBuild(const TCHAR* CmdLine);
	int32 RunUpdateTest(const TCHAR* CmdLine);
	int32 RunMainTest(const TCHAR* CmdLine);
	int32 RunDownloadTest(const TCHAR* CmdLine);
	int32 RunBenchTest(const TCHAR* CmdLine);
	int32 RunIoStoreTest(const TCHAR* CmdLine);
	int32 RunE2ePerfTest(const TCHAR* CmdLine);
	int32 RunPakInPlace(const TCHAR* CmdLine);
	int32 RunIoStoreInPlace(const TCHAR* CmdLine);
}
