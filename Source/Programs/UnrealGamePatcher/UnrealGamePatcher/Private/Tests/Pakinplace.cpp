// Copyright Epic Games, Inc. All Rights Reserved.
// -cli pakinplace —— pak 条目级原地写（CoW v2）
// 验证点：v1→v1.1 只写变化条目（91 字节而非整包）；whole-file hash 与
//         v2 清单一致；§4c 洞回填（新条目放进上代死区）；撕裂尾恢复
//         （footer 反向扫描截断）；fail-after-patch（footer=提交点）；
//         Unrecoverable → 整重建字节一致；删条目拒绝 CoW 走整重建
//         （假收敛修复钉）；压缩多块条目全链。

#include "Tests/TestFramework.h"

namespace UnrealGamePatcher::Tests
{
	int32 RunPakInPlace(const TCHAR* /*CmdLine*/)
	{

		// In-place pak entry-level patching (the differentiator vs whole-file
		// rewrite): build two pak versions (one entry changed in place, one
		// appended), ingest both through the real pipeline (with entry
		// metadata), install v1, then patch the pak in place and verify ONLY
		// the changed entries were written and the result is byte-exact.
		bool bAllPassed = true;
		auto Check = MakeCheck(bAllPassed);

		using namespace UnrealGamePatcher;

		const FString WorkDir = GetTestWorkDir(TEXT("pakinplace"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

		// --- Helpers ---------------------------------------------------------
		// The packed byte encoding is LSB-first for numbers (the manifest's
		// uint32/uint64 values), normal order for hashes.
		auto PackU32 = [](uint32 V) -> FString
		{
			return FString::Printf(TEXT("%03d%03d%03d%03d"), V & 0xFF, (V >> 8) & 0xFF, (V >> 16) & 0xFF, (V >> 24) & 0xFF);
		};
		auto PackU64 = [](uint64 V) -> FString
		{
			FString Out;
			for (int32 I = 0; I < 8; ++I) { Out += FString::Printf(TEXT("%03d"), (V >> (I * 8)) & 0xFF); }
			return Out;
		};
		auto Sha1HexOfBytes = [](const TArray64<uint8>& Data) -> FString
		{
			FSHA1 Hasher;
			Hasher.Update(Data.GetData(), (int32)Data.Num());
			uint8 Hash[20] = { 0 };
			Hasher.Final();
			Hasher.GetHash(Hash);
			FString Out;
			Out.Reserve(40);
			for (int32 I = 0; I < 20; ++I) { Out += FString::Printf(TEXT("%02x"), Hash[I]); }
			return Out;
		};

		/** Stages the crafted container into a version dir and ingests it
		 *  (the new staged-based pipeline - no official manifest, no BPT). */
		auto CraftAndIngest = [&](const TCHAR* Version, const FString& PakPath, const FString& LogicalName, FString& OutIndexPath) -> bool
		{
			const FString StagedDir = FPaths::Combine(WorkDir, FString::Printf(TEXT("staged_%s"), Version));
			if (!PF.DirectoryExists(*StagedDir) && !PF.CreateDirectoryTree(*StagedDir))
			{
				return false;
			}
			const FString Dst = FPaths::Combine(StagedDir, LogicalName);
			PF.DeleteFile(*Dst);
			if (!PF.CopyFile(*Dst, *PakPath))
			{
				return false;
			}
			FIngestResult Result;
			if (!FBuildIngestor::Ingest(StagedDir, TEXT("InplaceTest"), Version, FPaths::Combine(WorkDir, TEXT("cdn")), Result))
			{
				return false;
			}
			OutIndexPath = Result.IndexPath;
			return true;
		};

		// --- 1. v1 = the real staged pak; v2 = re-append entry[0] (CoW) + append.
		const FString ProjectRoot = FPaths::GetPath(FPaths::GetPath(FPaths::GetPath(FPlatformProcess::BaseDir())));
		const FString SrcPak = FPaths::Combine(ProjectRoot, TEXT(".manifesttest"), TEXT("chunkinstall"), TEXT("Windows"), TEXT("1.0.0"), TEXT("pakchunk1"), TEXT("pakchunk1-Windows.pak"));
		Check(PF.FileExists(*SrcPak), TEXT("inplace: source pak present"));
		const FString V1Pak = FPaths::Combine(WorkDir, TEXT("v1.pak"));
		const FString V2Pak = FPaths::Combine(WorkDir, TEXT("v2.pak"));
		PF.CopyFile(*V1Pak, *SrcPak);
		PF.CopyFile(*V2Pak, *SrcPak);

		int64 Entry0Size = 0;
		FString Entry0Path;
		TArray64<uint8> ReappendBytes;
		{
			// Read the entry info first; the reader must be destroyed before
			// the writer opens the file for writing (handle share flags).
			FPakReader R;
			Check(R.Open(V2Pak) && R.ReadIndex(), TEXT("inplace: v2 copy opens"));
			Check(R.GetFiles().Num() >= 1, TEXT("inplace: source pak has entries"));
			Entry0Path = R.GetFiles()[0].Path;
			Entry0Size = R.GetFiles()[0].Entry.Size;
		}
		{
			FPakWriter W;
			Check(W.Open(V2Pak), TEXT("inplace: writer opens v2"));
			ReappendBytes.SetNumUninitialized(Entry0Size);
			for (int32 I = 0; I < ReappendBytes.Num(); ++I) { ReappendBytes[I] = (uint8)(0xA0 + (I & 0x0F)); }
			// CoW v2: the changed entry is re-appended (never overwritten in
			// place); the writer computes the fresh built-in hash.
			FPakWriter::FEntryToWrite Replaced;
			Replaced.Path = Entry0Path;
			Replaced.CompressionMethodIndex = 0;
			Replaced.UncompressedSize = ReappendBytes.Num();
			FPakWriter::FStoredBlock ReplacedBlock;
			ReplacedBlock.Bytes = ReappendBytes; // copy: the test asserts on these bytes later
			Replaced.Blocks.Add(MoveTemp(ReplacedBlock));
			FPakEntry ReplacedOut;
			Check(W.AppendEntry(Replaced, ReplacedOut), TEXT("inplace: v2 re-appends entry[0] (CoW)"));
			FPakWriter::FEntryToWrite Appended;
			Appended.Path = TEXT("patched.txt");
			Appended.CompressionMethodIndex = 0;
			Appended.UncompressedSize = 15;
			FPakWriter::FStoredBlock Block;
			const char* Text = "in-place-patched";
			Block.Bytes.SetNumUninitialized(15);
			FMemory::Memcpy(Block.Bytes.GetData(), Text, 15);
			Appended.Blocks.Add(MoveTemp(Block));
			FPakEntry OutEntry;
			Check(W.AppendEntry(Appended, OutEntry), TEXT("inplace: v2 appends patched.txt"));
			Check(W.Commit(), TEXT("inplace: v2 committed"));
			// The commit pads to a 512-byte sector boundary and writes the
			// footer as one single-sector (<=221B) write - the crash-model
			// requirement. File size == FooterEnd, so the size itself pins it.
			Check(IFileManager::Get().FileSize(*V2Pak) % 512 == 0,
				TEXT("inplace: committed pak is 512-sector aligned (single-sector footer write)"));
		}

		// --- 2. Ingest both versions (entry metadata extracted by the ingestor).
		FString IndexV1, IndexV2;
		Check(CraftAndIngest(TEXT("1.0"), V1Pak, TEXT("game.pak"), IndexV1), TEXT("inplace: ingest v1"));
		Check(CraftAndIngest(TEXT("1.1"), V2Pak, TEXT("game.pak"), IndexV2), TEXT("inplace: ingest v2"));
		FIndexManifest Remote;
		Check(FIndexManifestFile::Load(IndexV2, Remote), TEXT("inplace: v2 index loads"));

		// The ingest must have recorded pak entry metadata.
		Check(Remote.Files[TEXT("game.pak")].PakEntries.Num() == 3, TEXT("inplace: v2 index has 3 pak entry records"));
		{
			bool bHasPatched = false;
			for (const FIndexPakEntry& PE : Remote.Files[TEXT("game.pak")].PakEntries)
			{
				if (PE.Path == TEXT("/patched.txt") || PE.Path == TEXT("patched.txt")) { bHasPatched = true; }
			}
			Check(bHasPatched, TEXT("inplace: appended entry recorded"));
		}

		// --- 3. Install v1 (whole-file path), then patch to v1.1 in place.
		const FString Cdn = FPaths::Combine(WorkDir, TEXT("cdn"));
		const FString Install = FPaths::Combine(WorkDir, TEXT("install"));
		PF.CreateDirectoryTree(*Install);
		FUpdateStats S0;
		Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.0"), Install, true, S0), TEXT("inplace: install v1"));

		FInPlacePakStats PStats;
		const FString InstalledPak = FPaths::Combine(Install, TEXT("game.pak"));
		Check(FInPlacePakPatcher::Patch(Remote, Cdn, TEXT("game.pak"), InstalledPak, PStats),
			TEXT("inplace: in-place patch succeeds"));
		Check(PStats.NumSame == 1 && PStats.NumAppended == 2,
			TEXT("inplace: 1 same (zero movement), 2 appended (CoW: changed entry re-append + new file)"));

		// The exact byte count is pinned below by the orchestrator-path run
		// (BytesWritten == 91); keep the ratio as log-only context.
		{
			const int64 PakSize = IFileManager::Get().FileSize(*InstalledPak);
			UE_LOG(LogUnrealGamePatcher, Display, TEXT("inplace: pak size %lld, bytes written %lld (%.1f%%)"),
				PakSize, PStats.BytesWritten, PakSize > 0 ? (100.0 * (double)PStats.BytesWritten / (double)PakSize) : 0.0);
		}

		// Whole-file hash must equal the v2 manifest.
		{
			FString Sha1;
			int64 Size = 0;
			FLocalTruthExtractor::HashWholeFile(InstalledPak, Sha1, Size);
			Check(Sha1.Equals(Remote.Files[TEXT("game.pak")].SHA1, ESearchCase::IgnoreCase),
				TEXT("inplace: patched pak is byte-exact vs the v2 manifest"));
		}

		// Re-open: 3 entries, the overwritten one has the new bytes.
		{
			FPakReader R;
			Check(R.Open(InstalledPak) && R.ReadIndex(), TEXT("inplace: patched pak re-opens"));
			Check(R.GetFiles().Num() == 3, TEXT("inplace: patched pak has 3 entries"));
			bool bReappendOk = false, bAppendOk = false;
			for (const FPakReader::FPakEntryLocation& Loc : R.GetFiles())
			{
				if (Loc.Path == Entry0Path)
				{
					TArray64<uint8> Payload;
					if (R.ReadEntryData(Loc.Entry, Payload) && Payload.Num() == ReappendBytes.Num() &&
						FMemory::Memcmp(Payload.GetData(), ReappendBytes.GetData(), Payload.Num()) == 0)
					{
						bReappendOk = true;
					}
				}
				if (Loc.Path == TEXT("/patched.txt") || Loc.Path == TEXT("patched.txt"))
				{
					TArray64<uint8> Payload;
					if (R.ReadEntryData(Loc.Entry, Payload) && Payload.Num() == 15 &&
						FMemory::Memcmp(Payload.GetData(), "in-place-patched", 15) == 0)
					{
						bAppendOk = true;
					}
				}
			}
			Check(bReappendOk, TEXT("inplace: re-appended (CoW) entry has the new bytes"));
			Check(bAppendOk, TEXT("inplace: appended entry present with the expected bytes"));
		}


		// --- 4b. The UPDATE FLOW itself must use the in-place path ----------
		// Reset the install to v1, then run a normal orchestrator update to
		// 1.1: the diff sees game.pak changed and patches it in place instead
		// of rewriting the whole file.
		{
			PF.CopyFile(*InstalledPak, *V1Pak); // back to v1
			FUpdateStats SOrch;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.1"), Install, true, SOrch),
				TEXT("inplace: orchestrator update to 1.1"));
			Check(SOrch.NumFilesChanged == 1, TEXT("inplace: update flow sees exactly the pak changed"));
			Check(SOrch.BytesWritten == 91, TEXT("inplace: update flow wrote only the changed entries (91 bytes, not the whole pak)"));
			{
				FString Sha1;
				int64 Size = 0;
				FLocalTruthExtractor::HashWholeFile(InstalledPak, Sha1, Size);
				Check(Sha1.Equals(Remote.Files[TEXT("game.pak")].SHA1, ESearchCase::IgnoreCase),
					TEXT("inplace: update-flow result is byte-exact vs the target"));
				// The patcher commits through FPakWriter, so the sector-alignment
				// invariant holds for orchestrator-produced containers too.
				Check(IFileManager::Get().FileSize(*InstalledPak) % 512 == 0,
					TEXT("inplace: orchestrator-patched pak is 512-sector aligned"));
			}
		}

		// The untouched entry is byte-identical to v1's (zero movement).
		{
			FPakReader A, B;
			A.Open(V1Pak); A.ReadIndex();
			B.Open(InstalledPak); B.ReadIndex();
			const FString SecondPath = A.GetFiles()[1].Path;
			TArray64<uint8> OldBytes, NewBytes;
			A.ReadEntryData(A.GetFiles()[1].Entry, OldBytes);
			const FPakReader::FPakEntryLocation* NewLoc = nullptr;
			for (const FPakReader::FPakEntryLocation& L : B.GetFiles())
			{
				if (L.Path == SecondPath) { NewLoc = &L; break; }
			}
			B.ReadEntryData(NewLoc->Entry, NewBytes);
			Check(NewLoc && OldBytes.Num() == NewBytes.Num() &&
				FMemory::Memcmp(OldBytes.GetData(), NewBytes.GetData(), OldBytes.Num()) == 0,
				TEXT("inplace: untouched entry byte-identical (zero movement)"));
		}

		// --- 4c. Backfill: the next update places new entries INTO the dead
		// ranges the previous patch left behind (superseded entry data + the
		// old index/footer) instead of growing the file. CoW v2 recomputes the
		// holes from the committed container, so the previous patch recorded
		// nothing - the dead ranges are visible by construction.
		{
			// v3 = v2 + one more small appended entry.
			const FString V3Pak = FPaths::Combine(WorkDir, TEXT("v3.pak"));
			PF.CopyFile(*V3Pak, *V2Pak);
			{
				FPakWriter W;
				Check(W.Open(V3Pak), TEXT("backfill: v3 writer opens"));
				FPakWriter::FEntryToWrite Extra;
				Extra.Path = TEXT("backfilled.txt");
				FPakWriter::FStoredBlock Block;
				const ANSICHAR* Text = "backfilled!!";
				Block.Bytes.SetNumUninitialized(12);
				FMemory::Memcpy(Block.Bytes.GetData(), Text, 12);
				Extra.Blocks.Add(MoveTemp(Block));
				FPakEntry OutEntry;
				Check(W.AppendEntry(Extra, OutEntry) && W.Commit(), TEXT("backfill: v3 committed"));
			}
			FString IndexV3;
			Check(CraftAndIngest(TEXT("1.2"), V3Pak, TEXT("game.pak"), IndexV3), TEXT("backfill: ingest v3"));

			const int64 SizeBefore = IFileManager::Get().FileSize(*InstalledPak);
			FUpdateStats SBackfill;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.2"), Install, true, SBackfill),
				TEXT("backfill: update to 1.2 succeeds"));
			Check(SBackfill.NumFilesChanged == 1, TEXT("backfill: exactly the pak changed"));

			// The new entry must sit BELOW the pre-update EOF - physical proof
			// it was placed into a recorded dead range, not appended at EOF.
			{
				FPakReader R;
				Check(R.Open(InstalledPak) && R.ReadIndex(), TEXT("backfill: patched pak opens"));
				const FPakReader::FPakEntryLocation* Loc = nullptr;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path == TEXT("backfilled.txt") || L.Path == TEXT("/backfilled.txt")) { Loc = &L; }
				}
				Check(Loc != nullptr, TEXT("backfill: new entry present"));
				if (Loc)
				{
					Check(Loc->Entry.Offset + Loc->Entry.Size + 64 <= SizeBefore,
						TEXT("backfill: the new entry was placed INSIDE a recorded hole"));
					TArray64<uint8> Payload;
					Check(R.ReadEntryData(Loc->Entry, Payload) && Payload.Num() == 12 &&
						FMemory::Memcmp(Payload.GetData(), "backfilled!!", 12) == 0,
						TEXT("backfill: placed entry decodes correctly"));
				}
			}

