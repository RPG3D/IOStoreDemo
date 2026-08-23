// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Formats/Index/FIndexManifest.h"

namespace UnrealGamePatcher
{
	/** Result classification of one item (file or chunk) when comparing the
	 *  local truth against the remote extension manifest (design D3 / task 3.3).
	 *  Same id + same content hash => unchanged (skip); same id + different
	 *  hash => Changed; remote-only => Added; local-only => Removed. */
	enum class EChangeKind : uint8
	{
		Same,
		Changed,
		Added,
		Removed,
	};

	/** One diffed item. Id is the file's relative path (posix) for file-level
	 *  entries, or the chunk id for chunk-level entries. */
	struct FDiffEntry
	{
		EChangeKind Kind = EChangeKind::Same;
		FString Id;

		FString LocalSha1;  // empty when the item does not exist locally
		FString RemoteSha1; // empty when the item does not exist remotely
		int64 LocalSize = 0;
		int64 RemoteSize = 0;
	};

	/** The full diff of a local install against a remote extension manifest. */
	struct FDiffResult
	{
		// File-level entries (whole-file SHA1 semantics, covers plain files and
		// IoStore containers until the FIoStoreReader lands).
		TArray<FDiffEntry> Files;
		// Chunk-level entries (pak content chunks).
		TArray<FDiffEntry> Chunks;

		int32 CountByKind(EChangeKind Kind) const
		{
			int32 N = 0;
			for (const FDiffEntry& E : Files) { if (E.Kind == Kind) ++N; }
			for (const FDiffEntry& E : Chunks) { if (E.Kind == Kind) ++N; }
			return N;
		}

		int32 SameCount() const { return CountByKind(EChangeKind::Same); }
		int32 ChangedCount() const { return CountByKind(EChangeKind::Changed); }
		int32 AddedCount() const { return CountByKind(EChangeKind::Added); }
		int32 RemovedCount() const { return CountByKind(EChangeKind::Removed); }
	};

	/** Compares the local truth (files/chunks extracted from the local install)
	 *  against the remote extension manifest and classifies every item.
	 *
	 *  The local truth has the same shape as the manifest, so the analyzer is a
	 *  pure function over two FIndexManifest-like maps: Same when the item id
	 *  exists on both sides with the same content hash; Changed when the hash
	 *  differs; Added when only remote has it; Removed when only local has it.
	 *  Size is reported for diagnostics; the content hash is the sole equality
	 *  criterion (design D3: "同 hash ⇒ 同内容"). */
	struct FDiffAnalyzer
	{
		/** Diff file-level entries (whole-file SHA1). */
		UNREALGAMEPATCHERCORE_API static void DiffFiles(
			const TMap<FString, FIndexFileEntry>& Local,
			const TMap<FString, FIndexFileEntry>& Remote,
			TArray<FDiffEntry>& OutDiff);

		/** Container-aware file diff (virtual-disk model): pak/ucas compare by
		 *  their INNER sub-file hashes, never the container whole-file hash.
		 *  - .pak: Same when every remote entry exists locally with a matching
		 *    per-entry SHA1 (layout / order agnostic).
		 *  - .ucas: Same when the local chunk-id multiset equals the remote
		 *    (order agnostic).
		 *  - .utoc: mirrors its sibling .ucas (container metadata, no truth of
		 *    its own).
		 *  - plain files (and containers without inner metadata): whole-file
		 *    SHA1 (existing semantics).
		 *  Pure function (no filesystem I/O); the ucas data-level verification
		 *  (verify mode) is a separate orchestrator step. */
		UNREALGAMEPATCHERCORE_API static void DiffFilesVirtualDisk(
			const TMap<FString, FIndexFileEntry>& Local,
			const TMap<FString, FIndexFileEntry>& Remote,
			TArray<FDiffEntry>& OutDiff);

		/** Diff chunk-level entries. */
		UNREALGAMEPATCHERCORE_API static void DiffChunks(
			const TMap<FString, FIndexChunkEntry>& Local,
			const TMap<FString, FIndexChunkEntry>& Remote,
			TArray<FDiffEntry>& OutDiff);

		/** Convenience: both levels at once. */
		UNREALGAMEPATCHERCORE_API static FDiffResult Diff(
			const FIndexManifest& LocalTruth,
			const FIndexManifest& RemoteManifest);
	};
}
