// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Download/FDownloadManager.h"
#include "Update/FUpdateEvent.h"
#include "Runtime/Launch/Resources/Version.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 9

@class NSString;
bool UGPIOSDispatchBackgroundSession(NSString* Identifier, void (^Completion)(void));

FString UGPIOSPendingIndexPath(const FString& Cdn);
FString UGPIOSBackgroundCacheDir();
bool UGPIOSPrepareBackgroundStorage();
UnrealGamePatcher::EPatcherError UGPIOSResumeBackgroundBatch(const FString& Key, const TFunction<void(int64, int64)>& Progress);
UnrealGamePatcher::EPatcherError UGPIOSDownloadBackgroundBatch(const FString& Key, int32 Window,
	const TArray<UnrealGamePatcher::FDownloadRequest>& Requests, UnrealGamePatcher::FDownloadStats& Stats,
	TArray<FString>& FailedIds, const TFunction<void(int64, int64)>& Progress);
void UGPIOSClearBackgroundTransfer(const FString& Key, const FString& IndexPath, bool bKeepCache = false);
void UGPIOSWaitUntilActive();

#endif
