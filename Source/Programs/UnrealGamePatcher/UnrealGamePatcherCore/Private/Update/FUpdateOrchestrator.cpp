// Copyright Epic Games, Inc. All Rights Reserved.

#include "Update/FUpdateOrchestrator.h"
#include "Update/FInPlacePakPatcher.h"
#include "Update/FInPlaceIoStorePatcher.h"
#include "Formats/IoStore/FIoStoreReader.h"
#include "Hash/FPatcherHash.h"
#include "IO/IoHash.h"
#include "Formats/Diff/FLocalTruthExtractor.h"
#include "Formats/Chunk/FChunkDataLoader.h"
#include "Formats/Pak/FPakRecovery.h"
#include "Download/FDownloadManager.h"
#include "State/FPatcherState.h"
#include "Stats/FIOStats.h"
#include "UnrealGamePatcherPaths.h"
#include "UnrealGamePatcherLog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Timespan.h"
#include "SecureHash.h"

namespace UnrealGamePatcher
{
	int32 FUpdateOrchestrator::TestFailAfterWrites = 0;
	int32 FUpdateOrchestrator::TestCrashAfterWrites = 0;

	namespace
	{
		// Run-report funnel (fixplan S2): one place binding the I/O snapshot
		// to the stats copy and the greppable summary line - four call
		// sites, one contract.
		void EmitIORunReport(const TCHAR* Label, const FString& StatTag, FUpdateStats& OutStats)
		{
			OutStats.IOStats = FIOStats::Get().Snapshot();
			UE_LOG(LogUnrealGamePatcher, Display, TEXT("FUpdateOrchestrator: %s I/O %s"),
				Label, *FIOStats::Get().FormatSummary(*StatTag));
		}

		/** Local content index (the installed version's own extension
		 *  manifest) lives in a subdirectory so the flat install scan never
		 *  sees it. */
		FString GetLocalStatePath(const FString& InstallDir)
		{
			return FPaths::Combine(InstallDir, TEXT(".ugp"), TEXT("index.json"));
		}

		FString GetBackupDir(const FString& InstallDir)
		{
			return FPaths::Combine(InstallDir, TEXT(".ugp"), TEXT("backup"));
		}

