// Copyright Epic Games, Inc. All Rights Reserved.

#include "UGPIOSBackground.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 9

#include "Algo/AllOf.h"
#include "Formats/Chunk/FChunkDataLoader.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Stats/FIOStats.h"

#include <cerrno>
#import <UIKit/UIKit.h>

using namespace UnrealGamePatcher;

namespace
{
	NSString* const SessionID = @"com.rpg3d.ugp.content-download.v1";
	constexpr int32 DefaultWindow = 128;
	constexpr int64 MaxChunkFileBytes = (4ll << 20) + FChunkDataLoader::HeaderSize;

	NSString* StorageRoot()
	{
		return [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject
			stringByAppendingPathComponent:@"UGPBackground"];
	}

	NSString* CacheRoot()
	{
		return [StorageRoot() stringByAppendingPathComponent:@"cache"];
	}

	NSString* PlanPath()
	{
		return [StorageRoot() stringByAppendingPathComponent:@"batch.plist"];
	}

	NSString* ChunkPath(NSString* ID)
	{
		FString Hash(ID);
		return [CacheRoot() stringByAppendingPathComponent:FChunkDataLoader::MakeChunkRelPath(Hash).GetNSString()];
	}

	bool EnsureDirectory(NSString* Path, NSError** Error = nil)
	{
		return [NSFileManager.defaultManager createDirectoryAtPath:Path withIntermediateDirectories:YES
			attributes:@{NSFileProtectionKey: NSFileProtectionCompleteUntilFirstUserAuthentication} error:Error];
	}

	bool HasExpectedSize(NSString* Path, int64 Size)
	{
		NSDictionary* Attributes = [NSFileManager.defaultManager attributesOfItemAtPath:Path error:nil];
		return [Attributes.fileType isEqualToString:NSFileTypeRegular] && Attributes.fileSize == (uint64)Size;
	}

	bool IsActive()
	{
		__block BOOL Active;
		dispatch_sync(dispatch_get_main_queue(), ^{ Active = UIApplication.sharedApplication.applicationState == UIApplicationStateActive; });
		return Active;
	}

	struct FBatchStatus
	{
		int64 Current = 0;
		int64 Total = 0;
		int64 BytesFetched = 0;
		int32 Completed = 0;
		int32 Reused = 0;
		bool bDone = false;
		EPatcherError Error = EPatcherError::Ok;
		NSString* FailureID = nil;
	};
}

// All transfer state, delegate calls, and reconciliation run on one serial
// queue. UIKit and the game thread are only clients of that queue.
@interface UGPIOSBackgroundSession : NSObject <NSURLSessionDownloadDelegate>
@property (nonatomic, strong, readonly) dispatch_queue_t queue;
+ (instancetype)shared;
- (void)receiveEvents:(void (^)(void))Completion;
- (BOOL)resumeBatch:(NSString*)Key;
- (BOOL)startBatch:(NSDictionary*)Plan;
- (FBatchStatus)status;
- (void)clearBatch:(NSString*)Key indexPath:(NSString*)IndexPath keepCache:(BOOL)KeepCache;
@end

@implementation UGPIOSBackgroundSession
{
	NSURLSession* _session;
	NSDictionary* _plan;
	NSDictionary<NSString*, NSDictionary*>* _objects;
	NSMutableOrderedSet<NSString*>* _missing;
	NSMutableDictionary<NSString*, NSURLSessionDownloadTask*>* _inflight;
	NSMutableSet<NSString*>* _failed;
	NSMutableDictionary<NSNumber*, NSNumber*>* _fileErrors;
	NSMutableArray<void (^)(void)>* _systemCompletions;
	BOOL _reconciling;
	BOOL _eventsDrained;
	EPatcherError _batchError;
	int64 _totalBytes;
	int64 _completeBytes;
	int64 _reusedBytes;
	int32 _reusedCount;
}

