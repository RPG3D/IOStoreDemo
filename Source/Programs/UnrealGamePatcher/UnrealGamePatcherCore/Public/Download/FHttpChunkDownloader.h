// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Download/IChunkDownloader.h"
#include "Download/FHttpRangeRequest.h"

namespace UnrealGamePatcher
{
	/** Legacy-name adapter kept for existing callers: the HTTP transport
	 *  primitive now lives in UnrealGamePatcherDownload as FHttpRangeRequest
	 *  (see Docs/DOWNLOAD.md). New code should use FHttpRangeRequest / the executor
	 *  directly - this alias only bridges the old type name to the migrated
	 *  implementation. */
	struct FHttpChunkDownloader : public IChunkDownloader
	{
		bool DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& OutData) override
		{
			int32 HttpStatus = 0;
			const bool bOk = Request.DownloadRange(Url, Offset, Size, OutData, ShouldCancel, &HttpStatus);
			if (IsHttpMissingResource(HttpStatus))
			{
				MissingResource->store(true);
			}
			return bOk;
		}

		FCancelHandle DownloadRangeAsync(const FString& Url, int64 Offset, int64 Size,
			TFunction<void(bool bOk, const TArray64<uint8>& Data, const FString& Error)>&& OnDone) override
		{
			// Native async: the engine HTTP thread drives the transfer; the
			// adapter folds semantic validation (status/length) into the
			// delivered bOk - the executor re-checks length against its own
			// expected size anyway.
			TSharedPtr<FHttpAsyncRequest> Handle;
			Request.RequestAsync(Url, Offset, Size, [Offset, Size, MissingResource = MissingResource, OnDone = MoveTemp(OnDone)](const FHttpAsyncRequest& R)
			{
				if (IsHttpMissingResource(R.Status))
				{
					MissingResource->store(true);
				}
				FString Error;
				bool bOk = R.bOk && !R.bTooLarge;
				if (bOk && !R.bStatusOk)
				{
					// A non-2xx response with a body (a CDN error page) must be
					// a FAILURE - the IChunkDownloader contract forbids handing
					// error-page bytes back as downloaded content. (The sync
					// path has always checked this; the async path did not.)
					bOk = false;
					Error = FString::Printf(TEXT("HTTP error %d"), R.Status);
				}
				if (bOk && Offset > 0 && R.Status != 206)
				{
					bOk = false;
					Error = FString::Printf(TEXT("answered %d for a range request (expected 206)"), R.Status);
				}
				if (bOk && Size >= 0 && R.Content.Num() != Size)
				{
					bOk = false;
					Error = FString::Printf(TEXT("returned %d bytes, expected %lld"), R.Content.Num(), Size);
				}
				if (bOk && R.Content.Num() == 0)
				{
					bOk = false;
					Error = TEXT("empty response");
				}
				OnDone(bOk, R.Content, Error);
			}, &Handle);
			return Handle;
		}

		void CancelDownload(const FCancelHandle& Handle) override
		{
			if (const TSharedPtr<FHttpAsyncRequest> Res = StaticCastSharedPtr<FHttpAsyncRequest>(Handle))
			{
				Res->Cancel();
			}
		}

		void ProbeSizeAsync(const FString& Url, TFunction<void(int64 TotalSize, bool bRangeOk)>&& OnDone) override
		{
			Request.ProbeSizeAsync(Url, MoveTemp(OnDone));
		}

		FHttpRangeRequest Request;
		TFunction<bool()> ShouldCancel;

		bool SawMissingResource() const { return MissingResource->load(); }

	private:
		// HTTP callbacks may settle after an interrupted update returns.
		TSharedRef<std::atomic<bool>> MissingResource = MakeShared<std::atomic<bool>>(false);
	};
}
