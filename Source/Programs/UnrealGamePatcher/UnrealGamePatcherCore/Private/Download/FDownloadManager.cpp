// Copyright Epic Games, Inc. All Rights Reserved.

#include "Download/FDownloadManager.h"
#include "Download/FDownloadExecutor.h"
#include "Download/FHttpRangeRequest.h"
#include "Stats/FIOStats.h"
#include "UnrealGamePatcherLog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Event.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"

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
			FIOStats::Get().AddDiskRead(Remaining);
			FIOStats::Get().AddHashed(Remaining);
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

	FDownloadManager::FDownloadManager(IChunkDownloader& InDownloader, int32 InMaxConcurrent, int32 InMaxRetries, double InStallTimeoutSec)
		: Downloader(InDownloader)
		, MaxConcurrent(FMath::Max(1, InMaxConcurrent))
		, MaxRetries(FMath::Max(0, InMaxRetries))
		, StallTimeoutSec(InStallTimeoutSec)
	{
	}

	bool FDownloadManager::DownloadAll(
		const TArray<FDownloadRequest>& Requests,
		FDownloadStats& OutStats,
		TArray<FString>& OutFailedIds,
		TFunction<void(int64, int64)> OnProgress)
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
		int64 TotalBytes = 0;
		int64 CachedBytes = 0;
		for (const FDownloadRequest& Request : Requests)
		{
			if (Request.Size < 0 || Request.Size > FHttpRangeRequest::MaxResponseBytes)
			{
				OutFailedIds.AddUnique(Request.Id);
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDownloadManager: '%s' declares %lld bytes (cap %lld) - refusing"),
					*Request.Id, Request.Size, FHttpRangeRequest::MaxResponseBytes);
				continue;
			}
			TotalBytes += Request.Size;

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
				CachedBytes += Request.Size;
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

		// Batch state lives on the heap and is captured by value: the watchdog
		// below may return while straggler callbacks are still pending (a
		// lost-callback transport), so no lambda may touch this stack frame -
		// including the failure list, which is owned here and moved into the
		// caller's array just before returning.
		struct FBatchContext
		{
			std::atomic<int32> Remaining{ 0 };
			std::atomic<int32> Retries{ 0 };
			std::atomic<int64> BytesDownloaded{ 0 };
			std::atomic<int64> CompletedBytes{ 0 };
			FCriticalSection FailLock;
			TArray<FString> Failed;
			TSet<FString> FinishedIds;
			FEventRef DoneEvent; // owned: outlives straggler Triggers
		};
		TSharedRef<FBatchContext> Ctx = MakeShared<FBatchContext>();
		Ctx->Remaining.store(ToFetch.Num(), std::memory_order_release);
		Ctx->CompletedBytes.store(CachedBytes, std::memory_order_release);
		int64 ReportedBytes = -1;
		auto ReportProgress = [&]()
		{
			const int64 Bytes = Ctx->CompletedBytes.load(std::memory_order_acquire);
			if (OnProgress && Bytes != ReportedBytes)
			{
				ReportedBytes = Bytes;
				OnProgress(Bytes, TotalBytes);
			}
		};
		ReportProgress();

		TArray<int64> Handles;
		Handles.Reserve(ToFetch.Num());
		for (const FDownloadRequest& Request : ToFetch)
		{
			FDownloadJob Job;
			Job.Id = Request.Id;
			Job.Url = Request.Url;
			Job.DestPath = Request.DestFile;
			Job.ExpectedSize = Request.Size;
			Job.ExpectedSha1 = Request.ExpectedSha1;

			Handles.Add(Executor.Submit(MoveTemp(Job),
				[Ctx, Id = Request.Id, Size = Request.Size](const FDownloadResult& Result)
			{
				Ctx->Retries.fetch_add(Result.Retries, std::memory_order_release);
				Ctx->BytesDownloaded.fetch_add(Result.BytesDownloaded, std::memory_order_release);
				if (Result.bSuccess)
				{
					Ctx->CompletedBytes.fetch_add(Size, std::memory_order_release);
				}
				{
					FScopeLock FailScope(&Ctx->FailLock);
					Ctx->FinishedIds.Add(Id);
					if (!Result.bSuccess)
					{
						Ctx->Failed.AddUnique(Result.Id);
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDownloadManager: '%s' failed: %s"), *Result.Id, *Result.Error);
					}
				}
				if (Ctx->Remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
				{
					Ctx->DoneEvent->Trigger();
				}
			}));
		}

		bool bStalled = false;
		if (ToFetch.Num() > 0)
		{
			// Watchdog: a transport that loses its settle callback would hang
			// this thread forever on the unbounded wait. Fail the batch when
			// the executor reports no segment-level progress for
			// StallTimeoutSec; straggler callbacks stay safe (heap context)
			// and still record through FailedIds.
			const double StallTimeout = FMath::Max(StallTimeoutSec, 1.0);
			double LastProgress = FPlatformTime::Seconds();
			int64 LastTick = Executor.GetProgressTick();
			while (Ctx->Remaining.load(std::memory_order_acquire) > 0)
			{
				const bool bDone = Ctx->DoneEvent->Wait(OnProgress ? 250 : 1000);
				ReportProgress();
				if (bDone)
				{
					break;
				}
				const int64 Tick = Executor.GetProgressTick();
				const double Now = FPlatformTime::Seconds();
				if (Tick != LastTick)
				{
					LastTick = Tick;
					LastProgress = Now;
				}
				else if (Now - LastProgress >= StallTimeout)
				{
					UE_LOG(LogUnrealGamePatcher, Error,
						TEXT("FDownloadManager: batch stalled for %.0fs with %d job(s) unfinished - failing the batch"),
						Now - LastProgress, Ctx->Remaining.load(std::memory_order_acquire));
					for (const int64 Handle : Handles)
					{
						Executor.Cancel(Handle);
					}
					bStalled = true;
					break;
				}
			}
			if (bStalled)
			{
				// Grace drain: canceled jobs on a healthy transport settle
				// within moments; a genuinely hung one never does - its late
				// callbacks only touch the heap context, so returning is safe.
				const double Deadline = FPlatformTime::Seconds() + 10.0;
				while (Ctx->Remaining.load(std::memory_order_acquire) > 0 &&
					FPlatformTime::Seconds() < Deadline)
				{
					Ctx->DoneEvent->Wait(250);
				}
				// Jobs that never reported count as failed (the late callbacks
				// would record them anyway, but the return value must already
				// reflect the failure).
				FScopeLock FailScope(&Ctx->FailLock);
				for (const FDownloadRequest& Request : ToFetch)
				{
					if (!Ctx->FinishedIds.Contains(Request.Id))
					{
						Ctx->Failed.AddUnique(Request.Id);
					}
				}
			}
		}
		ReportProgress();

		// Publish results into the caller's array. The pre-check above may
		// already have added ids here, so merge instead of replace. After
		// this, straggler callbacks keep writing into the (heap-owned)
		// context - safe to drop on the floor: the batch already returned.
		{
			FScopeLock FailScope(&Ctx->FailLock);
			for (const FString& F : Ctx->Failed)
			{
				OutFailedIds.AddUnique(F);
			}
		}

		OutStats.Completed = OutStats.SkippedExisting + (int32)ToFetch.Num() - OutFailedIds.Num();
		OutStats.Retries = Ctx->Retries.load(std::memory_order_acquire);
		OutStats.BytesDownloaded = Ctx->BytesDownloaded.load(std::memory_order_acquire);
		OutStats.Failed = OutFailedIds.Num();

		// Global I/O accounting: wire bytes that actually crossed the network,
		// plus the executor's staging-file writes (approximately the same
		// bytes - the retry re-transfers overwrite the same staging offsets,
		// and the whole-file verify reads are not counted here).
		FIOStats::Get().AddNetDownload(OutStats.BytesDownloaded);
		FIOStats::Get().AddDiskWrite(OutStats.BytesDownloaded);

		return OutFailedIds.Num() == 0;
	}
}
