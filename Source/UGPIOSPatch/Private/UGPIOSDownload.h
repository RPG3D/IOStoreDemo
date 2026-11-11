// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Download/IChunkDownloader.h"

// UE HTTP needs GConfig. This transport is available before engine PreInit.
class FUGPIOSDownload final : public UnrealGamePatcher::IChunkDownloader
{
public:
	bool DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& OutData) override;
	FCancelHandle DownloadRangeAsync(const FString& Url, int64 Offset, int64 Size,
		TFunction<void(bool, const TArray64<uint8>&, const FString&)>&& OnDone) override;
	void CancelDownload(const FCancelHandle& Handle) override;
};
