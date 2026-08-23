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

	/** Unified semantic error code. Replaces the bool+log propagation so
	 *  callers (UI, CLI, scripts) can distinguish failure categories
	 *  programmatically. Non-zero values are stable process exit codes. */
	enum class EPatcherError : int32
	{
		Ok = 0,
		Usage,              // bad arguments
		ManifestMissing,    // remote index not found / unreadable
		ManifestTampered,   // XXH3-64 self-check failed
		DownloadFailed,     // a chunk download failed permanently
		ChunkVerifyFailed,  // downloaded/assembled bytes failed verification
		WriteNoPermission,  // could not open the target for writing
		WriteNoSpace,       // write failed (disk full / IO error)
		WriteLocked,        // target cannot be replaced (locked / read-only)
		ApplyFailed,        // the apply step failed (rolled back)
		RollbackFailed,     // rolling back the failed transaction failed
		Internal,           // catch-all
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
