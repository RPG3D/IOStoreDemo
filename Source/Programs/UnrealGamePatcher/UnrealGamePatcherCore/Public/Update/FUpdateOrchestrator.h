// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Formats/Index/FIndexManifest.h"
#include "Formats/Diff/FDiffAnalyzer.h"
#include "Download/IChunkDownloader.h"
#include "Update/FUpdateEvent.h"

namespace UnrealGamePatcher
{
	/** Optional transport configuration for the update flow (tasks 6.1-6.6).
	 *  Defaults to local-directory mode: every resource is read straight from
	 *  the local CDN root. Set Downloader + BaseUrl (the http(s):// CDN root)
	 *  to run the update over the pluggable download backend - latest.json,
	 *  the version index and the chunk files are then all fetched over HTTP
	 *  (chunks verified and cached under <InstallDir>/.ugp/chunkcache before
	 *  the apply reads them); the rest of the flow is identical - the update
	 *  is decoupled from the transport, see task 6.6). */
	struct FUpdateOptions
	{
		IChunkDownloader* Downloader = nullptr;
		FString BaseUrl;       // CDN base, e.g. "http://127.0.0.1:8080/cdn"
		FString ChunkCacheDir; // where downloaded chunk files are cached
		int32 MaxConcurrent = 8;
		int32 MaxRetries = 3;
		/** Repair mode (task 5.4 `repair`): touch only damaged (Changed)
		 *  files, never Added/Removed, and rebuild without local reuse. */
		bool bRepairOnly = false;
		/** Force VERIFY mode: full truth extraction (hash every plain file,
		 *  re-read container structures, data-check ucas chunks) instead of
		 *  the default trust mode. Implicitly enabled by RepairOnly, the
		 *  remote manifest's verifyOnUpdate flag, or the time-based trigger. */
		bool bForceVerify = false;
		/** Time-based verify trigger: when the last verified run is older than
		 *  this many days, the update switches to verify mode. 0 disables the
		 *  time trigger. */
		int32 VerifyEveryDays = 10;
		/** Semantic size cap for METADATA resources (latest.json, the version
		 *  index): a CDN serving a bigger "json" is misconfigured or hostile,
		 *  and the fetch is refused instead of buffered. Sized for ~120k
		 *  chunks (~110B/chunk+part of JSON => a 100GB game index is ~30MB).
		 *  The transport's own MaxResponseBytes stays the hard memory bound. */
		int64 MaxManifestBytes = 64 << 20;
		/** Event broadcast (task 8.2): stage changes (Manifest/Diff/Download/
		 *  Apply/Commit/Done), progress ticks (Current/Total) and ERRORS
		 *  (bIsError + EPatcherError + Message) - the single binding point
		 *  for UI / CLI / scripts. Invoked from the update (worker) thread;
		 *  keep the handler cheap and thread-safe. */
		TFunction<void(const FUpdateEvent&)> Events;
	};


	/** Progress/statistics of one update run (task 5.1). */
	struct FUpdateStats
	{
		int32 NumFilesSame = 0;
		int32 NumFilesChanged = 0;
		int32 NumFilesAdded = 0;
		int32 NumFilesRemoved = 0;
		/** Chunks actually fetched from the source (changed/added or fallback). */
		int32 NumChunksFetched = 0;
		/** Chunks whose bytes were sourced from the local install (same SHA1
		 *  on both sides per the local index) instead of the source. */
		int32 NumChunksReused = 0;
		int64 BytesFetched = 0;
		int64 BytesReused = 0;
		int64 BytesWritten = 0;
		/** Distinct guids reused this run (committed only for successful
		 *  rebuilds; NumChunksReused is this set's size at the end). */
		TSet<FString> ReusedChunkGuids;
	};