+ (instancetype)shared
{
	static UGPIOSBackgroundSession* Transfer;
	static dispatch_once_t Once;
	dispatch_once(&Once, ^{ Transfer = [UGPIOSBackgroundSession new]; });
	return Transfer;
}

- (instancetype)init
{
	self = [super init];
	if (self)
	{
		_queue = dispatch_queue_create("com.rpg3d.ugp.content-download", DISPATCH_QUEUE_SERIAL);
		_inflight = [NSMutableDictionary new];
		_failed = [NSMutableSet new];
		_fileErrors = [NSMutableDictionary new];
		_systemCompletions = [NSMutableArray new];
	}
	return self;
}

- (void)loadPlan:(NSDictionary*)Plan
{
	_plan = Plan;
	_missing = [NSMutableOrderedSet new];
	[_failed removeAllObjects];
	[_fileErrors removeAllObjects];
	_batchError = EPatcherError::Ok;
	_totalBytes = 0;
	_completeBytes = 0;
	_reusedCount = 0;
	NSMutableDictionary* Objects = [NSMutableDictionary new];
	for (NSDictionary* Object in Plan[@"objects"])
	{
		@autoreleasepool
		{
			NSString* ID = Object[@"id"];
			const int64 Size = [Object[@"size"] longLongValue];
			Objects[ID] = Object;
			_totalBytes += Size;
			if (HasExpectedSize(ChunkPath(ID), Size))
			{
				_completeBytes += Size;
				++_reusedCount;
			}
			else
			{
				[_missing addObject:ID];
			}
		}
	}
	_reusedBytes = _completeBytes;
	_objects = Objects;
}

- (NSString*)objectIDForTask:(NSURLSessionTask*)Task
{
	NSString* Token = _plan[@"token"];
	NSString* Description = Task.taskDescription;
	if (!Token || Description.length != Token.length + 1 + 40 ||
		![Description hasPrefix:[Token stringByAppendingString:@"/"]])
	{
		return nil;
	}
	NSString* ID = [Description substringFromIndex:Token.length + 1];
	return _objects[ID] ? ID : nil;
}

- (void)openSession
{
	if (_session)
	{
		return;
	}
	[self loadPlan:[NSDictionary dictionaryWithContentsOfFile:PlanPath()]];
	NSURLSessionConfiguration* Config = [NSURLSessionConfiguration backgroundSessionConfigurationWithIdentifier:SessionID];
	Config.sessionSendsLaunchEvents = YES;
	Config.discretionary = NO;
	Config.timeoutIntervalForRequest = 30;
	NSOperationQueue* DelegateQueue = [NSOperationQueue new];
	DelegateQueue.maxConcurrentOperationCount = 1;
	DelegateQueue.underlyingQueue = _queue;
	_session = [NSURLSession sessionWithConfiguration:Config delegate:self delegateQueue:DelegateQueue];
	[self reconcile];
}

- (void)reconcile
{
	if (_reconciling)
	{
		return;
	}
	_reconciling = YES;
	[_session getAllTasksWithCompletionHandler:^(NSArray<NSURLSessionTask*>* Tasks)
	{
		dispatch_async(self.queue, ^{
			for (NSURLSessionDownloadTask* Task in Tasks)
			{
				NSString* ID = [self objectIDForTask:Task];
				if (!ID || ![self->_missing containsObject:ID] || [self->_failed containsObject:ID])
				{
					[Task cancel];
				}
				else if (Task.state == NSURLSessionTaskStateRunning || Task.state == NSURLSessionTaskStateSuspended)
				{
					NSURLSessionDownloadTask* Existing = self->_inflight[ID];
					if (Existing && Existing.taskIdentifier != Task.taskIdentifier)
					{
						[Task cancel];
						continue;
					}
					self->_inflight[ID] = Task;
					[Task resume];
				}
			}
			self->_reconciling = NO;
			[self refill];
			[self completeWake];
		});
	}];
}

- (void)receiveEvents:(void (^)(void))Completion
{
	NSLog(@"UGP: received background session events");
	[_systemCompletions addObject:[Completion copy]];
	[self openSession];
}

