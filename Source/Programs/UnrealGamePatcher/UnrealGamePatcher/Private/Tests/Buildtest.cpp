// Copyright Epic Games, Inc. All Rights Reserved.
// -cli buildtest —— 发布侧 ingest 确定性/一致性
// 验证点：两次 ingest 产出字节一致的清单；条目 hash 与 staged 文件一致；
//         chunk 文件存在且尺寸正确；§1b chunk 原子发布（覆盖写/垃圾预填
//         替换/round-trip/无 .tmp 残留——L-M3 修复钉）；同目录幂等重 ingest。

#include "Tests/TestFramework.h"

namespace UnrealGamePatcher::Tests
{
	int32 RunBuildTest(const TCHAR* /*CmdLine*/)
	{

		// Ingest acceptance (task 4.1): determinism (two ingests of the same
		// input produce byte-identical manifests), file hashes equal to the
		// staged files on disk, chunk files present with the recorded sizes,
		// and idempotent re-ingest into the same directory.
		bool bAllPassed = true;
		auto Check = MakeCheck(bAllPassed);

		using namespace UnrealGamePatcher;

		const FString WorkDir = GetTestWorkDir(TEXT("buildtest"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

		// Staged fixture: the real small containers + a plain file.
		const FString ProjectRoot = GetProjectRootFromExecutable();
		const FString StagedSrc = FPaths::Combine(ProjectRoot, TEXT(".manifesttest"), TEXT("chunkinstall"),
			TEXT("Windows"), TEXT("1.0.0"), TEXT("pakchunk1"));
		const FString StagedDir = FPaths::Combine(WorkDir, TEXT("staged"));
		PF.CreateDirectoryTree(*StagedDir);
		{
			for (const TCHAR* Name : { TEXT("pakchunk1-Windows.pak"), TEXT("pakchunk1-Windows.ucas"), TEXT("pakchunk1-Windows.utoc") })
			{
				Check(PF.CopyFile(*(FPaths::Combine(StagedDir, Name)), *(FPaths::Combine(StagedSrc, Name))),
					*FString::Printf(TEXT("copy staged %s"), Name));
			}
			Check(FFileHelper::SaveStringToFile(TEXT("plain file content"), *(FPaths::Combine(StagedDir, TEXT("boot.txt")))), TEXT("write staged plain file"));
		}

		// --- 1. Ingest twice into two different CDN roots -------------------
		FIngestResult R1, R2;
		const FString CdnA = FPaths::Combine(WorkDir, TEXT("cdnA"));
		const FString CdnB = FPaths::Combine(WorkDir, TEXT("cdnB"));
		Check(FBuildIngestor::Ingest(StagedDir, TEXT("IOStoreDemo"), TEXT("1.0.0"), CdnA, R1), TEXT("ingest #1 succeeds"));
		Check(FBuildIngestor::Ingest(StagedDir, TEXT("IOStoreDemo"), TEXT("1.0.0"), CdnB, R2), TEXT("ingest #2 succeeds"));
		Check(R1.NumFiles > 0 && R1.NumChunks > 0, TEXT("ingest: files + chunks produced (hashes pinned in section 3)"));

		// --- 1b. Chunk write atomicity (fixplan L-M3): overwriting an
		//     existing content-addressed chunk (cross-version shared) and a
		//     garbage-prefilled target must land complete bytes with no .tmp
		//     residue - readers never see a partial publish -----------------
		{
			const FString ChunkDir = FPaths::Combine(WorkDir, TEXT("chunks-atomic"));
			PF.CreateDirectoryTree(*ChunkDir);
			const uint8 A[] = { 1, 2, 3, 4, 5 };
			const uint8 B[] = { 9, 8, 7, 6 };
			FString Sha1A, Sha1B;
			uint8 StoredAs = 0;
			const FString PathA = FPaths::Combine(ChunkDir, TEXT("a.chunk"));
			Check(FChunkDataLoader::Write(PathA, A, sizeof(A), Sha1A, StoredAs), TEXT("chunk write #1 succeeds"));
			Check(FChunkDataLoader::Write(PathA, B, sizeof(B), Sha1B, StoredAs), TEXT("chunk write #2 (overwrite) succeeds"));
			Check(Sha1A != Sha1B, TEXT("chunk overwrite produced the new content address"));
			const FString PathC = FPaths::Combine(ChunkDir, TEXT("c.chunk"));
			{
				const uint8 Garbage[] = { 0xEE };
				Check(FFileHelper::SaveArrayToFile(TArray<uint8>(Garbage, 1), *PathC), TEXT("prefill garbage chunk"));
			}
			Check(FChunkDataLoader::Write(PathC, A, sizeof(A), Sha1A, StoredAs), TEXT("chunk write over garbage succeeds"));
			TArray64<uint8> Round;
			FIndexChunkEntry Entry;
			Entry.SHA1 = Sha1A;
			Entry.Size = sizeof(A);
			Check(FChunkDataLoader::Load(PathC, Entry, Round) && Round.Num() == (int64)sizeof(A),
				TEXT("chunk load round-trips after overwrite"));
			Check(!PF.FileExists(*(PathA + TEXT(".tmp"))), TEXT("no .tmp residue after chunk writes"));
		}

		// --- 2. Determinism: byte-identical index files ---------------------
		{
			FString BytesA, BytesB;
			const bool bReadBoth = FFileHelper::LoadFileToString(BytesA, *R1.IndexPath) &&
				FFileHelper::LoadFileToString(BytesB, *R2.IndexPath);
			Check(bReadBoth && BytesA == BytesB, TEXT("deterministic: two ingests byte-identical"));
		}

		// --- 3. Index loads; hashes match the staged files; container
		// metadata extracted directly from the staged containers -------------
		{
			FIndexManifest Index;
			Check(FIndexManifestFile::Load(R1.IndexPath, Index), TEXT("ingested index loads + passes self-check"));
			Check(Index.Chunks.Num() == R1.NumChunks, TEXT("index chunk count matches result"));

			const FIndexFileEntry* Pak = Index.Files.Find(TEXT("pakchunk1-Windows.pak"));
			// The entry COUNT is fixture-derived (any real container works) -
			// the invariant is that per-entry metadata was extracted at all.
			Check(Pak && Pak->PakEntries.Num() >= 1, TEXT("pak entry metadata extracted"));
			const FIndexFileEntry* Ucas = Index.Files.Find(TEXT("pakchunk1-Windows.ucas"));
			Check(Ucas && Ucas->IoStoreChunks.Num() > 0, TEXT("ucas IoStore metadata extracted"));

			// Whole-file SHA1 == staged files on disk.
			bool bDiskMatch = true;
			for (const TPair<FString, FIndexFileEntry>& Pair : Index.Files)
			{
				const FString Src = FPaths::Combine(StagedDir, Pair.Key);
				FString Sha;
				int64 Size = 0;
				if (!FLocalTruthExtractor::HashWholeFile(Src, Sha, Size) || Sha != Pair.Value.SHA1 || Size != Pair.Value.Size)
				{
					bDiskMatch = false;
					break;
				}
			}
			Check(bDiskMatch, TEXT("extension manifest file hashes == staged files on disk"));
		}

		// --- 4. Chunk files present in the CDN layout with recorded sizes ---
		{
			FIndexManifest Index;
			FIndexManifestFile::Load(R1.IndexPath, Index);
			bool bAllChunks = Index.Chunks.Num() > 0;
			for (const TPair<FString, FIndexChunkEntry>& Pair : Index.Chunks)
			{
				const FIndexChunkEntry& E = Pair.Value;
				if (E.File.IsEmpty())
				{
					bAllChunks = false;
					break;
				}
				const FString ChunkPath = FPaths::Combine(CdnA, E.File);
				const int64 Size = IFileManager::Get().FileSize(*ChunkPath);
				if (Size != E.Size)
				{
					bAllChunks = false;
					break;
				}
			}
			Check(bAllChunks, TEXT("all chunk files staged with recorded sizes"));
		}

		// --- 5. Idempotent re-ingest into the same CDN root -----------------
		{
			FIngestResult R3;
			Check(FBuildIngestor::Ingest(StagedDir, TEXT("IOStoreDemo"), TEXT("1.0.0"), CdnA, R3), TEXT("re-ingest into same CDN succeeds"));
			FString BytesA, BytesB;
			const bool bReadBoth = FFileHelper::LoadFileToString(BytesA, *R1.IndexPath) &&
				FFileHelper::LoadFileToString(BytesB, *R3.IndexPath);
			Check(bReadBoth && BytesA == BytesB, TEXT("re-ingest leaves index byte-identical"));
		}

		// --- 6. C6 regression: an EMPTY staged dir must be refused -----------
		// Publishing a zero-file index classifies EVERY file of every client
		// install as Removed - the next update deletes the whole install
		// (live-reproduced during the code review).
		{
			const FString EmptyStaged = FPaths::Combine(WorkDir, TEXT("empty_staged"));
			PF.CreateDirectoryTree(*EmptyStaged);
			FIngestResult REmpty;
			Check(!FBuildIngestor::Ingest(EmptyStaged, TEXT("IOStoreDemo"), TEXT("9.9.9"), FPaths::Combine(WorkDir, TEXT("empty_cdn")), REmpty),
				TEXT("buildtest: EMPTY staged dir is refused (install-wipe guard)"));
			// A trailing path separator must not corrupt file ids either
			// (RightChop(Dir.Len()+1) ate the filename's first character).
			const FString SepStaged = StagedDir + TEXT("/");
			FIngestResult RSep;
			Check(FBuildIngestor::Ingest(SepStaged, TEXT("IOStoreDemo"), TEXT("9.9.9"), FPaths::Combine(WorkDir, TEXT("sep_cdn")), RSep),
				TEXT("buildtest: trailing-separator -staged= ingests"));
			FIndexManifest SepIndex;
			bool bSepOk = FIndexManifestFile::Load(RSep.IndexPath, SepIndex);
			if (bSepOk)
			{
				for (const TPair<FString, FIndexFileEntry>& Pair : SepIndex.Files)
				{
					// No id may start with a separator or a stray fragment.
					if (Pair.Key.StartsWith(TEXT("/")) || Pair.Key.StartsWith(TEXT("\\")) ||
						Pair.Key.StartsWith(TEXT(".")))
					{
						bSepOk = false;
					}
				}
			}
			Check(bSepOk && SepIndex.Files.Num() > 0, TEXT("buildtest: trailing-separator ids are clean relative paths"));
		}

		// --- 7. CDC shift regression: content-defined boundaries survive
		// insertions. A 1-byte insert at offset 0 must re-chunk only the
		// first window - the Gear state re-synchronizes (it depends on the
		// last 64 bytes only), so every following cut point lands at the
		// same content position and all later chunks keep their identity.
		// Fixed-size windows would shift wholesale and re-chunk everything.
		{
			const FString BigStaged = FPaths::Combine(WorkDir, TEXT("big_staged"));
			PF.CreateDirectoryTree(*BigStaged);
			const int64 BigSize = 12 << 20; // 12 MB - spans many CDC chunks
			TArray64<uint8> Big;
			Big.SetNumUninitialized(BigSize);
			uint64 Seed = 0x123456789ABCDEFull;
			for (int64 I = 0; I < BigSize; ++I)
			{
				Seed = Seed * 6364136223846793005ull + 1442695040888963407ull;
				Big[I] = (uint8)(Seed >> 33);
			}
			Check(FFileHelper::SaveArrayToFile(Big, *(FPaths::Combine(BigStaged, TEXT("big.bin")))), TEXT("cdcshift: write big file v1"));

			FIngestResult RV1;
			const FString CdnCdc1 = FPaths::Combine(WorkDir, TEXT("cdncdc1"));
			Check(FBuildIngestor::Ingest(BigStaged, TEXT("IOStoreDemo"), TEXT("1.0.0"), CdnCdc1, RV1), TEXT("cdcshift: ingest v1"));

			// v2: insert one byte at offset 0.
			TArray64<uint8> Shifted;
			Shifted.SetNumUninitialized(BigSize + 1);
			Shifted[0] = 0x5A;
			FMemory::Memcpy(Shifted.GetData() + 1, Big.GetData(), BigSize);
			Check(FFileHelper::SaveArrayToFile(Shifted, *(FPaths::Combine(BigStaged, TEXT("big.bin")))), TEXT("cdcshift: write big file v2 (1 byte inserted at 0)"));

			FIngestResult RV2;
			const FString CdnCdc2 = FPaths::Combine(WorkDir, TEXT("cdncdc2"));
			Check(FBuildIngestor::Ingest(BigStaged, TEXT("IOStoreDemo"), TEXT("1.0.1"), CdnCdc2, RV2), TEXT("cdcshift: ingest v2"));

			FIndexManifest I1, I2;
			Check(FIndexManifestFile::Load(RV1.IndexPath, I1) && FIndexManifestFile::Load(RV2.IndexPath, I2), TEXT("cdcshift: both indexes load"));
			int32 Shared = 0;
			for (const TPair<FString, FIndexChunkEntry>& P : I2.Chunks)
			{
				if (I1.Chunks.Contains(P.Key))
				{
					++Shared;
				}
			}
			const int32 NewChunks = I2.Chunks.Num() - Shared;
			Check(NewChunks <= 2,
				*FString::Printf(TEXT("cdcshift: 1-byte insert adds <= 2 chunks (got %d new of %d)"), NewChunks, I2.Chunks.Num()));
			Check((double)Shared >= 0.9 * (double)I2.Chunks.Num(),
				*FString::Printf(TEXT("cdcshift: >= 90%% of v2 chunks shared with v1 (%d/%d)"), Shared, I2.Chunks.Num()));
		}

		PF.DeleteDirectoryRecursively(*WorkDir);

		return FinishTest(bAllPassed, TEXT("buildtest"));
	
	}

}