			// Convergence: the rerun is a no-op.
			FUpdateStats SBackfill2;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.2"), Install, true, SBackfill2),
				TEXT("backfill: rerun converges"));
			Check(SBackfill2.NumFilesChanged == 0 && SBackfill2.BytesWritten == 0,
				TEXT("backfill: rerun is a no-op"));
		}

		// --- 5. Crash recovery (CoW v2, ROADMAP §5.2): the recovery scanner
		// is the WAL replay - truncate the torn tail back to the last valid
		// footer (magic + version + geometry + index SHA1, not magic alone),
		// then converge by replay.
		{
			auto AppendRaw = [&](const FString& Path, int64 Num, uint8 Fill) -> bool
			{
				IFileHandle* H = PF.OpenWrite(*Path, true, true);
				if (!H) { return false; }
				H->SeekFromEnd(0);
				TArray64<uint8> Garbage;
				Garbage.SetNumUninitialized(Num);
				FMemory::Memset(Garbage.GetData(), Fill, Num);
				const bool bWrote = H->Write(Garbage.GetData(), Num);
				delete H;
				return bWrote;
			};
			auto CorruptTail = [&](const FString& Path, int64 Num) -> bool
			{
				IFileHandle* H = PF.OpenWrite(*Path, true, true);
				if (!H) { return false; }
				H->SeekFromEnd(-Num);
				TArray64<uint8> Garbage;
				Garbage.SetNumUninitialized(Num);
				FMemory::Memset(Garbage.GetData(), 0xBB, Num);
				const bool bWrote = H->Write(Garbage.GetData(), Num);
				delete H;
				return bWrote;
			};

			const int64 V1Size = IFileManager::Get().FileSize(*V1Pak);
			const FString V1Sha = FPatcherHash::FileSHA1Hex(V1Pak);

			// 5a. Committed pak: Clean fast path, validated footer returned.
			const FString RecClean = FPaths::Combine(WorkDir, TEXT("rec_clean.pak"));
			PF.CopyFile(*RecClean, *V2Pak);
			{
				const FPakRecovery::FOutcome O = FPakRecovery::ScanAndRepair(RecClean);
				Check(O.Result == FPakRecovery::EResult::Clean, TEXT("recovery: committed pak scans Clean"));
				Check(O.Info.Version >= 10 && O.Info.IndexOffset > 0, TEXT("recovery: Clean carries the validated footer"));
			}

			// 5b. Torn tail (crash before the footer commit write): the v1
			// footer below the garbage is still valid - truncate to it.
			const FString RecTail = FPaths::Combine(WorkDir, TEXT("rec_tail.pak"));
			PF.CopyFile(*RecTail, *V1Pak);
			Check(AppendRaw(RecTail, 5000, 0xA5), TEXT("recovery: torn tail appended"));
			{
				const FPakRecovery::FOutcome O = FPakRecovery::ScanAndRepair(RecTail);
				Check(O.Result == FPakRecovery::EResult::Truncated && O.TruncatedToBytes == V1Size,
					TEXT("recovery: torn tail truncated to the last committed footer"));
				Check(FPatcherHash::FileSHA1Hex(RecTail) == V1Sha,
					TEXT("recovery: truncation restores the byte-exact v1"));
			}

			// 5b2. Multi-window scan: a >64KB pak whose torn tail spans several
			// 64KB scan windows. Every other fixture here is tiny, so only this
			// case exercises the backward window-walk loop - it must cross the
			// window boundary and find the committed footer in an interior
			// window.
			const FString BigPak = FPaths::Combine(WorkDir, TEXT("rec_window.pak"));
			PF.CopyFile(*BigPak, *V2Pak);
			{
				FPakWriter W;
				Check(W.Open(BigPak), TEXT("recovery: multi-window writer opens"));
				FPakWriter::FEntryToWrite Big;
				Big.Path = TEXT("bigblob.bin");
				Big.CompressionMethodIndex = 0;
				Big.UncompressedSize = 200ll * 1024;
				FPakWriter::FStoredBlock BigBlock;
				BigBlock.Bytes.SetNumUninitialized(200ll * 1024);
				for (int64 I = 0; I < BigBlock.Bytes.Num(); ++I)
				{
					BigBlock.Bytes[(int32)I] = (uint8)((I * 131 + (I >> 7)) & 0xFF);
				}
				Big.Blocks.Add(MoveTemp(BigBlock));
				FPakEntry BigOut;
				Check(W.AppendEntry(Big, BigOut) && W.Commit(), TEXT("recovery: 200KB entry committed"));
			}
			// Hash AFTER the writer handle is closed: the writer holds WRITE
			// access with FILE_SHARE_READ only, and OpenRead (which does not
			// share WRITE by default) then fails with a sharing violation -
			// FileSHA1Hex would silently return an empty string.
			const int64 BigCommitted = IFileManager::Get().FileSize(*BigPak);
			const FString BigSha = FPatcherHash::FileSHA1Hex(BigPak);
			Check(BigCommitted > 64 * 1024, TEXT("recovery: multi-window fixture exceeds one scan window"));
			Check(!BigSha.IsEmpty(), TEXT("recovery: fixture hash captured"));
			Check(AppendRaw(BigPak, 150 * 1024, 0xA5), TEXT("recovery: 150KB torn tail appended"));
			{
				const FPakRecovery::FOutcome O = FPakRecovery::ScanAndRepair(BigPak);
				Check(O.Result == FPakRecovery::EResult::Truncated && O.TruncatedToBytes == BigCommitted,
					TEXT("recovery: multi-window walk truncates to the committed footer"));
				Check(FPatcherHash::FileSHA1Hex(BigPak) == BigSha,
					TEXT("recovery: multi-window truncation is byte-exact"));
			}

			// 5c. Torn footer (crash during the sector-atomic footer write):
			// the v2 footer is unreadable, the v1 footer below still is.
			const FString RecFooter = FPaths::Combine(WorkDir, TEXT("rec_footer.pak"));
			PF.CopyFile(*RecFooter, *V2Pak);
			Check(CorruptTail(RecFooter, 256), TEXT("recovery: footer corrupted"));
			{
				const FPakRecovery::FOutcome O = FPakRecovery::ScanAndRepair(RecFooter);
				Check(O.Result == FPakRecovery::EResult::Truncated && O.TruncatedToBytes == V1Size,
					TEXT("recovery: torn footer falls back to the previous generation"));
				Check(FPatcherHash::FileSHA1Hex(RecFooter) == V1Sha,
					TEXT("recovery: previous generation is byte-exact"));
			}

			// 5d. A footer-SHAPED byte string inside an entry payload must not
			// be mistaken for a footer: magic + version + plausible geometry
			// all present, only the index SHA1 gives it away.
			const FString RecFake = FPaths::Combine(WorkDir, TEXT("rec_fake.pak"));
			PF.CopyFile(*RecFake, *V1Pak);
			{
				TArray64<uint8> FakeFooter;
				auto Le32 = [&FakeFooter](uint32 V) { FakeFooter.Append((const uint8*)&V, 4); };
				auto Le64 = [&FakeFooter](uint64 V) { FakeFooter.Append((const uint8*)&V, 8); };
				FakeFooter.AddZeroed(16);  // EncryptionKeyGuid
				FakeFooter.Add(0);         // bEncryptedIndex
				Le32(PakFileMagic);        // magic
				Le32(12);                  // version
				Le64(0);                   // IndexOffset (claims the file start)
				Le64(1024);                // IndexSize
				FakeFooter.AddZeroed(20);  // IndexHash (will NOT match)
				FakeFooter.AddZeroed(160); // CompressionMethods

				FPakWriter W;
				Check(W.Open(RecFake), TEXT("recovery: fake-footer pak opens"));
				FPakWriter::FEntryToWrite FE;
				FE.Path = TEXT("fakefooter.bin");
				FPakWriter::FStoredBlock FB;
				FB.Bytes = FakeFooter;
				FB.Bytes.Append(FakeFooter); // payload carries 3 copies of the fake
				FB.Bytes.Append(FakeFooter);
				FE.UncompressedSize = FB.Bytes.Num();
				FE.Blocks.Add(MoveTemp(FB));
				FPakEntry Out;
				Check(W.AppendEntry(FE, Out) && W.Commit(), TEXT("recovery: fake-footer entry committed"));
			}
			Check(CorruptTail(RecFake, 256), TEXT("recovery: real footer corrupted"));
			{
				const FPakRecovery::FOutcome O = FPakRecovery::ScanAndRepair(RecFake);
				Check(O.Result == FPakRecovery::EResult::Truncated && O.TruncatedToBytes == V1Size,
					TEXT("recovery: footer-shaped payload bytes are rejected (index SHA1 is the filter)"));
				Check(FPatcherHash::FileSHA1Hex(RecFake) == V1Sha,
					TEXT("recovery: the real previous generation survives"));
			}

			// 5e. No valid footer anywhere: Unrecoverable (the orchestrator
			// deletes such files so the diff re-fetches them whole).
			const FString RecDead = FPaths::Combine(WorkDir, TEXT("rec_dead.pak"));
			Check(AppendRaw(RecDead, 8192, 0x00), TEXT("recovery: garbage file written"));
			{
				const FPakRecovery::FOutcome O = FPakRecovery::ScanAndRepair(RecDead);
				Check(O.Result == FPakRecovery::EResult::Unrecoverable, TEXT("recovery: garbage file is Unrecoverable"));
			}

			// 5f. End to end: a torn tail on the INSTALLED pak is repaired by
			// the update's startup scan, and the run converges with zero
			// writes to the identical committed bytes.
			const FString InstalledV3Sha = FPatcherHash::FileSHA1Hex(InstalledPak);
			Check(AppendRaw(InstalledPak, 3000, 0xC3), TEXT("recovery: torn tail appended to the installed pak"));
			FUpdateStats SRec;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.2"), Install, true, SRec),
				TEXT("recovery: update runs after the torn tail"));
			Check(SRec.NumFilesChanged == 0 && SRec.BytesWritten == 0,
				TEXT("recovery: replay converges with zero writes"));
			Check(FPatcherHash::FileSHA1Hex(InstalledPak) == InstalledV3Sha,
				TEXT("recovery: the installed pak is byte-identical to the committed v3"));
		}

		// --- Compressed multi-block entry: write / read / hash / ingest / update
		// (the real-engine-format blind spot that let the fpak block-alignment
		// bug ship: every other fixture pak is uncompressed single-block).
		{
			const int64 BlockSize = 64ll << 10; // 64 KiB (engine default quantum)
			const int32 SliceCount = 3;
			const int64 SliceBytes = BlockSize;
			const int64 TotalBytes = SliceBytes * SliceCount;

			// Deterministic pseudo-random content (3 independent 64KiB slices).
			TArray64<uint8> ContentA, ContentB;
			ContentA.SetNumUninitialized(TotalBytes);
			ContentB.SetNumUninitialized(TotalBytes);
			for (int32 S = 0; S < SliceCount; ++S)
			{
				for (int64 I = 0; I < SliceBytes; ++I)
				{
					const int64 G = S * SliceBytes + I;
					ContentA[G] = (uint8)((I * 31 + S * 17 + (I >> 9)) & 0xFF);
					ContentB[G] = (uint8)((I * 13 + S * 29 + (I >> 8) + 0x5A) & 0xFF);
				}
			}

			auto CompressSlices = [](const TArray64<uint8>& Content, int64 SliceBytes, TArray<TArray64<uint8>>& OutBlocks) -> bool
			{
				const int32 Count = (int32)(Content.Num() / SliceBytes);
				for (int32 S = 0; S < Count; ++S)
				{
					const uint8* Src = Content.GetData() + (int64)S * SliceBytes;
					TArray64<uint8> Dst;
					Dst.SetNumUninitialized(SliceBytes + 4096); // compressed can exceed raw
					int32 CompressedSize = Dst.Num();
					if (!FCompression::CompressMemory((FName)TEXT("Oodle"), Dst.GetData(), CompressedSize, Src, (int32)SliceBytes))
					{
						return false;
					}
					Dst.SetNum(CompressedSize);
					OutBlocks.Add(MoveTemp(Dst));
				}
				return true;
			};

			TArray<TArray64<uint8>> BlocksA, BlocksB;
			Check(CompressSlices(ContentA, SliceBytes, BlocksA) && BlocksA.Num() == SliceCount, TEXT("zcomp: v1 content compressed into 3 Oodle blocks"));
			Check(CompressSlices(ContentB, SliceBytes, BlocksB) && BlocksB.Num() == SliceCount, TEXT("zcomp: v2 content compressed into 3 Oodle blocks"));

			// Write v1: based on the installed pak (inherits the engine Oodle
			// method table in its footer), one compressed multi-block entry.
			const FString ZPakV1 = FPaths::Combine(WorkDir, TEXT("zcomp_v1.pak"));
			const FString ZPakV2 = FPaths::Combine(WorkDir, TEXT("zcomp_v2.pak"));
			PF.CopyFile(*ZPakV1, *InstalledPak);
			int32 OodleIndex = INDEX_NONE;
			{
				FPakWriter W;
				Check(W.Open(ZPakV1), TEXT("zcomp: writer opens"));
				FPakWriter::FEntryToWrite ZEntry;
				ZEntry.Path = TEXT("Content/ZCompressed.bin");
				// Register Oodle into the footer method table (a pak that never
				// compressed anything has an empty table - a real gap this test
				// exposed; RegisterCompressionMethod fills it on first use).
				OodleIndex = W.RegisterCompressionMethod(TEXT("Oodle"));
				Check(OodleIndex > 0, TEXT("zcomp: Oodle registered in the method table"));
				ZEntry.CompressionMethodIndex = (uint32)OodleIndex;
				ZEntry.UncompressedSize = TotalBytes;
				ZEntry.CompressionBlockSize = (uint32)SliceBytes;
				for (int32 S = 0; S < SliceCount; ++S)
				{
					FPakWriter::FStoredBlock B;
					B.Bytes = BlocksA[S];
					ZEntry.Blocks.Add(MoveTemp(B));
				}
				FPakEntry Out;
				Check(W.AppendEntry(ZEntry, Out) && W.Commit(), TEXT("zcomp: compressed multi-block entry appended"));
				Check(Out.CompressionBlocks.Num() == SliceCount, TEXT("zcomp: entry stored as 3 blocks"));
			}
			// v2: same base, but the entry carries the B content blocks (the
			// stored size differs between versions - the real update case).
			PF.CopyFile(*ZPakV2, *InstalledPak);
			{
				FPakWriter W;
				Check(W.Open(ZPakV2), TEXT("zcomp: v2 writer opens"));
				FPakWriter::FEntryToWrite ZEntry;
				ZEntry.Path = TEXT("Content/ZCompressed.bin");
				const int32 OodleIndexV2 = W.RegisterCompressionMethod(TEXT("Oodle"));
				Check(OodleIndexV2 > 0, TEXT("zcomp: v2 Oodle registered"));
				ZEntry.CompressionMethodIndex = (uint32)OodleIndexV2;
				ZEntry.UncompressedSize = TotalBytes;
				ZEntry.CompressionBlockSize = (uint32)SliceBytes;
				for (int32 S = 0; S < SliceCount; ++S)
				{
					FPakWriter::FStoredBlock B;
					B.Bytes = BlocksB[S];
					ZEntry.Blocks.Add(MoveTemp(B));
				}
				FPakEntry Out;
				Check(W.AppendEntry(ZEntry, Out) && W.Commit(), TEXT("zcomp: v2 compressed entry appended"));
			}

			// Read back: decompressed bytes identical, payload hash consistent.
			{
				FPakReader R;
				Check(R.Open(ZPakV1) && R.ReadIndex(), TEXT("zcomp: v1 re-opens"));
				const FPakReader::FPakEntryLocation* Loc = nullptr;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path.EndsWith(TEXT("ZCompressed.bin"))) { Loc = &L; }
				}
				Check(Loc != nullptr, TEXT("zcomp: compressed entry found"));
				if (Loc)
				{
					Check(Loc->Entry.CompressionMethodIndex > 0 && Loc->Entry.CompressionBlocks.Num() == SliceCount,
						TEXT("zcomp: entry is Oodle multi-block on disk"));
					TArray64<uint8> Data;
					Check(R.ReadEntryData(Loc->Entry, Data) && Data.Num() == TotalBytes &&
						FMemory::Memcmp(Data.GetData(), ContentA.GetData(), TotalBytes) == 0,
						TEXT("zcomp: decompressed bytes identical to the source"));
					uint8 Computed[20] = { 0 };
					Check(R.ComputePayloadHash(Loc->Entry, Computed), TEXT("zcomp: payload hash computes"));
					uint8 Stored[20] = { 0 };
					Check(R.ReadPayloadHash(Loc->Entry, Stored) && FMemory::Memcmp(Stored, Computed, 20) == 0,
						TEXT("zcomp: built-in hash == computed payload hash"));
				}
			}

			// Ingest both versions and run the UPDATE flow: the compressed
			// entry's stored size changes between versions, so the update
			// appends a raw re-store (the production main path) and the final
			// container decodes to the v2 content.
			FIngestResult ZIngestResult;
			auto IngestZ = [&](const TCHAR* Ver, const FString& PakFile) -> bool
			{
				const FString StagedDir = FPaths::Combine(WorkDir, FString::Printf(TEXT("zstaged_%s"), Ver));
				if (!PF.DirectoryExists(*StagedDir) && !PF.CreateDirectoryTree(*StagedDir))
				{
					return false;
				}
				const FString Dst = FPaths::Combine(StagedDir, TEXT("game.pak"));
				PF.DeleteFile(*Dst);
				return PF.CopyFile(*Dst, *PakFile) &&
					FBuildIngestor::Ingest(StagedDir, TEXT("InplaceTest"), Ver, FPaths::Combine(WorkDir, TEXT("cdn")), ZIngestResult);
			};
			Check(IngestZ(TEXT("1.0"), ZPakV1), TEXT("zcomp: v1 ingested"));
			Check(IngestZ(TEXT("1.1"), ZPakV2), TEXT("zcomp: v2 ingested"));

			const FString ZInstall = FPaths::Combine(WorkDir, TEXT("zinstall"));
			PF.CreateDirectoryTree(*ZInstall);
			FUpdateStats ZS1;
			Check(FUpdateOrchestrator::Update(FPaths::Combine(WorkDir, TEXT("cdn")), TEXT("1.0"), ZInstall, true, ZS1),
						TEXT("zcomp: fresh install of the compressed container succeeds"));
			{
				TArray64<uint8> Data;
				FPakReader R;
				const FString ZPak = FPaths::Combine(ZInstall, TEXT("game.pak"));
				Check(R.Open(ZPak) && R.ReadIndex(), TEXT("zcomp: installed compressed container opens"));
				const FPakReader::FPakEntryLocation* Loc = nullptr;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path.EndsWith(TEXT("ZCompressed.bin"))) { Loc = &L; }
				}
				Check(Loc != nullptr && R.ReadEntryData(Loc->Entry, Data) && Data.Num() == TotalBytes &&
					FMemory::Memcmp(Data.GetData(), ContentA.GetData(), TotalBytes) == 0,
					TEXT("zcomp: installed content decodes to v1 bytes"));
			}

			FUpdateStats ZS2;
			Check(FUpdateOrchestrator::Update(FPaths::Combine(WorkDir, TEXT("cdn")), TEXT("1.1"), ZInstall, true, ZS2),
						TEXT("zcomp: update to v2 (stored-size change -> raw re-store) succeeds"));
			{
				TArray64<uint8> Data;
				FPakReader R;
				const FString ZPak = FPaths::Combine(ZInstall, TEXT("game.pak"));
				Check(R.Open(ZPak) && R.ReadIndex(), TEXT("zcomp: updated container opens"));
				const FPakReader::FPakEntryLocation* Loc = nullptr;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path.EndsWith(TEXT("ZCompressed.bin"))) { Loc = &L; }
				}
				Check(Loc != nullptr && R.ReadEntryData(Loc->Entry, Data) && Data.Num() == TotalBytes &&
					FMemory::Memcmp(Data.GetData(), ContentB.GetData(), TotalBytes) == 0,
					TEXT("zcomp: updated content decodes to v2 bytes"));
			}

			// --- NEW compressed entry (remote-only): it can be neither
			// appended raw (method-0 append would hand the engine compressed
			// bytes as content - the append path has no block table) nor
			// overwritten. The patcher must bail to the whole-file rebuild,
			// which reproduces the real compressed layout. Before this bail
			// existed, the new compressed entry was appended as method-0
			// garbage that passed every per-entry hash check.
			const FString ZPakV3 = FPaths::Combine(WorkDir, TEXT("zcomp_v3.pak"));
			PF.CopyFile(*ZPakV3, *ZPakV2);
			{
				FPakWriter W;
				Check(W.Open(ZPakV3), TEXT("zcomp: v3 writer opens"));
				FPakWriter::FEntryToWrite ZEntry;
				ZEntry.Path = TEXT("Content/ZAdded.bin");
				const int32 OodleIndexV3 = W.RegisterCompressionMethod(TEXT("Oodle"));
				Check(OodleIndexV3 > 0, TEXT("zcomp: v3 Oodle registered"));
				ZEntry.CompressionMethodIndex = (uint32)OodleIndexV3;
				ZEntry.UncompressedSize = TotalBytes;
				ZEntry.CompressionBlockSize = (uint32)SliceBytes;
				for (int32 S = 0; S < SliceCount; ++S)
				{
					FPakWriter::FStoredBlock B;
					B.Bytes = BlocksB[S];
					ZEntry.Blocks.Add(MoveTemp(B));
				}
				FPakEntry Out;
				Check(W.AppendEntry(ZEntry, Out) && W.Commit(), TEXT("zcomp: v3 adds a NEW compressed entry"));
			}
			Check(IngestZ(TEXT("1.2"), ZPakV3), TEXT("zcomp: v3 ingested"));
			{
				FUpdateStats ZS3;
				Check(FUpdateOrchestrator::Update(FPaths::Combine(WorkDir, TEXT("cdn")), TEXT("1.2"), ZInstall, true, ZS3),
					TEXT("zcomp: update with a NEW compressed entry succeeds (in-place bailed to rebuild)"));
				Check(ZS3.NumFilesChanged == 1, TEXT("zcomp: the pak went through the rebuild path"));
				TArray64<uint8> Data;
				FPakReader R;
				const FString ZPak = FPaths::Combine(ZInstall, TEXT("game.pak"));
				Check(R.Open(ZPak) && R.ReadIndex(), TEXT("zcomp: v3 container opens"));
				const FPakReader::FPakEntryLocation* Loc = nullptr;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path.EndsWith(TEXT("ZAdded.bin"))) { Loc = &L; }
				}
				Check(Loc != nullptr && Loc->Entry.CompressionMethodIndex > 0 &&
					R.ReadEntryData(Loc->Entry, Data) && Data.Num() == TotalBytes &&
					FMemory::Memcmp(Data.GetData(), ContentB.GetData(), TotalBytes) == 0,
					TEXT("zcomp: the added entry is COMPRESSED on disk and decodes correctly"));
			}
		}

		// --- 6. Fail-after-pak-patch (ROADMAP §5.3): the footer flip is the
		// commit point - once it lands, the container IS the new version even
		// if the update fails afterwards. The orchestrator must not back up or
		// roll back the .pak (the container is its own WAL): no .pak backup on
		// disk, the patched container survives the rollback intact, and the
		// rerun converges without re-writing container bytes.
		FString IndexV13;
		FIndexManifest Remote13;
		{
			auto ReadStatus = [&]() -> ETransactionStatus
			{
				FPatcherState S;
				if (!FPatcherStateFile::Load(Paths::GetStatePath(Install), S))
				{
					return ETransactionStatus::None;
				}
				return S.Transaction.Status;
			};

			// 1.3 = v2 + backfilled.txt (so the entry set is a superset of the
			// installed 1.2 - no deletions) + one more appended entry. A
			// version that REMOVES a pak entry takes the rebuild path instead
			// (section 8); here the in-place CoW path is the one under test.
			const FString V4Pak = FPaths::Combine(WorkDir, TEXT("v4.pak"));
			PF.CopyFile(*V4Pak, *V2Pak);
			{
				FPakWriter W;
				Check(W.Open(V4Pak), TEXT("failpak: v4 writer opens"));
				FPakWriter::FEntryToWrite Backfill;
				Backfill.Path = TEXT("backfilled.txt");
				Backfill.CompressionMethodIndex = 0;
				Backfill.UncompressedSize = 12;
				FPakWriter::FStoredBlock BackfillBlock;
				BackfillBlock.Bytes.SetNumUninitialized(12);
				FMemory::Memcpy(BackfillBlock.Bytes.GetData(), "backfilled!!", 12);
				Backfill.Blocks.Add(MoveTemp(BackfillBlock));
				FPakEntry BackfillOut;
				Check(W.AppendEntry(Backfill, BackfillOut), TEXT("failpak: backfill entry appended"));
				FPakWriter::FEntryToWrite Extra;
				Extra.Path = TEXT("failtest.txt");
				Extra.CompressionMethodIndex = 0;
				Extra.UncompressedSize = 8;
				FPakWriter::FStoredBlock Block;
				Block.Bytes.SetNumUninitialized(8);
				FMemory::Memcpy(Block.Bytes.GetData(), "failtest", 8);
				Extra.Blocks.Add(MoveTemp(Block));
				FPakEntry Out;
				Check(W.AppendEntry(Extra, Out) && W.Commit(), TEXT("failpak: v4 committed"));
			}
			Check(CraftAndIngest(TEXT("1.3"), V4Pak, TEXT("game.pak"), IndexV13), TEXT("failpak: ingest v4 as 1.3"));
			Check(FIndexManifestFile::Load(IndexV13, Remote13), TEXT("failpak: 1.3 index loads"));

			FUpdateOrchestrator::TestFailAfterWrites = 1;
			FUpdateStats SFail;
			const bool bFailed = !FUpdateOrchestrator::Update(Cdn, TEXT("1.3"), Install, true, SFail);
			FUpdateOrchestrator::TestFailAfterWrites = 0;
			Check(bFailed, TEXT("failpak: injected failure after the pak patch makes update fail"));
			Check(ReadStatus() == ETransactionStatus::RolledBack, TEXT("failpak: sidecar reports rolled-back"));
			{
				// No .pak ever lands in the backup dir: the container is its
				// own rollback mechanism (footer commit + recovery scanner).
				bool bPakBackup = false;
				const FString BackupDir = FPaths::Combine(Install, TEXT(".ugp"), TEXT("backup"));
				TArray<FString> BackupFiles;
				PF.FindFilesRecursively(BackupFiles, *BackupDir, TEXT("*.pak"));
				if (PF.DirectoryExists(*BackupDir))
				{
					bPakBackup = BackupFiles.Num() > 0;
				}
				Check(!bPakBackup, TEXT("failpak: the patched pak was never backed up"));
			}
			{
				// The CoW commit survived the rollback: the container on disk
				// already carries the 1.3 entry set and parses cleanly.
				FPakReader R;
				Check(R.Open(InstalledPak) && R.ReadIndex(), TEXT("failpak: patched container survived the rollback"));
				bool bHasNew = false;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path.Contains(TEXT("failtest.txt"))) { bHasNew = true; }
				}
				Check(bHasNew, TEXT("failpak: the committed entry is in the surviving container"));
			}

			// Rerun (as the next launch): the disk container already matches
			// the 1.3 metadata, so the rerun converges with zero writes and
			// the sidecar commits the new version.
			FUpdateStats SRetry;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.3"), Install, true, SRetry), TEXT("failpak: rerun after rollback succeeds"));
			Check(SRetry.BytesWritten == 0, TEXT("failpak: rerun re-writes nothing (container already committed)"));
			Check(CheckContainerLogicallyEqual(Remote13, Install, TEXT("game.pak")), TEXT("failpak: container logically equal to 1.3"));
			Check(ReadStatus() == ETransactionStatus::Committed, TEXT("failpak: sidecar committed after the rerun"));
		}

		// --- 7. Unrecoverable container: no valid footer anywhere, so the
		// scanner reports Unrecoverable, the orchestrator deletes the pak for
		// the whole-file rebuild, and the rebuilt file is byte-identical to
		// the CDN copy.
		{
			// A zero-length file is the degenerate Unrecoverable case (the
			// real-world "container zeroed by a failing disk / crashed writer").
			IFileHandle* Truncate = PF.OpenWrite(*InstalledPak); // CREATE_ALWAYS: 0 bytes
			Check(Truncate != nullptr, TEXT("zeropak: opened for truncation"));
			delete Truncate;
			Check(IFileManager::Get().FileSize(*InstalledPak) == 0, TEXT("zeropak: container is zero bytes"));

			FUpdateStats SZero;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.3"), Install, true, SZero), TEXT("zeropak: update over a zeroed pak succeeds"));
			Check(SZero.NumFilesAdded + SZero.NumFilesChanged == 1, TEXT("zeropak: exactly the pak rebuilt"));
			Check(SZero.BytesWritten == Remote13.Files[TEXT("game.pak")].Size, TEXT("zeropak: the whole container was re-written"));
			Check(FPatcherHash::FileSHA1Hex(InstalledPak) == Remote13.Files[TEXT("game.pak")].SHA1,
				TEXT("zeropak: rebuilt container is byte-identical to the CDN copy"));

			// Convergence: the rerun is a no-op.
			FUpdateStats SZero2;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.3"), Install, true, SZero2), TEXT("zeropak: rerun converges"));
			Check(SZero2.NumFilesChanged == 0 && SZero2.BytesWritten == 0, TEXT("zeropak: rerun is a no-op"));
		}

		// --- 8. Removed pak entry: the CoW append path cannot express a
		// deletion (the re-committed index would still carry the stale
		// entry), so the patcher must refuse and the orchestrator must take
		// the whole-file rebuild - otherwise the container never converges to
		// the target metadata while every update reports success.
		{
			// 1.4 = v2 + backfilled.txt only: vs the installed 1.3 (v2 +
			// backfilled.txt + failtest.txt) the failtest entry is GONE.
			const FString V5Pak = FPaths::Combine(WorkDir, TEXT("v5.pak"));
			PF.CopyFile(*V5Pak, *V2Pak);
			{
				FPakWriter W;
				Check(W.Open(V5Pak), TEXT("entryremove: v5 writer opens"));
				FPakWriter::FEntryToWrite Backfill;
				Backfill.Path = TEXT("backfilled.txt");
				Backfill.CompressionMethodIndex = 0;
				Backfill.UncompressedSize = 12;
				FPakWriter::FStoredBlock BackfillBlock;
				BackfillBlock.Bytes.SetNumUninitialized(12);
				FMemory::Memcpy(BackfillBlock.Bytes.GetData(), "backfilled!!", 12);
				Backfill.Blocks.Add(MoveTemp(BackfillBlock));
				FPakEntry Out;
				Check(W.AppendEntry(Backfill, Out) && W.Commit(), TEXT("entryremove: v5 committed"));
			}
			FString IndexV14;
			Check(CraftAndIngest(TEXT("1.4"), V5Pak, TEXT("game.pak"), IndexV14), TEXT("entryremove: ingest v5 as 1.4"));
			FIndexManifest Remote14;
			Check(FIndexManifestFile::Load(IndexV14, Remote14), TEXT("entryremove: 1.4 index loads"));

			FUpdateStats SRemoveEntry;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.4"), Install, true, SRemoveEntry), TEXT("entryremove: removal update succeeds"));
			Check(SRemoveEntry.NumFilesChanged == 1, TEXT("entryremove: exactly the pak changed"));
			// The rebuild rewrites the whole container; the CoW path would
			// have written only the changed bytes (here: none).
			Check(SRemoveEntry.BytesWritten == Remote14.Files[TEXT("game.pak")].Size,
				TEXT("entryremove: the whole container was rebuilt (CoW cannot remove entries)"));
			Check(CheckContainerLogicallyEqual(Remote14, Install, TEXT("game.pak")),
				TEXT("entryremove: container logically equal to 1.4"));
			{
				FPakReader R;
				Check(R.Open(InstalledPak) && R.ReadIndex(), TEXT("entryremove: container opens"));
				bool bHasRemoved = false;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path.Contains(TEXT("failtest.txt"))) { bHasRemoved = true; }
				}
				Check(!bHasRemoved, TEXT("entryremove: the removed entry is gone from the live index"));
			}

			// Convergence: the rerun is a no-op.
			FUpdateStats SRemoveEntry2;
			Check(FUpdateOrchestrator::Update(Cdn, TEXT("1.4"), Install, true, SRemoveEntry2), TEXT("entryremove: rerun converges"));
			Check(SRemoveEntry2.NumFilesChanged == 0 && SRemoveEntry2.BytesWritten == 0, TEXT("entryremove: rerun is a no-op"));
		}

				if (bAllPassed)
		{
			PF.DeleteDirectoryRecursively(*WorkDir);
		}
		return FinishTest(bAllPassed, TEXT("pakinplace"));
	
	}

}
