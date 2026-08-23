// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Formats/Index/FIndexManifest.h"
#include "State/FPatcherState.h"

namespace UnrealGamePatcher
{
	/** Progress/statistics of one in-place pak patch (the "only write the
	 *  changed bytes" path - no whole-file rewrite). */
	struct FInPlacePakStats
	{
		int32 NumSame = 0;          // entries already correct, zero movement
		int32 NumOverwritten = 0;   // same-size changed entries, overwritten in place
		int32 NumAppended = 0;      // new / size-changed entries appended at the end
		int64 BytesReused = 0;      // stored bytes of Same entries (not rewritten)
		int64 BytesWritten = 0;     // stored bytes actually written (overwrite + append)
		int32 NumHolesFilled = 0;   // appends backfilled into recorded dead ranges
		/** The file's complete post-patch hole list: InAvailableHoles minus
		 *  consumed ranges plus their remainders, plus the dead regions this
		 *  patch created (superseded entry data + the old index/footer).
		 *  The caller persists it as the file's State.Holes entry. */
		TArray<FHole> UpdatedHoles;
	};

	/**
	 * Entry-level in-place pak patcher (the differentiator vs Wegame/Steam
	 * whole-file rewrite): compares the local pak's per-entry CONTENT hashes
	 * against the remote index's pak entry metadata, and writes ONLY the
	 * changed entries - same entries untouched (zero movement), same-size
	 * changed entries overwritten in place, new/size-changed entries appended
	 * with a directory/footer rebuild. Remote entry bytes are sourced from the
	 * covering chunk slices.
	 */
	struct FInPlacePakPatcher
	{
		/** Patches LocalPakPath in place to match the remote manifest's pak
		 *  entry metadata. Requires Remote.Files[PakId].PakEntries (the ingest
		 *  records them). Chunk bytes come from ChunkRoot (the CDN version dir
		 *  or the download cache - whichever the update flow reads).
		 *  bVerifyLocalData (default true) recomputes every local entry's
		 *  CONTENT hash from its payload bytes instead of trusting the built-in
		 *  stored hash - a same-size corruption with an intact stored-hash
		 *  header is still caught and repaired in place. Trust-mode updates
		 *  pass false (stored hashes; the periodic verify / repair catches
		 *  silent damage).
		 *
		 *  Returns false when the metadata is missing, the patch fails, or the
		 *  patched pak does not verify per-entry against the remote metadata
		 *  (the container is a virtual disk - the whole-file hash is not a
		 *  valid equality signal) - the caller then falls back to the
		 *  whole-file rebuild. */
		UNREALGAMEPATCHERCORE_API static bool Patch(
			const FIndexManifest& Remote,
			const FString& ChunkRoot,
			const FString& PakId,
			const FString& LocalPakPath,
			FInPlacePakStats& OutStats,
			bool bVerifyLocalData = true,
			const TArray<FHole>& InAvailableHoles = TArray<FHole>());
	};
}
