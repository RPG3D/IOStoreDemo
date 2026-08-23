// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Formats/Index/FIndexManifest.h"

namespace UnrealGamePatcher
{
	/** Builds the "local truth" — the chunk/file list of the local install —
	 *  so it can be diffed against the remote extension manifest (design D3 /
	 *  task 3.3).
	 *
	 *  Pak path: every *.pak in a directory is parsed with FPakReader and each
	 *  entry contributes its relative path -> built-in per-file SHA1 + size
	 *  (read from the payload header; no full-file decompression needed).
	 *  Non-pak files in the same directory (plain dll/exe/... and IoStore
	 *  containers) contribute a whole-file SHA1, matching the official
	 *  manifest's FileHash semantics (task 3.1, verified) — the IoStore
	 *  chunk-level path lands with FIoStoreReader in task 7.1. */
	struct FLocalTruthExtractor
	{
		/** Scans PakDir for *.pak, parses each with FPakReader and fills
		 *  OutTruth.Files (relative posix path -> SHA1 + size). Returns the
		 *  number of pak files successfully parsed; on any pak failing to open
		 *  or index it returns -1 with a logged error (a damaged pak must fail
		 *  the extraction, not be silently skipped). */
		UNREALGAMEPATCHERCORE_API static int32 ExtractPaks(const FString& PakDir, FIndexManifest& OutTruth);

		/** Whole-file SHA1 for a single file (used for plain files and IoStore
		 *  containers). Returns false when the file cannot be read. */
		UNREALGAMEPATCHERCORE_API static bool HashWholeFile(const FString& Path, FString& OutSha1Hex, int64& OutSize);

		/** Scans PakDir: ExtractPaks for *.pak plus HashWholeFile for every
		 *  other regular file (top-level only). Returns false when any target
		 *  failed to read. */
		UNREALGAMEPATCHERCORE_API static bool ExtractAll(const FString& PakDir, FIndexManifest& OutTruth);

		/** Truth of an install directory for the update flow (task 5.1;
		 *  RECURSIVE scan of Dir - the manifest's ids are the staged game's
		 *  nested relative paths, so the truth must produce the same keys).
		 *
		 *  Virtual-disk model: pak and ucas containers contribute their INNER
		 *  sub-file hashes (per-entry SHA1 / per-chunk ids), never the container
		 *  whole-file hash - in-place appends make the physical layout differ
		 *  per machine. .utoc entries carry no truth of their own (their status
		 *  derives from the sibling ucas). Plain files contribute a whole-file
		 *  SHA1.
		 *
		 *  bVerify=false is the default TRUST mode: TrustedIndex (the last
		 *  committed version's manifest) stands in for the filesystem without
		 *  re-reading - a plain file whose size matches is trusted, and
		 *  container metadata is carried over verbatim. bVerify=true re-reads
		 *  the local containers (cheap: pak index / utoc only, never the
		 *  container data) and hashes every plain file.
		 *
		 *  Patcher metadata is never part of the truth: the root sidecar
		 *  UnrealGamePatcher.state.json and the .ugp/ state dir are skipped. */
		UNREALGAMEPATCHERCORE_API static bool ExtractInstall(
			const FString& Dir,
			FIndexManifest& OutTruth,
			const FIndexManifest* TrustedIndex = nullptr,
			bool bVerify = true);

		/** Names-only install walk (trust mode): lists the installed relative
		 *  paths (same skip rules as ExtractInstall - no sidecar files) and
		 *  sizes of the non-container files. No container is opened and no
		 *  hash is computed - the trust-mode diff is manifest-to-manifest and
		 *  uses this only to heal on-disk drift (missing files re-fetch,
		 *  strays get deleted). Returns false when the dir is missing. */
		UNREALGAMEPATCHERCORE_API static bool ExtractInstallNames(
			const FString& Dir,
			TSet<FString>& OutNames,
			TMap<FString, int64>& OutPlainSizes);
	};
}
