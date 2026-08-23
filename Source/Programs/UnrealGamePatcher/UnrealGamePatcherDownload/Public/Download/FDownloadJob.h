// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UnrealGamePatcher
{
	/** Executor configuration (design D6). All fields have static defaults and
	 *  may be overridden per executor instance; no ini binding (Programs keep
	 *  configuration explicit). */
	struct FDownloadExecutorConfig
	{
		int64 SegmentSize = 4ll << 20;   // 4MB: memory/scheduling/resume quantum
		int32 Window = 24;               // in-flight segments (concurrency knob)
		int32 MaxRetries = 3;            // per segment / per whole-job verify
	};

	/** One logical download submitted to the executor. The business layer
	 *  never sees segments/threads/journal - only this in and a result out. */
	struct FDownloadJob
	{
		FString Id;              // business identifier (returned in the result)
		FString Url;             // source (backend-resolved)
		FString DestPath;        // final path (executor stages beside it)
		int64 ExpectedSize = -1; // -1 = unknown (the executor probes first)
		FString ExpectedSha1;    // optional whole-file SHA1 for final verification
	};

	/** Outcome of one submitted job (delivered exactly once). */
	struct FDownloadResult
	{
		FString Id;
		bool bSuccess = false;
		int64 BytesDownloaded = 0;   // wire bytes consumed by this job
		int32 Retries = 0;           // transport/verify retries issued
		FString Error;               // failure reason (empty on success)
	};
}
