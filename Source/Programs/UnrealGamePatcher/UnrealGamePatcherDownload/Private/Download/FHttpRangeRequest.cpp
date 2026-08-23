// Copyright Epic Games, Inc. All Rights Reserved.

#include "Download/FHttpRangeRequest.h"
#include "UnrealGamePatcherDownload.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include <atomic>

namespace UnrealGamePatcher
{
	FCriticalSection FHttpRangeRequest::RequestCreateLock;

	int64 FHttpRangeRequest::MaxResponseBytes = 256ll << 20; // 256 MB

	namespace
	{
		/** Shared request assembly for both modes: URL/verb/Range construction,
		 *  transport-cap enforcement (header-time cancel + progress watchdog),
		 *  and the completion delivery. Returns false when the request could
		 *  not even be created (delivery has happened inline in that case). */
		bool IssueRequest(const FString& Url, int64 Offset, int64 Size,
			const TSharedRef<FHttpAsyncRequest>& Res, TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>& OutRequest)
		{
			// Degenerate empty range: a caller bug - fail fast before any IO.
			if (Size == 0)
			{
				Res->Error = TEXT("Size==0");
				Res->DeliverOnce();
				return false;
			}

			FScopeLock CreateLock(&FHttpRangeRequest::RequestCreateLock);
			OutRequest = FHttpModule::Get().CreateRequest();
			if (!OutRequest.IsValid())
			{
				Res->Error = TEXT("cannot create request");
				Res->DeliverOnce();
				return false;
			}
			Res->StrongRequest = OutRequest; // OWNERSHIP: HttpThread tracks raw pointers
			Res->WeakRequest = OutRequest;
			OutRequest->SetURL(Url);
			OutRequest->SetVerb(TEXT("GET"));
			// Range construction. Size >= 0 is ALWAYS an exact range
			// [Offset, Offset+Size) - including Offset == 0 (a multi-segment
			// download's first segment must not fetch the whole body). Only
			// Offset == 0 && Size < 0 is a whole-file GET without a Range
			// header (see Docs/DOWNLOAD.md §3).
			bool bRangeSent = false;
			if (Offset > 0 || (Offset == 0 && Size > 0))
			{
				if (Size < 0)
				{
					OutRequest->SetHeader(TEXT("Range"), FString::Printf(TEXT("bytes=%lld-"), Offset));
					bRangeSent = true;
				}
				else
				{
					OutRequest->SetHeader(TEXT("Range"), FString::Printf(TEXT("bytes=%lld-%lld"), Offset, Offset + Size - 1));
					bRangeSent = true;
				}
			}

			// Transport-cap enforcement: refuse oversized responses at HEADER
			// time (body never starts) with a progress watchdog for servers
			// that lie about Content-Length. The cancel leads to exactly one
			// completion delivery (via the atomic Done).
			const TWeakPtr<IHttpRequest, ESPMode::ThreadSafe> WeakRequest = OutRequest;
			OutRequest->OnHeaderReceived().BindLambda(
				[Res, WeakRequest, Url](FHttpRequestPtr, const FString& HeaderName, const FString& NewHeaderValue)
				{
					if (HeaderName.Equals(TEXT("Content-Length"), ESearchCase::IgnoreCase))
					{
						const int64 ContentLength = FCString::Atoi64(*NewHeaderValue);
						if (ContentLength > FHttpRangeRequest::MaxResponseBytes)
						{
							Res->bTooLarge = true;
							UE_LOG(LogUnrealGamePatcherDownload, Error, TEXT("FHttpRangeRequest: '%s' Content-Length %lld exceeds the %lld-byte transport cap - cancelled at header time"),
								*Url, ContentLength, FHttpRangeRequest::MaxResponseBytes);
							if (TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Req = WeakRequest.Pin())
							{
								Req->CancelRequest();
							}
						}
					}
				});
			OutRequest->OnRequestProgress64().BindLambda(
				[Res, WeakRequest, Url](FHttpRequestPtr, uint64, uint64 Received)
				{
					if ((int64)Received > FHttpRangeRequest::MaxResponseBytes)
					{
						Res->bTooLarge = true;
						UE_LOG(LogUnrealGamePatcherDownload, Error, TEXT("FHttpRangeRequest: '%s' exceeded the %lld-byte transport cap mid-body - cancelled"),
							*Url, FHttpRangeRequest::MaxResponseBytes);
						if (TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Req = WeakRequest.Pin())
						{
							Req->CancelRequest();
						}
					}
				});

			OutRequest->OnProcessRequestComplete().BindLambda(
				[Res, Url](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
				{
					Res->bOk = bConnectedSuccessfully && Response.IsValid();
					Res->Content = Response.IsValid() ? Response->GetContent() : TArray<uint8>();
					if (Res->bOk)
					{
						Res->Status = Response->GetResponseCode();
						Res->bStatusOk = (Res->Status == 200) || (Res->Status == 206);
					}
					// Semantic validation happens in the caller (sync checks /
					// async callback) - delivery is unconditional, exactly once.
					UE_LOG(LogUnrealGamePatcherDownload, Verbose, TEXT("FHttpRangeRequest: '%s' settled (ok=%d, status=%d, tooLarge=%d, %d bytes)"),
						*Url, Res->bOk ? 1 : 0, Res->Status, Res->bTooLarge ? 1 : 0, Res->Content.Num());
					Res->DeliverOnce();
				});
			// The HTTP manager runs its own I/O thread; dispatch the completion
			// there so no game-thread ticking is needed (safe from any thread).
			OutRequest->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread);

			if (!OutRequest->ProcessRequest())
			{
				// The engine may defer the completion to the HTTP thread even
				// on a failed ProcessRequest - DeliverOnce keeps the delivery
				// single, whether it lands inline or deferred.
				Res->bOk = false;
				Res->Error = TEXT("cannot start request");
				Res->DeliverOnce();
				return false;
			}
			return true;
		}

