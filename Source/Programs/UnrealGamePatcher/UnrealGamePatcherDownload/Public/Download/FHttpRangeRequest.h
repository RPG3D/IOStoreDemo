// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Interfaces/IHttpRequest.h"
#include <atomic>

namespace UnrealGamePatcher
{
	/**
	 * Transport primitive (synchronous single HTTP request): engine FHttpModule
	 * with a Range header for byte ranges. The HTTP manager runs its own I/O
	 * thread and delegates are dispatched on it (CompleteOnHttpThread), so this
	 * is safe to call concurrently from any thread. Semantics spec: see
	 * Docs/DOWNLOAD.md §3 and the download-executor spec.
	 */
	/** One in-flight (or settled) async request. The completion callback
	 *  (invoked exactly once, on the engine HTTP thread under
	 *  CompleteOnHttpThread) receives this struct; Cancel() aborts the
	 *  transfer (the completion still fires, reporting failure). */
	struct FHttpAsyncRequest
	{
		std::atomic<bool> bSettled{ false }; // completion delivered (once)
		bool bOk = false;
		bool bStatusOk = false;              // HTTP 200/206
		bool bTooLarge = false;              // cancelled by the transport cap
		int32 Status = 0;
		TArray64<uint8> Content;             // valid only on success
		FString Error;                       // failure reason (empty on success)
		/** STRONG ref: the engine HttpThread tracks in-flight requests by RAW
		 *  pointer ("requests remain valid until they exit the system") - the
		 *  caller's shared ref is the only owner. Dropping it (as the first
		 *  async draft did) destroys the request mid-flight and the completion
		 *  never fires. WeakRequest mirrors it for Cancel(). */
		TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> StrongRequest;
		TWeakPtr<IHttpRequest, ESPMode::ThreadSafe> WeakRequest; // for Cancel()
		TFunction<void()> Deliver;           // internal: exactly-once delivery

		/** Delivers the result exactly once (atomic guard). */
		UNREALGAMEPATCHERDOWNLOAD_API void DeliverOnce();

		/** Aborts the transfer; the completion still fires reporting failure. */
		void Cancel()
		{
			if (TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Req = WeakRequest.Pin())
			{
				Req->CancelRequest();
			}
		}
	};

	/** Async completion callback: fired exactly once per request, on the
	 *  engine HTTP thread - keep it cheap (light bookkeeping only). */
	using FOnRequestDone = TFunction<void(const FHttpAsyncRequest&)>;

	struct FHttpRangeRequest
	{
		/** Transport-side memory cap: any response larger than this is
		 *  cancelled at header time (Content-Length known before the body) or
		 *  by the progress watchdog - the client never buffers more than this
		 *  regardless of what the CDN serves. Public static so tests can lower
		 *  it without shipping oversized transfers. */
		static int64 MaxResponseBytes;

		/** Downloads bytes of Url via a GET. Size >= 0 is ALWAYS an exact
		 *  range [Offset, Offset+Size) - including Offset==0 (a multi-segment
		 *  download's first segment must not fetch the whole body).
		 *  Offset==0 && Size<0 is a WHOLE-FILE GET (no Range header);
		 *  Offset>0 && Size<0 is open-ended [Offset, end).
		 *  Fails (logged) on transport errors, non-2xx status, a response that
		 *  does not carry the requested bytes, or one exceeding
		 *  MaxResponseBytes. */
		UNREALGAMEPATCHERDOWNLOAD_API bool DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& OutData);

		/** Size probe: `Range: bytes=0-0` - delivers the total size (parsed
		 *  from Content-Range/Content-Length at HEADER time, then the request
		 *  is cancelled so the body is never read) and whether the server
		 *  honors Range at all. OnDone fires exactly once on the engine HTTP
		 *  thread; TotalSize < 0 means the probe failed. */
		UNREALGAMEPATCHERDOWNLOAD_API void ProbeSizeAsync(const FString& Url, TFunction<void(int64 TotalSize, bool bRangeOk)> OnDone);

		/** ASYNC form (the executor's entry point): issues the request and
		 *  returns immediately; OnDone fires exactly once on the engine HTTP
		 *  thread. Never blocks, never uses a pooled event. Returns false
		 *  only when the request could not even be created - OnDone has
		 *  already fired with the failure in that case. */
		UNREALGAMEPATCHERDOWNLOAD_API bool RequestAsync(const FString& Url, int64 Offset, int64 Size, FOnRequestDone OnDone);

		/** Serializes request creation: the curl backend runs its easy-handle
		 *  setup on the calling thread and races when several download-pool
		 *  threads create requests at once. Transfers themselves stay
		 *  concurrent (the HTTP event-loop thread drives all in-flight
		 *  handles), only the setup is serialized. */
		static FCriticalSection RequestCreateLock;
	};
}
