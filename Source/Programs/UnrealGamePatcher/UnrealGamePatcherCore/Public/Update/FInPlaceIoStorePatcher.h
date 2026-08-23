// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Formats/Index/FIndexManifest.h"
#include "State/FPatcherState.h"

namespace UnrealGamePatcher
{
	/** Progress/statistics of one in-place ucas patch (chunk-id based). */
	struct FInPlaceIoStoreStats
	{
		int32 NumSame = 0;       // chunks already present (id+meta match), zero movement
		int32 NumReplaced = 0;   // changed chunks appended + entry redirected
		int32 NumAppended = 0;   // new chunks appended
		int32 NumDropped = 0;    // local chunks absent from the target (dropped)
		int64 BytesReused = 0;   // raw bytes of Same chunks (not rewritten)
		int64 BytesWritten = 0;  // raw bytes actually appended
		int32 NumHolesFilled = 0; // replaced/new chunks backfilled into dead ranges
		/** The file's complete post-patch hole list (see FInPlacePakStats). */
		TArray<FHole> UpdatedHoles;
	};

	/**
	 * Chunk-level in-place ucas patcher (design D6): diffs the local utoc's
	 * chunk ids against the remote index's IoStore metadata, fetches only the
	 * changed/new chunks' raw bytes from the remote ucas, appends them (block
	 * aligned, unchanged chunks never move) and atomically rewrites the utoc.
	 * The result is LOGICALLY equivalent (same chunk ids + data), not
	 * byte-identical - verification is by chunk-id / chunk-data.
	 */
	struct FInPlaceIoStorePatcher
	{
		/** Patches the local ucas (with sibling utoc) in place to match the
		 *  remote IoStore chunk metadata. Chunk bytes come from ChunkRoot
		 *  (the CDN version dir or the download cache).
		 *  bVerifyLocalData (default true) additionally re-hashes every local
		 *  chunk against its utoc meta hash BEFORE patching, so a damaged
		 *  container falls back to the whole-file rebuild instead of being
		 *  surgically patched; trust-mode updates pass false (the container's
		 *  chunk-id set was already accepted, the periodic verify / repair
		 *  catches silent damage).
		 *  Returns false on any failure - the caller falls back to the
		 *  whole-file rebuild. */
		UNREALGAMEPATCHERCORE_API static bool Patch(
			const FIndexManifest& Remote,
			const FString& ChunkRoot,
			const FString& UcasId,
			const FString& LocalUcasPath,
			const FString& LocalUtocPath,
			FInPlaceIoStoreStats& OutStats,
			bool bVerifyLocalData = true,
			const TArray<FHole>& InAvailableHoles = TArray<FHole>());
	};
}