- (void)completeWake
{
	if (!_reconciling && _eventsDrained && _systemCompletions.count)
	{
		NSArray<void (^)(void)>* Completions = [_systemCompletions copy];
		[_systemCompletions removeAllObjects];
		_eventsDrained = NO;
		dispatch_async(dispatch_get_main_queue(), ^{
			for (void (^Completion)(void) in Completions)
			{
				Completion();
			}
		});
	}
}

- (void)refill
{
	const NSUInteger Window = [_plan[@"window"] unsignedIntegerValue];
	if (_reconciling || _batchError != EPatcherError::Ok || _inflight.count > Window / 2 || !_missing.count)
	{
		return;
	}
	NSUInteger Started = 0;
	for (NSString* ID in _missing)
	{
		if (_inflight.count >= Window)
		{
			break;
		}
		if (_inflight[ID])
		{
			continue;
		}
		NSDictionary* Object = _objects[ID];
		NSMutableURLRequest* Request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:Object[@"url"]]];
		[Request setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];
		NSURLSessionDownloadTask* Task = [_session downloadTaskWithRequest:Request];
		Task.countOfBytesClientExpectsToReceive = [Object[@"size"] longLongValue];
		Task.taskDescription = [NSString stringWithFormat:@"%@/%@", _plan[@"token"], ID];
		_inflight[ID] = Task;
		[Task resume];
		++Started;
	}
	if (Started)
	{
		NSLog(@"UGP: queued %lu background downloads (%lu active, %lu missing)",
			(unsigned long)Started, (unsigned long)_inflight.count, (unsigned long)_missing.count);
	}
}

- (BOOL)resumeBatch:(NSString*)Key
{
	[self openSession];
	if (![_plan[@"key"] isEqualToString:Key])
	{
		return NO;
	}
	[_failed removeAllObjects];
	_batchError = EPatcherError::Ok;
	[self reconcile];
	return YES;
}

- (BOOL)startBatch:(NSDictionary*)Plan
{
	[self openSession];
	if (![Plan writeToFile:PlanPath() atomically:YES])
	{
		return NO;
	}
	for (NSURLSessionTask* Task in _inflight.allValues)
	{
		[Task cancel];
	}
	[_inflight removeAllObjects];
	[self loadPlan:Plan];
	[self reconcile];
	return YES;
}

- (FBatchStatus)status
{
	FBatchStatus Status;
	Status.Current = _completeBytes;
	for (NSString* ID in _inflight)
	{
		const int64 Size = [_objects[ID][@"size"] longLongValue];
		Status.Current += FMath::Clamp<int64>(_inflight[ID].countOfBytesReceived, 0, Size);
	}
	Status.Total = _totalBytes;
	Status.BytesFetched = _completeBytes - _reusedBytes;
	Status.Completed = static_cast<int32>(_objects.count - _missing.count);
	Status.Reused = _reusedCount;
	Status.bDone = !_reconciling && !_missing.count;
	Status.Error = _batchError;
	Status.FailureID = _failed.anyObject ?: _missing.firstObject;
	if (!Status.bDone && Status.Total > 0)
	{
		Status.Current = FMath::Min(Status.Current, Status.Total - 1);
	}
	return Status;
}

- (void)clearBatch:(NSString*)Key indexPath:(NSString*)IndexPath keepCache:(BOOL)KeepCache
{
	[self openSession];
	if ([_plan[@"key"] isEqualToString:Key])
	{
		[NSFileManager.defaultManager removeItemAtPath:PlanPath() error:nil];
		for (NSURLSessionTask* Task in _inflight.allValues)
		{
			[Task cancel];
		}
		[_inflight removeAllObjects];
		[self loadPlan:nil];
	}
	// Another CDN can have a pending plan in this session.
	if (!KeepCache && !_plan)
	{
		[NSFileManager.defaultManager removeItemAtPath:CacheRoot() error:nil];
	}
	[NSFileManager.defaultManager removeItemAtPath:IndexPath error:nil];
}

