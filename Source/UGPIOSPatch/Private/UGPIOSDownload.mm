// Copyright Epic Games, Inc. All Rights Reserved.

#include "UGPIOSDownload.h"

#import <Foundation/Foundation.h>

namespace
{
	constexpr int64 MaxBodyBytes = 64ll << 20;

	struct FRequest
	{
		NSURLSessionDataTask* Task = nil;
		TArray64<uint8> Bytes;
		int64 Offset = 0;
		int64 Size = -1;
		FString Error;
		TFunction<void(bool, const TArray64<uint8>&, const FString&)> OnDone;

		int64 Limit() const { return Size >= 0 ? Size : MaxBodyBytes; }
	};
}

@interface UGPIOSRequestDelegate : NSObject <NSURLSessionDataDelegate>
{
@public
	TSharedPtr<FRequest> Request;
}
@end

@implementation UGPIOSRequestDelegate

- (void)URLSession:(NSURLSession*)Session dataTask:(NSURLSessionDataTask*)Task
	didReceiveResponse:(NSURLResponse*)Response completionHandler:(void (^)(NSURLSessionResponseDisposition))Complete
{
	NSHTTPURLResponse* Http = [Response isKindOfClass:[NSHTTPURLResponse class]] ? (NSHTTPURLResponse*)Response : nil;
	const NSInteger Code = Http.statusCode;
	const bool bRange = Request->Size >= 0 || Request->Offset > 0;
	bool bValid = bRange ? Code == 206 : Code == 200;
	if (bValid && bRange)
	{
		NSString* ContentRange = [Http valueForHTTPHeaderField:@"Content-Range"];
		NSString* Prefix = Request->Size >= 0
			? [NSString stringWithFormat:@"bytes %lld-%lld/", Request->Offset, Request->Offset + Request->Size - 1]
			: [NSString stringWithFormat:@"bytes %lld-", Request->Offset];
		bValid = [ContentRange hasPrefix:Prefix];
	}
	if (!bValid || Response.expectedContentLength > Request->Limit())
	{
		Request->Error = FString::Printf(TEXT("unexpected HTTP status, range or size (%ld)"), (long)Code);
		Complete(NSURLSessionResponseCancel);
		return;
	}
	Complete(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)Session dataTask:(NSURLSessionDataTask*)Task didReceiveData:(NSData*)Data
{
	if (Data.length > (uint64)(Request->Limit() - Request->Bytes.Num()))
	{
		Request->Error = TEXT("response exceeded byte limit");
		[Task cancel];
		return;
	}
	Request->Bytes.Append(static_cast<const uint8*>(Data.bytes), Data.length);
}

- (void)URLSession:(NSURLSession*)Session task:(NSURLSessionTask*)Task didCompleteWithError:(NSError*)Error
{
	if (Error && Request->Error.IsEmpty())
	{
		Request->Error = FString(Error.localizedDescription);
	}
	if (Request->Error.IsEmpty() && (Request->Bytes.IsEmpty() ||
		(Request->Size >= 0 && Request->Bytes.Num() != Request->Size)))
	{
		Request->Error = TEXT("response body length did not match the request");
	}
	Request->OnDone(Request->Error.IsEmpty(), Request->Bytes, Request->Error);
	Request->OnDone.Reset();
	Request->Bytes.Empty();
	Request.Reset();
	[Session finishTasksAndInvalidate];
}

@end

FUGPIOSDownload::FCancelHandle FUGPIOSDownload::DownloadRangeAsync(const FString& Url, int64 Offset, int64 Size,
	TFunction<void(bool, const TArray64<uint8>&, const FString&)>&& OnDone)
{
	@autoreleasepool
	{
		NSURL* Resource = [NSURL URLWithString:Url.GetNSString()];
		if (!Resource.host.length || Offset < 0 || Size == 0 || Size > MaxBodyBytes ||
			(Size > 0 && Offset > MAX_int64 - Size))
		{
			OnDone(false, {}, TEXT("invalid download URL or range"));
			return nullptr;
		}

		const TSharedRef<FRequest> State = MakeShared<FRequest>();
		State->Offset = Offset;
		State->Size = Size;
		State->OnDone = MoveTemp(OnDone);
		UGPIOSRequestDelegate* Delegate = [UGPIOSRequestDelegate new];
		Delegate->Request = State;
		NSURLSessionConfiguration* Configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
		Configuration.timeoutIntervalForRequest = 15;
		Configuration.timeoutIntervalForResource = 60;
		NSURLSession* Session = [NSURLSession sessionWithConfiguration:Configuration delegate:Delegate delegateQueue:nil];
		NSMutableURLRequest* Request = [NSMutableURLRequest requestWithURL:Resource];
		[Request setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];
		if (Size >= 0 || Offset > 0)
		{
			NSString* Range = Size >= 0 ? [NSString stringWithFormat:@"bytes=%lld-%lld", Offset, Offset + Size - 1]
				: [NSString stringWithFormat:@"bytes=%lld-", Offset];
			[Request setValue:Range forHTTPHeaderField:@"Range"];
		}
		State->Task = [Session dataTaskWithRequest:Request];
		[State->Task resume];
		return State;
	}
}

bool FUGPIOSDownload::DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& OutData)
{
	dispatch_semaphore_t Done = dispatch_semaphore_create(0);
	bool bSuccess = false;
	DownloadRangeAsync(Url, Offset, Size, [Done, &bSuccess, &OutData](bool bOk, const TArray64<uint8>& Bytes, const FString&)
	{
		bSuccess = bOk;
		if (bOk)
		{
			OutData = Bytes;
		}
		dispatch_semaphore_signal(Done);
	});
	dispatch_semaphore_wait(Done, DISPATCH_TIME_FOREVER);
	return bSuccess;
}

void FUGPIOSDownload::CancelDownload(const FCancelHandle& Handle)
{
	if (const TSharedPtr<FRequest> Request = StaticCastSharedPtr<FRequest>(Handle))
	{
		[Request->Task cancel];
	}
}