		/** Post-completion semantic validation shared by both modes: the
		 *  response must actually carry what was asked for. Returns an empty
		 *  string on success, else the failure reason. */
		FString ValidateResponse(const FHttpAsyncRequest& Res, int64 Offset, int64 Size, bool bRangeSent)
		{
			if (Res.bTooLarge)
			{
				return FString::Printf(TEXT("exceeded the %lld-byte transport cap"), FHttpRangeRequest::MaxResponseBytes);
			}
			if (!Res.bOk)
			{
				return TEXT("request failed");
			}
			if (!Res.bStatusOk)
			{
				return FString::Printf(TEXT("HTTP error %d"), Res.Status);
			}
			// A Range request must actually carry the requested range: a proxy
			// that strips the Range header answers 200 with the FULL body.
			if (bRangeSent && Res.Status != 206)
			{
				return FString::Printf(TEXT("answered %d for a range request (expected 206)"), Res.Status);
			}
			if (Size >= 0 && Res.Content.Num() != Size)
			{
				return FString::Printf(TEXT("returned %d bytes, expected %lld"), Res.Content.Num(), Size);
			}
			if (Res.Content.Num() == 0)
			{
				return TEXT("empty response");
			}
			return FString();
		}
	}

	void FHttpAsyncRequest::DeliverOnce()
	{
		bool bExpected = false;
		const bool bCas = bSettled.compare_exchange_strong(bExpected, true);
		if (bCas && Deliver)
		{
			Deliver();
		}
	}

	bool FHttpRangeRequest::RequestAsync(const FString& Url, int64 Offset, int64 Size, FOnRequestDone OnDone)
	{
		if (Size == 0)
		{
			UE_LOG(LogUnrealGamePatcherDownload, Error, TEXT("FHttpRangeRequest: '%s' requested with Size==0"), *Url);
			return false; // no callback: a Size==0 request is a caller bug
		}
		const TSharedRef<FHttpAsyncRequest> Res = MakeShared<FHttpAsyncRequest>();
		Res->Deliver = [Res, OnDone]() mutable
		{
			OnDone(*Res);
		};
		TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request;
		const bool bIssued = IssueRequest(Url, Offset, Size, Res, Request);
		// Whether the completion lands inline (failed ProcessRequest) or on the
		// HTTP thread later, OnDone fires exactly once via DeliverOnce.
		return bIssued;
	}

