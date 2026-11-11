// Copyright Epic Games, Inc. All Rights Reserved.

#include "Formats/Pak/FPakRecovery.h"
#include "Stats/FIOStats.h"
#include "UnrealGamePatcherLog.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/SecureHash.h"

namespace UnrealGamePatcher
{
	namespace
	{
		// Backward scan reads the tail in 64 KiB windows and searches each
		// window for the footer magic from its end backwards.
		constexpr int64 ScanChunkBytes = 64 * 1024;

		// Cheap sanity ceiling for a candidate index claim (real directory
		// indexes are MBs at most). Exists to reject absurd geometry before
		// allocating; the SHA1 check below is the real filter.
		constexpr int64 MaxSaneIndexBytes = 512ll * 1024 * 1024;

		// FSHA1::Update takes an int32 count; hash large indexes in chunks.
		constexpr int64 Sha1StepBytes = TNumericLimits<int32>::Max();

		/** True when the index blob the footer points at hashes to the footer's
		 *  IndexHash. Mirrors FPakReader::ReadIndex: frozen indexes may carry
		 *  the pre-freeze hash and are not re-verified. */
		bool IndexHashMatches(IFileHandle* Handle, const FPakInfo& Info)
		{
			if (Info.bIndexIsFrozen) return true;
			if (Info.IndexSize > MaxSaneIndexBytes) return false;

			TArray64<uint8> IndexData;
			IndexData.SetNumUninitialized(Info.IndexSize);
			if (!Handle->Seek(Info.IndexOffset)) return false;
			FIOStats::Get().AddDiskRead(Info.IndexSize);
			FIOStats::Get().AddHashed(Info.IndexSize);
			for (int64 Done = 0; Done < Info.IndexSize; )
			{
				const int64 Step = FMath::Min<int64>(Info.IndexSize - Done, Sha1StepBytes);
				if (!Handle->Read(IndexData.GetData() + Done, Step)) return false;
				Done += Step;
			}

			FSHA1 Sha;
			for (int64 Done = 0; Done < Info.IndexSize; )
			{
				const int32 Step = (int32)FMath::Min<int64>(Info.IndexSize - Done, Sha1StepBytes);
				Sha.Update(IndexData.GetData() + Done, Step);
				Done += Step;
			}
			Sha.Final();
			uint8 Computed[20];
			Sha.GetHash(Computed);
			return FMemory::Memcmp(Computed, Info.IndexHash, 20) == 0;
		}

		/**
		 * Parses + fully validates a footer ending exactly at FooterEnd.
		 * Returns false for any malformation: bad magic/version, index
		 * geometry outside [0, footer start), or index hash mismatch. A
		 * payload byte sequence can reproduce the magic (and even the
		 * version field); only the index SHA1 separates a real footer from
		 * a coincidence, so geometry checks are just cheap pre-filters.
		 */
		bool ValidateFooterEndingAt(IFileHandle* Handle, int64 FileSize, int64 FooterEnd, FPakInfo& OutInfo)
		{
			const int64 MaxFooter = GetPakFooterFixedSize(PakFileVersionMax);
			const int64 WindowStart = FMath::Max<int64>(0, FooterEnd - MaxFooter);
			const int64 WindowBytes = FooterEnd - WindowStart;
			if (WindowBytes <= 0) return false;

			TArray64<uint8> Buf;
			Buf.SetNumUninitialized(WindowBytes);
			if (!Handle->Seek(WindowStart) || !Handle->Read(Buf.GetData(), WindowBytes)) return false;
			FIOStats::Get().AddDiskRead(WindowBytes);
			const uint8* Base = Buf.GetData();
			for (int32 V = PakFileVersionMax; V >= 1; --V)
			{
				const int64 Fixed = GetPakFooterFixedSize(V);
				if (Fixed > WindowBytes) continue;
				const uint8* P = Base + WindowBytes - Fixed;
				const uint8* End = Base + WindowBytes;

				if (V >= 7) P += 16; // EncryptionKeyGuid
				uint8 Enc = 0;
				uint32 Magic = 0, Ver = 0;
				P += 1; // bEncryptedIndex
				if (P + 4 > End) break;
				FMemory::Memcpy(&Magic, P, 4); P += 4;
				FMemory::Memcpy(&Ver, P, 4); P += 4;
				if (Magic != PakFileMagic || Ver != (uint32)V) continue;

				FPakInfo Info;
				Info.Magic = Magic;
				Info.Version = Ver;
				P = Base + WindowBytes - Fixed;
				if (V >= 7) P += 16;
				FMemory::Memcpy(&Enc, P, 1); P += 1;
				Info.bEncryptedIndex = Enc != 0;
				P += 4; // magic
				P += 4; // version
				FMemory::Memcpy(&Info.IndexOffset, P, 8); P += 8;
				FMemory::Memcpy(&Info.IndexSize, P, 8); P += 8;
				FMemory::Memcpy(Info.IndexHash, P, 20); P += 20;
				if (V == 9)
				{
					uint8 Frozen = 0;
					FMemory::Memcpy(&Frozen, P, 1);
					Info.bIndexIsFrozen = Frozen != 0;
				}

				// The index immediately precedes the footer in every layout we
				// write and in engine layouts; anything else is a coincidence.
				// Subtraction-ordered comparisons: no int64 overflow possible.
				const int64 IndexEndLimit = FooterEnd - Fixed;
				if (Info.IndexOffset < 0 || Info.IndexSize <= 0) continue;
				if (Info.IndexOffset > IndexEndLimit) continue;
				if (Info.IndexSize > IndexEndLimit - Info.IndexOffset) continue;
				if (IndexEndLimit > FileSize) continue;

				if (!IndexHashMatches(Handle, Info)) continue;

				OutInfo = MoveTemp(Info);
				return true;
			}
			return false;
		}
	}

