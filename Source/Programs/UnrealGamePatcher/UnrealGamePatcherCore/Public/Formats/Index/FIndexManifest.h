// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UnrealGamePatcher
{
	/** One byte range of a file assembled from a chunk (self-designed format):
	 *  the chunk's uncompressed data bytes [Offset, Offset+Size) go to the
	 *  file at this part's running offset. Chunk is the chunk's content
	 *  address (SHA1 hex - also the key in the manifest's Chunks map). */
	struct FIndexFileChunkRef
	{
		FString Chunk;
		int64 Offset = 0;
		int64 Size = 0;
	};

	/** One pak entry's metadata inside a pak file, enabling entry-level
	 *  in-place patching (task: compare pak entries, download only the changed
	 *  entries' bytes, overwrite in place / append - no whole-file rewrite).
	 *  OffsetInFile/StoredSize are the entry's stored payload position within
	 *  the pak FILE; SHA1 is the pak's built-in per-entry hash. */
	struct FIndexPakEntry
	{
		FString Path;           // mount-relative path inside the pak
		int64 OffsetInFile = 0; // stored payload offset within the pak file
		int64 StoredSize = 0;
		int64 UncompressedSize = 0;
		FString SHA1;           // 40 hex, built-in per-entry hash
	};

	/** One IoStore chunk inside a ucas container, enabling the chunk diff +
	 *  in-place apply for ucas (design D6 / task: compare ucas chunks, append
	 *  only the changed chunks, atomically rewrite the utoc).
	 *  Id semantics: for UGP-crafted containers the chunk id is content-
	 *  addressed (first 12 bytes of the FIoHash); REAL engine containers
	 *  derive chunk ids from the PACKAGE name (FPackageId::FromName) - a
	 *  content edit inside an existing package keeps its id. The content
	 *  signal is therefore MetaHash (the source utoc's meta FIoHash of the
	 *  chunk's raw data): the diff compares (Id, MetaHash) pairs. MetaHash is
	 *  empty in manifests written before the field existed (the diff falls
	 *  back to id-only for those). */
	struct FIndexIoStoreChunk
	{
		FString Id;        // 24 hex
		int64 RawOffset = 0; // raw (uncompressed) offset within the ucas
		int64 RawSize = 0;
		FString MetaHash;  // 40 hex, FIoHash of the raw data (content hash)
		/** Physical byte span of the chunk's blocks inside the .ucas
		 *  (recorded by the ingest from the utoc block table). Lets the
		 *  download phase fetch exactly the windows covering changed
		 *  chunks without parsing the remote utoc. FileOffset < 0 in
		 *  manifests predating the field (conservative full fetch). */
		int64 FileOffset = -1;
		int64 FileSize = 0;
	};

	/** One file's entry in the extension manifest. Semantics follow the
	 *  official BuildPatchServices manifest (task 3.1, verified): SHA1 is the
	 *  whole-file hash (== local built-in per-file SHA1 for uncompressed pak
	 *  entries), Size the exact byte length, Parts the contiguous chunk
	 *  assembly plan (in file order; absent => whole-file download).
	 *  PakEntries (present for pak files) carries per-entry metadata for the
	 *  in-place patch path; IoStoreChunks (present for ucas containers) the
	 *  per-chunk id/offset metadata for the chunk-id diff + in-place apply. */
	struct FIndexFileEntry
	{
		FString SHA1; // 40 lowercase hex (whole-file SHA1)
		int64 Size = 0;
		TArray<FIndexFileChunkRef> Parts;
		TArray<FIndexPakEntry> PakEntries;
		TArray<FIndexIoStoreChunk> IoStoreChunks;
	};

	/** One content chunk (self-designed format). The map key is the chunk's
	 *  content address = SHA1 (40 hex) of the UNCOMPRESSED data. SHA1 is that
	 *  same value (stored for convenience), Size is the chunk FILE size on
	 *  disk (compressed), File the CDN-relative path ("chunks/<sha1>.chunk").
	 *  There are no rolling hashes, no data groups, no version branching. */
	struct FIndexChunkEntry
	{
		FString SHA1;  // 40 hex, uncompressed-data SHA1 (== map key)
		int64 Size = 0;
		FString File;  // CDN-relative chunk file path, e.g. "chunks/<sha1>.chunk"
	};

	/** The CDN-side extension manifest: `UnrealGamePatcher.index.json`
	 *  (design D3 / task 3.2). Carries the per-file + per-chunk lists,
	 *  versioning (contentVersion/baseVersion) and a self-check XXH3-64 of
	 *  the canonicalized JSON so any tamper or corruption is detected on load.
	 *  NOTE: there is deliberately no top-level "update strategy" enum - the
	 *  apply strategy is decided PER FILE by the client (extension + presence
	 *  of pak/IoStore metadata), a real install is always a mix. */
	struct FIndexManifest
	{
		static constexpr int32 FormatVersion = 2;
		static constexpr const TCHAR* FormatName = TEXT("UnrealGamePatcher.index");

		FString AppId;
		FString ContentVersion; // the version this manifest describes
		FString BaseVersion;    // chain base (empty = full/base content)
		/** Optional per-version flag (set when publishing): forces the client to
		 *  run this update in VERIFY mode (full hash pass + container data
		 *  checks) instead of the default trust mode. Used for large or
		 *  long-absent updates that should also self-check the install. */
		bool bVerifyOnUpdate = false;

		// Relative paths (posix style) -> entry. Order is irrelevant on disk;
		// the serializer sorts keys so the self-check hash is deterministic.
		TMap<FString, FIndexFileEntry> Files;
		// Chunk id -> entry. Pak path: 1 MiB window content chunks (official
		// chunk guid). IoStore path: chunk id (hex).
		TMap<FString, FIndexChunkEntry> Chunks;
	};

	/** Loads and saves FIndexManifest as canonical JSON. */
	struct FIndexManifestFile
	{
		/** Serializes Manifest and writes it to Path, computing and embedding
		 *  the self-check XXH3-64 over the canonicalized body (everything but
		 *  the selfCheck field). Write is atomic (temp + rename). Returns false
		 *  (with a logged reason) on failure. */
		UNREALGAMEPATCHERCORE_API static bool Save(const FString& Path, const FIndexManifest& Manifest);

		/** Loads Path, validates format/version/required fields, strips and
		 *  re-verifies the selfCheck hash. Returns false (with a logged
		 *  reason) when the file is missing, malformed, wrong schema, or
		 *  fails the self-check — i.e. tampered or damaged manifests are
		 *  detected and rejected, never silently accepted. */
		UNREALGAMEPATCHERCORE_API static bool Load(const FString& Path, FIndexManifest& OutManifest);

		/** Same validation over an in-memory JSON string (used when the
		 *  manifest was fetched over HTTP rather than read from disk). */
		UNREALGAMEPATCHERCORE_API static bool LoadFromString(const FString& Json, FIndexManifest& OutManifest);
	};

	/** Latest-version announcement: <cdn>/index/latest.json - the fixed file
	 *  the client polls to learn the current version (a cheap stand-in for a
	 *  dedicated update server; a real one can later replace this behind the
	 *  same resolve step). Published AFTER the version's index and chunks are
	 *  uploaded, so the announcement is effectively atomic. Advisory only -
	 *  the real per-version index still self-checks on load. */
	struct FLatestVersion
	{
		FString Version;     // required: version to update to
		FString MinVersion;  // optional: older clients should update first
		bool bForce = false; // optional: force this update
		FString Channel;     // optional: channel tag (public/staging/...)
	};

	struct FLatestVersionFile
	{
		static constexpr int32 FormatVersion = 1;
		static constexpr const TCHAR* FormatName = TEXT("UnrealGamePatcher.latest");

		/** <cdn>/index/latest.json */
		UNREALGAMEPATCHERCORE_API static FString GetPath(const FString& CdnDir);

		/** Atomic write (temp + rename), deterministic field order. */
		UNREALGAMEPATCHERCORE_API static bool Save(const FString& Path, const FLatestVersion& Latest);

		/** Strict read: missing/invalid JSON or a bad 'version' => false. */
		UNREALGAMEPATCHERCORE_API static bool Load(const FString& Path, FLatestVersion& OutLatest);

		/** Same validation over an in-memory JSON string (fetched over HTTP). */
		UNREALGAMEPATCHERCORE_API static bool LoadFromString(const FString& Json, FLatestVersion& OutLatest);
	};
}
