// Copyright Epic Games, Inc. All Rights Reserved.

#include "Download/FDownloadManager.h"
#include "Download/FDownloadExecutor.h"
#include "Download/FHttpRangeRequest.h"
#include "UnrealGamePatcherLog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "SecureHash.h"

namespace UnrealGamePatcher
{
	namespace
	{
		/** Streams the whole file through FSHA1 (int32-bounded Update calls). */
		bool HashFileMatches(const FString& Path, const FString& ExpectedSha1)
		{
			IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
			IFileHandle* Handle = PF.OpenRead(*Path);
			if (!Handle)
			{
				return false;
			}
			FSHA1 Hasher;
			TArray<uint8> Buffer;
			Buffer.SetNumUninitialized(1 << 20);
			int64 Remaining = Handle->Size();
			while (Remaining > 0)
			{
				const int32 ToRead = (int32)FMath::Min<int64>(Remaining, Buffer.Num());
				if (!Handle->Read(Buffer.GetData(), ToRead))
				{
					delete Handle;
					return false;
				}
				Hasher.Update(Buffer.GetData(), ToRead);
				Remaining -= ToRead;
			}
			delete Handle;
			Hasher.Final();
			uint8 Hash[20] = { 0 };
			Hasher.GetHash(Hash);
			FString Hex;
			Hex.Reserve(40);
			for (int32 I = 0; I < 20; ++I)
			{
				Hex += FString::Printf(TEXT("%02x"), Hash[I]);
			}
			return Hex.Equals(ExpectedSha1, ESearchCase::IgnoreCase);
		}
	}

	FDownloadManager::FDownloadManager(IChunkDownloader& InDownloader, int32 InMaxConcurrent, int32 InMaxRetries)
		: Downloader(InDownloader)
		, MaxConcurrent(FMath::Max(1, InMaxConcurrent))
		, MaxRetries(FMath::Max(0, InMaxRetries))
	{
	}

	bool FDownloadManager::DownloadAll(
		const TArray<FDownloadRequest>& Requests,
		FDownloadStats& OutStats,
		TArray<FString>& OutFailedIds)
	{
		OutStats = FDownloadStats();
		OutFailedIds.Reset();
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

		// ---- Thin business shell (Docs/DOWNLOAD.md §7) --------------------------
		// Cache check / final verification / placement stay HERE (business
		// responsibilities); transport, threading, segmentation, retries and
		// resume live inside the executor - invisible to this layer.

		// Tamper/insanity pre-check: a manifest-declared size over the
		// transport cap (or negative) is refused before any transfer.
		TArray<FDownloadRequest> ToFetch;
		for (const FDownloadRequest& Request : Requests)
		{
			if (Request.Size < 0 || Request.Size > FHttpRangeRequest::MaxResponseBytes)
			{
				OutFailedIds.AddUnique(Request.Id);
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDownloadManager: '%s' declares %lld bytes (cap %lld) - refusing"),
					*Request.Id, Request.Size, FHttpRangeRequest::MaxResponseBytes);
				continue;
			}

			bool bCacheOk = false;
			if (PF.FileExists(*Request.DestFile))
			{
				const int64 OnDisk = IFileManager::Get().FileSize(*Request.DestFile);
				bCacheOk = OnDisk == Request.Size &&
					(Request.ExpectedSha1.IsEmpty() || HashFileMatches(Request.DestFile, Request.ExpectedSha1));
			}
			if (bCacheOk)
			{
				OutStats.SkippedExisting += 1;
			}
			else
			{
				ToFetch.Add(Request);
			}
		}

		// Submit jobs to the executor; OnDone fires exactly once per job.
		FDownloadExecutorConfig Cfg;
		Cfg.Window = MaxConcurrent;   // the manager's concurrency knob maps 1:1
		Cfg.MaxRetries = MaxRetries;
		FDownloadExecutor Executor(Downloader, Cfg);

		std::atomic<int32> Remaining{ ToFetch.Num() };
		std::atomic<int32> Retries{ 0 };
		std::atomic<int64> BytesDownloaded{ 0 };
		FCriticalSection FailLock;
		FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool(true);

		for (const FDownloadRequest& Request : ToFetch)
		{
			FDownloadJob Job;
			Job.Id = Request.Id;
			Job.Url = Request.Url;
			Job.DestPath = Request.DestFile;
			Job.ExpectedSize = Request.Size;
			Job.ExpectedSha1 = Request.ExpectedSha1;

			Executor.Submit(MoveTemp(Job),
				[&Remaining, &Retries, &BytesDownloaded, &FailLock, &OutFailedIds,
					DoneEvent, Id = Request.Id](const FDownloadResult& Result)
			{
				Retries.fetch_add(Result.Retries, std::memory_order_release);
				BytesDownloaded.fetch_add(Result.BytesDownloaded, std::memory_order_release);
				if (!Result.bSuccess)
				{
					FScopeLock FailScope(&FailLock);
					OutFailedIds.AddUnique(Result.Id);
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDownloadManager: '%s' failed: %s"), *Result.Id, *Result.Error);
				}
				if (Remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
				{
					DoneEvent->Trigger();
				}
			});
		}

		if (ToFetch.Num() > 0)
		{
			DoneEvent->Wait();
		}
		FPlatformProcess::ReturnSynchEventToPool(DoneEvent);

		OutStats.Completed = OutStats.SkippedExisting + (int32)ToFetch.Num() - OutFailedIds.Num();
		OutStats.Retries = Retries.load(std::memory_order_acquire);
		OutStats.BytesDownloaded = BytesDownloaded.load(std::memory_order_acquire);
		OutStats.Failed = OutFailedIds.Num();

		return OutFailedIds.Num() == 0;
	}
}
