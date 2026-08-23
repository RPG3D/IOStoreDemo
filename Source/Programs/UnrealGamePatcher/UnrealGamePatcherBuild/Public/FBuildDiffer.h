// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Formats/Index/FIndexManifest.h"
#include "Formats/Diff/FDiffAnalyzer.h"

namespace UnrealGamePatcher
{
	/** Summary of a build-side diff between two versions' extension
	 *  manifests (task 4.2). */
	struct FDeltaSummary
	{
		int32 NumFilesSame = 0;
		int32 NumFilesChanged = 0;
		int32 NumFilesAdded = 0;
		int32 NumFilesRemoved = 0;
		int32 NumChunksSame = 0;
		int32 NumChunksChanged = 0;
		int32 NumChunksAdded = 0;
		int32 NumChunksRemoved = 0;
		/** Total bytes of chunk data the client must download (added +
		 *  changed chunks on the target side). */
		int64 DownloadBytes = 0;

		int64 TotalChunkDelta() const
		{
			return int64(NumChunksAdded) + NumChunksChanged - NumChunksRemoved;
		}
	};

	/** The delta manifest `UnrealGamePatcher.delta.json` (task 4.2): a
	 *  self-contained description of how to move a client from BaseVersion
	 *  to TargetVersion. Written by the build-side differ, consumed by the
	 *  client update flow (task 5.x).
	 *
	 *  Content model:
	 *   - files: the TARGET state of every non-removed file (added/changed);
	 *     removed files are listed in removedFiles.
	 *   - chunks: every chunk the client must download (target-side added or
	 *     changed), with the CDN-relative file path carried over from the
	 *     target index.
	 *   - self-check XXH3-64, same scheme as the extension manifest. */
	struct FDeltaManifest
	{
		static constexpr int32 FormatVersion = 1;
		static constexpr const TCHAR* FormatName = TEXT("UnrealGamePatcher.delta");

		FString AppId;
		FString BaseVersion;
		FString TargetVersion;

		TMap<FString, FIndexFileEntry> Files;       // target-state files to update
		TArray<FString> RemovedFiles;               // files present in base but not target
		TMap<FString, FIndexChunkEntry> Chunks;     // chunks to download (from target)
	};

	/** Loads and saves FDeltaManifest as canonical JSON (same conventions as
	 *  FIndexManifestFile: sorted keys, self-check, atomic write). */
	struct FDeltaManifestFile
	{
		UNREALGAMEPATCHERBUILD_API static bool Save(const FString& Path, const FDeltaManifest& Delta);
		UNREALGAMEPATCHERBUILD_API static bool Load(const FString& Path, FDeltaManifest& OutDelta);
	};

	/** Result of a build-side diff run. */
	struct FDiffOutput
	{
		FDeltaSummary Summary;
		FDeltaManifest Delta;
		FDiffResult Raw; // full classification (diagnostics / reporting)
	};

	/**
	 * Build-side differ (design D8 / tasks 4.2 + 4.3): compares two ingested
	 * versions and produces the incremental artifacts - the delta manifest
	 * plus a directory of the chunk files the delta references.
	 *
	 *   FBuildDiffer::Diff(BaseIndex, TargetIndex, TargetCdnRoot, OutDir)
	 *     -> <OutDir>/UnrealGamePatcher.delta.json
	 *     -> <OutDir>/chunks/<group>/<file>.chunk   (copied from target CDN)
	 *
	 * Any two published versions can be diffed (v1->v3 skips versions), and
	 * an identical pair yields an empty delta (zero download bytes).
	 */
	struct FBuildDiffer
	{
		/** Diffs two loaded extension manifests. TargetCdnRoot is only needed
		 *  when bCopyChunks copies the referenced chunk files. */
		UNREALGAMEPATCHERBUILD_API static bool Diff(
			const FIndexManifest& Base,
			const FIndexManifest& Target,
			const FString& TargetCdnRoot,
			const FString& OutDir,
			bool bCopyChunks,
			FDiffOutput& Out);

		/** Convenience overload: loads both index files first. */
		UNREALGAMEPATCHERBUILD_API static bool DiffIndexFiles(
			const FString& BaseIndexPath,
			const FString& TargetIndexPath,
			const FString& TargetCdnRoot,
			const FString& OutDir,
			bool bCopyChunks,
			FDiffOutput& Out);
	};
}