	/**
	 * The client-side update orchestrator (task 5.1; CLI and GUI both drive
	 * this). Works against a local directory standing in for the CDN - the
	 * download layer (task 6.x) will replace the local reads behind the same
	 * flow:
	 *
	 *   1. load the remote extension manifest (<CdnDir>/<Version>/index.json)
	 *   2. extract the local truth: whole-file SHA1 of every file directly in
	 *      InstallDir (flat layout, same file ids as the manifest) plus the
	 *      chunk list of the last installed version from the local state file
	 *      <InstallDir>/.ugp/index.json (absent on a fresh install => no
	 *      chunk reuse, everything is fetched)
	 *   3. diff local vs remote (FDiffAnalyzer): files by whole-file SHA1,
	 *      chunks by the local index's chunk SHA1s
	 *   4. apply: removed files deleted; changed/added files reassembled from
	 *      their chunk plan - parts whose chunk is unchanged AND identically
	 *      sliced locally are copied from the local file, everything else is
	 *      fetched from the source - written to a temp file, whole-file
	 *      hash-verified, then atomically swapped in; a hash mismatch (local
	 *      bytes were silently damaged) triggers a full-fetch retry
	 *   5. refresh the local state file so the next run can diff at chunk
	 *      level; the resulting install is hash-identical to the remote
	 *      version.
	 */
	struct FUpdateOrchestrator
	{
		/** Runs the full update. CdnDir is ONE CDN base - an HTTP(S) URL
		 *  (everything, including the version index, is fetched over HTTP via
		 *  Options.Downloader) or a local directory (bare path or file://...).
		 *  InstallDir holds the files (same namespace as the manifest's Files
		 *  map). bApply=false performs the check-only analysis (no writes).
		 *  bRepairOnly (task 5.4 `repair`) touches only damaged (Changed)
		 *  files - never Added or Removed - and rebuilds them without local
		 *  reuse.
		 *
		 *  Apply is transactional (design D5, task 5.2): every change is
		 *  planned and logged to the sidecar as a pending transaction BEFORE
		 *  any write; each replaced file's original bytes are backed up; a
		 *  failure at any point rolls the install back to the pre-update
		 *  bytes (the sidecar then reads rolled-back). An interrupted
		 *  (Pending) transaction is rolled back before the new run starts
		 *  (task 5.3). */
		UNREALGAMEPATCHERCORE_API static bool Update(
			const FString& CdnDir,
			const FString& Version,
			const FString& InstallDir,
			bool bApply,
			FUpdateStats& OutStats,
			bool bRepairOnly = false);

		/** Same flow with an explicit transport (tasks 6.1-6.6). With
		 *  Options.Downloader set, all chunks the apply needs are first
		 *  downloaded/verified into the chunk cache (aborting cleanly before
		 *  any file is touched if a download fails), then the apply proceeds
		 *  exactly as in local mode. */
		UNREALGAMEPATCHERCORE_API static bool UpdateWithOptions(
			const FString& CdnDir,
			const FString& Version,
			const FString& InstallDir,
			bool bApply,
			const FUpdateOptions& Options,
			FUpdateStats& OutStats);

		/** Resolves the target version from the CDN's fixed announcement file
		 *  index/latest.json (the cheap stand-in for a dedicated update server)
		 *  when -version= is omitted. CdnRoot is ONE base: an HTTP(S) URL (the
		 *  file is fetched through Options.Downloader) or a local directory
		 *  (bare path or file://...). Returns false when the announcement is
		 *  missing or invalid. */
		UNREALGAMEPATCHERCORE_API static bool ResolveLatest(
			const FString& CdnRoot,
			const FUpdateOptions& Options,
			FString& OutVersion);

		/** Rebuilds the given files unconditionally from the CDN chunks
		 *  (task 5.4 `compact`): temp file + whole-file hash verify + atomic
		 *  swap, reclaiming any dead weight (recorded holes) in the on-disk
		 *  files. Returns the number of files rebuilt, or -1 on any failure
		 *  (nothing is written for the failing file). */
		UNREALGAMEPATCHERCORE_API static int32 Compact(
			const FString& CdnDir,
			const FString& Version,
			const FString& InstallDir,
			const TArray<FString>& FileIds,
			FUpdateStats& OutStats);

		/** TEST-ONLY failure injection (task 5.2 verification): when > 0,
		 *  the apply fails after that many successful target writes, forcing
		 *  the rollback path. Reset to 0 before any production use. */
		UNREALGAMEPATCHERCORE_API static int32 TestFailAfterWrites;

		/** TEST-ONLY crash injection (task 5.3 verification): when > 0, the
		 *  apply stops after that many successful target writes and returns
		 *  false WITHOUT rolling back, leaving the transaction Pending - the
		 *  exact on-disk state a killed process leaves behind. The next
		 *  update run recovers it. Reset to 0 before any production use. */
		UNREALGAMEPATCHERCORE_API static int32 TestCrashAfterWrites;
	};
}
