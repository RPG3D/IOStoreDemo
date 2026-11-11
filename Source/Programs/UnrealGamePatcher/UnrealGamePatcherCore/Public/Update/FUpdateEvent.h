// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UnrealGamePatcher
{
	/** High-level phase of an update run (drives the UI's stage display). */
	enum class EUpdatePhase : uint8
	{
		None = 0,
		Manifest,   // loading + self-checking the remote index
		Diff,       // local truth + classification
		Download,   // network: fetching chunks into the cache
		Apply,      // writing changed files (in-place / rebuild)
		Commit,     // refreshing the local state
		Done,       // terminal success
		Failed,     // terminal failure (see Error)
	};

	/** Stable diagnostic codes. The thousands digit identifies the failing
	 *  stage. The CLI translates these to its existing process exit statuses. */
	enum class EPatcherError : int32
	{
		Ok = 0,
		Usage = 1,

		VersionUnavailable = 1001,
		VersionInvalid = 1002,

		ManifestMissing = 2001,
		ManifestTampered = 2002, // invalid JSON, schema, size or self-check
		ChunkPlanInvalid = 2003,

		InstallScanFailed = 3001,
		LocalTruthFailed = 3002,

		DownloadFailed = 4001,

		ApplyFailed = 5001,
		WriteNoPermission = 5002,
		WriteNoSpace = 5003, // short write, including disk full or other I/O failures
		WriteLocked = 5004, // could not replace the destination
		BackupFailed = 5005,
		RemoveFailed = 5006,
		ChunkReadFailed = 5007,

		ChunkVerifyFailed = 6001,
		FileVerifyFailed = 6002,
		InstalledContentInvalid = 6003,

		TransactionStateWriteFailed = 7001,
		CommitStateFailed = 7002,
		CommitIndexFailed = 7003,
		ActivateContentFailed = 7004,

		RollbackFailed = 8001,
		Interrupted = 8002,
		LegacyMigrationFailed = 8003,

		Internal = 9000,
		InstallLockFailed = 9001,
		InstallDirectoryFailed = 9002,
		InvalidConfiguration = 9003,
		StagingSnapshotFailed = 9004,
		StagingCleanupFailed = 9005,
	};

	/** One broadcast event: a stage change, a progress tick, or an error.
	 *  Emitted from the update (worker) thread - handlers must be cheap and
	 *  marshal to their own thread as needed (the GUI stores the event under
	 *  a lock and the Slate tick reads it). */
	struct FUpdateEvent
	{
		EUpdatePhase Phase = EUpdatePhase::None;
		EPatcherError Error = EPatcherError::Ok;
		bool bIsError = false;
		int32 Current = 0;  // progress within the phase (0 when N/A)
		int32 Total = 0;    // phase total (0 when N/A)
		FString Message;    // human-readable detail (error cause / summary)
	};
}