- (void)URLSession:(NSURLSession*)Session downloadTask:(NSURLSessionDownloadTask*)Task
	didWriteData:(int64_t)BytesWritten totalBytesWritten:(int64_t)Total
	totalBytesExpectedToWrite:(int64_t)Expected
{
	NSString* ID = [self objectIDForTask:Task];
	const int64 Size = [_objects[ID ?: @""][@"size"] longLongValue];
	if (ID && (Total > Size || Expected > Size))
	{
		[Task cancel];
	}
}

- (void)URLSession:(NSURLSession*)Session downloadTask:(NSURLSessionDownloadTask*)Task
	didFinishDownloadingToURL:(NSURL*)Location
{
	NSString* ID = [self objectIDForTask:Task];
	if (!ID || ![_missing containsObject:ID])
	{
		return;
	}
	const int64 Size = [_objects[ID][@"size"] longLongValue];
	NSHTTPURLResponse* Http = [Task.response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse*)Task.response : nil;
	// A system-resumed GET can finish with a 206 response. The temporary
	// file must still contain the entire chunk and pass its content hash.
	if ((Http.statusCode != 200 && Http.statusCode != 206) || !HasExpectedSize(Location.path, Size))
	{
		NSLog(@"UGP: background response rejected for %@ (HTTP %ld)", ID, (long)Http.statusCode);
		return;
	}
	NSString* Path = ChunkPath(ID);
	NSString* Staged = [Path stringByAppendingString:@".download"];
	NSError* Error = nil;
	auto RecordWriteError = [&](int MoveErrno)
	{
		const bool bPermission = Error ? Error.code == NSFileWriteNoPermissionError : MoveErrno == EACCES || MoveErrno == EPERM;
		const EPatcherError Code = bPermission ? EPatcherError::WriteNoPermission : EPatcherError::WriteNoSpace;
		_fileErrors[@(Task.taskIdentifier)] = @(static_cast<int32>(Code));
		NSLog(@"UGP: could not store background chunk %@: %@", ID,
			Error.localizedDescription ?: [NSString stringWithUTF8String:strerror(MoveErrno)]);
	};
	// NSURLSession can hand out a /.nofollow path. Move it into our sandbox
	// cache before passing it through Unreal's iOS path mapping.
	if (!EnsureDirectory([Path stringByDeletingLastPathComponent], &Error) ||
		rename(Location.fileSystemRepresentation, Staged.fileSystemRepresentation) != 0)
	{
		RecordWriteError(errno);
		return;
	}
	ON_SCOPE_EXIT { [NSFileManager.defaultManager removeItemAtPath:Staged error:nil]; };
	if (![NSFileManager.defaultManager setAttributes:@{NSFileProtectionKey: NSFileProtectionCompleteUntilFirstUserAuthentication}
		ofItemAtPath:Staged error:&Error])
	{
		RecordWriteError(errno);
		return;
	}

	FIndexChunkEntry Entry;
	Entry.SHA1 = FString(ID);
	Entry.Size = Size;
	TArray64<uint8> Decoded;
	if (!FChunkDataLoader::Load(FString(Staged), Entry, Decoded))
	{
		_fileErrors[@(Task.taskIdentifier)] = @(static_cast<int32>(EPatcherError::ChunkVerifyFailed));
		NSLog(@"UGP: [UGP-6001] background chunk verification failed for %@", ID);
		return;
	}
	if (rename(Staged.fileSystemRepresentation, Path.fileSystemRepresentation) != 0)
	{
		RecordWriteError(errno);
	}
}

