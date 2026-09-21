// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Formats/Index/FIndexManifest.h"
#include "State/FPatcherState.h"

namespace UnrealGamePatcher
{
	/** Progress/statistics of one copy-on-write pak patch (the "only write the
	 *  changed bytes" path - no whole-file rewrite, no payload overwrites). */
	struct FInPlacePakStats
	{
		int32 NumSame = 0;          // entries already correct, zero movement
		int32 NumAppended = 0;      // new / changed entries appended (dead ranges first, EOF fallback)
		int64 BytesReused = 0;      // stored bytes of Same entries (not rewritten)
		int64 BytesWritten = 0;     // stored bytes actually written
		int32 NumHolesFilled = 0;   // appends backfilled into dead ranges recomputed from the container
	};

	/**
	 * Entry-level copy-on-write pak patcher (the differentiator vs Wegame/Steam
	 * whole-file rewrite): compares the local pak's per-entry CONTENT hashes
	 * against the remote index's pak entry metadata, and writes ONLY the
	 * changed entries - same entries untouched (zero movement), changed/new
	 * entries appended (dead ranges first, EOF fallback) with a
	 * directory/footer rebuild. Remote entry bytes are sourced from the
	 * covering chunk slices.
	 *
	 * Phase 3 CoW v2 (ROADMAP §5): payload bytes are never overwritten in
	 * place. Placement holes are recomputed from the committed container
	 * (complement of every span the committed footer references) - sidecar
	 * hole bookkeeping is gone, and "mark dead vs reuse a hole" is separated
	 * by construction: a superseded entry is still referenced by the current
	 * footer, so it only becomes a placement candidate for the NEXT patch.
	 * Encrypted-index and signed paks are refused (rewriting index+footer
	 * breaks either) - the caller falls back to the whole-file rebuild.
	 */
	struct FInPlacePakPatcher
	{
		/** Patches LocalPakPath copy-on-write to match the remote manifest's
		 *  pak entry metadata. Requires Remote.Files[PakId].PakEntries (the
		 *  ingest records them). Chunk bytes come from ChunkRoot (the CDN
		 *  version dir or the download cache - whichever the update flow
		 *  reads). bVerifyLocalData (default true) recomputes every local
		 *  entry's CONTENT hash from its payload bytes instead of trusting
		 *  the built-in stored hash - a same-size corruption with an intact
		 *  stored-hash header is still caught and repaired. Trust-mode
		 *  updates pass false (stored hashes; the periodic verify / repair
		 *  catches silent damage).
		 *
		 *  Returns false when the metadata is missing, the container is
		 *  encrypted/signed, the patch fails, or the patched pak does not
		 *  verify per-entry against the remote metadata (entries written by
		 *  this patch are re-hashed from the stored bytes; the container is
		 *  a virtual disk - the whole-file hash is not a valid equality
		 *  signal) - the caller then falls back to the whole-file rebuild. */
		UNREALGAMEPATCHERCORE_API static bool Patch(
			const FIndexManifest& Remote,
			const FString& ChunkRoot,
			const FString& PakId,
			const FString& LocalPakPath,
			FInPlacePakStats& OutStats,
			bool bVerifyLocalData = true);
	};
}
