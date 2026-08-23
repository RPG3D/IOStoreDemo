// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Download/IChunkDownloader.h"
#include "Download/FDownloadJob.h"

namespace UnrealGamePatcher
{
	/**
	 * Window-driven download executor (Docs/DOWNLOAD.md §5): submits jobs whose
	 * segments (4MB quantum) are issued onto the engine HTTP thread with an
	 * in-flight window (the concurrency knob); hash/write/promote run on a
	 * small compute pool; three-rule fair scheduling fills the window;
	 * journal-bitmap resume survives interruptions with per-segment
	 * granularity. The business layer only Submits and waits - segments,
	 * threads, retries and resume are invisible.
	 *
	 * Per-batch instance semantics: create one executor per download batch
	 * (matches FDownloadManager::DownloadAll); no cross-batch state.
	 */
	struct FDownloadExecutor
	{
		UNREALGAMEPATCHERDOWNLOAD_API FDownloadExecutor(IChunkDownloader& InBackend, const FDownloadExecutorConfig& InConfig = FDownloadExecutorConfig());
		UNREALGAMEPATCHERDOWNLOAD_API ~FDownloadExecutor();

		FDownloadExecutor(const FDownloadExecutor&) = delete;
		FDownloadExecutor& operator=(const FDownloadExecutor&) = delete;

		/** Submits a job. OnDone is invoked EXACTLY once, on the settle
		 *  thread (engine HttpThread for the HTTP backend, the issuing
		 *  thread for inline synchronous backends), when the job completes,
		 *  fails or is canceled. */
		UNREALGAMEPATCHERDOWNLOAD_API int64 Submit(FDownloadJob&& Job, TFunction<void(const FDownloadResult&)> OnDone);

		/** Requests cancellation of a submitted job: in-flight segments are
		 *  aborted and no further segments are issued. OnDone still fires
		 *  exactly once (with a canceled-failure result). */
		UNREALGAMEPATCHERDOWNLOAD_API void Cancel(int64 Handle);

	private:
		struct FExecutorState;                 // defined in the cpp
		TSharedPtr<FExecutorState> State;      // kept alive by in-flight callbacks
	};
}
