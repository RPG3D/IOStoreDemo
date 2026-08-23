// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UnrealGamePatcher
{
	/**
	 * Pluggable byte-range download backend (design D9 / task 6.5). The whole
	 * update pipeline (download scheduling, concurrency, resume, hash verify)
	 * depends only on this interface, never on a concrete transport - a custom
	 * CDN / P2P / encrypted backend plugs in via the same injection point
	 * (task 6.6).
	 *
	 * Implementations must be safe to call from multiple threads concurrently
	 * (the download pool drives one worker thread per in-flight request).
	 */
	struct IChunkDownloader
	{
		virtual ~IChunkDownloader() = default;

		/** Downloads bytes of Url. Size >= 0 is an EXACT range [Offset,
		 *  Offset+Size) - including Offset==0. Size<0 is open-ended [Offset,
		 *  end) for Offset>0, and a WHOLE-FILE GET for Offset==0. On success OutData
		 *  holds the requested bytes; returns false (with a logged reason) on
		 *  any transport failure - a partial or corrupt response must be
		 *  reported as failure, never handed back as success. */
		virtual bool DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& OutData) = 0;

		/** Opaque handle for cancelling an in-flight async request. */
		using FCancelHandle = TSharedPtr<void, ESPMode::ThreadSafe>;

		/** ASYNC form (the executor drives segments through this): issues the
		 *  request and invokes OnDone EXACTLY once - on the engine HTTP thread
		 *  for native-async backends, inline on the issuing thread for the
		 *  default synchronous fallback. Returns false only when the request
		 *  could not even be issued (OnDone has already fired with failure).
		 *  The returned handle is opaque; pass it to CancelDownload to abort. */
		virtual FCancelHandle DownloadRangeAsync(const FString& Url, int64 Offset, int64 Size,
			TFunction<void(bool bOk, const TArray64<uint8>& Data, const FString& Error)>&& OnDone)
		{
			// Default: synchronous fallback for backends without native async
			// (file-mapped test backends etc.). Custom transports override.
			TArray64<uint8> Data;
			const bool bOk = DownloadRange(Url, Offset, Size, Data);
			OnDone(bOk, Data, bOk ? FString() : TEXT("download failed"));
			return nullptr;
		}

		/** Best-effort abort of an in-flight async request (no-op for backends
		 *  that cannot cancel). The completion still fires exactly once,
		 *  reporting failure. */
		virtual void CancelDownload(const FCancelHandle& Handle)
		{
		}

		/** Size probe (the executor resolves unknown ExpectedSize through
		 *  this): issues `Range: bytes=0-0` natively (the engine backend
		 *  cancels at header time once the total is known - the body is never
		 *  read) and delivers the total size plus the Range-support verdict.
		 *  Default: synchronous whole-body fallback (total = body size;
		 *  bounded by the transport cap; Range reported unsupported). */
		virtual void ProbeSizeAsync(const FString& Url, TFunction<void(int64 TotalSize, bool bRangeOk)>&& OnDone)
		{
			TArray64<uint8> Data;
			const bool bOk = DownloadRange(Url, 0, -1, Data);
			OnDone(bOk ? Data.Num() : -1, false);
		}
	};
}
