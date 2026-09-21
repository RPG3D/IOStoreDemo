// Copyright Epic Games, Inc. All Rights Reserved.

#include "Stats/FIOStats.h"
#include "UnrealGamePatcherLog.h"

namespace UnrealGamePatcher
{
	FIOStats& FIOStats::Get()
	{
		static FIOStats Instance;
		return Instance;
	}

	void FIOStats::Reset()
	{
		auto Zero = [](std::atomic<int64>& C) { C.store(0, std::memory_order_relaxed); };
		Zero(DiskReadBytes); Zero(DiskReadOps);
		Zero(DiskWriteBytes); Zero(DiskWriteOps);
		Zero(NetDownBytes); Zero(NetUpBytes);
		Zero(CompressInBytes); Zero(CompressOutBytes); Zero(CompressOps);
		Zero(DecompressInBytes); Zero(DecompressOutBytes); Zero(DecompressOps);
		Zero(ChunkLoads);
		Zero(HashedBytes);
	}

	FIOStatsSnapshot FIOStats::Snapshot() const
	{
		auto Load = [](const std::atomic<int64>& C) { return C.load(std::memory_order_relaxed); };
		FIOStatsSnapshot S;
		S.DiskReadBytes = Load(DiskReadBytes);
		S.DiskReadOps = Load(DiskReadOps);
		S.DiskWriteBytes = Load(DiskWriteBytes);
		S.DiskWriteOps = Load(DiskWriteOps);
		S.NetDownBytes = Load(NetDownBytes);
		S.NetUpBytes = Load(NetUpBytes);
		S.CompressInBytes = Load(CompressInBytes);
		S.CompressOutBytes = Load(CompressOutBytes);
		S.CompressOps = Load(CompressOps);
		S.DecompressInBytes = Load(DecompressInBytes);
		S.DecompressOutBytes = Load(DecompressOutBytes);
		S.DecompressOps = Load(DecompressOps);
		S.ChunkLoads = Load(ChunkLoads);
		S.HashedBytes = Load(HashedBytes);
		return S;
	}

	void FIOStats::AddDiskRead(int64 Bytes)
	{
		DiskReadBytes.fetch_add(Bytes, std::memory_order_relaxed);
		DiskReadOps.fetch_add(1, std::memory_order_relaxed);
	}

	void FIOStats::AddDiskWrite(int64 Bytes)
	{
		DiskWriteBytes.fetch_add(Bytes, std::memory_order_relaxed);
		DiskWriteOps.fetch_add(1, std::memory_order_relaxed);
	}

	void FIOStats::AddNetDownload(int64 Bytes)
	{
		NetDownBytes.fetch_add(Bytes, std::memory_order_relaxed);
	}

	void FIOStats::AddNetUpload(int64 Bytes)
	{
		NetUpBytes.fetch_add(Bytes, std::memory_order_relaxed);
	}

	void FIOStats::AddCompress(int64 RawBytes, int64 StoredBytes)
	{
		CompressInBytes.fetch_add(RawBytes, std::memory_order_relaxed);
		CompressOutBytes.fetch_add(StoredBytes, std::memory_order_relaxed);
		CompressOps.fetch_add(1, std::memory_order_relaxed);
	}

	void FIOStats::AddDecompress(int64 StoredBytes, int64 RawBytes)
	{
		DecompressInBytes.fetch_add(StoredBytes, std::memory_order_relaxed);
		DecompressOutBytes.fetch_add(RawBytes, std::memory_order_relaxed);
		DecompressOps.fetch_add(1, std::memory_order_relaxed);
	}

	void FIOStats::AddChunkLoad()
	{
		ChunkLoads.fetch_add(1, std::memory_order_relaxed);
	}

	void FIOStats::AddHashed(int64 Bytes)
	{
		HashedBytes.fetch_add(Bytes, std::memory_order_relaxed);
	}

	FString FIOStats::FormatBytes(int64 Bytes)
	{
		constexpr double KB = 1024.0;
		constexpr double MB = KB * 1024.0;
		constexpr double GB = MB * 1024.0;
		if (Bytes >= (int64)GB)
		{
			return FString::Printf(TEXT("%.2f GB"), Bytes / GB);
		}
		if (Bytes >= (int64)MB)
		{
			return FString::Printf(TEXT("%.2f MB"), Bytes / MB);
		}
		if (Bytes >= (int64)KB)
		{
			return FString::Printf(TEXT("%.1f KB"), Bytes / KB);
		}
		return FString::Printf(TEXT("%lld B"), Bytes);
	}

	FString FIOStats::FormatSummary(const TCHAR* Tag) const
	{
		const FIOStatsSnapshot S = Snapshot();
		return FString::Printf(
			TEXT("%s: disk read %s (%lld ops) | disk write %s (%lld ops) | net down %s | net up %s | compress %s -> %s (%lld ops) | decompress %s -> %s (%lld ops) | chunk loads %lld | hashed %s"),
			Tag,
			*FormatBytes(S.DiskReadBytes), S.DiskReadOps,
			*FormatBytes(S.DiskWriteBytes), S.DiskWriteOps,
			*FormatBytes(S.NetDownBytes),
			*FormatBytes(S.NetUpBytes),
			*FormatBytes(S.CompressInBytes), *FormatBytes(S.CompressOutBytes), S.CompressOps,
			*FormatBytes(S.DecompressInBytes), *FormatBytes(S.DecompressOutBytes), S.DecompressOps,
			S.ChunkLoads,
			*FormatBytes(S.HashedBytes));
	}

	FString FIOStats::FormatDelta(const FIOStatsSnapshot& Start, const FIOStatsSnapshot& End)
	{
		return FString::Printf(
			TEXT("disk read %s / write %s / net down %s / decompress %s -> %s (%lld ops) / chunk loads %lld"),
			*FormatBytes(End.DiskReadBytes - Start.DiskReadBytes),
			*FormatBytes(End.DiskWriteBytes - Start.DiskWriteBytes),
			*FormatBytes(End.NetDownBytes - Start.NetDownBytes),
			*FormatBytes(End.DecompressInBytes - Start.DecompressInBytes),
			*FormatBytes(End.DecompressOutBytes - Start.DecompressOutBytes),
			End.DecompressOps - Start.DecompressOps,
			End.ChunkLoads - Start.ChunkLoads);
	}
}