- (void)URLSession:(NSURLSession*)Session task:(NSURLSessionTask*)Task didCompleteWithError:(NSError*)Error
{
	NSString* ID = [self objectIDForTask:Task];
	NSNumber* TaskID = @(Task.taskIdentifier);
	NSNumber* FileError = _fileErrors[TaskID];
	[_fileErrors removeObjectForKey:TaskID];
	if (!ID || ![_missing containsObject:ID] ||
		(_inflight[ID] && _inflight[ID].taskIdentifier != Task.taskIdentifier))
	{
		return;
	}
	[_inflight removeObjectForKey:ID];
	const int64 Size = [_objects[ID][@"size"] longLongValue];
	if (HasExpectedSize(ChunkPath(ID), Size))
	{
		_completeBytes += Size;
		[_missing removeObject:ID];
	}
	else
	{
		[_failed addObject:ID];
		NSHTTPURLResponse* Response = [Task.response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse*)Task.response : nil;
		if (IsHttpMissingResource(static_cast<int32>(Response.statusCode)))
		{
			_batchError = EPatcherError::DownloadSourceUnavailable;
		}
		else if (_batchError == EPatcherError::Ok)
		{
			_batchError = FileError ? static_cast<EPatcherError>(FileError.intValue) : EPatcherError::DownloadFailed;
		}
		NSLog(@"UGP: [UGP-%04d] background download failed for %@: %@",
			static_cast<int32>(_batchError), ID, Error.localizedDescription ?: @"invalid or missing file");
	}
	if (!_systemCompletions.count)
	{
		[self refill];
	}
}

- (void)URLSessionDidFinishEventsForBackgroundURLSession:(NSURLSession*)Session
{
	[self refill];
	_eventsDrained = _systemCompletions.count > 0;
	[self completeWake];
}
@end

bool UGPIOSDispatchBackgroundSession(NSString* Identifier, void (^Completion)(void))
{
	if (![Identifier isEqualToString:SessionID])
	{
		return false;
	}
	UGPIOSBackgroundSession* Transfer = [UGPIOSBackgroundSession shared];
	dispatch_async(Transfer.queue, ^{ [Transfer receiveEvents:Completion]; });
	return true;
}

FString UGPIOSPendingIndexPath(const FString& Cdn)
{
	FTCHARToUTF8 Utf8(*Cdn);
	const FSHAHash Hash = FSHA1::HashBuffer(Utf8.Get(), Utf8.Length());
	return FString(StorageRoot()) / (Hash.ToString() + TEXT(".index.json"));
}

FString UGPIOSBackgroundCacheDir()
{
	return FString(CacheRoot());
}

