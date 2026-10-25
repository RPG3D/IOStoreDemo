// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealGamePatcherLog.h"
#include <atomic>

namespace UnrealGamePatcher
{
	/** Plain-value copy of the global I/O counters (test assertions + the
	 *  end-of-run report). */
	struct FIOStatsSnapshot
	{
		int64 DiskReadBytes = 0;
		int64 DiskReadOps = 0;
		int64 DiskWriteBytes = 0;
		int64 DiskWriteOps = 0;
		int64 NetDownBytes = 0;
		/** Reserved: no upload path exists in this codebase (the CDN is fed
		 *  by external tooling); the category stays for symmetry. */
		int64 NetUpBytes = 0;
		int64 CompressInBytes = 0;   // raw bytes fed to a compressor
		int64 CompressOutBytes = 0; // stored bytes the compressor produced
		int64 CompressOps = 0;
		int64 DecompressInBytes = 0;
		int64 DecompressOutBytes = 0;
		int64 DecompressOps = 0;
		/** .chunk file load ATTEMPTS (a missing/corrupt chunk counts too) -
		 *  the counter that exposes a patcher reading windows the prefetch
		 *  deliberately skipped. */
		int64 ChunkLoads = 0;
		/** Bytes pushed through content hashes (SHA1/FIoHash) - the verify
		 *  modes' dominant CPU cost, made visible. */
		int64 HashedBytes = 0;

		bool operator==(const FIOStatsSnapshot&) const = default;
	};

	/**
	 * Process-wide I/O accounting (2026-10-09 review): every disk read/write,
	 * network byte, compression pass, chunk-store load and hash pass is
	 * counted at its natural choke point. The purpose is not precision
	 * bookkeeping - it is making SILENT DEGRADATION visible: a CoW patch that
	 * quietly falls back to a whole-container rebuild shows up as a
	 * full-container download + rewrite in one log line (and in test
	 * assertions on FUpdateStats.IOStats) instead of hiding behind a green
	 * result. That exact failure mode (review H1) survived the whole suite
	 * because the stats only measured what was WRITTEN, never what was read.
	 *
	 * All counters are std::atomic - call from any thread. The update flow
	 * resets at entry (per-run accounting); the in-place patchers print a
	 * per-file DELTA so direct patcher use stays observable too.
	 */
	struct FIOStats
	{
		/** The single instance. */
		UNREALGAMEPATCHERCORE_API static FIOStats& Get();

		UNREALGAMEPATCHERCORE_API void Reset();
		UNREALGAMEPATCHERCORE_API FIOStatsSnapshot Snapshot() const;

		UNREALGAMEPATCHERCORE_API void AddDiskRead(int64 Bytes);
		UNREALGAMEPATCHERCORE_API void AddDiskWrite(int64 Bytes);
		UNREALGAMEPATCHERCORE_API void AddNetDownload(int64 Bytes);
		UNREALGAMEPATCHERCORE_API void AddNetUpload(int64 Bytes);
		/** One compression pass: RawBytes in, StoredBytes out. */
		UNREALGAMEPATCHERCORE_API void AddCompress(int64 RawBytes, int64 StoredBytes);
		/** One decompression pass: StoredBytes in, RawBytes out. */
		UNREALGAMEPATCHERCORE_API void AddDecompress(int64 StoredBytes, int64 RawBytes);
		UNREALGAMEPATCHERCORE_API void AddChunkLoad();
		UNREALGAMEPATCHERCORE_API void AddHashed(int64 Bytes);

		/** One greppable summary line: "<tag>: disk read 1.2 MB (3 ops) | disk
		 *  write 912 B (1 op) | net down ... | chunk loads ...". */
		UNREALGAMEPATCHERCORE_API FString FormatSummary(const TCHAR* Tag) const;

		/** Short delta between two snapshots: "disk read X / write Y / net down
		 *  Z / decompress W (n ops) / chunk loads N" - used by the per-file
		 *  in-place patch reports. */
		UNREALGAMEPATCHERCORE_API static FString FormatDelta(const FIOStatsSnapshot& Start, const FIOStatsSnapshot& End);

		/** Human-readable byte count ("912 B", "1.4 MB", "3.06 GB"). */
		UNREALGAMEPATCHERCORE_API static FString FormatBytes(int64 Bytes);

	private:
		std::atomic<int64> DiskReadBytes{ 0 };
		std::atomic<int64> DiskReadOps{ 0 };
		std::atomic<int64> DiskWriteBytes{ 0 };
		std::atomic<int64> DiskWriteOps{ 0 };
		std::atomic<int64> NetDownBytes{ 0 };
		std::atomic<int64> NetUpBytes{ 0 };
		std::atomic<int64> CompressInBytes{ 0 };
		std::atomic<int64> CompressOutBytes{ 0 };
		std::atomic<int64> CompressOps{ 0 };
		std::atomic<int64> DecompressInBytes{ 0 };
		std::atomic<int64> DecompressOutBytes{ 0 };
		std::atomic<int64> DecompressOps{ 0 };
		std::atomic<int64> ChunkLoads{ 0 };
		std::atomic<int64> HashedBytes{ 0 };
	};

	/** RAII per-file patch accounting (fixplan S1): the ctor takes the first
	 *  snapshot, the dtor logs the delta on EVERY exit path - the in-place
	 *  patchers have 20+ early-return failure branches that used to drop it
	 *  silently. An all-zero delta (nothing happened) stays silent. */
	struct FScopedIOPatchDelta
	{
		explicit FScopedIOPatchDelta(FString InTag)
			: Tag(MoveTemp(InTag))
			, Start(FIOStats::Get().Snapshot())
		{
		}
		~FScopedIOPatchDelta()
		{
			const FIOStatsSnapshot End = FIOStats::Get().Snapshot();
			if (End == Start)
			{
				return;
			}
			UE_LOG(LogUnrealGamePatcher, Display, TEXT("%s %s"),
				*Tag, *FIOStats::FormatDelta(Start, End));
		}
		FScopedIOPatchDelta(const FScopedIOPatchDelta&) = delete;
		FScopedIOPatchDelta& operator=(const FScopedIOPatchDelta&) = delete;

	private:
		FString Tag;
		FIOStatsSnapshot Start;
	};
}
