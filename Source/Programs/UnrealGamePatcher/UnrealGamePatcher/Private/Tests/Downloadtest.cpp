// Copyright Epic Games, Inc. All Rights Reserved.
// -cli downloadtest —— HTTP 下载层（下载层，130+ 项）
// 验证点：Range 请求/journal 位图续传/窗口并发/超时重试/响应上限/
//         探测回退（无 Range 服务器整文件 GET）/自定义后端；
//         H2b 自然 journal 中断续传（只补缺失段）；H2c 零在飞任务的
//         Cancel 立即 OnDone（M23 修复钉）；H2d 批次停滞看门狗即失败
//         （N-C2 修复钉）；坏缓存块重取。

#include "Tests/TestFramework.h"

namespace UnrealGamePatcher::Tests
{
	int32 RunDownloadTest(const TCHAR* /*CmdLine*/)
	{

		// Download layer acceptance (tasks 6.1-6.6): the real HTTP backend
		// against an in-process HTTP server with Range support and fault
		// injection. Covers basic fetch + verify, cache reuse, byte-range
		// resume, concurrency, retry/limit with no corrupt bytes landing,
		// end-to-end network update, a custom downloader backend driving the
		// full update flow, and occupied-file handling with rollback.
		bool bAllPassed = true;
		auto Check = MakeCheck(bAllPassed);

		using namespace UnrealGamePatcher;

		const FString WorkDir = GetTestWorkDir(TEXT("downloadtest"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

		const FString Cdn = FPaths::Combine(WorkDir, TEXT("cdn"));
		const FString Install = FPaths::Combine(WorkDir, TEXT("install"));
		const FString Cache = FPaths::Combine(WorkDir, TEXT("cache"));
		PF.CreateDirectoryTree(*Install);
		PF.CreateDirectoryTree(*Cache);

		FIndexManifest Index;
		Check(BuildFixtureCdn(WorkDir, Cdn, Index), TEXT("downloadtest: ingest staged fixture"));

		// Serve the CDN over HTTP with the EXTERNAL python server (the patcher
		// contains no HTTP server; Scripts/cdn_server.py provides it).
		const FString ProjectRoot = GetProjectRootFromExecutable();
		const FString CdnScript = FPaths::Combine(ProjectRoot, TEXT("Source"), TEXT("Programs"), TEXT("UnrealGamePatcher"), TEXT("Scripts"), TEXT("cdn_server.py"));
		FCdnServerProc CdnServer;
		Check(CdnServer.Start(CdnScript, Cdn, FPaths::Combine(WorkDir, TEXT("cdn.log"))),
			TEXT("downloadtest: external CDN server started"));
		const FString BaseUrl = FString::Printf(TEXT("http://127.0.0.1:%d"), CdnServer.Port);

		// Force-load the HTTP module on the game thread before any callback
		// thread touches it (FHttpModule::Get() asserts off the game thread
		// on first use).
		FHttpModule::Get();
		FHttpChunkDownloader Http;
		FDownloadManager Manager(Http, /*MaxConcurrent=*/8, /*MaxRetries=*/3);

		// Single-request probe on the game thread: smoke-tests the transport
		// before the executor batches below settle on the HttpThread.
		{
			const FString FirstFile = Index.Chunks.CreateConstIterator().Value().File;
			const FString ProbeUrl = FString::Printf(TEXT("%s/%s"), *BaseUrl, *FirstFile);
			TArray64<uint8> Probe;
			const bool bProbe = Http.DownloadRange(ProbeUrl, 0, -1, Probe);
			Check(bProbe && Probe.Num() > 0, TEXT("A-probe: game-thread HTTP request succeeds"));
			if (bProbe)
			{
				Check(Probe.Num() == Index.Chunks.CreateConstIterator().Value().Size, TEXT("A-probe: probe returned the full chunk file"));
			}

		}

		auto MakeChunkRequests = [&](const FString& CacheDir, TArray<FDownloadRequest>& OutRequests)
		{
			OutRequests.Reset();
			for (const TPair<FString, FIndexChunkEntry>& Pair : Index.Chunks)
			{
				FDownloadRequest Req;
				Req.Id = Pair.Key;
				Req.Url = FString::Printf(TEXT("%s/%s"), *BaseUrl, *Pair.Value.File);
				Req.Size = Pair.Value.Size;
				Req.DestFile = FPaths::Combine(CacheDir, Pair.Value.File);
				OutRequests.Add(Req);
			}
		};

		// --- A. Basic download + verify + cache reuse (6.5/6.1) -------------
		{
			// The fixture's chunk count is content-defined (CDC), not a fixed
			// 1 MiB window count - assert relative to the index, never a pin.
			const int32 NumFixtureChunks = Index.Chunks.Num();
			TArray<FDownloadRequest> Requests;
			MakeChunkRequests(Cache, Requests);
			// A light per-connection throttle makes the concurrency evidence
			// deterministic: on an unthrottled loopback the transfers can
			// finish inside the connection-setup window and the peak overlap
			// stays at 1 even though the manager issued everything in
			// parallel (same flake class the H3 throttle fixed). Disarmed
			// right after the Peak check.
			{
				TArray64<uint8> Ctrl;
				Http.DownloadRange(FString::Printf(TEXT("%s/__ctl/throttle?kbps=32768"), *BaseUrl), 0, -1, Ctrl);
			}
			FDownloadStats Stats;
			TArray<FString> Failed;
			Check(Manager.DownloadAll(Requests, Stats, Failed) && Stats.SkippedExisting == 0,
				*FString::Printf(TEXT("A: all %d chunks downloaded over HTTP"), NumFixtureChunks));
			Check(CdnServer.Stats().Peak >= 2, TEXT("A: server saw overlapping connections (real concurrency)"));
			{
				TArray64<uint8> Ctrl;
				Http.DownloadRange(FString::Printf(TEXT("%s/__ctl/throttle?kbps=0"), *BaseUrl), 0, -1, Ctrl);
			}

			bool bAllCached = true;
			for (const FDownloadRequest& Req : Requests)
			{
				if (!PF.FileExists(*Req.DestFile) || IFileManager::Get().FileSize(*Req.DestFile) != Req.Size)
				{
					bAllCached = false;
				}
			}
			Check(bAllCached, TEXT("A: every chunk cached with the right size"));

			// Every cached chunk file passes the loader (content verified).
			bool bLoadOk = true;
			for (const TPair<FString, FIndexChunkEntry>& Pair : Index.Chunks)
			{
				TArray64<uint8> Data;
				if (!FChunkDataLoader::Load(FPaths::Combine(Cache, Pair.Value.File), Pair.Value, Data))
				{
					bLoadOk = false;
				}
			}
			Check(bLoadOk, TEXT("A: every downloaded chunk passes content verification"));

			const int32 ReqAfterFirst = CdnServer.Stats().Files;

			// A second pass must be served entirely from cache.
			FDownloadStats Stats2;
			TArray<FString> Failed2;
			Check(Manager.DownloadAll(Requests, Stats2, Failed2), TEXT("A: second download pass succeeds"));
			Check(Stats2.SkippedExisting == NumFixtureChunks && Stats2.BytesDownloaded == 0,
				TEXT("A: second pass served from cache (0 bytes over the wire)"));
			Check(CdnServer.Stats().Files == ReqAfterFirst, TEXT("A: no new HTTP requests on cache hit"));
		}

		// --- B. Journal bitmap resume of an interrupted file (6.1) ----------
		{
			// Simulate an interrupted download exactly the way the executor
			// leaves it: a preallocated staging file with the first two 4MB
			// segments on disk + a journal bitmap marking them done. The file
			// is 9MB, so the missing tail is one 1MB segment - only that may
			// be re-fetched, over a 206 Range request.
			const int64 BigTotal = 9ll << 20;
			const int64 SegSize = 4ll << 20;
			const FString BigName = TEXT("chunks/resume-tail-big.chunk");
			const FString Dest = FPaths::Combine(Cache, BigName);
			const FString Staging = Dest + TEXT(".download");
			const FString Journal = Dest + TEXT(".journal");

			TArray64<uint8> Whole;
			Whole.SetNumUninitialized(BigTotal);
			{
				uint8* P = Whole.GetData();
				for (int64 I = 0; I < BigTotal; ++I)
				{
					P[I] = (uint8)(I * 31 + (I >> 13));
				}
			}
			Check(FFileHelper::SaveArrayToFile(Whole, *FPaths::Combine(Cdn, BigName)), TEXT("B: synthetic 9MB chunk staged on the CDN"));

			{
				IFileHandle* H = PF.OpenWrite(*Staging);
				Check(H != nullptr, TEXT("B: staging file opened"));
				if (H)
				{
					H->Seek(BigTotal - 1); // preallocate to the full size
					uint8 Zero = 0;
					H->Write(&Zero, 1);
					H->Seek(0); // first two segments are on disk
					H->Write(Whole.GetData(), (int32)(SegSize * 2));
					delete H;
				}
			}
			{
				const FString Text = FString::Printf(TEXT("UGPJRN1 %lld %lld\nSEG 0\nSEG 1\n"), SegSize, BigTotal);
				Check(FFileHelper::SaveStringToFile(Text, *Journal, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("B: journal written"));
			}

			TArray<FDownloadRequest> Requests;
			FDownloadRequest Big;
			Big.Id = TEXT("resume-tail-big");
			Big.Url = FString::Printf(TEXT("%s/%s"), *BaseUrl, *BigName);
			Big.Size = BigTotal;
			Big.DestFile = Dest;
			Requests.Add(Big);

			FDownloadStats Stats;
			TArray<FString> Failed;
			Check(Manager.DownloadAll(Requests, Stats, Failed), TEXT("B: resume download succeeds"));

			const int64 FinalSize = IFileManager::Get().FileSize(*Dest);
			Check(FinalSize == BigTotal, TEXT("B: resumed file has the full size"));
			bool bBResumeOk = false;
			{
				// The promoted file must match the CDN copy byte for byte.
				TArray64<uint8> Final;
				bBResumeOk = FFileHelper::LoadFileToArray(Final, *Dest) && Final.Num() == BigTotal &&
					FMemory::Memcmp(Final.GetData(), Whole.GetData(), BigTotal) == 0;
			}
			Check(bBResumeOk, TEXT("B: resumed content matches the CDN copy"));
			Check(!PF.FileExists(*Staging) && !PF.FileExists(*Journal), TEXT("B: staged file promoted (no .download/.journal left)"));

			// The server must have served a 206 Range request for the tail.
			{
				const FCdnServerProc::FCdnStats BStats = CdnServer.Stats();
				Check(BStats.Ranges > 0, TEXT("B: resume used a 206 Range request"));
				Check(BStats.MaxRangeBytes == BigTotal - SegSize * 2, TEXT("B: only the missing tail was re-fetched"));
			}
		}

		// --- D. Retry with cap + no corrupt data on disk (6.3) --------------
		{
			const FString CacheD = FPaths::Combine(WorkDir, TEXT("cacheD"));
			PF.CreateDirectoryTree(*CacheD);
			TArray<FDownloadRequest> Requests;
			MakeChunkRequests(CacheD, Requests);

			// Arm a one-shot 500 on the next request via the control endpoint,
			// then the batch must retry and succeed.
			{
				TArray64<uint8> Ctrl;
				Http.DownloadRange(FString::Printf(TEXT("%s/__ctl/fail_next"), *BaseUrl), 0, -1, Ctrl);
			}
			FDownloadStats Stats;
			TArray<FString> Failed;
			FDownloadManager ManagerD(Http, /*MaxConcurrent=*/1, /*MaxRetries=*/3);
			Check(ManagerD.DownloadAll(Requests, Stats, Failed), TEXT("D: batch succeeds after a retried 500"));
			Check(Stats.Retries >= 1 && CdnServer.Stats().Errors > 0, TEXT("D: a transport retry was issued for a 500"));

			// Permanent failure: a 404 for one chunk fails the whole batch,
			// reports the id, and leaves no DestFile.
			FString Guid;
			for (const auto& BadPair : Index.Chunks) { Guid = BadPair.Key; break; }
			const FIndexChunkEntry& Chunk = Index.Chunks[Guid];
			FDownloadRequest Bad;
			Bad.Id = Guid;
			Bad.Url = FString::Printf(TEXT("%s/chunks/missing_%s.chunk"), *BaseUrl, *Guid);
			Bad.Size = Chunk.Size;
			Bad.DestFile = FPaths::Combine(CacheD, TEXT("missing.chunk"));
			TArray<FDownloadRequest> BadBatch = Requests;
			BadBatch.Add(Bad);

			FDownloadStats StatsBad;
			TArray<FString> FailedBad;
			FDownloadManager ManagerBad(Http, /*MaxConcurrent=*/1, /*MaxRetries=*/1);
			const bool bBatchFailed = !ManagerBad.DownloadAll(BadBatch, StatsBad, FailedBad);
			Check(bBatchFailed, TEXT("D: batch fails when a chunk 404s past the retry cap"));
			Check(FailedBad.Num() == 1 && FailedBad[0] == Guid, TEXT("D: the failed chunk id is reported"));
			Check(!PF.FileExists(*Bad.DestFile), TEXT("D: no corrupt file landed on disk"));

			// Manager precheck refusals: a negative and an over-cap declared
			// size are refused BEFORE any transfer (zero HTTP requests).
			const int32 FilesBeforePrecheck = CdnServer.Stats().Files;
			TArray<FDownloadRequest> Insane;
			FDownloadRequest Negative = Bad;
			Negative.Id = TEXT("D-negative");
			Negative.Size = -5;
			Insane.Add(Negative);
			FDownloadRequest Huge = Bad;
			Huge.Id = TEXT("D-huge");
			Huge.Size = (256ll << 20) + 1; // one byte over the transport cap
			Insane.Add(Huge);
			FDownloadStats StatsInsane;
			TArray<FString> FailedInsane;
			Check(!ManagerBad.DownloadAll(Insane, StatsInsane, FailedInsane), TEXT("D: insane sizes refused"));
			Check(FailedInsane.Num() == 2, TEXT("D: both insane sizes reported"));
			Check(CdnServer.Stats().Files == FilesBeforePrecheck, TEXT("D: insane sizes made zero HTTP requests"));

			// Mid-body truncation fault: a short response is a retryable
			// transport failure - the batch still succeeds, nothing corrupt
			// lands. (truncate_next arms exactly one response.)
			{
				TArray64<uint8> Ctrl;
				Http.DownloadRange(FString::Printf(TEXT("%s/__ctl/truncate_next?bytes=100"), *BaseUrl), 0, -1, Ctrl);
			}
			// A FRESH cache dir forces real HTTP requests (cacheD is fully
			// warm by now - the truncation must hit a live transfer).
			const FString CacheDTrunc = FPaths::Combine(WorkDir, TEXT("cacheD-trunc"));
			PF.CreateDirectoryTree(*CacheDTrunc);
			TArray<FDownloadRequest> TruncRequests;
			MakeChunkRequests(CacheDTrunc, TruncRequests);
			FDownloadStats StatsTrunc;
			TArray<FString> FailedTrunc;
			FDownloadManager ManagerTrunc(Http, /*MaxConcurrent=*/1, /*MaxRetries=*/3);
			Check(ManagerTrunc.DownloadAll(TruncRequests, StatsTrunc, FailedTrunc), TEXT("D: batch succeeds after a truncated body"));
			Check(StatsTrunc.Retries >= 1, TEXT("D: a truncated body was retried"));
		}

		// --- E. End-to-end network update + interruption (6.1) --------------
		{
			FUpdateOptions Options;
			Options.Downloader = &Http;
			Options.BaseUrl = BaseUrl;
			Options.ChunkCacheDir = FPaths::Combine(WorkDir, TEXT("installcache"));

			FUpdateStats S1;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.0.0"), Install, true, Options, S1),
				TEXT("E: full network update succeeds"));
			Check(S1.NumFilesAdded == Index.Files.Num(), TEXT("E: every manifest file installed"));
			Check(S1.NumChunksFetched == Index.Chunks.Num(), TEXT("E: every chunk downloaded over HTTP"));

			bool bAllMatch = true;
			for (const TPair<FString, FIndexFileEntry>& Pair : Index.Files)
			{
				FString Sha1;
				int64 Size = 0;
				if (!FLocalTruthExtractor::HashWholeFile(FPaths::Combine(Install, Pair.Key), Sha1, Size) ||
					!Sha1.Equals(Pair.Value.SHA1, ESearchCase::IgnoreCase))
				{
					bAllMatch = false;
				}
			}
			Check(bAllMatch, TEXT("E: installed files hash-identical to the manifest"));

			// No-op rerun: everything served from the chunk cache.
			const int32 ReqBefore = CdnServer.Stats().Files;
			FUpdateStats S2;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.0.0"), Install, true, Options, S2),
				TEXT("E: no-op rerun succeeds"));
			Check(S2.NumFilesSame == Index.Files.Num() && S2.NumChunksFetched == 0, TEXT("E: no-op rerun fetches nothing"));
			Check(CdnServer.Stats().Files == ReqBefore, TEXT("E: rerun hit the cache, not the network"));

			// Interruption: drop one chunk from the cache and damage a file.
			// The rerun must re-fetch exactly that one chunk.
			// Drop one of the UCAS's chunks (the damaged container is rebuilt,
			// so exactly that one chunk is re-downloaded).
			FString DropGuid;
			for (const FIndexFileChunkRef& DropPart : Index.Files[TEXT("pakchunk1-Windows.ucas")].Parts) { DropGuid = DropPart.Chunk; break; }
			const FIndexChunkEntry& DropChunk = Index.Chunks[DropGuid];
			PF.DeleteFile(*(FPaths::Combine(Options.ChunkCacheDir, DropChunk.File)));

			{
				const FString UcasPath = FPaths::Combine(Install, TEXT("pakchunk1-Windows.ucas"));
				TArray64<uint8> Bytes;
				// Guarded: if the previous update aborted the install may be
				// absent - record the failure via the checks below instead of
				// crashing on an empty array.
				if (FFileHelper::LoadFileToArray(Bytes, *UcasPath) && Bytes.Num() > 0)
				{
					Bytes[Bytes.Num() / 2] ^= 0xFF;
					FFileHelper::SaveArrayToFile(Bytes, *UcasPath);
				}
			}

			// Same-size damage is only caught by verify/repair runs (trust
			// model) - force verify for this interruption+repair scenario.
			Options.bForceVerify = true;
			const int32 ReqBeforeRepair = CdnServer.Stats().Files;
			FUpdateStats S3;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.0.0"), Install, true, Options, S3),
				TEXT("E: repair over network succeeds"));
			Check(S3.NumFilesChanged == 2, TEXT("E: damaged ucas + sibling utoc rebuilt"));
			// The download cache is a TEMP store (no double data on the client),
			// so a later run re-fetches what the repair rebuilds over HTTP.
			Check(CdnServer.Stats().Files > ReqBeforeRepair, TEXT("E: repair re-downloaded the rebuilt files over HTTP"));
			{
				FString Sha1;
				int64 Size = 0;
				FLocalTruthExtractor::HashWholeFile(FPaths::Combine(Install, TEXT("pakchunk1-Windows.ucas")), Sha1, Size);
				Check(Sha1.Equals(Index.Files[TEXT("pakchunk1-Windows.ucas")].SHA1, ESearchCase::IgnoreCase),
					TEXT("E: damaged file repaired to the manifest hash"));
			}

			// --- Network container delta (optimistic-prefetch regression): a
			// small change to a CONTAINER over HTTP must fetch only the
			// changed entry's covering window. The old download phase pulled
			// the target's ENTIRE part set for any Changed file - a 4-byte
			// container edit cost the whole 10GB over the wire.
			Options.bForceVerify = false; // trust mode: exercises the optimistic path
			// Own install dir + cache: this scenario must not disturb the E
			// install that the G group continues from.
			const FString InstallND = FPaths::Combine(WorkDir, TEXT("installnetdelta"));
			Options.ChunkCacheDir = FPaths::Combine(WorkDir, TEXT("netdeltacache"));
			PF.CreateDirectoryTree(*InstallND);
			const FString NDPak = FPaths::Combine(InstallND, TEXT("pakchunk1-Windows.pak"));
			FUpdateStats SInstND;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.0.0"), InstallND, true, Options, SInstND),
				TEXT("netdelta: baseline install over HTTP succeeds"));

			const FString V2Pak = FPaths::Combine(WorkDir, TEXT("netdelta_v2.pak"));
			PF.CopyFile(*V2Pak, *NDPak);
			{
				FPakWriter W;
				Check(W.Open(V2Pak), TEXT("netdelta: writer opens"));
				FPakWriter::FEntryToWrite Extra;
				Extra.Path = TEXT("Content/netdelta.txt");
				FPakWriter::FStoredBlock Block;
				const ANSICHAR* Text = "network-delta";
				Block.Bytes.SetNumUninitialized(13);
				FMemory::Memcpy(Block.Bytes.GetData(), Text, 13);
				Extra.Blocks.Add(MoveTemp(Block));
				FPakEntry OutEntry;
				Check(W.AppendEntry(Extra, OutEntry) && W.Commit(), TEXT("netdelta: v2 pak crafted"));
			}
			{
				// The staged 1.1.0 dir carries ALL FOUR files (the modified pak
				// + unchanged ucas/utoc/boot) - a one-file manifest would be a
				// shrink release that deletes the rest of the install.
				const FString Staged = FPaths::Combine(WorkDir, TEXT("netdelta_staged"));
				PF.CreateDirectoryTree(*Staged);
				PF.CopyFile(*FPaths::Combine(Staged, TEXT("pakchunk1-Windows.pak")), *V2Pak);
				PF.CopyFile(*FPaths::Combine(Staged, TEXT("pakchunk1-Windows.ucas")), *FPaths::Combine(InstallND, TEXT("pakchunk1-Windows.ucas")));
				PF.CopyFile(*FPaths::Combine(Staged, TEXT("pakchunk1-Windows.utoc")), *FPaths::Combine(InstallND, TEXT("pakchunk1-Windows.utoc")));
				PF.CopyFile(*FPaths::Combine(Staged, TEXT("boot.txt")), *FPaths::Combine(InstallND, TEXT("boot.txt")));
				FIngestResult R;
				Check(FBuildIngestor::Ingest(Staged, TEXT("DownloadTest"), TEXT("1.1.0"), Cdn, R),
					TEXT("netdelta: 1.1.0 ingested into the HTTP-served CDN"));
			}
			const int32 ReqBeforeDelta = CdnServer.Stats().Files;
			FUpdateStats SDelta;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.1.0"), InstallND, true, Options, SDelta),
				TEXT("netdelta: container delta update over HTTP succeeds"));
			Check(SDelta.NumFilesChanged == 1 && SDelta.NumFilesSame == 3,
				TEXT("netdelta: exactly the pak changed, the rest Same"));
			// THE ASSERTION: one changed entry -> exactly ONE window chunk
			// fetched over HTTP (the whole-container prefetch would have
			// fetched the pak's full part set).
			Check(SDelta.NumChunksFetched == 1,
				TEXT("netdelta: ONLY the changed entry's window was fetched"));
			Check(CdnServer.Stats().Files - ReqBeforeDelta <= 3,
				TEXT("netdelta: server saw ~1 index + 1 chunk request"));
			{
				FPakReader R;
				Check(R.Open(NDPak) && R.ReadIndex(), TEXT("netdelta: patched pak opens"));
				bool bOk = false;
				for (const FPakReader::FPakEntryLocation& L : R.GetFiles())
				{
					if (L.Path.Contains(TEXT("netdelta.txt")))
					{
						TArray64<uint8> Payload;
						bOk = R.ReadEntryData(L.Entry, Payload) && Payload.Num() == 13 &&
							FMemory::Memcmp(Payload.GetData(), "network-delta", 13) == 0;
					}
				}
				Check(bOk, TEXT("netdelta: appended entry decodes"));
			}
			{
				FUpdateStats SD2;
				Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.1.0"), InstallND, true, Options, SD2),
					TEXT("netdelta: rerun converges"));
				Check(SD2.NumFilesSame == 4 && SD2.NumChunksFetched == 0, TEXT("netdelta: rerun is a no-op"));
			}
		}

		// --- Network pak delta at REALISTIC scale (review 2026-10-09 H1
		// regression). The netdelta scenario above runs on the 939B fixture
		// pak, which is a SINGLE CDC window: same and changed entries share
		// that one chunk, so the fetch-before-same-check bug was invisible
		// there (the same entries' "fetch" hit the cached window and the CoW
		// patch looked healthy). This scenario crafts a multi-window pak and
		// pins the CoW contract with I/O stats:
		//   (a) a changed 100KB entry of a ~4.3MB pak must be written CoW
		//       (append + index/footer), never a whole-container rewrite;
		//   (b) the chunk store must be read ONLY for the changed entry's
		//       covering windows (the old order fetched every SAME entry's
		//       bytes - in network mode those windows are deliberately not
		//       prefetched, so the patch aborted and silently fell back to a
		//       full-container download + rebuild).
		{
			const FString InstallBig = FPaths::Combine(WorkDir, TEXT("installbigpak"));
			// Own options (E's went out of scope with its block): HTTP-served
			// CDN, trust mode - the same shape the netdelta scenario used.
			FUpdateOptions BigOptions;
			BigOptions.Downloader = &Http;
			BigOptions.BaseUrl = BaseUrl;
			BigOptions.ChunkCacheDir = FPaths::Combine(WorkDir, TEXT("bigpakcache"));
			const FString BigPakName = TEXT("pakchunk1-Windows.pak");

			// Deterministic pseudo-random bytes (xorshift64) - random content
			// is what makes FastCDC split the pak into several windows (a
			// structured pattern could fail to cut at all).
			auto MakeRandomBytes = [](int64 Size, uint64 Seed)
			{
				TArray64<uint8> Bytes;
				Bytes.SetNumUninitialized(Size);
				uint64 S = Seed ? Seed : 0x9E3779B97F4A7C15ull;
				for (int64 I = 0; I < Size; ++I)
				{
					S ^= S << 13; S ^= S >> 7; S ^= S << 17;
					Bytes[(int32)I] = (uint8)(S >> 32);
				}
				return Bytes;
			};
			auto AddBigEntry = [&MakeRandomBytes](FPakWriter& W, const TCHAR* Path, int64 Size, uint64 Seed) -> bool
			{
				FPakWriter::FEntryToWrite Item;
				Item.Path = Path;
				FPakWriter::FStoredBlock Block;
				Block.Bytes = MakeRandomBytes(Size, Seed);
				Item.Blocks.Add(MoveTemp(Block));
				FPakEntry Out;
				return W.AppendEntry(Item, Out);
			};

			// v1: seed from the installed 939B fixture pak (2 entries) + the
			// 100KB entry v2 will change + two 2MB entries that stay identical.
			const FString BigV1 = FPaths::Combine(WorkDir, TEXT("bigpak_v1.pak"));
			PF.CopyFile(*BigV1, *FPaths::Combine(Install, BigPakName));
			{
				FPakWriter W;
				Check(W.Open(BigV1), TEXT("bigpak: v1 writer opens"));
				Check(AddBigEntry(W, TEXT("Content/big_a.bin"), 100 << 10, 1), TEXT("bigpak: v1 entry a"));
				Check(AddBigEntry(W, TEXT("Content/big_b.bin"), 2 << 20, 2), TEXT("bigpak: v1 entry b"));
				Check(AddBigEntry(W, TEXT("Content/big_c.bin"), 2 << 20, 3), TEXT("bigpak: v1 entry c"));
				Check(W.Commit(), TEXT("bigpak: v1 committed"));
			}

			const FString StagedV1 = FPaths::Combine(WorkDir, TEXT("bigpak_staged_v1"));
			PF.CreateDirectoryTree(*StagedV1);
			PF.CopyFile(*FPaths::Combine(StagedV1, BigPakName), *BigV1);
			FIngestResult RBig1;
			Check(FBuildIngestor::Ingest(StagedV1, TEXT("DownloadTest"), TEXT("1.2.0"), Cdn, RBig1),
				TEXT("bigpak: 1.2.0 ingested"));

			FUpdateStats SBig1;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.2.0"), InstallBig, true, BigOptions, SBig1),
				TEXT("bigpak: baseline install over HTTP"));
			Check(SBig1.NumFilesAdded == 1, TEXT("bigpak: single-file install"));

			// v2: 'big_a' re-appended with DIFFERENT content (the CoW replace-
			// generation path, same path); b/c and the two seed entries stay
			// byte-identical - they are the SAME majority the buggy order
			// would have tried to fetch.
			const FString BigV2 = FPaths::Combine(WorkDir, TEXT("bigpak_v2.pak"));
			PF.CopyFile(*BigV2, *BigV1);
			{
				FPakWriter W;
				Check(W.Open(BigV2), TEXT("bigpak: v2 writer opens"));
				Check(AddBigEntry(W, TEXT("Content/big_a.bin"), 100 << 10, 0xDEADBEEFull), TEXT("bigpak: v2 changed entry"));
				Check(W.Commit(), TEXT("bigpak: v2 committed"));
			}
			const FString StagedV2 = FPaths::Combine(WorkDir, TEXT("bigpak_staged_v2"));
			PF.CreateDirectoryTree(*StagedV2);
			PF.CopyFile(*FPaths::Combine(StagedV2, BigPakName), *BigV2);
			FIngestResult RBig2;
			Check(FBuildIngestor::Ingest(StagedV2, TEXT("DownloadTest"), TEXT("1.3.0"), Cdn, RBig2),
				TEXT("bigpak: 1.3.0 ingested"));

			FUpdateStats SBig2;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.3.0"), InstallBig, true, BigOptions, SBig2),
				TEXT("bigpak: container delta over HTTP succeeds"));
			Check(SBig2.NumFilesChanged == 1 && SBig2.NumFilesSame == 0, TEXT("bigpak: exactly the pak changed"));
			Check(SBig2.BytesWritten < (256 << 10),
				TEXT("bigpak: CoW wrote only the changed entry + index/footer (no whole-container rewrite)"));
			Check(SBig2.IOStats.ChunkLoads <= 3,
				TEXT("bigpak: chunk-store loads limited to the changed entry's covering windows"));
			{
				FIndexManifest BigIdx2;
				Check(FIndexManifestFile::Load(
					FPaths::Combine(Cdn, TEXT("index"), TEXT("1.3.0"), TEXT("UnrealGamePatcher.index.json")), BigIdx2),
					TEXT("bigpak: 1.3.0 index loads"));
				Check(CheckContainerLogicallyEqual(BigIdx2, InstallBig, BigPakName),
					TEXT("bigpak: patched pak logically equals 1.3.0"));
			}
			{
				FUpdateStats SBig3;
				Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.3.0"), InstallBig, true, BigOptions, SBig3),
					TEXT("bigpak: rerun converges"));
				Check(SBig3.NumFilesSame == 1 && SBig3.BytesWritten == 0 && SBig3.NumChunksFetched == 0,
					TEXT("bigpak: rerun is a no-op"));
			}
		}

		// --- F. Custom downloader backend drives the full update (6.6) -----
		{
			// A file-backed backend: ignores the transport, maps the URL path
			// onto a local CDN dir. The update flow must be identical.
			struct FFileChunkDownloader : public IChunkDownloader
			{
				FString Root;
				bool DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& OutData) override
				{
					// Extract the path after the scheme://authority part.
					FString Rest = Url;
					const int32 Scheme = Rest.Find(TEXT("://"), ESearchCase::CaseSensitive);
					if (Scheme != INDEX_NONE)
					{
						const int32 PathStart = Rest.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Scheme + 3);
						if (PathStart != INDEX_NONE)
						{
							Rest = Rest.RightChop(PathStart + 1);
						}
					}
					const FString Path = FPaths::Combine(Root, Rest);
					TArray64<uint8> Whole;
					if (!FFileHelper::LoadFileToArray(Whole, *Path))
					{
						return false;
					}
					if (Offset > 0 || (Size >= 0 && Size != Whole.Num()))
					{
						const int64 End = Size < 0 ? Whole.Num() : FMath::Min<int64>(Offset + Size, Whole.Num());
						OutData.SetNumUninitialized(End - Offset);
						FMemory::Memcpy(OutData.GetData(), Whole.GetData() + Offset, End - Offset);
					}
					else
					{
						OutData = MoveTemp(Whole);
					}
					return true;
				}
			};

			FFileChunkDownloader Custom;
			Custom.Root = Cdn;

			const FString Install2 = FPaths::Combine(WorkDir, TEXT("install2"));
			PF.CreateDirectoryTree(*Install2);
			FUpdateOptions Options;
			Options.Downloader = &Custom;
			Options.BaseUrl = TEXT("custom://cdn");
			Options.ChunkCacheDir = FPaths::Combine(WorkDir, TEXT("customcache"));

			FUpdateStats SF;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.0.0"), Install2, true, Options, SF),
				TEXT("F: custom backend drives the full update"));
			Check(SF.NumFilesAdded == 4, TEXT("F: 4 files installed via the custom backend"));
			{
				bool bAllMatch = true;
				for (const TPair<FString, FIndexFileEntry>& Pair : Index.Files)
				{
					FString Sha1;
					int64 Size = 0;
					if (!FLocalTruthExtractor::HashWholeFile(FPaths::Combine(Install2, Pair.Key), Sha1, Size) ||
						!Sha1.Equals(Pair.Value.SHA1, ESearchCase::IgnoreCase))
					{
						bAllMatch = false;
					}
				}
				Check(bAllMatch, TEXT("F: custom-backend install is hash-identical to the manifest"));
			}
		}

		// --- G. Occupied file: write fails, rollback, retry (6.4) -----------
		{
			const FString PakPath = FPaths::Combine(Install, TEXT("pakchunk1-Windows.pak"));

			// Corrupt a byte INSIDE the first entry's payload data (verify mode
			// recomputes per-entry payload hashes), then hold the pak open with
			// no sharing (simulating a running game locking its binaries).
			FString CorruptedSha1;
			{
				TArray64<uint8> Bytes;
				FFileHelper::LoadFileToArray(Bytes, *PakPath);
				{
					UnrealGamePatcher::FPakReader Locator;
					if (Locator.Open(PakPath) && Locator.ReadIndex())
					{
					const FPakReader::FPakEntryLocation& First = Locator.GetFiles()[0];
					const int64 HeaderSize = UnrealGamePatcher::FPakReader::GetSerializedEntrySize(First.Entry, Locator.GetInfo().Version);
					const int64 Flip = First.Entry.Offset + HeaderSize + (First.Entry.Size > 0 ? First.Entry.Size / 2 : 0);
					if (Flip < (int64)Bytes.Num())
					{
						Bytes[Flip] ^= 0xFF;
					}
					}
				}
				// The Locator (read handle) is destroyed above - its share flags
				// would block the write open below.
				FFileHelper::SaveArrayToFile(Bytes, *PakPath);
				FSHA1 H;
				H.Update(Bytes.GetData(), (int32)Bytes.Num());
				uint8 Hash[20] = { 0 };
				H.Final();
				H.GetHash(Hash);
				for (int32 I = 0; I < 20; ++I) { CorruptedSha1 += FString::Printf(TEXT("%02x"), Hash[I]); }
			}
			// Same-size corruption needs a verify run to be detected at all.
			FUpdateOptions VerifyOptions;
			VerifyOptions.bForceVerify = true;
#if PLATFORM_WINDOWS
			// Lock the file for reads only (share read, no write): the update
			// can analyze it but cannot replace it, so the write fails mid-
			// apply and the transaction must roll back. macOS has no mandatory
			// share locks (advisory flock does not block the patcher's opens),
			// so the locked-failure half of this case cannot exist there; the
			// rollback path itself is exercised by the other failure cases.
			HANDLE Lock = CreateFileW(*PakPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			Check(Lock != INVALID_HANDLE_VALUE, TEXT("G: file locked (read-only share)"));
			// Capture the terminal failure event: the unified error code must
			// classify the locked target as WriteLocked (not a generic failure).
			UnrealGamePatcher::EPatcherError FailCode = UnrealGamePatcher::EPatcherError::Ok;
			VerifyOptions.Events = [&FailCode](const UnrealGamePatcher::FUpdateEvent& E)
			{
				if (E.bIsError) { FailCode = E.Error; }
			};
			FUpdateStats SFail;
			const bool bFailed = !FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.0.0"), Install, true, VerifyOptions, SFail);
			Check(bFailed, TEXT("G: update fails while the file is locked"));
			Check(FailCode == UnrealGamePatcher::EPatcherError::WriteLocked,
				TEXT("G: failure classified as WriteLocked (unified error code)"));
			{
				FPatcherState State;
				FPatcherStateFile::Load(Paths::GetStatePath(Install), State);
				Check(State.Transaction.Status == ETransactionStatus::RolledBack, TEXT("G: transaction rolled back"));
			}
			// Unlock first: the locked file cannot even be read, and the
			// whole point is that a failed update left it untouched.
			CloseHandle(Lock);
			{
				// The file must be byte-identical to before the update
				// (still the corrupted bytes, not the manifest's).
				FString Sha1;
				int64 Size = 0;
				FLocalTruthExtractor::HashWholeFile(PakPath, Sha1, Size);
				Check(Sha1 == CorruptedSha1, TEXT("G: locked file unchanged after failed update"));
			}
#endif

			// Unlocked: the same verify update now repairs the file.
			FUpdateStats SOk;
			Check(FUpdateOrchestrator::UpdateWithOptions(Cdn, TEXT("1.0.0"), Install, true, VerifyOptions, SOk),
				TEXT("G: update succeeds after unlock"));
			{
				// Virtual-disk model: logically equivalent, not byte-identical.
				Check(CheckContainerLogicallyEqual(Index, Install, TEXT("pakchunk1-Windows.pak")),
					TEXT("G: pak repaired after unlock (per-entry hashes match remote)"));
			}
		}

		// --- H. Executor-level cases: segmentation / resume-only-missing /
		//     fairness / probe fallback (unified-downloader tasks 4.2-4.5) --
		{
			const int64 BigTotal = 9ll << 20; // the 9MB synthetic from group B
			const int64 SegSize = 4ll << 20;
			const FString BigName = TEXT("chunks/resume-tail-big.chunk");
			const FString BigUrl = FString::Printf(TEXT("%s/%s"), *BaseUrl, *BigName);

			// A recording fake backend: file-backed delivery with request
			// bookkeeping (offsets, sizes, count) - deterministic, no network.
			struct FRecordingBackend : public IChunkDownloader
			{
				FString Root;
				FCriticalSection StatsLock;
				TArray<int64> Offsets;
				TArray<int64> Sizes;
				int32 WholeFileGets = 0;

				bool DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& OutData) override
				{
					{
						FScopeLock Scope(&StatsLock);
						if (Offset == 0) { WholeFileGets += 1; }
						Offsets.Add(Offset);
						Sizes.Add(Size);
					}
					FString Rest = Url;
					const int32 Scheme = Rest.Find(TEXT("://"), ESearchCase::CaseSensitive);
					if (Scheme != INDEX_NONE)
					{
						const int32 PathStart = Rest.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Scheme + 3);
						if (PathStart != INDEX_NONE)
						{
							Rest = Rest.RightChop(PathStart + 1);
						}
					}
					TArray64<uint8> Whole;
					if (!FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Root, Rest)))
					{
						return false;
					}
					const int64 End = Size < 0 ? Whole.Num() : FMath::Min<int64>(Offset + Size, Whole.Num());
					OutData.SetNumUninitialized(End - Offset);
					FMemory::Memcpy(OutData.GetData(), Whole.GetData() + Offset, End - Offset);
					return true;
				}
			};

			// --- H1: multi-segment fetch of a >4MB file (4.2) ---------------
			{
				const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH1"), TEXT("big.chunk"));
				FRecordingBackend Backend;
				Backend.Root = Cdn;
				FDownloadExecutor Executor(Backend);

				std::atomic<bool> bDone{ false };
				FDownloadJob Job;
				Job.Id = TEXT("H1");
				Job.Url = BigUrl;
				Job.DestPath = Dest;
				Job.ExpectedSize = BigTotal;
				Executor.Submit(MoveTemp(Job), [&bDone](const FDownloadResult&) { bDone.store(true); });
				// The file backend settles inline - by the time Submit returns
				// the job has already run to completion.
				Check(bDone.load(), TEXT("H1: big-file job completed"));

				const bool bOkFile = IFileManager::Get().FileSize(*Dest) == BigTotal;
				Check(bOkFile, TEXT("H1: big file downloaded with the full size"));
				int32 SegmentCount = 0;
				int64 MaxSeg = 0;
				{
					FScopeLock Scope(&Backend.StatsLock);
					SegmentCount = Backend.Offsets.Num();
					for (const int64 S : Backend.Sizes) { MaxSeg = FMath::Max(MaxSeg, S); }
				}
				Check(SegmentCount == 3, TEXT("H1: 9MB file split into 3 segments (4MB quantum)"));
				Check(MaxSeg <= SegSize, TEXT("H1: no segment exceeds the 4MB quantum"));
				if (bOkFile)
				{
					TArray64<uint8> Final;
					TArray64<uint8> Whole;
					if (FFileHelper::LoadFileToArray(Final, *Dest) &&
						FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Cdn, BigName)) &&
						FMemory::Memcmp(Final.GetData(), Whole.GetData(), BigTotal) == 0)
					{
						Check(true, TEXT("H1: big-file content matches the CDN copy"));
					}
					else
					{
						Check(false, TEXT("H1: big-file content matches the CDN copy"));
					}
				}
			}

			// --- H2: journal resume fetches ONLY the missing segments (4.3) -
			{
				const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH2"), TEXT("big.chunk"));
				PF.CreateDirectoryTree(*FPaths::GetPath(Dest));
				const FString Staging = Dest + TEXT(".download");
				const FString Journal = Dest + TEXT(".journal");

				TArray64<uint8> Whole;
				FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Cdn, BigName));

				// Crash simulation: segment 0 is on disk + journaled; segments
				// 1-2 are missing. The restart must fetch exactly those two.
				bool bStaged = false;
				{
					IFileHandle* H = PF.OpenWrite(*Staging);
					if (H)
					{
						H->Seek(BigTotal - 1);
						uint8 Zero = 0;
						H->Write(&Zero, 1);
						H->Seek(0);
						H->Write(Whole.GetData(), (int32)SegSize);
						delete H;
						bStaged = true;
					}
				}
				Check(bStaged, TEXT("H2: staging file written"));
				const FString Text = FString::Printf(TEXT("UGPJRN1 %lld %lld\nSEG 0\n"), SegSize, BigTotal);
				Check(FFileHelper::SaveStringToFile(Text, *Journal, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM),
					TEXT("H2: journal written"));

				FRecordingBackend Backend;
				Backend.Root = Cdn;
				FDownloadExecutor Executor(Backend);

				FDownloadJob Job;
				Job.Id = TEXT("H2");
				Job.Url = BigUrl;
				Job.DestPath = Dest;
				Job.ExpectedSize = BigTotal;
				Executor.Submit(MoveTemp(Job), [](const FDownloadResult&) {});

				bool bOnlyMissing = false;
				int64 MaxRange = 0;
				{
					FScopeLock Scope(&Backend.StatsLock);
					bOnlyMissing = Backend.Offsets.Num() == 2 &&
						Backend.Offsets[0] == SegSize && Backend.Offsets[1] == SegSize * 2;
					for (const int64 S : Backend.Sizes) { MaxRange = FMath::Max(MaxRange, S); }
					if (!bOnlyMissing)
					{
						FString Off;
						for (const int64 O : Backend.Offsets) { Off += FString::Printf(TEXT(" %lld"), O); }
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("H2 offsets:%s"), *Off);
					}
				}
				Check(bOnlyMissing, TEXT("H2: only segments 1-2 were re-fetched (journal replay honored)"));
				Check(MaxRange <= SegSize, TEXT("H2: refetched segments stay within the quantum"));
				const bool bFinalOk = IFileManager::Get().FileSize(*Dest) == BigTotal &&
					!PF.FileExists(*Staging) && !PF.FileExists(*Journal);
				Check(bFinalOk, TEXT("H2: resumed file promoted (full size, no journal left)"));
				if (bFinalOk)
				{
					TArray64<uint8> Final;
					if (FFileHelper::LoadFileToArray(Final, *Dest) &&
						FMemory::Memcmp(Final.GetData(), Whole.GetData(), BigTotal) == 0)
					{
						Check(true, TEXT("H2: resumed content matches the CDN copy"));
					}
					else
					{
						Check(false, TEXT("H2: resumed content matches the CDN copy"));
					}
				}
			}

			// --- H2b: journal resume from a NATURALLY-produced journal (C5
			// regression). H2 hand-crafts the journal - which masked the bug
			// where a fresh download NEVER wrote the UGPJRN1 header (only
			// JournalReset in the verify-retry path did), so real crash-resume
			// always replay-rejected and re-downloaded everything. Here the
			// journal is produced by the executor itself: a stalling backend
			// holds segments 1-2 in flight, segment 0 settles (written +
			// journaled), then the executor is destroyed - the exact on-disk
			// state a killed process leaves behind.
			{
				const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH2b"), TEXT("big.chunk"));
				PF.CreateDirectoryTree(*FPaths::GetPath(Dest));
				const FString Staging = Dest + TEXT(".download");
				const FString Journal = Dest + TEXT(".journal");
				TArray64<uint8> Whole;
				FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Cdn, BigName));

				struct FStallingBackend : public IChunkDownloader
				{
					FString Root;
					FCriticalSection Lock;
					struct FPending
					{
						int64 Offset;
						TFunction<void(bool, const TArray64<uint8>&, const FString&)> OnDone;
					};
					TArray<FPending> Pending;

					virtual bool DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& OutData) override
					{
						return false; // unused (jobs carry known sizes)
					}
					virtual FCancelHandle DownloadRangeAsync(const FString& Url, int64 Offset, int64 Size,
						TFunction<void(bool, const TArray64<uint8>&, const FString&)>&& OnDone) override
					{
						FScopeLock Scope(&Lock);
						Pending.Add({ Offset, MoveTemp(OnDone) });
						return MakeShared<int32>(0);
					}
					bool DeliverOne(int32 Index, const TArray64<uint8>& Whole, int64 Total)
					{
						TFunction<void(bool, const TArray64<uint8>&, const FString&)> Cb;
						int64 Offset = -1;
						{
							FScopeLock Scope(&Lock);
							if (!Pending.IsValidIndex(Index)) { return false; }
							Offset = Pending[Index].Offset;
							Cb = MoveTemp(Pending[Index].OnDone);
							Pending[Index].OnDone = nullptr;
						}
						if (!Cb) { return false; }
						const int64 Sz = FMath::Min<int64>(Total - Offset, 4 << 20);
						TArray64<uint8> Data;
						Data.SetNumUninitialized(Sz);
						FMemory::Memcpy(Data.GetData(), Whole.GetData() + Offset, Sz);
						Cb(true, Data, FString());
						return true;
					}
				};

				{
					FStallingBackend Stall;
					Stall.Root = Cdn;
					{
						FDownloadExecutor Executor(Stall);
						FDownloadJob Job;
						Job.Id = TEXT("H2b");
						Job.Url = BigUrl;
						Job.DestPath = Dest;
						Job.ExpectedSize = BigTotal;
						Executor.Submit(MoveTemp(Job), [](const FDownloadResult&) {});
						// Three segments issued and pending; deliver segment 0
						// only, then destroy the executor mid-flight.
						Check(Stall.DeliverOne(0, Whole, BigTotal), TEXT("H2b: segment 0 delivered"));
						Check(Stall.Pending.Num() == 3, TEXT("H2b: three segments were in flight"));
					} // executor destroyed: journal + staging MUST survive (no Finish ran)
					Check(PF.FileExists(*Journal), TEXT("H2b: journal survived the interruption"));
					Check(PF.FileExists(*Staging), TEXT("H2b: staging survived the interruption"));
					// THE C5 ASSERTION: the journal must carry the production
					// header (a fresh download never wrote it before the fix).
					FString JournalText;
					Check(FFileHelper::LoadFileToString(JournalText, *Journal) &&
						JournalText.StartsWith(TEXT("UGPJRN1")),
						TEXT("H2b: the natural journal carries the UGPJRN1 header"));
					Check(JournalText.Contains(TEXT("SEG 0")), TEXT("H2b: segment 0 is journaled"));
				} // stalling backend dies WITHOUT firing segments 1-2 (no cleanup ran)

				// Restart: only the missing segments may be re-fetched.
				FRecordingBackend Backend;
				Backend.Root = Cdn;
				FDownloadExecutor Executor(Backend);
				FDownloadJob Job;
				Job.Id = TEXT("H2b-restart");
				Job.Url = BigUrl;
				Job.DestPath = Dest;
				Job.ExpectedSize = BigTotal;
				Executor.Submit(MoveTemp(Job), [](const FDownloadResult&) {});
				bool bOnlyMissing = false;
				{
					FScopeLock Scope(&Backend.StatsLock);
					bOnlyMissing = Backend.Offsets.Num() == 2 &&
						Backend.Offsets[0] == SegSize && Backend.Offsets[1] == SegSize * 2;
					if (!bOnlyMissing)
					{
						FString Off;
						for (const int64 O : Backend.Offsets) { Off += FString::Printf(TEXT(" %lld"), O); }
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("H2b restart offsets:%s"), *Off);
					}
				}
				Check(bOnlyMissing, TEXT("H2b: restart fetched ONLY the missing segments (natural journal replayed)"));
				const bool bPromoted = IFileManager::Get().FileSize(*Dest) == BigTotal &&
					!PF.FileExists(*Staging) && !PF.FileExists(*Journal);
				Check(bPromoted, TEXT("H2b: resumed file promoted"));
				if (bPromoted)
				{
					TArray64<uint8> Final;
					Check(FFileHelper::LoadFileToArray(Final, *Dest) &&
						FMemory::Memcmp(Final.GetData(), Whole.GetData(), BigTotal) == 0,
						TEXT("H2b: resumed content matches the CDN copy"));
				}
			// --- H2c: Cancel fast path - a job with InFlight == 0 (here:
			//     planned but window-starved, undone > 0) fires OnDone
			//     immediately. NOTE: the stale-InFlight state N-C1 described
			//     (issued segments all settled, remainder unissued) is NOT
			//     reachable through scheduling - refill-on-settle always
			//     re-takes the freed slot. The settle-side InFlight decrement
			//     (fixplan N-C1/M23) is accounting hygiene that keeps the
			//     InFlight==0 check exact; this test pins the check's
			//     contract. -------------------------------------------------
			{
				FStallingBackend Stall;
				Stall.Root = Cdn;
				bool bBCanceledDone = false;
				{
					FDownloadExecutorConfig CfgC;
					CfgC.Window = 2;
					FDownloadExecutor Executor(Stall, CfgC);
					// Job A: 3 segments at 12MB - the 2-slot window fills with
					// A0/A1; A2 stays unissued, so A is never exhausted.
					FDownloadJob A;
					A.Id = TEXT("H2c-A");
					A.Url = BigUrl;
					A.DestPath = FPaths::Combine(WorkDir, TEXT("h2c-a.bin"));
					A.ExpectedSize = 12ll << 20;
					const int64 HandleA = Executor.Submit(MoveTemp(A), [](const FDownloadResult&) {});
					// Job B: planned but starved - 0 issued, InFlight == 0.
					FDownloadJob B;
					B.Id = TEXT("H2c-B");
					B.Url = BigUrl;
					B.DestPath = FPaths::Combine(WorkDir, TEXT("h2c-b.bin"));
					B.ExpectedSize = BigTotal;
					const int64 HandleB = Executor.Submit(MoveTemp(B),
						[&bBCanceledDone](const FDownloadResult&) { bBCanceledDone = true; });
					{
						FScopeLock Scope(&Stall.Lock);
						Check(Stall.Pending.Num() == 2, TEXT("H2c: window holds exactly A's two segments"));
					}
					Executor.Cancel(HandleB);
					Check(bBCanceledDone, TEXT("H2c: cancel of a zero-in-flight job fires OnDone immediately"));
					Executor.Cancel(HandleA);
				} // destructor cancels A's pending requests; nothing hangs
			}

			// --- H2d: batch watchdog - a transport that never settles must
			//     fail the batch at the stall timeout instead of hanging. ---
			{
				FStallingBackend Stall;
				Stall.Root = Cdn;
				FDownloadManager Manager(Stall, 2, 0, /*StallTimeoutSec=*/1.5);
				TArray<FDownloadRequest> Reqs;
				for (int32 I = 0; I < 2; ++I)
				{
					FDownloadRequest R;
					R.Id = FString::Printf(TEXT("H2d-%d"), I);
					R.Url = BigUrl;
					R.Size = BigTotal;
					R.DestFile = FPaths::Combine(WorkDir, FString::Printf(TEXT("h2d-%d.bin"), I));
					Reqs.Add(R);
				}
				FDownloadStats Stats;
				TArray<FString> Failed;
				const double T0 = FPlatformTime::Seconds();
				const bool bOk = Manager.DownloadAll(Reqs, Stats, Failed);
				const double Elapsed = FPlatformTime::Seconds() - T0;
				Check(!bOk, TEXT("H2d: stalled batch fails"));
				Check(Failed.Contains(TEXT("H2d-0")) && Failed.Contains(TEXT("H2d-1")),
					TEXT("H2d: stalled jobs reported failed"));
				Check(Elapsed < 30.0, TEXT("H2d: watchdog fired near the stall timeout (no hang)"));
			}
			} // H2b/H2c/H2d scope: FStallingBackend and the 9MB fixture live here

			// --- H3: fairness - later small jobs finish before the big (4.4) -
			{
				// Real HTTP backend: completion order depends on actual transfer
				// timing. The big 9MB job is submitted first, three small
				// manifest chunks after - the finishing-priority + fair-share
				// rules must let the small ones finish first. On a loopback
				// transport the transfer-vs-connection-setup race can reorder
				// finishes (observed flaky on macOS), so arm the server's
				// per-connection throttle: the big transfer then takes seconds
				// while the smalls stay sub-second - the fairness property is
				// exercised deterministically. Disarmed at the end of the case.
				{
					TArray64<uint8> Ctrl;
					Http.DownloadRange(FString::Printf(TEXT("%s/__ctl/throttle?kbps=2048"), *BaseUrl), 0, -1, Ctrl);
				}
				const FString CacheH3 = FPaths::Combine(WorkDir, TEXT("cacheH3"));
				PF.CreateDirectoryTree(*CacheH3);

				FCriticalSection OrderLock;
				TArray<FString> FinishOrder;
				FDownloadExecutor Executor(Http);
				std::atomic<int32> Remaining{ 4 };
				FEvent* H3Done = FPlatformProcess::GetSynchEventFromPool(true);
				auto Track = [&OrderLock, &FinishOrder, &Remaining, H3Done](const FDownloadResult& R)
				{
					{
						FScopeLock Scope(&OrderLock);
						FinishOrder.Add(R.Id);
					}
					if (Remaining.fetch_sub(1) == 1)
					{
						H3Done->Trigger();
					}
				};

				FDownloadJob Big;
				Big.Id = TEXT("H3-big");
				Big.Url = BigUrl;
				Big.DestPath = FPaths::Combine(CacheH3, TEXT("big.chunk"));
				Big.ExpectedSize = BigTotal;
				Executor.Submit(MoveTemp(Big), Track);

				int32 SmallIdx = 0;
				for (const TPair<FString, FIndexChunkEntry>& Pair : Index.Chunks)
				{
					if (++SmallIdx > 3) { break; }
					FDownloadJob Small;
					Small.Id = Pair.Key;
					Small.Url = FString::Printf(TEXT("%s/%s"), *BaseUrl, *Pair.Value.File);
					Small.DestPath = FPaths::Combine(CacheH3, Pair.Value.File);
					Small.ExpectedSize = Pair.Value.Size;
					Executor.Submit(MoveTemp(Small), Track);
				}

				const bool bAllDone = H3Done->Wait(60 * 1000);
				FPlatformProcess::ReturnSynchEventToPool(H3Done);
				Check(bAllDone, TEXT("H3: all 4 jobs completed"));

				bool bSmallsFirst = false;
				{
					FScopeLock Scope(&OrderLock);
					const int32 BigPos = FinishOrder.Find(TEXT("H3-big"));
					int32 SmallCount = 0;
					for (int32 I = 0; I < BigPos; ++I)
					{
						if (FinishOrder[I] != TEXT("H3-big")) { ++SmallCount; }
					}
					bSmallsFirst = BigPos >= 0 && SmallCount >= 2;
					UE_LOG(LogUnrealGamePatcher, Display, TEXT("H3: finish order = [%s]"), *FString::Join(FinishOrder, TEXT(", ")));
				}
				Check(bSmallsFirst, TEXT("H3: small jobs finish before the big one (fair share + finishing priority)"));
			}
			{
				// Disarm the H3 throttle (H4 measures transport-cap behavior).
				TArray64<uint8> Ctrl;
				Http.DownloadRange(FString::Printf(TEXT("%s/__ctl/throttle?kbps=0"), *BaseUrl), 0, -1, Ctrl);
			}

			// --- H4a: lied manifest vs transport cap (4.5) ------------------
			{
				// The manifest declares 512KB but the server serves the full
				// 9MB body: the transport cap (lowered for the test) must
				// cancel at header time and nothing may land on disk.
				const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH4"), TEXT("big.chunk"));
				const int64 SavedCap = FHttpRangeRequest::MaxResponseBytes;
				FHttpRangeRequest::MaxResponseBytes = 1ll << 20;

				TArray<FDownloadRequest> Requests;
				FDownloadRequest Lied;
				Lied.Id = TEXT("H4-cap");
				Lied.Url = BigUrl;
				Lied.Size = 512ll << 10;
				Lied.DestFile = Dest;
				Requests.Add(Lied);

				FDownloadStats StatsH4;
				TArray<FString> FailedH4;
				const bool bBatchOk = Manager.DownloadAll(Requests, StatsH4, FailedH4);
				FHttpRangeRequest::MaxResponseBytes = SavedCap;

				Check(!bBatchOk && FailedH4.Num() == 1 && FailedH4[0] == TEXT("H4-cap"),
					TEXT("H4a: lied-size transfer refused (whole-body fetch over the cap)"));
				Check(!PF.FileExists(*Dest) && !PF.FileExists(*(Dest + TEXT(".download"))) &&
					!PF.FileExists(*(Dest + TEXT(".journal"))),
					TEXT("H4a: nothing landed on disk"));
			}

			// --- H4b: no-Range server falls back to a single whole-file GET (4.5)
			{
				const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH4b"), TEXT("big.chunk"));

				struct FNoRangeBackend : public FRecordingBackend
				{
					void ProbeSizeAsync(const FString& Url, TFunction<void(int64, bool)>&& OnDone) override
					{
						// A server that strips Range: probe gets the total from
						// the plain 200 but reports Range as unsupported.
						TArray64<uint8> Whole;
						FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Root, TEXT("chunks"), TEXT("resume-tail-big.chunk")));
						OnDone(Whole.Num(), false);
					}
				};

				FNoRangeBackend Backend;
				Backend.Root = Cdn;
				FDownloadExecutor Executor(Backend);

				FDownloadJob Job;
				Job.Id = TEXT("H4b");
				Job.Url = BigUrl;
				Job.DestPath = Dest;
				Job.ExpectedSize = -1; // unknown: the executor must probe first
				Executor.Submit(MoveTemp(Job), [](const FDownloadResult&) {});

				bool bSingleWholeGet = false;
				{
					FScopeLock Scope(&Backend.StatsLock);
					bSingleWholeGet = Backend.WholeFileGets == 1 && Backend.Offsets.Num() == 1;
				}
				Check(bSingleWholeGet, TEXT("H4b: no-Range server -> exactly one whole-file GET (no segmentation)"));

				TArray64<uint8> Whole;
				TArray64<uint8> Final;
				const bool bContentOk = FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Cdn, BigName)) &&
					FFileHelper::LoadFileToArray(Final, *Dest) &&
					Final.Num() == Whole.Num() &&
					FMemory::Memcmp(Final.GetData(), Whole.GetData(), Whole.Num()) == 0;
				Check(bContentOk, TEXT("H4b: whole-file fallback content matches the CDN copy"));
			}

			// --- H5: job cancellation (executor Cancel, untested until now) --
			{
				// A DEFERRED backend: requests are queued and settle only on
				// Flush() - the only way to interleave Cancel with in-flight
				// segments against a synchronous test backend.
				struct FDeferredBackend : public IChunkDownloader
				{
					struct FPending
					{
						FString Url;
						int64 Offset = 0;
						int64 Size = 0;
						TFunction<void(bool, const TArray64<uint8>&, const FString&)> OnDone;
						bool bCanceled = false;
					};
					TArray<TSharedPtr<FPending>> Pending;

					bool DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& OutData) override
					{
						bool bOk = false;
						DownloadRangeAsync(Url, Offset, Size, [&bOk, &OutData](bool bSuccess, const TArray64<uint8>& Data, const FString&)
						{
							bOk = bSuccess;
							if (bSuccess) { OutData = Data; }
						});
						return bOk;
					}

					FCancelHandle DownloadRangeAsync(const FString& Url, int64 Offset, int64 Size,
						TFunction<void(bool bOk, const TArray64<uint8>& Data, const FString& Error)>&& OnDone) override
					{
						TSharedPtr<FPending> P = MakeShared<FPending>();
						P->Url = Url;
						P->Offset = Offset;
						P->Size = Size;
						P->OnDone = MoveTemp(OnDone);
						Pending.Add(P);
						return P;
					}

					void CancelDownload(const FCancelHandle& Handle) override
					{
						for (const TSharedPtr<FPending>& P : Pending)
						{
							if (P.Get() == Handle.Get())
							{
								P->bCanceled = true;
							}
						}
					}

					void Flush(const FString& Root)
					{
						for (const TSharedPtr<FPending>& P : Pending)
						{
							if (P->bCanceled)
							{
								P->OnDone(false, TArray64<uint8>(), TEXT("canceled by test"));
								continue;
							}
							TArray64<uint8> Whole;
							// Slice from the CDN file by absolute offset.
							FString Rel = P->Url;
							const int32 Scheme = Rel.Find(TEXT("://"), ESearchCase::CaseSensitive);
							if (Scheme != INDEX_NONE)
							{
								const int32 PathStart = Rel.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Scheme + 3);
								if (PathStart != INDEX_NONE)
								{
									Rel = Rel.RightChop(PathStart + 1);
								}
							}
							const bool bOk = FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Root, Rel));
							TArray64<uint8> Out;
							if (bOk)
							{
								const int64 End = P->Size < 0 ? Whole.Num() : FMath::Min<int64>(P->Offset + P->Size, Whole.Num());
								if (P->Offset >= 0 && End > P->Offset && End <= Whole.Num())
								{
									Out.SetNumUninitialized(End - P->Offset);
									FMemory::Memcpy(Out.GetData(), Whole.GetData() + P->Offset, End - P->Offset);
								}
							}
							P->OnDone(bOk && Out.Num() > 0, Out, bOk ? FString() : TEXT("slice failed"));
						}
						Pending.Reset();
					}
				};

				// Case a: cancel while segments are in flight -> exactly one
				// failure result, staging/journal cleaned up.
				{
					const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH5"), TEXT("big.chunk"));
					FDeferredBackend Backend;
					FDownloadExecutor Executor(Backend);

					std::atomic<int32> DoneCount{ 0 };
					std::atomic<bool> bCancelReason{ false };
					FDownloadJob Job;
					Job.Id = TEXT("H5-cancel");
					Job.Url = BigUrl;
					Job.DestPath = Dest;
					Job.ExpectedSize = BigTotal;
					const int64 Handle = Executor.Submit(MoveTemp(Job), [&DoneCount, &bCancelReason](const FDownloadResult& R)
					{
						DoneCount.fetch_add(1);
						if (!R.bSuccess && R.Error.Contains(TEXT("cancel")))
						{
							bCancelReason.store(true);
						}
					});
					Check(Backend.Pending.Num() > 0, TEXT("H5: segments issued before the cancel"));
					Executor.Cancel(Handle);
					// Review H2: the cancel must reach the TRANSPORT - the
					// executor has to store the backend's cancel handle and
					// hand it to CancelDownload. With the dropped-return-value
					// bug this stayed bookkeeping-only and every pending
					// request kept transferring to completion.
					{
						int32 NumTransportCanceled = 0;
						for (const TSharedPtr<FDeferredBackend::FPending>& P : Backend.Pending)
						{
							if (P->bCanceled)
							{
								++NumTransportCanceled;
							}
						}
						Check(NumTransportCanceled > 0, TEXT("H5: the in-flight transport request received CancelDownload"));
					}
					Backend.Flush(Cdn);

					Check(DoneCount.load() == 1, TEXT("H5: OnDone fired exactly once after cancel"));
					Check(bCancelReason.load(), TEXT("H5: canceled job fails with a cancel reason"));
					Check(!PF.FileExists(*Dest) && !PF.FileExists(*(Dest + TEXT(".download"))) &&
						!PF.FileExists(*(Dest + TEXT(".journal"))),
						TEXT("H5: canceled job leaves no sidecars"));
				}

				// Case b: cancel AFTER completion is a guarded no-op.
				{
					const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH5b"), TEXT("big.chunk"));
					FRecordingBackend Backend;
					Backend.Root = Cdn;
					FDownloadExecutor Executor(Backend);

					std::atomic<int32> DoneCount{ 0 };
					FDownloadJob Job;
					Job.Id = TEXT("H5-done");
					Job.Url = BigUrl;
					Job.DestPath = Dest;
					Job.ExpectedSize = BigTotal;
					const int64 Handle = Executor.Submit(MoveTemp(Job), [&DoneCount](const FDownloadResult&) { DoneCount.fetch_add(1); });
					const int32 AfterDone = DoneCount.load();
					Executor.Cancel(Handle);
					Check(DoneCount.load() == AfterDone, TEXT("H5: cancel after completion is a no-op"));
				}
			}

			// --- H6: whole-file SHA1 verify path (ExpectedSha1, untested) -----
			{
				// Success: correct SHA1 promotes.
				{
					const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH6"), TEXT("big.chunk"));
					FRecordingBackend Backend;
					Backend.Root = Cdn;
					FDownloadExecutor Executor(Backend);

					TArray64<uint8> Whole;
					FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Cdn, BigName));
					FSHA1 Hasher;
					Hasher.Update(Whole.GetData(), (int32)Whole.Num());
					Hasher.Final();
					uint8 Digest[20];
					Hasher.GetHash(Digest);

					std::atomic<bool> bOk{ false };
					FDownloadJob Job;
					Job.Id = TEXT("H6-ok");
					Job.Url = BigUrl;
					Job.DestPath = Dest;
					Job.ExpectedSize = BigTotal;
					Job.ExpectedSha1 = FPatcherHash::BytesToHex(Digest, 20);
					Executor.Submit(MoveTemp(Job), [&bOk](const FDownloadResult& R) { bOk.store(R.bSuccess); });
					Check(bOk.load(), TEXT("H6: correct ExpectedSha1 promotes"));
				}

				// Failure: wrong SHA1 on a correct-size file triggers the
				// verify-failure re-download loop (all segments reissued every
				// round: 3 segments x (1 + MaxRetries) rounds) and terminal
				// failure with no sidecars.
				{
					const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH6bad"), TEXT("big.chunk"));
					FRecordingBackend Backend;
					Backend.Root = Cdn;
					FDownloadExecutor Executor(Backend);

					std::atomic<bool> bOk{ true };
					FDownloadJob Job;
					Job.Id = TEXT("H6-bad");
					Job.Url = BigUrl;
					Job.DestPath = Dest;
					Job.ExpectedSize = BigTotal;
					Job.ExpectedSha1 = TEXT("0000000000000000000000000000000000000000");
					Executor.Submit(MoveTemp(Job), [&bOk](const FDownloadResult& R) { bOk.store(R.bSuccess); });

					int32 SegmentRequests = 0;
					{
						FScopeLock Scope(&Backend.StatsLock);
						SegmentRequests = Backend.Offsets.Num();
					}
					Check(!bOk.load(), TEXT("H6: wrong ExpectedSha1 fails the job"));
					Check(SegmentRequests == 3 * 4, TEXT("H6: verify failure re-downloads all segments every round (3 segs x 4 rounds)"));
					Check(!PF.FileExists(*(Dest + TEXT(".download"))) && !PF.FileExists(*(Dest + TEXT(".journal"))),
						TEXT("H6: failed verify leaves no sidecars"));
				}
			}

			// --- H7: journal resume with a NON-CONTIGUOUS hole (only the
			//     middle segment journaled done -> segments 0 and 2 refetch) -
			{
				const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH7"), TEXT("big.chunk"));
				PF.CreateDirectoryTree(*FPaths::GetPath(Dest));
				const FString Staging = Dest + TEXT(".download");
				const FString Journal = Dest + TEXT(".journal");

				TArray64<uint8> Whole;
				FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Cdn, BigName));
				{
					IFileHandle* H = PF.OpenWrite(*Staging);
					if (H)
					{
						H->Seek(BigTotal - 1);
						uint8 Zero = 0;
						H->Write(&Zero, 1);
						H->Seek(SegSize); // middle segment bytes on disk
						H->Write(Whole.GetData() + SegSize, (int32)SegSize);
						delete H;
					}
					const FString Text = FString::Printf(TEXT("UGPJRN1 %lld %lld\nSEG 1\n"), SegSize, BigTotal);
					Check(FFileHelper::SaveStringToFile(Text, *Journal, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM),
						TEXT("H7: journal written (only SEG 1 done)"));
				}

				FRecordingBackend Backend;
				Backend.Root = Cdn;
				FDownloadExecutor Executor(Backend);

				FDownloadJob Job;
				Job.Id = TEXT("H7");
				Job.Url = BigUrl;
				Job.DestPath = Dest;
				Job.ExpectedSize = BigTotal;
				Executor.Submit(MoveTemp(Job), [](const FDownloadResult&) {});

				bool bHolesOnly = false;
				{
					FScopeLock Scope(&Backend.StatsLock);
					bHolesOnly = Backend.Offsets.Num() == 2 &&
						Backend.Offsets[0] == 0 && Backend.Offsets[1] == SegSize * 2;
				}
				Check(bHolesOnly, TEXT("H7: non-contiguous journal refetches exactly segments 0 and 2"));
				{
					TArray64<uint8> Final;
					const bool bOk = IFileManager::Get().FileSize(*Dest) == BigTotal &&
						FFileHelper::LoadFileToArray(Final, *Dest) &&
						FMemory::Memcmp(Final.GetData(), Whole.GetData(), BigTotal) == 0;
					Check(bOk, TEXT("H7: assembled content matches the CDN copy"));
				}
			}

			// --- H8: probe SUCCESS drives a segmented download; real-HTTP
			//     segment 0 uses an exact Range (bytes=0-(n-1), 206) ----------
			{
				const FString Dest = FPaths::Combine(WorkDir, TEXT("cacheH8"), TEXT("big.chunk"));

				// (a) Recording backend with a working probe: unknown size ->
				// probed total -> segmented fetch (the bRangeOk==true mainline).
				{
					struct FProbingBackend : public FRecordingBackend
					{
						void ProbeSizeAsync(const FString& Url, TFunction<void(int64, bool)>&& OnDone) override
						{
							TArray64<uint8> Whole;
							FFileHelper::LoadFileToArray(Whole, *FPaths::Combine(Root, TEXT("chunks"), TEXT("resume-tail-big.chunk")));
							OnDone(Whole.Num(), true);
						}
					};
					FProbingBackend Backend;
					Backend.Root = Cdn;
					FDownloadExecutor Executor(Backend);

					FDownloadJob Job;
					Job.Id = TEXT("H8a");
					Job.Url = BigUrl;
					Job.DestPath = Dest;
					Job.ExpectedSize = -1; // unknown: probe first
					Executor.Submit(MoveTemp(Job), [](const FDownloadResult&) {});

					bool bSegmentedFromProbe = false;
					{
						FScopeLock Scope(&Backend.StatsLock);
						bSegmentedFromProbe = Backend.Offsets.Num() == 3 &&
							Backend.Offsets[0] == 0 && Backend.Offsets[1] == SegSize && Backend.Offsets[2] == SegSize * 2;
					}
					Check(bSegmentedFromProbe, TEXT("H8a: successful probe drives the full segmented fetch"));
				}

				// (b) Real HTTP: every segment of a fresh multi-segment fetch -
				// INCLUDING segment 0 - goes out as an exact Range (206).
				{
					const FString Dest2 = FPaths::Combine(WorkDir, TEXT("cacheH8b"), TEXT("big.chunk"));
					const int32 FilesBefore = CdnServer.Stats().Files;
					const int32 RangesBefore = CdnServer.Stats().Ranges;

					FDownloadManager ManagerH8(Http, /*MaxConcurrent=*/8, /*MaxRetries=*/3);
					TArray<FDownloadRequest> H8Requests;
					FDownloadRequest H8;
					H8.Id = TEXT("H8b");
					H8.Url = BigUrl;
					H8.Size = BigTotal;
					H8.DestFile = Dest2;
					H8Requests.Add(H8);
					FDownloadStats H8Stats;
					TArray<FString> H8Failed;
					Check(ManagerH8.DownloadAll(H8Requests, H8Stats, H8Failed), TEXT("H8b: multi-segment fetch over real HTTP succeeds"));

					const int32 FilesDelta = CdnServer.Stats().Files - FilesBefore;
					const int32 RangesDelta = CdnServer.Stats().Ranges - RangesBefore;
					Check(FilesDelta == 3 && RangesDelta == 3,
						TEXT("H8b: all 3 segments (incl. segment 0) used exact 206 Range requests"));
				}

				// (c) Real FHttpRangeRequest::ProbeSizeAsync against the live
				// server: total + Range-support verdict, body never read.
				{
					std::atomic<int64> ProbedTotal{ -1 };
					std::atomic<bool> ProbedRange{ false };
					FEvent* ProbeDone = FPlatformProcess::GetSynchEventFromPool(true);
					Http.ProbeSizeAsync(BigUrl, [ProbeDone, &ProbedTotal, &ProbedRange](int64 TotalSize, bool bRangeOk)
					{
						ProbedTotal.store(TotalSize);
						ProbedRange.store(bRangeOk);
						ProbeDone->Trigger();
					});
					Check(ProbeDone->Wait(60 * 1000), TEXT("H8c: probe completes"));
					FPlatformProcess::ReturnSynchEventToPool(ProbeDone);
					Check(ProbedTotal.load() == BigTotal && ProbedRange.load(),
						TEXT("H8c: real probe returns the total and Range support"));
				}
			}
		}

		CdnServer.Stop();

		return FinishTest(bAllPassed, TEXT("downloadtest"));
	}

}