bool UGPIOSPrepareBackgroundStorage()
{
	if (!EnsureDirectory(StorageRoot()))
	{
		return false;
	}
	NSURL* URL = [NSURL fileURLWithPath:StorageRoot() isDirectory:YES];
	return [URL setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
}

void UGPIOSWaitUntilActive()
{
	while (!IsActive())
	{
		FPlatformProcess::Sleep(0.25f);
	}
}

namespace
{
	EPatcherError WaitForBatch(UGPIOSBackgroundSession* Transfer, FBatchStatus& Final, const TFunction<void(int64, int64)>& Progress)
	{
		int64 LastBytes = -1;
		double LastTime = FPlatformTime::Seconds();
		double ActiveStall = 0;
		for (;;)
		{
			__block FBatchStatus Status;
			dispatch_sync(Transfer.queue, ^{ Status = [Transfer status]; });
			Final = Status;
			if (Status.Error != EPatcherError::Ok)
			{
				return Status.Error;
			}
			if (Progress && Status.Current != LastBytes && Status.Total > 0)
			{
				Progress(Status.Current, Status.Total);
			}
			if (Status.bDone)
			{
				UGPIOSWaitUntilActive();
				return EPatcherError::Ok;
			}
			const double Now = FPlatformTime::Seconds();
			if (Status.Current != LastBytes)
			{
				LastBytes = Status.Current;
				ActiveStall = 0;
			}
			else if (IsActive())
			{
				// Exclude gaps caused by process suspension from the UI timeout.
				ActiveStall += FMath::Min(1.0, Now - LastTime);
				if (ActiveStall >= 60)
				{
					NSLog(@"UGP: background batch still pending; leaving system downloads queued");
					return EPatcherError::DownloadFailed;
				}
			}
			LastTime = Now;
			FPlatformProcess::Sleep(0.25f);
		}
	}
}

EPatcherError UGPIOSResumeBackgroundBatch(const FString& Key, const TFunction<void(int64, int64)>& Progress)
{
	UGPIOSBackgroundSession* Transfer = [UGPIOSBackgroundSession shared];
	NSString* BatchKey = Key.GetNSString();
	__block BOOL HasSavedBatch;
	dispatch_sync(Transfer.queue, ^{ HasSavedBatch = [Transfer resumeBatch:BatchKey]; });
	FBatchStatus Status;
	return HasSavedBatch ? WaitForBatch(Transfer, Status, Progress) : EPatcherError::Ok;
}

EPatcherError UGPIOSDownloadBackgroundBatch(const FString& Key, int32 Window, const TArray<FDownloadRequest>& Requests,
	FDownloadStats& Stats, TArray<FString>& FailedIds, const TFunction<void(int64, int64)>& Progress)
{
	NSMutableArray* Objects = [NSMutableArray arrayWithCapacity:Requests.Num()];
	const FString CacheDir = UGPIOSBackgroundCacheDir();
	for (const FDownloadRequest& Request : Requests)
	{
		@autoreleasepool
		{
			const FString Relative = FChunkDataLoader::MakeChunkRelPath(Request.Id);
			const bool bHex = Request.Id.Len() == 40 && Algo::AllOf(Request.Id, [](TCHAR Digit) { return FChar::IsHexDigit(Digit); });
			NSString* URL = Request.Url.GetNSString();
			NSURL* Resource = [NSURL URLWithString:URL];
			NSString* Scheme = Resource.scheme.lowercaseString;
			if (!bHex || !Resource.host.length || Request.Size <= 0 || Request.Size > MaxChunkFileBytes ||
				(![Scheme isEqualToString:@"https"] && ![Scheme isEqualToString:@"http"]) ||
				Request.DestFile != CacheDir / Relative)
			{
				FailedIds.Add(Request.Id);
				return EPatcherError::ChunkPlanInvalid;
			}
			[Objects addObject:@{@"id": Request.Id.GetNSString(), @"url": URL, @"size": @(Request.Size)}];
		}
	}
	NSDictionary* Plan = @{@"key": Key.GetNSString(), @"token": NSUUID.UUID.UUIDString, @"objects": Objects,
		@"window": @(Window > 0 ? FMath::Clamp(Window, 1, DefaultWindow) : DefaultWindow)};
	UGPIOSBackgroundSession* Transfer = [UGPIOSBackgroundSession shared];
	__block BOOL Started;
	dispatch_sync(Transfer.queue, ^{ Started = [Transfer startBatch:Plan]; });
	FBatchStatus Status;
	const EPatcherError Error = Started ? WaitForBatch(Transfer, Status, Progress) : EPatcherError::WriteNoSpace;
	Stats.Completed = Status.Completed;
	Stats.SkippedExisting = Status.Reused;
	Stats.BytesDownloaded = Status.BytesFetched;
	FIOStats::Get().AddNetDownload(Status.BytesFetched);
	if (Error != EPatcherError::Ok && !Requests.IsEmpty())
	{
		FailedIds.Add(Status.FailureID ? FString(Status.FailureID) : Requests[0].Id);
	}
	return Error;
}

void UGPIOSClearBackgroundTransfer(const FString& Key, const FString& IndexPath, bool bKeepCache)
{
	UGPIOSBackgroundSession* Transfer = [UGPIOSBackgroundSession shared];
	NSString* BatchKey = Key.GetNSString();
	NSString* Path = IndexPath.GetNSString();
	dispatch_sync(Transfer.queue, ^{ [Transfer clearBatch:BatchKey indexPath:Path keepCache:bKeepCache]; });
}

#endif