		FPakRecovery::FOutcome FPakRecovery::ScanAndRepair(const FString& PakFilePath)
		{
			FOutcome Out;
			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
			// bAppend=true is load-bearing: the engine's OpenWrite maps
			// bAppend=false to CREATE_ALWAYS, which would truncate the pak to
			// zero before the scan even looks at it. Append-open (OPEN_ALWAYS)
			// plus read access is the non-destructive read/write handle the
			// scan and the Truncate call need.
			IFileHandle* Handle = PlatformFile.OpenWrite(*PakFilePath, true, true);
			IFileHandle* OwnedReadHandle = nullptr;
			const bool bCanRepair = (Handle != nullptr);
			if (!Handle)
			{
				// Fall back to a pure read handle: still able to classify the
				// pak, just unable to truncate (reported as Unrecoverable so
				// the caller rebuilds the whole file).
				OwnedReadHandle = PlatformFile.OpenRead(*PakFilePath);
				if (!OwnedReadHandle)
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakRecovery: cannot open '%s'"), *PakFilePath);
					return Out;
				}
				Handle = OwnedReadHandle;
			}

		const int64 FileSize = Handle->Size();
		if (FileSize < GetPakFooterFixedSize(1))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakRecovery: '%s' too small (%lld bytes)"), *PakFilePath, FileSize);
			delete Handle;
			return Out;
		}

		// Fast path: fully valid footer at the physical end — the common case
		// for committed paks, engine- or patcher-written.
		if (ValidateFooterEndingAt(Handle, FileSize, FileSize, Out.Info))
		{
			Out.Result = EResult::Clean;
			delete Handle;
			return Out;
		}

		// Torn tail: walk backwards in windows, and for every magic occurrence
		// (searched end-to-start so the winner is the commit nearest EOF) try
		// each version's footer geometry. First fully valid candidate wins.
		int64 Pos = FileSize;
		while (Pos > 0)
		{
			const int64 ChunkStart = FMath::Max<int64>(0, Pos - ScanChunkBytes);
			const int64 ChunkBytes = Pos - ChunkStart;
			TArray64<uint8> Chunk;
			Chunk.SetNumUninitialized(ChunkBytes);
			if (Handle->Seek(ChunkStart) && Handle->Read(Chunk.GetData(), ChunkBytes))
			{
				FIOStats::Get().AddDiskRead(ChunkBytes);
				for (int64 I = ChunkBytes - 4; I >= 0; --I)
				{
					uint32 Magic = 0;
					FMemory::Memcpy(&Magic, Chunk.GetData() + I, 4);
					if (Magic != PakFileMagic) continue;

					const int64 MagicOffset = ChunkStart + I;
					for (int32 V = PakFileVersionMax; V >= 1; --V)
					{
						// Footer layout: [EncryptionKeyGuid?][bEncryptedIndex][Magic][Version][...]
						// so a footer of version V ends (Fixed - magic-relative-offset) past the magic.
						const int64 Fixed = GetPakFooterFixedSize(V);
						const int64 MagicRel = (V >= 7 ? 16 : 0) + 1;
						const int64 FooterEnd = MagicOffset + (Fixed - MagicRel);
						if (FooterEnd <= 0 || FooterEnd > FileSize) continue;

						FPakInfo Info;
						if (!ValidateFooterEndingAt(Handle, FileSize, FooterEnd, Info)) continue;

						if (!bCanRepair || !Handle->Truncate(FooterEnd))
						{
							UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakRecovery: valid footer at %lld in '%s' but truncation failed"), FooterEnd, *PakFilePath);
							delete Handle;
							return Out;
						}
						UE_LOG(LogUnrealGamePatcher, Warning, TEXT("FPakRecovery: '%s' had a torn tail; truncated %lld -> %lld bytes (pak version %d, index at %lld size %lld)"),
							*PakFilePath, FileSize, FooterEnd, Info.Version, Info.IndexOffset, Info.IndexSize);
						Out.Result = EResult::Truncated;
						Out.TruncatedToBytes = FooterEnd;
						Out.Info = MoveTemp(Info);
						delete Handle;
						return Out;
					}
				}
			}
			Pos = ChunkStart;
		}

		UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPakRecovery: no valid footer found in '%s' (%lld bytes scanned)"), *PakFilePath, FileSize);
		delete Handle;
		return Out;
	}
}
