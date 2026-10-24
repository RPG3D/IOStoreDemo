// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Download/IChunkDownloader.h"

namespace UnrealGamePatcher
{
	/** One file the download manager fetches and caches (design D9 / tasks
	 *  6.1-6.3). The manager writes verified bytes to DestFile and treats an
	 *  existing, verified DestFile as already-downloaded (resume: only
	 *  incomplete files are re-fetched). */
	struct FDownloadRequest
	{
		FString Id;           // logical id for logs/errors (e.g. the chunk guid)
		FString Url;          // absolute resource URL
		int64 Size = 0;       // expected total byte count of the resource
		FString ExpectedSha1; // optional (40 hex): verified when set
		FString DestFile;     // where the verified bytes are written
	};

	/** Aggregated result of a download batch. */
	struct FDownloadStats
	{
		int32 Completed = 0;        // requests finished successfully (incl. already-cached)
		int32 SkippedExisting = 0;  // requests satisfied by a verified cache file
		int32 Failed = 0;           // requests that exhausted all retries
		int32 Retries = 0;          // transport retries issued
		int64 BytesDownloaded = 0;  // bytes fetched over the wire (not cache hits)
	};

	/**
	 * Coordinates downloads over an IChunkDownloader (tasks 6.1-6.3/6.5):
	 * a worker pool (default 8 connections) pulls requests, retries transport
	 * failures up to a cap, resumes interrupted files from their partial
	 * downloads (byte-range continuation), verifies each result, and only
	 * then moves it into place - a corrupt or failed download never lands as
	 * a verified file. If any request ultimately fails, the whole batch fails
	 * and the caller is told which ids failed.
	 */
	struct FDownloadManager
	{
		/** InDownloader must outlive the manager. MaxConcurrent bounds the
		 *  in-flight window; MaxRetries bounds transport retries per request;
		 *  StallTimeoutSec is the batch watchdog: no segment-level progress
		 *  for this long fails the whole batch (guards against a transport
		 *  that loses its settle callback - that would otherwise hang the
		 *  waiting thread forever). */
		UNREALGAMEPATCHERCORE_API FDownloadManager(IChunkDownloader& InDownloader, int32 MaxConcurrent = 8, int32 MaxRetries = 3, double InStallTimeoutSec = 300.0);

		/** Downloads every request. Returns false when at least one request
		 *  failed permanently (its id is reported through OutFailedIds; no
		 *  corrupt bytes are on disk - only fully verified files exist). */
		UNREALGAMEPATCHERCORE_API bool DownloadAll(
			const TArray<FDownloadRequest>& Requests,
			FDownloadStats& OutStats,
			TArray<FString>& OutFailedIds);

	private:
		IChunkDownloader& Downloader;
		int32 MaxConcurrent;
		int32 MaxRetries;
		double StallTimeoutSec;
	};
}