		/** True when the CDN root is an HTTP(S) base: every resource
		 *  (latest.json, the version index, chunk bytes) is fetched over HTTP. */
		bool IsHttpCdn(const FString& CdnRoot)
		{
			return CdnRoot.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase) ||
				CdnRoot.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase);
		}

		/** The local directory behind a file://... (or bare) CDN root.
		 *  file:///D:/x and file://D:/x both become D:/x. */
		FString LocalCdnRoot(const FString& CdnRoot)
		{
			FString Root = CdnRoot;
			if (Root.StartsWith(TEXT("file://"), ESearchCase::IgnoreCase))
			{
				Root = Root.RightChop(7);
				if (Root.StartsWith(TEXT("/")))
				{
					Root = Root.RightChop(1);
				}
			}
			return Root;
		}

		/** Fetches a whole CDN resource (RelPath, posix) as UTF-8 JSON text via
		 *  the downloader - used for latest.json and the version index in HTTP
		 *  mode. Returns false on any transport failure or when the resource
		 *  exceeds Options.MaxManifestBytes (a "100GB json" must never be
		 *  buffered; the transport's own MaxResponseBytes already bounds the
		 *  worst case, this enforces the metadata semantics on top). */
		bool FetchCdnResource(const FString& CdnRoot, const FString& RelPath,
			const FUpdateOptions& Options, FString& OutJson)
		{
			if (!Options.Downloader)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: HTTP CDN needs a downloader (pass http(s):// -cdn with the HTTP backend wired up)"));
				return false;
			}
			const FString Url = CdnRoot + TEXT("/") + RelPath;
			TArray64<uint8> Bytes;
			if (!Options.Downloader->DownloadRange(Url, 0, -1, Bytes) || Bytes.Num() == 0)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: failed fetching '%s'"), *Url);
				return false;
			}
			FIOStats::Get().AddNetDownload(Bytes.Num());
			if (Bytes.Num() > Options.MaxManifestBytes)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: '%s' is %lld bytes, over the %lld-byte metadata cap (hostile or misconfigured CDN?)"),
					*Url, Bytes.Num(), Options.MaxManifestBytes);
				return false;
			}
			const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), (int32)Bytes.Num());
			OutJson = FString(Conv.Length(), Conv.Get());
			return true;
		}

		/// Default metadata cap (FUpdateOptions.MaxManifestBytes mirrors this).
		constexpr int64 DefaultMaxManifestBytes = 64 << 20;

		/** Local-mode counterpart of the metadata cap: a CDN DIRECTORY holding
		 *  an oversized index/latest.json must be refused before it is read
		 *  into memory. Returns true when within the cap. */
		bool CheckLocalMetaSize(const FString& Path, int64 MaxBytes)
		{
			const int64 Size = IFileManager::Get().FileSize(*Path);
			if (Size > MaxBytes)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: '%s' is %lld bytes, over the %lld-byte metadata cap"),
					*Path, Size, MaxBytes);
				return false;
			}
			return true;
		}

		bool ReadFileRange(const FString& Path, int64 Offset, int64 Size, TArray64<uint8>& OutBytes)
		{
			IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
			IFileHandle* Handle = PF.OpenRead(*Path);
			if (!Handle)
			{
				return false;
			}
			// A negative or absurd size (tampered manifest, integer overflow in
			// the caller) must fail here, not allocate.
			const int64 FileSize = Handle->Size();
			if (Size < 0 || Offset < 0 || Offset + Size > FileSize)
			{
				delete Handle;
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: invalid read range [%lld, %lld) on '%s' (file %lld bytes)"),
					Offset, Offset + Size, *Path, FileSize);
				return false;
			}
			OutBytes.SetNumUninitialized(Size);
			const bool bOk = Size == 0 ||
				(Handle->Seek(Offset) && Handle->Read(OutBytes.GetData(), Size));
			delete Handle;
			if (bOk)
			{
				FIOStats::Get().AddDiskRead(Size);
			}
			return bOk;
		}

		FString Sha1HexOf(const TArray64<uint8>& Data)
		{
			// FSHA1::Update takes an int32 count, so a file larger than 2 GiB
			// (e.g. a debug .pdb) must be fed to the hasher in int32-sized
			// chunks - a single (int32)Data.Num() cast would overflow and
			// corrupt/crash the hash.
			FSHA1 Hasher;
			static constexpr int64 HashChunk = 1 << 30; // 1 GiB per Update call
			int64 Offset = 0;
			while (Offset < Data.Num())
			{
				const int64 N = FMath::Min<int64>(HashChunk, Data.Num() - Offset);
				Hasher.Update(Data.GetData() + Offset, (int32)N);
				Offset += N;
			}
			uint8 Hash[20] = { 0 };
			Hasher.Final();
			Hasher.GetHash(Hash);
			FString Out;
			Out.Reserve(40);
			for (int32 I = 0; I < 20; ++I)
			{
				Out += FString::Printf(TEXT("%02x"), Hash[I]);
			}
			return Out;
		}

		/** Writes Bytes to Path via a temp file + rename (atomic-ish: a crash
		 *  mid-write leaves either the old or the new file, never a partial
		 *  one). Classifies the failure so the caller can report/recover by
		 *  category: open failed (permission / share) vs write failed (disk
		 *  full / IO) vs swap failed (target locked / read-only). */
		EPatcherError WriteAtomic(const FString& Path, const TArray64<uint8>& Bytes)
		{
			IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
			const FString TempPath = Path + TEXT(".ugp-tmp");
			IFileHandle* Handle = PF.OpenWrite(*TempPath);
			if (!Handle)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: cannot open '%s' for writing"), *TempPath);
				return EPatcherError::WriteNoPermission;
			}
			const bool bOk = Bytes.Num() == 0 || Handle->Write(Bytes.GetData(), Bytes.Num());
			delete Handle;
			if (!bOk)
			{
				PF.DeleteFile(*TempPath);
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: short write to '%s' (disk full?)"), *TempPath);
				return EPatcherError::WriteNoSpace;
			}
			FIOStats::Get().AddDiskWrite(Bytes.Num());
			if (PF.FileExists(*Path))
			{
				PF.SetReadOnly(*Path, false);
				PF.DeleteFile(*Path);
			}
			if (!PF.MoveFile(*Path, *TempPath))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: failed swapping '%s' (locked?)"), *Path);
				PF.DeleteFile(*TempPath);
				return EPatcherError::WriteLocked;
			}
			return EPatcherError::Ok;
		}

		/** Where a chunk's bytes live in the installed files, according to the
		 *  local state manifest: the chunk slice [ChunkOffset, +PartSize) is
		 *  stored in FileId at FileOffset. A chunk can back several files at
		 *  different slices, so all occurrences are recorded and the first one
		 *  that matches the needed slice is used. */
		struct FLocalChunkSource
		{
			FString FileId;
			int64 FileOffset = 0;
			int64 ChunkOffset = 0;
			int64 PartSize = 0;
		};

		/**
		 * Rebuilds FileId's content from the remote manifest's chunk plan and
		 * returns the whole-file-hash-verified bytes. When bReuseAllowed, parts
		 * whose chunk is unchanged AND identically sliced locally are copied
		 * from the installed files; otherwise (and on any whole-file hash
		 * mismatch - silently damaged local data) every part is fetched from
		 * the CDN. The hash check is the sole arbiter of success.
		 *
		 * Fetched chunks are cached in ChunkCache (verified once per guid);
		 * reuse stats are committed to OutStats only on success. Returns false
		 * (with a logged error) when the file has no chunk plan or the source
		 * data fails verification - the caller must not write anything.
		 */
		bool RebuildFileFromChunks(
			const FIndexManifest& Remote,
			const FString& ChunkRoot,
			const FString& InstallDir,
			const FString& FileId,
			const TSet<FString>& SameChunks,
			const TMap<FString, TArray<FLocalChunkSource>>& LocalSources,
			const TSet<FString>& ReplacedFiles,
			bool bReuseAllowed,
			bool bTrackFetchStats,
			bool bChunkRootIsCache,
			TMap<FString, TArray64<uint8>>& ChunkCache,
			FUpdateStats& OutStats,
			TArray64<uint8>& OutAssembled)
		{
			IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

			const FIndexFileEntry* Target = Remote.Files.Find(FileId);
			// A 0-byte file legitimately has zero parts (the ingestor emits
			// none for an empty staged file) - only a non-empty file without a
			// plan is broken.
			if (!Target || (Target->Size > 0 && Target->Parts.Num() == 0))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: '%s' has no chunk plan (re-ingest the version)"), *FileId);
				return false;
			}

			auto GetChunkData = [&](const FString& Guid, const TArray64<uint8>*& OutBytes) -> bool
			{
				if (const TArray64<uint8>* Cached = ChunkCache.Find(Guid))
				{
					OutBytes = Cached;
					return true;
				}
				const FIndexChunkEntry* Entry = Remote.Chunks.Find(Guid);
				if (!Entry || Entry->File.IsEmpty())
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: chunk '%s' missing from remote manifest"), *Guid);
					return false;
				}
				TArray64<uint8> Data;
				if (!FChunkDataLoader::Load(FPaths::Combine(ChunkRoot, Entry->File), *Entry, Data))
				{
					// The chunk file failed its embedded SHA1 - it is bad data.
					// When the root is the network download CACHE, remove the
					// file so the next run re-downloads it instead of failing
					// forever on the corrupt cached copy. Never touch a local
					// CDN dir (bChunkRootIsCache == false).
					if (bChunkRootIsCache)
					{
						PF.DeleteFile(*(FPaths::Combine(ChunkRoot, Entry->File)));
						UE_LOG(LogUnrealGamePatcher, Warning, TEXT("FUpdateOrchestrator: removed corrupt cached chunk '%s' - rerun will re-download"), *Guid);
					}
					return false;
				}
				if (bTrackFetchStats)
				{
					OutStats.NumChunksFetched += 1;
					OutStats.BytesFetched += Entry->Size;
				}
				OutBytes = &ChunkCache.Add(Guid, MoveTemp(Data));
				return true;
			};

			// Attempt 1 (only with bReuseAllowed) reuses local bytes for
			// unchanged, identically sliced chunks; attempt 2 fetches
			// everything. Per-file reuse stats commit only on success so a
			// damaged-install retry does not over-count reuse.
			TArray64<uint8> Assembled;
			int64 FileBytesReused = 0;
			TArray<FString> FileReusedChunks;

			// In HTTP mode the manifest is unauthenticated network input (the
			// self-check only detects corruption, not a malicious rewrite):
			// parts whose total exceeds the file size would overflow the
			// assembly buffer below - validate before allocating.
			int64 PartsTotal = 0;
			for (const FIndexFileChunkRef& P : Target->Parts)
			{
				PartsTotal += P.Size;
			}
			if (PartsTotal != Target->Size || Target->Size < 0)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: '%s' parts sum %lld != file size %lld (tampered manifest?)"),
					*FileId, PartsTotal, Target->Size);
				return false;
			}

			for (int32 Attempt = 0; Attempt < 2; ++Attempt)
			{
				const bool bAllowReuse = bReuseAllowed && Attempt == 0;
				Assembled.SetNumUninitialized(Target->Size);
				FileBytesReused = 0;
				FileReusedChunks.Reset();

				bool bFailed = false;
				int64 FileOffset = 0;
				for (const FIndexFileChunkRef& Part : Target->Parts)
				{
					if (bAllowReuse && SameChunks.Contains(Part.Chunk))
					{
						const FLocalChunkSource* Src = nullptr;
						if (const TArray<FLocalChunkSource>* All = LocalSources.Find(Part.Chunk))
						{
							for (const FLocalChunkSource& Candidate : *All)
							{
								if (Candidate.ChunkOffset == Part.Offset &&
									Candidate.PartSize == Part.Size &&
									!ReplacedFiles.Contains(Candidate.FileId) &&
									PF.FileExists(*(FPaths::Combine(InstallDir, Candidate.FileId))))
								{
									Src = &Candidate;
									break;
								}
							}
						}
						if (Src)
						{
							TArray64<uint8> LocalRange;
							// A failed local read (drifted/shortened source file -
							// the size is only checked for plain files in the
							// names-only trust walk) DEGRADES to a CDN fetch; it
							// must never fail the whole rebuild.
							if (ReadFileRange(FPaths::Combine(InstallDir, Src->FileId), Src->FileOffset, Part.Size, LocalRange))
							{
								FMemory::Memcpy(Assembled.GetData() + FileOffset, LocalRange.GetData(), Part.Size);
								FileBytesReused += Part.Size;
								FileReusedChunks.AddUnique(Part.Chunk);
								FileOffset += Part.Size;
								continue;
							}
							UE_LOG(LogUnrealGamePatcher, Warning,
								TEXT("FUpdateOrchestrator: local reuse source for chunk '%s' unreadable - fetching from the CDN instead"),
								*Part.Chunk);
						}
					}

					const TArray64<uint8>* ChunkData = nullptr;
					if (!GetChunkData(Part.Chunk, ChunkData))
					{
						bFailed = true;
						break;
					}
					if (Part.Offset + Part.Size > ChunkData->Num())
					{
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: part range [%lld,%lld) outside chunk '%s' (%lld bytes)"),
							Part.Offset, Part.Offset + Part.Size, *Part.Chunk, ChunkData->Num());
						bFailed = true;
						break;
					}
					FMemory::Memcpy(Assembled.GetData() + FileOffset, ChunkData->GetData() + Part.Offset, Part.Size);
					FileOffset += Part.Size;
				}

				if (bFailed)
				{
					return false;
				}

				const FString Actual = Sha1HexOf(Assembled);
				if (Actual.Equals(Target->SHA1, ESearchCase::IgnoreCase))
				{
					OutStats.BytesReused += FileBytesReused;
					for (const FString& Guid : FileReusedChunks)
					{
						OutStats.ReusedChunkGuids.Add(Guid);
					}
					OutAssembled = MoveTemp(Assembled);
					return true;
				}
				if (Attempt == 0)
				{
					UE_LOG(LogUnrealGamePatcher, Warning,
						TEXT("FUpdateOrchestrator: '%s' assembled from local bytes has hash %s (manifest %s) - local data damaged, retrying with full fetch"),
						*FileId, *Actual, *Target->SHA1);
				}
				else
				{
					UE_LOG(LogUnrealGamePatcher, Error,
						TEXT("FUpdateOrchestrator: '%s' rebuilt from source chunks has hash %s (manifest %s) - source data damaged"),
						*FileId, *Actual, *Target->SHA1);
					return false;
				}
			}
			return false; // unreachable
		}
	}

	bool FUpdateOrchestrator::Update(
		const FString& CdnDir,
		const FString& Version,
		const FString& InstallDir,
		bool bApply,
		FUpdateStats& OutStats,
		bool bRepairOnly)
	{
		FUpdateOptions Options;
		Options.bRepairOnly = bRepairOnly;
		return UpdateWithOptions(CdnDir, Version, InstallDir, bApply, Options, OutStats);
	}

	bool FUpdateOrchestrator::UpdateWithOptions(
		const FString& CdnDir,
		const FString& Version,
		const FString& InstallDir,
		bool bApply,
		const FUpdateOptions& Options,
		FUpdateStats& OutStats)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

		// Single-writer guard (fixplan L-M5): mutating runs take the install
		// lock so a parallel CLI/GUI run on the same directory cannot
		// interleave transactions, sidecar writes or CoW commits. Check-only
		// runs stay lock-free read-only. The lock is held for the whole run.
		Paths::FInstallLock InstallLock;
		if (bApply && !InstallLock.Acquire(InstallDir))
		{
			return false;
		}

		// Fresh installs are the PRIMARY scenario (a patcher bootstrapping a
		// new directory / new player machine): create the install dir instead
		// of demanding it pre-exist. Idempotent when it is already there.
		// Check-only runs stay read-only (documented contract) - they require
		// an existing install.
		if (bApply && !PF.CreateDirectoryTree(*InstallDir))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: cannot create install directory '%s'"), *InstallDir);
			return false;
		}

		// Per-run I/O accounting (review 2026-10-09): everything this run
		// reads/writes/downloads/compresses accumulates in the global stats
		// until the end-of-run report below. Reset FIRST so the numbers
		// describe exactly this run.
		FIOStats::Get().Reset();

		// The CDN root is ONE base, either an HTTP(S) URL or a local directory
		// (file://... or a bare path). Every resource - latest.json, the
		// version index, chunk bytes - resolves against this same base.
		const bool bCdnIsHttp = IsHttpCdn(CdnDir);
		const bool bNetwork = bCdnIsHttp || (Options.Downloader != nullptr && !Options.BaseUrl.IsEmpty());
		if (bCdnIsHttp && Options.BaseUrl.IsEmpty())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: http(s):// CDN root needs the downloader wired up (Options.Downloader + BaseUrl = the CDN root)"));
			return false;
		}
		const FString CdnRootLocal = LocalCdnRoot(CdnDir);

		// Unified event broadcast (FUpdateEvent.h): stage changes, progress
		// ticks and errors. Invoked from this (update) thread; handlers must be
		// cheap and thread-safe. LastError is the terminal failure code, used
		// by the Failed event and (via the caller) as the process exit code.
		EPatcherError LastError = EPatcherError::Ok;
		auto Emit = [&Options, &LastError](EUpdatePhase Phase, EPatcherError Error, bool bIsError,
			int32 Current, int32 Total, const TCHAR* Message)
		{
			if (bIsError)
			{
				LastError = Error;
			}
			if (Options.Events)
			{
				FUpdateEvent E;
				E.Phase = Phase; E.Error = Error; E.bIsError = bIsError;
				E.Current = Current; E.Total = Total; E.Message = Message;
				Options.Events(E);
			}
		};

		// --- 1. Remote manifest (metadata lives under <base>/index/<version>/).
		const FString IndexRel = FString::Printf(TEXT("index/%s/UnrealGamePatcher.index.json"), *Version);
		FIndexManifest Remote;
		if (bCdnIsHttp)
		{
			// HTTP mode: the manifest is fetched like any other resource.
			FString Json;
			if (!FetchCdnResource(CdnDir, IndexRel, Options, Json))
			{
				Emit(EUpdatePhase::Failed, EPatcherError::ManifestMissing, true, 0, 0, *IndexRel);
				return false;
			}
			if (!FIndexManifestFile::LoadFromString(Json, Remote))
			{
				Emit(EUpdatePhase::Failed, EPatcherError::ManifestTampered, true, 0, 0, *IndexRel);
				return false;
			}
		}
		else
		{
			const FString IndexPath = FPaths::Combine(CdnRootLocal, IndexRel);
			if (!PF.FileExists(*IndexPath))
			{
				Emit(EUpdatePhase::Failed, EPatcherError::ManifestMissing, true, 0, 0, *IndexPath);
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: remote manifest not found: '%s'"), *IndexPath);
				return false;
			}
			if (!CheckLocalMetaSize(IndexPath, Options.MaxManifestBytes))
			{
				Emit(EUpdatePhase::Failed, EPatcherError::ManifestTampered, true, 0, 0, *IndexPath);
				return false;
			}
			if (!FIndexManifestFile::Load(IndexPath, Remote))
			{
				Emit(EUpdatePhase::Failed, EPatcherError::ManifestTampered, true, 0, 0, *IndexPath);
				return false;
			}
		}
		Emit(EUpdatePhase::Manifest, EPatcherError::Ok, false, 0, 0, TEXT("manifest loaded"));

		// Where chunk FILES are read from: the CDN root in local mode (chunks
		// are global/content-addressed at <cdn>/chunks/...), the verified
		// download cache in network mode.
		FString ChunkRoot = CdnRootLocal;
		if (bNetwork)
		{
			ChunkRoot = Options.ChunkCacheDir.IsEmpty()
				? FPaths::Combine(InstallDir, TEXT(".ugp"), TEXT("chunkcache"))
				: Options.ChunkCacheDir;
		}

		// Transaction machinery (design D5, tasks 5.2/5.3). Loaded early so a
		// pending (interrupted) transaction can be resolved BEFORE the local
		// truth is read - the crashed run may have left half-applied files on
		// disk, and the diff must compare against the pre-transaction state.
		const FString StatePath = Paths::GetStatePath(InstallDir);
		const FString BackupDir = GetBackupDir(InstallDir);
		FPatcherState State;
		FPatcherStateFile::Load(StatePath, State); // absent/old file -> fresh state

		// Restores every touched target to its pre-transaction bytes, deletes
		// the backups and marks the transaction rolled-back.
		auto Rollback = [&](FPatcherState& Txn) -> void
		{
			for (int32 I = Txn.Transaction.Targets.Num() - 1; I >= 0; --I)
			{
				const FTransactionTarget& T = Txn.Transaction.Targets[I];
				if (T.File.EndsWith(TEXT(".pak"), ESearchCase::IgnoreCase))
				{
					// CoW v2 (ROADMAP §5.2b): pak containers are never backed
					// up and never rolled back. A crash mid-CoW leaves a torn
					// tail that FPakRecovery truncates back to the last
					// committed footer - the container is a complete old or
					// new version either way. Restoring or deleting it here
					// would only destroy that guarantee (and an added pak has
					// no backup to restore from by design).
					continue;
				}
				if (T.Op == EApplyOp::Delete)
				{
					// Removed file: bring it back from its backup.
					if (!T.BackupPath.IsEmpty() && PF.FileExists(*T.BackupPath))
					{
						PF.SetReadOnly(*T.File, false);
						PF.CopyFile(*T.File, *T.BackupPath);
					}
					continue;
				}
				// ReplaceFile, decided by the recorded pre-existence (NOT by
				// OriginalHash emptiness: containers legitimately carry no
				// whole-file hash, and the old heuristic deleted a pre-existing
				// pak/ucas whose backup had not been taken yet).
				if (!T.bPreExisted)
				{
					// Was an Added file: remove the new file.
					if (PF.FileExists(*T.File))
					{
						PF.SetReadOnly(*T.File, false);
						PF.DeleteFile(*T.File);
					}
				}
				else if (!T.BackupPath.IsEmpty() && PF.FileExists(*T.BackupPath))
				{
					// Changed file: restore the original bytes.
					PF.SetReadOnly(*T.File, false);
					PF.CopyFile(*T.File, *T.BackupPath);
				}
				// Pre-existing file with no backup yet (crash before the backup
				// was taken): the original bytes are untouched, nothing to do.
			}
			Txn.Transaction.Status = ETransactionStatus::RolledBack;
			// Persist the RolledBack state BEFORE deleting the backups: the
			// reverse order left a window where a failed save (or a crash)
			// meant Pending with no backups - unrecoverable as designed.
			if (!FPatcherStateFile::Save(StatePath, Txn))
			{
				UE_LOG(LogUnrealGamePatcher, Warning, TEXT("FUpdateOrchestrator: failed persisting rolled-back state '%s' (backups kept)"), *StatePath);
				// Keep the backups: they are the only remaining recovery data.
				return;
			}
			PF.DeleteDirectoryRecursively(*BackupDir);
		};

		// --- 1b. Crash recovery (task 5.3), apply runs only: a Pending
		// transaction means a previous run died mid-apply (its backups + log
		// are intact, exactly as the kill left them). Roll it back to the
		// pre-transaction state first; the diff below then sees the restored
		// install and the fresh apply converges to the target version.
		// Check-only runs stay read-only and leave the pending transaction for
		// the next apply run.
		if (bApply && State.Transaction.Status == ETransactionStatus::Pending)
		{
			UE_LOG(LogUnrealGamePatcher, Warning,
				TEXT("FUpdateOrchestrator: recovering interrupted transaction (version %s, %d targets)"),
				*State.Transaction.ContentVersion, State.Transaction.Targets.Num());
			Rollback(State);
		}

		// --- 1c. CoW v2 container recovery (ROADMAP §5.2): truncate any torn
		// pak tail back to the last committed footer BEFORE anything opens a
		// container. Crash-safe by construction: a pak interrupted mid-CoW is
		// a complete old version once the tail is gone. An unrecoverable pak
		// (no valid footer at all) is deleted so the diff below re-fetches it
		// whole - it is unmountable anyway (the engine fails closed).
		if (bApply)
		{
			TArray<FString> PakPaths;
			PF.FindFilesRecursively(PakPaths, *InstallDir, TEXT("pak"));
			for (const FString& PakPath : PakPaths)
			{
				const FPakRecovery::FOutcome Outcome = FPakRecovery::ScanAndRepair(PakPath);
				if (Outcome.Result == FPakRecovery::EResult::Truncated)
				{
					UE_LOG(LogUnrealGamePatcher, Warning, TEXT("FUpdateOrchestrator: recovered torn pak tail '%s' (truncated to %lld bytes)"),
						*PakPath, Outcome.TruncatedToBytes);
				}
				else if (Outcome.Result == FPakRecovery::EResult::Unrecoverable)
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: pak '%s' has no valid footer - deleting for the whole-file rebuild"), *PakPath);
					PF.SetReadOnly(*PakPath, false);
					PF.DeleteFile(*PakPath);
				}
			}
		}

		// --- 2. Local truth. Trust mode (the default) lets the committed local
		// index stand in for the filesystem without re-reading; verify mode
		// re-extracts everything. Containers contribute their INNER sub-file
		// hashes (virtual-disk model), never the container whole-file hash.
		FIndexManifest LocalIndex;
		const FString LocalStatePath = GetLocalStatePath(InstallDir);
		const bool bHaveLocalIndex = PF.FileExists(*LocalStatePath) &&
			FIndexManifestFile::Load(LocalStatePath, LocalIndex);

		// Verify mode: forced by the options / repair, by the remote manifest's
		// per-version verifyOnUpdate flag, by check-only runs (a report must be
		// trustworthy), or by the time-based trigger (the last verified run is
		// older than Options.VerifyEveryDays; an unverified install verifies
		// once to establish the baseline).
		bool bVerify = Options.bForceVerify || Options.bRepairOnly || !bApply ||
			Remote.bVerifyOnUpdate;
		if (!bVerify && Options.VerifyEveryDays > 0)
		{
			FDateTime LastVerify;
			if (State.LastVerifiedAt.IsEmpty() ||
				!FDateTime::ParseIso8601(*State.LastVerifiedAt, LastVerify) ||
				FDateTime::UtcNow() - LastVerify > FTimespan::FromDays(Options.VerifyEveryDays))
			{
				bVerify = true;
			}
		}

		FIndexManifest LocalTruth;
		if (!bVerify && bHaveLocalIndex)
		{
			// Trust mode: the sidecar IS the local truth - the diff is
			// manifest-to-manifest, no container is opened and no hash is
			// computed. A names-only disk walk heals on-disk drift: sidecar
			// files missing on disk drop out (diff re-fetches them), stray
			// disk files show up (diff deletes them), and a plain file whose
			// size drifted drops out too (re-fetch). Container sizes are
			// never compared - in-place appends change them legitimately.
			// Anything subtler (same-size corruption, container damage) is
			// the verify mode's job (repair / check / verifyOnUpdate /
			// VerifyEveryDays).
			LocalTruth = LocalIndex;
			TSet<FString> DiskNames;
			TMap<FString, int64> PlainSizes;
			if (!FLocalTruthExtractor::ExtractInstallNames(InstallDir, DiskNames, PlainSizes))
			{
				Emit(EUpdatePhase::Failed, EPatcherError::Internal, true, 0, 0, TEXT("install directory not readable"));
				return false;
			}
			for (auto It = LocalTruth.Files.CreateIterator(); It; ++It)
			{
				const bool bIsContainer = It->Key.EndsWith(TEXT(".pak"), ESearchCase::IgnoreCase) ||
					It->Key.EndsWith(TEXT(".ucas"), ESearchCase::IgnoreCase) ||
					It->Key.EndsWith(TEXT(".utoc"), ESearchCase::IgnoreCase);
				const int64* DiskSize = PlainSizes.Find(It->Key);
				if (!DiskNames.Contains(It->Key) || (!bIsContainer && DiskSize && *DiskSize != It->Value.Size))
				{
					It.RemoveCurrent();
				}
			}
			for (const FString& DiskName : DiskNames)
			{
				if (!LocalTruth.Files.Contains(DiskName) && !Remote.Files.Contains(DiskName))
				{
					FIndexFileEntry Stray;
					Stray.Size = PlainSizes.FindRef(DiskName);
					LocalTruth.Files.Add(DiskName, Stray);
				}
			}
		}
		else if (!FLocalTruthExtractor::ExtractInstall(InstallDir, LocalTruth,
			bHaveLocalIndex ? &LocalIndex : nullptr, bVerify))
		{
			Emit(EUpdatePhase::Failed, EPatcherError::Internal, true, 0, 0, TEXT("local truth extraction failed"));
			return false;
		}

		// --- 3. Diff: files by the container-aware classifier (inner hashes
		// for pak/ucas, whole-file for plain files), chunks against the local
		// state (no state => every remote chunk is Added => full fetch).
		FDiffResult Diff;
		FDiffAnalyzer::DiffFilesVirtualDisk(LocalTruth.Files, Remote.Files, Diff.Files);
		FDiffAnalyzer::DiffChunks(LocalIndex.Chunks, Remote.Chunks, Diff.Chunks);

		// Verify mode only: a ucas classified Same by its chunk-id set must
		// also carry intact data - re-hash each local chunk against its utoc
		// meta hash; damaged data flips it to Changed so the whole-file
		// rebuild repairs it. Trust mode accepts a Same id-set as-is (the
		// periodic verify / manual repair catches silent damage).
		if (bVerify)
		{
			// A Same-classified ucas whose local data fails the per-chunk
			// content check is flipped to Changed (the whole-file rebuild
			// repairs it) - and its sibling utoc mirrors the flip (the rebuilt
			// pair must be consistent).
			const auto FlipUcasAndUtoc = [&Diff](FDiffEntry& E)
			{
				E.Kind = EChangeKind::Changed;
				const FString UtocId = E.Id.LeftChop(5) + TEXT(".utoc");
				for (FDiffEntry& U : Diff.Files)
				{
					if (U.Id == UtocId)
					{
						U.Kind = EChangeKind::Changed;
						break;
					}
				}
			};

			for (FDiffEntry& E : Diff.Files)
			{
				if (E.Kind != EChangeKind::Same || !E.Id.EndsWith(TEXT(".ucas")))
				{
					continue;
				}
				const FIndexFileEntry* RemoteEntry = Remote.Files.Find(E.Id);
				if (!RemoteEntry || RemoteEntry->IoStoreChunks.Num() == 0)
				{
					continue;
				}
				const FString UtocPath = FPaths::Combine(InstallDir, E.Id.LeftChop(5) + TEXT(".utoc"));
				FIoStoreReader Io;
				if (!Io.Open(UtocPath, FPaths::Combine(InstallDir, E.Id)))
				{
					FlipUcasAndUtoc(E);
					continue;
				}
				bool bAllIntact = true;
				for (int32 I = 0; I < Io.GetChunkCount() && bAllIntact; ++I)
				{
					const FIoStoreChunk& C = Io.GetChunks()[I];
					TArray64<uint8> Data;
					if (!Io.ReadChunk(I, Data))
					{
						bAllIntact = false;
						break;
					}
					const FString HashHex = FPatcherHash::FIoHashToHex(FIoHash::HashBuffer(Data.GetData(), Data.Num()));
					// The utoc's meta hash is the content hash of the raw data
					// (scheme-independent).
					if (!C.Hash.Equals(HashHex, ESearchCase::IgnoreCase))
					{
						bAllIntact = false;
					}
				}
				if (!bAllIntact)
				{
					FlipUcasAndUtoc(E);
				}
			}
		}

		Emit(EUpdatePhase::Diff, EPatcherError::Ok, false, 0, 0, TEXT("diff analyzed"));

		TSet<FString> SameChunks; // guid -> identical content on both sides
		for (const FDiffEntry& E : Diff.Chunks)
		{
			if (E.Kind == EChangeKind::Same)
			{
				SameChunks.Add(E.Id);
			}
		}

		// Count file-level stats. For check-only runs the full diff is the
		// report; for apply runs the counts reflect the actual targets (which
		// repair mode filters down to Changed files only).
		if (!bApply)
		{
			OutStats.NumFilesSame = 0;
			OutStats.NumFilesChanged = 0;
			OutStats.NumFilesAdded = 0;
			OutStats.NumFilesRemoved = 0;
			for (const FDiffEntry& E : Diff.Files)
			{
				switch (E.Kind)
				{
				case EChangeKind::Same: ++OutStats.NumFilesSame; break;
				case EChangeKind::Changed: ++OutStats.NumFilesChanged; break;
				case EChangeKind::Added: ++OutStats.NumFilesAdded; break;
				case EChangeKind::Removed: ++OutStats.NumFilesRemoved; break;
				}
			}
			UE_LOG(LogUnrealGamePatcher, Display,
				TEXT("FUpdateOrchestrator: local -> %s: files %d same / %d changed / %d added / %d removed; %d/%d chunks unchanged%s"),
				*Version,
				OutStats.NumFilesSame, OutStats.NumFilesChanged, OutStats.NumFilesAdded, OutStats.NumFilesRemoved,
				SameChunks.Num(), Remote.Chunks.Num(),
				bHaveLocalIndex ? TEXT("") : TEXT(" (no local state: full fetch)"));
			EmitIORunReport(TEXT("check"), TEXT("check"), OutStats);
			return true;
		}

		// --- 4. Plan every change of THIS run up front and log it as pending
		// BEFORE any write, so a failure or crash can be rolled back from the
		// log + backups. Repair mode (task 5.4) touches only damaged files -
		// never Added/Removed.
		State.Transaction.Status = ETransactionStatus::Pending;
		State.Transaction.ContentVersion = Version;
		State.Transaction.Targets.Reset();

		TMap<FString, FString> TargetIdByPath; // absolute target path -> manifest id
		for (const FDiffEntry& E : Diff.Files)
		{
			if (E.Kind == EChangeKind::Same)
			{
				continue;
			}
			if (Options.bRepairOnly && E.Kind != EChangeKind::Changed)
			{
				continue;
			}
			FTransactionTarget T;
			T.File = FPaths::Combine(InstallDir, E.Id);
			TargetIdByPath.Add(T.File, E.Id);
			switch (E.Kind)
			{
			case EChangeKind::Removed:
				T.Op = EApplyOp::Delete;
				T.Size = E.LocalSize;
				break;
			case EChangeKind::Changed:
			case EChangeKind::Added:
				T.Op = EApplyOp::ReplaceFile;
				T.Size = E.RemoteSize;
				T.NewHash = E.RemoteSha1;
				break;
			}
			// Original content hash from the filesystem truth (empty for
			// containers - the virtual-disk model gives them no whole-file
			// hash). Pre-existence is recorded EXPLICITLY: it, not hash
			// emptiness, decides rollback behavior (restore vs remove).
			if (const FIndexFileEntry* Existing = LocalTruth.Files.Find(E.Id))
			{
				T.OriginalHash = Existing->SHA1;
				T.bPreExisted = true;
			}
			State.Transaction.Targets.Add(T);
		}

		// Stats reflect the targets this run will actually touch (repair mode
		// filtered out Added/Removed), with Same counted from the full diff.
		OutStats.NumFilesSame = 0;
		OutStats.NumFilesChanged = 0;
		OutStats.NumFilesAdded = 0;
		OutStats.NumFilesRemoved = 0;
		for (const FDiffEntry& E : Diff.Files)
		{
			if (E.Kind == EChangeKind::Same)
			{
				++OutStats.NumFilesSame;
			}
		}
		for (const FTransactionTarget& T : State.Transaction.Targets)
		{
			switch (T.Op)
			{
			case EApplyOp::Delete: ++OutStats.NumFilesRemoved; break;
			case EApplyOp::ReplaceFile:
				// Containers carry no whole-file truth (SHA1 empty), so the
				// Added/Changed split must come from file existence, not the hash.
				if (!PF.FileExists(*T.File)) { ++OutStats.NumFilesAdded; }
				else { ++OutStats.NumFilesChanged; }
				break;
			}
		}
		UE_LOG(LogUnrealGamePatcher, Display,
			TEXT("FUpdateOrchestrator: apply %s: files %d same / %d changed / %d added / %d removed; %d/%d chunks unchanged%s%s"),
			*Version,
			OutStats.NumFilesSame, OutStats.NumFilesChanged, OutStats.NumFilesAdded, OutStats.NumFilesRemoved,
			SameChunks.Num(), Remote.Chunks.Num(),
			bHaveLocalIndex ? TEXT("") : TEXT(" (no local state: full fetch)"),
			Options.bRepairOnly ? TEXT(" (repair only)") : TEXT(""));

		const int32 NumTargets = State.Transaction.Targets.Num();
		if (NumTargets == 0)
		{
			// Nothing to change (no-op): still refresh the local index below.
			UE_LOG(LogUnrealGamePatcher, Display, TEXT("FUpdateOrchestrator: no changes to apply"));
		}
		else if (!FPatcherStateFile::Save(StatePath, State))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: cannot log pending transaction '%s'"), *StatePath);
			Emit(EUpdatePhase::Failed, EPatcherError::Internal, true, 0, 0, TEXT("cannot record pending transaction"));
			return false;
		}

		if (!PF.CreateDirectoryTree(*BackupDir))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: cannot create backup dir '%s'"), *BackupDir);
			Emit(EUpdatePhase::Failed, EPatcherError::WriteNoPermission, true, 0, 0, TEXT("cannot create backup directory"));
			return false;
		}

		// --- 4c. DOWNLOAD phase (network mode, design D5/D9): fetch what the
		// apply phase will ACTUALLY read, per target - not the worst case:
		//  - plain files: parts whose chunk GUID is not in the local state
		//    (unchanged windows are sliced from the local containers by the
		//    rebuild's reuse pass);
		//  - pak targets: only the windows covering entries the entry-level
		//    patcher will write (local hash != remote, or absent locally);
		//  - ucas targets: the (small) remote utoc's parts + the windows
		//    covering changed chunks' physical spans (recorded at ingest).
		// Falls back to the target's FULL part set whenever local truth or
		// metadata is unavailable (legacy manifests), and the apply phase
		// lazily completes the set before any whole-file rebuild
		// (EnsureChunksCached) - so a small container delta no longer pulls
		// the whole container. A failure aborts cleanly, install untouched.
		if (bNetwork && NumTargets > 0)
		{
			TArray<FDownloadRequest> Requests;
			TSet<FString> Seen;
			const TMap<FString, FIndexChunkEntry>& KnownChunks = (LocalTruth.Chunks.Num() > 0)
				? LocalTruth.Chunks
				: LocalIndex.Chunks;

			// Registers one chunk download (dedup by guid). Returns false when
			// the manifest references a chunk it does not carry (tampered).
			auto AddChunkRequest = [&](const FString& Guid) -> bool
			{
				if (Seen.Contains(Guid))
				{
					return true;
				}
				Seen.Add(Guid);
				const FIndexChunkEntry* Entry = Remote.Chunks.Find(Guid);
				if (!Entry || Entry->File.IsEmpty())
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: chunk '%s' missing from remote manifest"), *Guid);
					Emit(EUpdatePhase::Failed, EPatcherError::ManifestTampered, true, 0, 0, TEXT("remote manifest references a missing chunk"));
					return false;
				}
				FDownloadRequest Req;
				Req.Id = Guid;
				// Chunk entities are global: <base>/chunks/... (no version).
				Req.Url = FString::Printf(TEXT("%s/%s"), *Options.BaseUrl, *Entry->File);
				Req.Size = Entry->Size;
				Req.DestFile = FPaths::Combine(ChunkRoot, Entry->File);
				Requests.Add(Req);
				return true;
			};

			// Adds every window of FileEntry overlapping [RangeStart, RangeEnd).
			auto AddWindowsForRange = [&](const FIndexFileEntry* FileEntry, int64 RangeStart, int64 RangeEnd) -> bool
			{
				int64 FilePos = 0;
				for (const FIndexFileChunkRef& Part : FileEntry->Parts)
				{
					const int64 PartEnd = FilePos + Part.Size;
					if (PartEnd > RangeStart && FilePos < RangeEnd && Part.Size > 0)
					{
						if (!AddChunkRequest(Part.Chunk))
						{
							return false;
						}
					}
					FilePos = PartEnd;
				}
				return true;
			};

			// Adds the file's full part set (conservative fallback).
			auto AddAllWindows = [&](const FIndexFileEntry* FileEntry) -> bool
			{
				for (const FIndexFileChunkRef& Part : FileEntry->Parts)
				{
					if (!AddChunkRequest(Part.Chunk))
					{
						return false;
					}
				}
				return true;
			};

			for (const FTransactionTarget& T : State.Transaction.Targets)
			{
				const FString* FileIdPtr = TargetIdByPath.Find(T.File);
				if (!FileIdPtr)
				{
					continue;
				}
				const FString FileId = *FileIdPtr;
				const FIndexFileEntry* FileEntry = Remote.Files.Find(FileId);
				if (!FileEntry)
				{
					continue; // Removed: nothing to fetch.
				}

				if (FileId.EndsWith(TEXT(".pak")) && FileEntry->PakEntries.Num() > 0)
				{
					// pak: windows covering entries the entry-level patcher
					// will write (Same entries are verified locally: zero fetch).
					const FIndexFileEntry* LocalEntry = LocalTruth.Files.Find(FileId);
					if (!LocalEntry || LocalEntry->PakEntries.Num() == 0)
					{
						// In-place impossible -> the rebuild needs everything.
						if (!AddAllWindows(FileEntry))
						{
							return false;
						}
						continue;
					}
					for (const FIndexPakEntry& RE : FileEntry->PakEntries)
					{
						const FIndexPakEntry* L = LocalEntry->PakEntries.FindByPredicate(
							[&RE](const FIndexPakEntry& PE) { return PE.Path.Equals(RE.Path, ESearchCase::IgnoreCase); });
						const bool bSame = L &&
							L->SHA1.Equals(RE.SHA1, ESearchCase::IgnoreCase) &&
							L->StoredSize == RE.StoredSize;
						if (!bSame &&
							!AddWindowsForRange(FileEntry, RE.OffsetInFile, RE.OffsetInFile + RE.StoredSize))
						{
							return false;
						}
					}
				}
				else if (FileId.EndsWith(TEXT(".ucas")) && FileEntry->IoStoreChunks.Num() > 0)
				{
					// ucas: the remote utoc's parts (block layout for the
					// per-chunk decode - small) + windows covering changed
					// chunks' physical spans.
					const FString UtocId = FileId.LeftChop(5) + TEXT(".utoc");
					if (const FIndexFileEntry* UtocEntry = Remote.Files.Find(UtocId))
					{
						if (!AddAllWindows(UtocEntry))
						{
							return false;
						}
					}
					const FIndexFileEntry* LocalEntry = LocalTruth.Files.Find(FileId);
					if (!LocalEntry || LocalEntry->IoStoreChunks.Num() == 0)
					{
						// In-place impossible -> the rebuild needs everything.
						if (!AddAllWindows(FileEntry))
						{
							return false;
						}
						continue;
					}
					// Match by ID (multiset semantics), never by position: a
					// cook may insert/remove/reorder chunks freely - position
					// matching would misread same-content chunks as changed.
					TMap<FString, const FIndexIoStoreChunk*> LocalById;
					for (const FIndexIoStoreChunk& LC : LocalEntry->IoStoreChunks)
					{
						LocalById.Add(LC.Id, &LC);
					}
					bool bFull = false;
					for (int32 I = 0; I < FileEntry->IoStoreChunks.Num(); ++I)
					{
						const FIndexIoStoreChunk& RC = FileEntry->IoStoreChunks[I];
						const FIndexIoStoreChunk* L = LocalById.FindRef(RC.Id);
						const bool bSame = L &&
							(RC.MetaHash.IsEmpty() || L->MetaHash.Equals(RC.MetaHash, ESearchCase::IgnoreCase));
						if (bSame)
						{
							continue;
						}
						if (RC.FileOffset < 0 || RC.FileSize <= 0)
						{
							// Legacy manifest without physical spans: the
							// covering windows cannot be targeted - full set.
							bFull = true;
							break;
						}
						if (!AddWindowsForRange(FileEntry, RC.FileOffset, RC.FileOffset + RC.FileSize))
						{
							return false;
						}
					}
					// Local chunks the target dropped: nothing to fetch (they
					// become dead bytes/holes during the in-place patch).
				}
				else
				{
					// Plain file (rebuilt): windows whose content is not in the
					// local state - unchanged windows are sliced locally by the
					// rebuild's reuse pass (SameChunks + LocalSources).
					for (const FIndexFileChunkRef& Part : FileEntry->Parts)
					{
						if (!KnownChunks.Contains(Part.Chunk) && !AddChunkRequest(Part.Chunk))
						{
							return false;
						}
					}
				}
			}

			Emit(EUpdatePhase::Download, EPatcherError::Ok, false, 0, Requests.Num(), *FString::Printf(TEXT("downloading %d chunks..."), Requests.Num()));

			if (Requests.Num() > 0)
			{
				FDownloadManager Manager(*Options.Downloader, Options.MaxConcurrent, Options.MaxRetries);
				FDownloadStats DownloadStats;
				TArray<FString> FailedIds;
				if (!Manager.DownloadAll(Requests, DownloadStats, FailedIds))
				{
					UE_LOG(LogUnrealGamePatcher, Error,
						TEXT("FUpdateOrchestrator: %d chunk downloads failed (e.g. '%s') - update aborted, install untouched"),
						FailedIds.Num(), FailedIds.Num() > 0 ? *FailedIds[0] : TEXT("?"));
					Emit(EUpdatePhase::Failed, EPatcherError::DownloadFailed, true,
						FailedIds.Num(), FailedIds.Num() > 0 ? FailedIds.Num() : 0,
						*FString::Printf(TEXT("download failed: %d chunks (e.g. %s)"), FailedIds.Num(), FailedIds.Num() > 0 ? *FailedIds[0] : TEXT("?")));
					return false;
				}
				Emit(EUpdatePhase::Download, EPatcherError::Ok, false, Requests.Num(), Requests.Num(), TEXT("download finished"));
				OutStats.NumChunksFetched = DownloadStats.Completed - DownloadStats.SkippedExisting;
				OutStats.BytesFetched = DownloadStats.BytesDownloaded;
				UE_LOG(LogUnrealGamePatcher, Display,
					TEXT("FUpdateOrchestrator: downloaded %d chunks (%lld bytes, %d from cache)"),
					OutStats.NumChunksFetched, OutStats.BytesFetched, DownloadStats.SkippedExisting);
			}
		}

		// Lazy completion (network mode): downloads every part of FileId not
		// already in the cache. The optimistic prefetch covers the in-place
		// path; this covers the whole-file rebuild fallback (which needs the
		// file's full part set) and the rebuild's re-fetch attempt.
		auto EnsureChunksCached = [&](const FString& FileId) -> bool
		{
			if (!bNetwork)
			{
				return true;
			}
			const FIndexFileEntry* FileEntry = Remote.Files.Find(FileId);
			if (!FileEntry)
			{
				return true;
			}
			TArray<FDownloadRequest> Requests;
			for (const FIndexFileChunkRef& Part : FileEntry->Parts)
			{
				const FIndexChunkEntry* Entry = Remote.Chunks.Find(Part.Chunk);
				if (!Entry || Entry->File.IsEmpty())
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: chunk '%s' missing from remote manifest"), *Part.Chunk);
					Emit(EUpdatePhase::Failed, EPatcherError::ManifestTampered, true, 0, 0, TEXT("remote manifest references a missing chunk"));
					return false;
				}
				const FString DestFile = FPaths::Combine(ChunkRoot, Entry->File);
				if (IFileManager::Get().FileSize(*DestFile) == Entry->Size)
				{
					continue; // already cached (size-checked; content verified on load)
				}
				FDownloadRequest Req;
				Req.Id = Part.Chunk;
				Req.Url = FString::Printf(TEXT("%s/%s"), *Options.BaseUrl, *Entry->File);
				Req.Size = Entry->Size;
				Req.DestFile = DestFile;
				Requests.Add(Req);
			}
			if (Requests.Num() == 0)
			{
				return true;
			}
			UE_LOG(LogUnrealGamePatcher, Display,
				TEXT("FUpdateOrchestrator: lazily fetching %d missing chunks of '%s' for the whole-file rebuild"),
				Requests.Num(), *FileId);
			Emit(EUpdatePhase::Download, EPatcherError::Ok, false, 0, Requests.Num(), TEXT("fetching remaining chunks for the rebuild..."));
			FDownloadManager Manager(*Options.Downloader, Options.MaxConcurrent, Options.MaxRetries);
			FDownloadStats DownloadStats;
			TArray<FString> FailedIds;
			if (!Manager.DownloadAll(Requests, DownloadStats, FailedIds))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: %d lazy chunk downloads failed for '%s'"),
					FailedIds.Num(), *FileId);
				Emit(EUpdatePhase::Failed, EPatcherError::DownloadFailed, true, FailedIds.Num(), Requests.Num(),
					*FString::Printf(TEXT("lazy chunk fetch failed: %d chunks"), FailedIds.Num()));
				return false;
			}
			OutStats.NumChunksFetched += DownloadStats.Completed - DownloadStats.SkippedExisting;
			OutStats.BytesFetched += DownloadStats.BytesDownloaded;
			return true;
		};

		bool bOk = true;

		// Reverse map: chunk guid -> every place its bytes sit in the
		// installed files per the local state manifest. Any occurrence is a
		// valid source (content is identical, SHA1-checked).
		TMap<FString, TArray<FLocalChunkSource>> LocalSources;
		if (bHaveLocalIndex)
		{
			for (const TPair<FString, FIndexFileEntry>& Pair : LocalIndex.Files)
			{
				int64 RunningOffset = 0;
				for (const FIndexFileChunkRef& Part : Pair.Value.Parts)
				{
					FLocalChunkSource Src;
					Src.FileId = Pair.Key;
					Src.FileOffset = RunningOffset;
					Src.ChunkOffset = Part.Offset;
					Src.PartSize = Part.Size;
					LocalSources.FindOrAdd(Part.Chunk).Add(Src);
					RunningOffset += Part.Size;
				}
			}
		}

		// Chunk cache: guid -> uncompressed verified bytes. Fetched chunks are
		// read and verified once even when several files reference them.
		TMap<FString, TArray64<uint8>> ChunkCache;

		// Files already swapped this run: their old bytes are gone, so they
		// are no longer a reuse source.
		TSet<FString> ReplacedFiles;
		// Files already handled by the ucas in-place patch (the sibling utoc
		// is rewritten atomically by it, so its own target must be skipped).
		TSet<FString> HandledFiles;

		int32 WritesApplied = 0;

		for (int32 TI = 0; TI < NumTargets && bOk; ++TI)
		{
			FTransactionTarget& T = State.Transaction.Targets[TI];
			// The sidecar IS the rollback record: a save that silently fails
			// (disk full) would drop the just-recorded backup paths, converting
			// an interrupted apply into an unrecoverable one. Treat every save
			// during the apply as fatal.
			auto SaveTransactionState = [&State, &StatePath]() -> bool
			{
				if (FPatcherStateFile::Save(StatePath, State))
				{
					return true;
				}
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: failed saving transaction state '%s' - aborting apply"), *StatePath);
				return false;
			};
			const FString FileId = FPaths::GetCleanFilename(T.File);
			// Fresh installs carry nested paths (IOStoreDemo/Content/Paks/...):
			// ensure the target's parent directory exists before backing up or
			// writing, or the temp-file open fails (WriteNoPermission).
			PF.CreateDirectoryTree(*FPaths::GetPath(T.File));
			const FString* HFileIdPtr = TargetIdByPath.Find(T.File);
			const FString HFileIdKey = HFileIdPtr ? *HFileIdPtr : FPaths::GetCleanFilename(T.File);
			if (HandledFiles.Contains(HFileIdKey))
			{
				UE_LOG(LogUnrealGamePatcher, Display, TEXT("FUpdateOrchestrator: '%s' handled by the ucas in-place patch (skipped)"), *HFileIdKey);
				continue;
			}

			if (T.Op == EApplyOp::Delete)
			{
				// Backup the original so rollback can restore it. CoW v2: paks
				// are exempt - rollback never restores them either (the next
				// replay re-adds a removed pak).
				if (!T.File.EndsWith(TEXT(".pak"), ESearchCase::IgnoreCase) && PF.FileExists(*T.File))
				{
					const FString BackupPath = FPaths::Combine(BackupDir,
						FString::Printf(TEXT("%s.%d.bak"), *FileId, TI));
					if (!PF.CopyFile(*BackupPath, *T.File))
					{
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: failed backing up '%s'"), *T.File);
						bOk = false;
						break;
					}
					T.BackupPath = BackupPath;
					T.OriginalHash = FPatcherHash::FileSHA1Hex(T.File);
				}
				if (!SaveTransactionState())
				{
					bOk = false;
					break;
				}

				PF.SetReadOnly(*T.File, false);
				if (!PF.DeleteFile(*T.File))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: failed deleting '%s'"), *T.File);
					bOk = false;
					break;
				}
				if (!SaveTransactionState())
				{
					bOk = false;
					break;
				}
				++WritesApplied;
				UE_LOG(LogUnrealGamePatcher, Display, TEXT("FUpdateOrchestrator: removed '%s'"), *FileId);
			}
			else
			{
				// ReplaceFile (Changed/Added).
				const FString* FileIdPtr = TargetIdByPath.Find(T.File);
				const FString FileIdKey = FileIdPtr ? *FileIdPtr : FPaths::GetCleanFilename(T.File);

				// Back up the original bytes (if any) before touching it.
				// CoW v2: pak containers are exempt (ROADMAP §5.2b) - the
				// in-pak old version is the backup, so the 1x-container copy
				// is pure write amplification. Rollback skips paks likewise.
				if (!T.File.EndsWith(TEXT(".pak"), ESearchCase::IgnoreCase) && PF.FileExists(*T.File))
				{
					const FString BackupPath = FPaths::Combine(BackupDir,
						FString::Printf(TEXT("%s.%d.bak"), *FileId, TI));
					if (!PF.CopyFile(*BackupPath, *T.File))
					{
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: failed backing up '%s'"), *T.File);
						bOk = false;
						break;
					}
					T.BackupPath = BackupPath;
					T.OriginalHash = FPatcherHash::FileSHA1Hex(T.File);
				}
				if (!SaveTransactionState())
				{
					bOk = false;
					break;
				}

				// Chunk-level in-place ucas patch (design D6): append only the
				// changed/new chunks (raw), atomically rewrite the utoc. The
				// sibling utoc is rewritten by the patch, so its own target is
				// skipped and backed up here for rollback symmetry.
				bool bPatchedInPlace = false;
				if (FileIdKey.EndsWith(TEXT(".ucas")))
				{
					const FString UtocId = FileIdKey.LeftChop(5) + TEXT(".utoc");
					const FString UtocPath = FPaths::Combine(InstallDir, UtocId);
					if (PF.FileExists(*UtocPath))
					{
						for (int32 U = 0; U < NumTargets; ++U)
						{
							FTransactionTarget& UT = State.Transaction.Targets[U];
							if (UT.File == UtocPath && UT.BackupPath.IsEmpty())
							{
								UT.BackupPath = FPaths::Combine(BackupDir,
									FString::Printf(TEXT("%s.%d.bak"), *FPaths::GetCleanFilename(UtocPath), U));
								// An unchecked failed backup would leave a
								// BackupPath pointing at nothing - rollback then
								// silently skips restoring the utoc while the
								// patch rewrites it. Fail the apply instead.
								if (!PF.CopyFile(*UT.BackupPath, *UtocPath))
								{
									UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: failed backing up sibling utoc '%s'"), *UtocPath);
									bOk = false;
									break;
								}
								UT.OriginalHash = FPatcherHash::FileSHA1Hex(UtocPath);
							}
							if (!bOk)
							{
								// Backup failed: nothing has been written yet -
								// abort the file (rollback restores the ucas from
								// its already-persisted backup). Patching anyway
								// would rewrite the pair with no recoverable
								// utoc backup.
								break;
							}
							// Persist the sibling-utoc backup BEFORE the patch
							// runs: the transaction record must already carry
							// UT.BackupPath by the time the utoc can be
							// rewritten. A crash between the patch and a
							// later save otherwise rolls the ucas back while
							// the PATCHED utoc survives (the rollback's "no
							// backup yet" branch assumes untouched bytes) -
							// and the next run then reports success on a
							// corrupt pair (id-multiset verification cannot
							// see it; only trust-unsafe data reads would).
							if (!SaveTransactionState())
							{
								bOk = false;
								break;
							}
						}
						FInPlaceIoStoreStats IoStats;
						// Backfill: hand the file's recorded dead ranges to the patcher
						// (appends fill them before falling back to EOF); on success its
						// UpdatedHoles (consumers' remainders + newly dead regions from
						// replaced chunks) replace the old list.
						TArray<FHole> UcasHoles = State.Holes.FilterByPredicate(
							[&FileIdKey](const FHole& H) { return H.File == FileIdKey; });
						if (FInPlaceIoStorePatcher::Patch(Remote, ChunkRoot, FileIdKey, T.File, UtocPath, IoStats, bVerify, UcasHoles))
						{
							bPatchedInPlace = true;
							HandledFiles.Add(UtocId);
							OutStats.BytesWritten += IoStats.BytesWritten;
							ReplacedFiles.Add(FileIdKey);
							State.Holes.RemoveAll([&FileIdKey](const FHole& H) { return H.File == FileIdKey; });
							for (const FHole& H : IoStats.UpdatedHoles)
							{
								State.Holes.Add(H);
							}
							if (!SaveTransactionState())
							{
								bOk = false;
								break;
							}
							++WritesApplied;
							UE_LOG(LogUnrealGamePatcher, Display,
								TEXT("FUpdateOrchestrator: in-place patched '%s' (%d chunks: %d same, %d replaced, %d appended; %lld bytes written, %d holes filled, %d recorded)"),
								*FileIdKey, IoStats.NumSame + IoStats.NumReplaced + IoStats.NumAppended,
								IoStats.NumSame, IoStats.NumReplaced, IoStats.NumAppended, IoStats.BytesWritten,
								IoStats.NumHolesFilled, State.Holes.FilterByPredicate([&FileIdKey](const FHole& H) { return H.File == FileIdKey; }).Num());
						}
					}
				}

				// Entry-level in-place pak patch (only the changed entries are
				// written - the Wegame/Steam whole-file rewrite is avoided).
				// Requires the pak entry metadata from the ingest; falls back
				// to the whole-file rebuild when unavailable or on any hash
				// mismatch (the patch leaves the file untouched on failure).
				if (!bPatchedInPlace && FileIdKey.EndsWith(TEXT(".pak")))
				{
					FInPlacePakStats PatchStats;
					// CoW v2 (ROADMAP §5.3): placement holes are recomputed from
					// the committed container inside the patcher; the pak's
					// sidecar hole list is retired (dropped here, never
					// re-added).
					if (FInPlacePakPatcher::Patch(Remote, ChunkRoot, FileIdKey, T.File, PatchStats, bVerify))
					{
						bPatchedInPlace = true;
						OutStats.BytesWritten += PatchStats.BytesWritten;
						ReplacedFiles.Add(FileIdKey);
						State.Holes.RemoveAll([&FileIdKey](const FHole& H) { return H.File == FileIdKey; });
						if (!SaveTransactionState())
						{
							bOk = false;
							break;
						}
						++WritesApplied;
						UE_LOG(LogUnrealGamePatcher, Display,
							TEXT("FUpdateOrchestrator: CoW patched '%s' (%d entries: %d same, %d appended; %lld bytes written, %d holes filled)"),
							*FileIdKey, PatchStats.NumSame + PatchStats.NumAppended,
							PatchStats.NumSame, PatchStats.NumAppended, PatchStats.BytesWritten,
							PatchStats.NumHolesFilled);
					}
				}

				if (!bPatchedInPlace)
				{
					// The rebuild reads the file's full part set from the
					// cache - fetch whatever the optimistic prefetch skipped.
					if (!EnsureChunksCached(FileIdKey))
					{
						bOk = false;
						break;
					}
					TArray64<uint8> Assembled;
					if (!RebuildFileFromChunks(Remote, ChunkRoot, InstallDir, FileIdKey,
						SameChunks, LocalSources, ReplacedFiles, !Options.bRepairOnly, !bNetwork, bNetwork, ChunkCache, OutStats, Assembled))
					{
						bOk = false;
						break;
					}

					const EPatcherError WErr = WriteAtomic(T.File, Assembled);
					if (WErr != EPatcherError::Ok)
					{
						LastError = WErr;
						bOk = false;
						break;
					}
					OutStats.BytesWritten += Remote.Files[FileIdKey].Size;
					ReplacedFiles.Add(FileIdKey);
					// The rebuilt file is byte-identical to the CDN copy: any recorded
					// holes for it are stale (that layout is gone).
					State.Holes.RemoveAll([&FileIdKey](const FHole& H) { return H.File == FileIdKey; });
					if (!SaveTransactionState())
					{
						bOk = false;
						break;
					}
					++WritesApplied;

					UE_LOG(LogUnrealGamePatcher, Display, TEXT("FUpdateOrchestrator: %s '%s' (%lld bytes)"),
						T.BackupPath.IsEmpty() ? TEXT("added") : TEXT("updated"), *FileIdKey, Remote.Files[FileIdKey].Size);
				}
			}

			// TEST-ONLY crash injection: leave the transaction Pending (as a
			// killed process would) and bail without rolling back.
			if (TestCrashAfterWrites > 0 && WritesApplied >= TestCrashAfterWrites)
			{
				UE_LOG(LogUnrealGamePatcher, Error,
					TEXT("FUpdateOrchestrator: simulated crash after %d writes (test hook) - transaction left pending for recovery"),
					WritesApplied);
				return false;
			}

			// TEST-ONLY failure injection: force the rollback path.
			if (TestFailAfterWrites > 0 && WritesApplied >= TestFailAfterWrites)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: injected failure after %d writes (test hook)"), WritesApplied);
				bOk = false;
				break;
			}

			Emit(EUpdatePhase::Apply, EPatcherError::Ok, false, WritesApplied, NumTargets, TEXT("applying..."));
		}

		if (!bOk)
		{
			Rollback(State);
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: update to %s failed - transaction rolled back"), *Version);
			EmitIORunReport(TEXT("failed-run"), TEXT("update-failed"), OutStats);
			Emit(EUpdatePhase::Failed, (LastError != EPatcherError::Ok) ? LastError : EPatcherError::ApplyFailed, true,
				WritesApplied, NumTargets, TEXT("apply failed, rolled back"));
			return false;
		}

		OutStats.NumChunksReused = OutStats.ReusedChunkGuids.Num();

		// --- 5. COMMIT. Order matters for crash safety:
		//   1. mark the transaction Committed and save the sidecar - from this
		//      point on, an interrupted run NEVER rolls back (the files on disk
		//      are the new version's);
		//   2. write the local content index - if this step is interrupted the
		//      index simply lags a version, so the next update re-applies the
		//      same version (self-healing); writing it while the transaction is
		//      still Pending would instead poison the next diff with
		//      new-version hashes over old-content files after a rollback;
		//   3. drop the backups - only safe once nothing can roll back.
		if (!PF.CreateDirectoryTree(*FPaths::Combine(InstallDir, TEXT(".ugp"))))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: cannot create state dir in '%s'"), *InstallDir);
			Emit(EUpdatePhase::Failed, EPatcherError::Internal, true, 0, 0, TEXT("cannot create state directory"));
			return false;
		}
		State.Transaction.Status = ETransactionStatus::Committed;
		State.Transaction.ContentVersion = Version;
		// Verify runs refresh the verify timestamp (drives the time-based
		// trigger); trust runs leave it untouched.
		if (bVerify)
		{
			State.LastVerifiedAt = FDateTime::UtcNow().ToIso8601();
		}
		if (!FPatcherStateFile::Save(StatePath, State))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: failed committing state '%s'"), *StatePath);
			Emit(EUpdatePhase::Failed, EPatcherError::Internal, true, 0, 0, TEXT("cannot commit transaction"));
			return false;
		}
		// The sidecar is the committed REMOTE index verbatim: it is the
		// trust-mode truth (manifest-to-manifest diff, no disk reads) AND the
		// chunk-reuse record (Parts + Chunks). Container entry metadata stays
		// - the in-place patchers consume the remote copy's, and the sidecar's
		// must match the remote for the manifest diff to classify correctly.
		if (!FIndexManifestFile::Save(LocalStatePath, Remote))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: failed writing local state '%s'"), *LocalStatePath);
			Emit(EUpdatePhase::Failed, EPatcherError::Internal, true, 0, 0, TEXT("cannot write local state"));
			return false;
		}
		PF.DeleteDirectoryRecursively(*GetBackupDir(InstallDir));

		Emit(EUpdatePhase::Commit, EPatcherError::Ok, false, 1, 1, TEXT("committed"));

		Emit(EUpdatePhase::Done, EPatcherError::Ok, false, 1, 1,
			*FString::Printf(TEXT("done: %d same / %d changed / %d added / %d removed; %lld bytes written"),
				OutStats.NumFilesSame, OutStats.NumFilesChanged, OutStats.NumFilesAdded, OutStats.NumFilesRemoved, OutStats.BytesWritten));

		// Network mode: the download cache is a TEMP store. The downloaded
		// chunk files must not linger - they would double the install size
		// (reuse never reads from them; it slices the installed containers).
		// Deleting after a successful commit is safe: resume only matters for
		// interrupted runs, which the download phase re-covers anyway.
		if (bNetwork)
		{
			PF.DeleteDirectoryRecursively(*ChunkRoot);
		}

		UE_LOG(LogUnrealGamePatcher, Display,
			TEXT("FUpdateOrchestrator: update to %s complete: %d chunks fetched (%lld bytes), %d chunks reused (%lld bytes), %lld bytes written"),
			*Version, OutStats.NumChunksFetched, OutStats.BytesFetched, OutStats.NumChunksReused, OutStats.BytesReused, OutStats.BytesWritten);

		// End-of-run I/O report (the stat line a single test run can be read
		// from): a small delta MUST show small read/write/net numbers - any
		// container-sized volume on a byte-sized change means a silent
		// fallback happened (review H1).
		EmitIORunReport(TEXT("update"), *FString::Printf(TEXT("update %s"), *Version), OutStats);

		return true;
	}

		int32 FUpdateOrchestrator::Compact(
		const FString& CdnDir,
		const FString& Version,
		const FString& InstallDir,
		const TArray<FString>& FileIds,
		FUpdateStats& OutStats)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		FIOStats::Get().Reset();

		// --- 1. Remote manifest (metadata lives under <base>/index/<version>/).
		// Compact/check/repair are local maintenance commands: the CDN root is a
		// local directory (bare path or file://...).
		const FString CdnRootLocal = LocalCdnRoot(CdnDir);
		const FString IndexPath = FPaths::Combine(CdnRootLocal, TEXT("index"), Version, TEXT("UnrealGamePatcher.index.json"));
		if (!CheckLocalMetaSize(IndexPath, DefaultMaxManifestBytes))
		{
			return -1;
		}
		FIndexManifest Remote;
		if (!FIndexManifestFile::Load(IndexPath, Remote))
		{
			return -1;
		}

		// --- 2. Chunk-level truth from the local state (optional; enables
		// reuse, but compact is a full rebuild so it never relies on it).
		FIndexManifest LocalIndex;
		const FString LocalStatePath = GetLocalStatePath(InstallDir);
		const bool bHaveLocalIndex = PF.FileExists(*LocalStatePath) &&
			FIndexManifestFile::Load(LocalStatePath, LocalIndex);

		TMap<FString, TArray<FLocalChunkSource>> LocalSources;
		if (bHaveLocalIndex)
		{
			for (const TPair<FString, FIndexFileEntry>& Pair : LocalIndex.Files)
			{
				int64 RunningOffset = 0;
				for (const FIndexFileChunkRef& Part : Pair.Value.Parts)
				{
					FLocalChunkSource Src;
					Src.FileId = Pair.Key;
					Src.FileOffset = RunningOffset;
					Src.ChunkOffset = Part.Offset;
					Src.PartSize = Part.Size;
					LocalSources.FindOrAdd(Part.Chunk).Add(Src);
					RunningOffset += Part.Size;
				}
			}
		}

		TSet<FString> SameChunks; // all chunks are unchanged by definition
		for (const TPair<FString, FIndexChunkEntry>& Pair : Remote.Chunks)
		{
			SameChunks.Add(Pair.Key);
		}

		TMap<FString, TArray64<uint8>> ChunkCache;
		TSet<FString> ReplacedFiles; // not used (no reuse), kept for the API
		int32 Rebuilt = 0;

		for (const FString& FileId : FileIds)
		{
			TArray64<uint8> Assembled;
			if (!RebuildFileFromChunks(Remote, CdnRootLocal, InstallDir, FileId,
				SameChunks, LocalSources, ReplacedFiles, /*bReuseAllowed=*/false, /*bTrackFetchStats=*/true, /*bChunkRootIsCache=*/false, ChunkCache, OutStats, Assembled))
			{
				return -1;
			}
			const FString Path = FPaths::Combine(InstallDir, FileId);
			if (WriteAtomic(Path, Assembled) != EPatcherError::Ok)
			{
				return -1;
			}
			OutStats.BytesWritten += Remote.Files[FileId].Size;
			++Rebuilt;
			UE_LOG(LogUnrealGamePatcher, Display, TEXT("FUpdateOrchestrator: compact rebuilt '%s' (%lld bytes)"), *FileId, Remote.Files[FileId].Size);
		}

		// The rebuilt files are byte-identical to the CDN copy: any holes
		// recorded for them are STALE (that physical layout is gone - the
		// bytes under a stale hole are LIVE after the rebuild, and the next
		// in-place patch would backfill new chunk data over them). The update
		// path clears holes for files it rebuilds (WriteAtomic branch);
		// compact must keep the same invariant.
		{
			const FString StatePath = Paths::GetStatePath(InstallDir);
			FPatcherState State;
			if (FPatcherStateFile::Load(StatePath, State))
			{
				bool bDirty = false;
				for (const FString& FileId : FileIds)
				{
					bDirty |= State.Holes.RemoveAll([&FileId](const FHole& H) { return H.File == FileId; }) > 0;
				}
				if (bDirty && !FPatcherStateFile::Save(StatePath, State))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: compact rebuilt the files but could not update the sidecar"));
					return -1;
				}
			}
		}

		OutStats.NumChunksReused = OutStats.ReusedChunkGuids.Num();
		EmitIORunReport(TEXT("compact"), TEXT("compact"), OutStats);
		return Rebuilt;
	}

	bool FUpdateOrchestrator::ResolveLatest(
		const FString& CdnRoot,
		const FUpdateOptions& Options,
		FString& OutVersion)
	{
		// The target version comes from the server's announcement file
		// index/latest.json - over HTTP when the CDN root is a URL, from disk
		// when it is a local directory (bare path or file://...).
		FLatestVersion Latest;
		if (IsHttpCdn(CdnRoot))
		{
			FString Json;
			if (!FetchCdnResource(CdnRoot, TEXT("index/latest.json"), Options, Json) ||
				!FLatestVersionFile::LoadFromString(Json, Latest))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: cannot resolve latest version from '%s'"), *CdnRoot);
				return false;
			}
		}
		else
		{
			const FString Path = FPaths::Combine(LocalCdnRoot(CdnRoot), TEXT("index"), TEXT("latest.json"));
			if (!CheckLocalMetaSize(Path, Options.MaxManifestBytes) || !FLatestVersionFile::Load(Path, Latest))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FUpdateOrchestrator: cannot resolve latest version from '%s'"), *Path);
				return false;
			}
		}
		OutVersion = Latest.Version;
		return true;
	}
}