	bool FHttpRangeRequest::DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& OutData)
	{
		const TSharedRef<FHttpAsyncRequest> Res = MakeShared<FHttpAsyncRequest>();
		FEvent* WaitEvent = FPlatformProcess::GetSynchEventFromPool(true);
		Res->Deliver = [WaitEvent]()
		{
			WaitEvent->Trigger();
		};

		TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request;
		IssueRequest(Url, Offset, Size, Res, Request);

		// Wait for completion (with a generous timeout so a hung server cannot
		// stall the caller forever).
		const bool bCompleted = WaitEvent->Wait(60 * 1000);
		if (!bCompleted)
		{
			// Abandon the transfer: cancel it, and DO NOT return the event to
			// the pool. The completion lambda still holds the event pointer and
			// its DeliverOnce will fire when the (cancelled) request settles -
			// handing that event to the next waiter would corrupt the pool. One
			// leaked event per timeout is the safe trade.
			if (TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Req = Res->WeakRequest.Pin())
			{
				Req->CancelRequest();
			}
			UE_LOG(LogUnrealGamePatcherDownload, Error, TEXT("FHttpRangeRequest: timeout downloading '%s' (request cancelled, event abandoned)"), *Url);
			return false;
		}
		FPlatformProcess::ReturnSynchEventToPool(WaitEvent);

		const FString Error = ValidateResponse(*Res, Offset, Size, Offset > 0);
		if (!Error.IsEmpty())
		{
			UE_LOG(LogUnrealGamePatcherDownload, Error, TEXT("FHttpRangeRequest: '%s' %s"), *Url, *Error);
			return false;
		}

		OutData.SetNumUninitialized(Res->Content.Num());
		FMemory::Memcpy(OutData.GetData(), Res->Content.GetData(), Res->Content.Num());
		return true;
	}
	void FHttpRangeRequest::ProbeSizeAsync(const FString& Url, TFunction<void(int64 TotalSize, bool bRangeOk)> OnDone)
		{
			// Probe state: delivers exactly once. Content-Range is authoritative
			// (Range supported). Content-Length is only RECORDED at header time -
			// on a 206 it carries the 1-byte partial size, and its wire order
			// relative to Content-Range is server-dependent, so delivering it
			// first-wins misclassified healthy Range servers. The recorded value
			// becomes the answer only at completion, when no Content-Range
			// arrived and the status is a plain 200 (full body = the total).
			struct FProbeState
			{
				std::atomic<bool> bDelivered{ false };
				std::atomic<int64> ContentLength{ -1 };
				TWeakPtr<IHttpRequest, ESPMode::ThreadSafe> WeakRequest;
				TFunction<void(int64, bool)> Deliver;
			};
			const TSharedRef<FProbeState> State = MakeShared<FProbeState>();
			State->Deliver = [State, OnDone](int64 Total, bool bRangeOk)
			{
				bool bExpected = false;
				if (State->bDelivered.compare_exchange_strong(bExpected, true))
				{
					if (TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Req = State->WeakRequest.Pin())
					{
						Req->CancelRequest();
					}
					OnDone(Total, bRangeOk);
				}
			};

			TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request;
			{
				FScopeLock CreateLock(&FHttpRangeRequest::RequestCreateLock);
				Request = FHttpModule::Get().CreateRequest();
			}
			if (!Request.IsValid())
			{
				State->Deliver(-1, false);
				return;
			}
			State->WeakRequest = Request;
			Request->SetURL(Url);
			Request->SetVerb(TEXT("GET"));
			Request->SetHeader(TEXT("Range"), TEXT("bytes=0-0"));
			Request->OnHeaderReceived().BindLambda(
				[State, Url](FHttpRequestPtr, const FString& HeaderName, const FString& NewHeaderValue)
				{
					if (HeaderName.Equals(TEXT("Content-Range"), ESearchCase::IgnoreCase))
					{
						// "bytes 0-0/TOTAL" -> total after the '/' (Range supported).
						const int32 Slash = NewHeaderValue.Find(TEXT("/"), ESearchCase::IgnoreCase);
						if (Slash != INDEX_NONE)
						{
							const int64 Total = FCString::Atoi64(*NewHeaderValue.RightChop(Slash + 1));
							UE_LOG(LogUnrealGamePatcherDownload, Display, TEXT("FHttpRangeRequest: probe '%s' -> %lld bytes (Range supported)"), *Url, Total);
							State->Deliver(Total, true);
						}
					}
					else if (HeaderName.Equals(TEXT("Content-Length"), ESearchCase::IgnoreCase))
					{
						// Recorded only - the answer is deferred to completion
						// (see FProbeState): a 206's Content-Length is the 1-byte
						// partial size, not the total, and its wire order relative to
						// Content-Range is server-dependent (first-header-wins here
						// would misclassify healthy Range servers).
						State->ContentLength = FCString::Atoi64(*NewHeaderValue);
					}
				});
			Request->OnProcessRequestComplete().BindLambda(
				[State, Url](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
				{
					// Fallback when no Content-Range was seen: a plain 200 (Range
					// stripped/unsupported) answers with the FULL body, whose
					// length is the total. Anything else - a 404 with a body, a 206
					// without Content-Range, connection failure - is a probe
					// FAILURE: guessing a total from an error page's length planned
					// a bogus download, and a 206 without Content-Range has no
					// knowable total.
					if (bConnectedSuccessfully && Response.IsValid() &&
						Response->GetResponseCode() == 200 &&
						State->ContentLength.load() > 0)
					{
						const int64 Total = State->ContentLength.load();
						UE_LOG(LogUnrealGamePatcherDownload, Warning, TEXT("FHttpRangeRequest: probe '%s' -> %lld bytes (Range NOT supported, answering 200)"), *Url, Total);
						State->Deliver(Total, false);
						return;
					}
					UE_LOG(LogUnrealGamePatcherDownload, Error, TEXT("FHttpRangeRequest: probe '%s' failed (no total)"), *Url);
					State->Deliver(-1, false);
				});
			Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread);
			Request->ProcessRequest();
		}
}
