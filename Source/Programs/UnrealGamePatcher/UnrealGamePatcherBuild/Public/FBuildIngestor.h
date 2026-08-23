// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Formats/Index/FIndexManifest.h"

namespace UnrealGamePatcher
{
	/** Result summary of an ingest run. */
	struct FIngestResult
	{
		int32 NumFiles = 0;
		int32 NumChunks = 0;
		int64 NumChunkBytesWritten = 0;
		FString IndexPath; // full path of the written extension manifest
	};

	/**
	 * Build-side ingest (self-designed format): chunks a STAGED game build
	 * directly - no BuildPatchServices involvement.
	 *
	 *   <OutCdnDir>/index/<ContentVersion>/UnrealGamePatcher.index.json
	 *                              (extension manifest, self-checking)
	 *   <OutCdnDir>/chunks/<h0><h1>/<h2><h3>/<rest>.chunk
	 *                              our 1 MiB windows (Oodle/raw), GLOBAL and
	 *                              content-addressed by the window SHA1 so a
	 *                              chunk shared across versions is stored once
	 *
	 * Each staged file is split into 1 MiB windows (FChunkDataLoader::Write,
	 * content-addressed by the SHA1 of the uncompressed window). Containers are
	 * parsed DIRECTLY from the staged files (no reassembly): paks contribute
	 * per-entry hashes (entries[]), ucas containers their IoStore chunk
	 * metadata (ioChunks[]). Output is fully deterministic.
	 */
	struct FBuildIngestor
	{
		/** Ingests a staged build directory.
		 *  StagedDir:       e.g. Saved/StagedBuilds/Windows (the game build)
		 *  AppId:           app name for the index
		 *  ContentVersion:  version string (index subdirectory)
		 *  OutCdnDir:        CDN root; version subdirectory is appended. */
		UNREALGAMEPATCHERBUILD_API static bool Ingest(
			const FString& StagedDir,
			const FString& AppId,
			const FString& ContentVersion,
			const FString& OutCdnDir,
			FIngestResult& OutResult);
	};
}
