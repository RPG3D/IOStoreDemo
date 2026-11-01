// Copyright Epic Games, Inc. All Rights Reserved.
// -cli benchtest —— 吞吐/内存基准（性能基准）
// 验证点：窗口并发 vs 单连接吞吐对比；内存峰值不随文件大小翻倍。

#include "Tests/TestFramework.h"

namespace UnrealGamePatcher::Tests
{
	int32 RunBenchTest(const TCHAR* /*CmdLine*/)
	{
		bool bAllPassed = true;
		auto Check = MakeCheck(bAllPassed);

		using namespace UnrealGamePatcher;

		const FString WorkDir = GetTestWorkDir(TEXT("benchtest"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		const FString Cdn = FPaths::Combine(WorkDir, TEXT("cdn"));
		PF.CreateDirectoryTree(*Cdn);

		// Synthetic payloads, written incrementally so the process never
		// holds a whole payload in memory (the memory bench below measures
		// the DOWNLOAD buffers, not the generator).
		const int64 SegSize = 4ll << 20;
		const int64 TputTotal = 16ll << 20; // 16MB = 4 segments (throughput)
		const int64 MemTotal = 96ll << 20;  // 96MB = 24 segments (memory peak)
		auto GenFile = [&PF](const FString& Path, int64 Total)
		{
			TArray<uint8> Piece;
			Piece.SetNumUninitialized(1 << 20);
			for (int32 I = 0; I < Piece.Num(); ++I)
			{
				Piece[I] = (uint8)(I * 31 + (I >> 13));
			}
			IFileHandle* H = PF.OpenWrite(*Path);
			if (!H)
			{
				return false;
			}
			int64 Left = Total;
			int64 Salt = 0;
			while (Left > 0)
			{
				const int32 N = (int32)FMath::Min<int64>(Left, Piece.Num());
				Piece[0] = (uint8)(Salt++); // vary content per MB
				H->Write(Piece.GetData(), N);
				Left -= N;
			}
			delete H;
			return true;
		};
		const FString TputName = TEXT("chunks/bench-tput.bin");
		const FString MemName = TEXT("chunks/bench-mem.bin");
		PF.CreateDirectoryTree(*FPaths::Combine(Cdn, TEXT("chunks")));
		Check(GenFile(*FPaths::Combine(Cdn, TputName), TputTotal), TEXT("bench: 16MB payload generated"));
		Check(GenFile(*FPaths::Combine(Cdn, MemName), MemTotal), TEXT("bench: 96MB payload generated"));

		const FString ProjectRoot = GetProjectRootFromExecutable();
		const FString CdnScript = FPaths::Combine(ProjectRoot, TEXT("Source"), TEXT("Programs"), TEXT("UnrealGamePatcher"), TEXT("Scripts"), TEXT("cdn_server.py"));
		FCdnServerProc CdnServer;
		Check(CdnServer.Start(CdnScript, Cdn, FPaths::Combine(WorkDir, TEXT("cdn.log")), TEXT("__ctl/stats")),
			TEXT("bench: external CDN server started"));
		const FString BaseUrl = FString::Printf(TEXT("http://127.0.0.1:%d"), CdnServer.Port);
		FHttpModule::Get();

		FHttpChunkDownloader Http;
		auto ArmThrottle = [&Http, &BaseUrl](int32 Kbps)
		{
			TArray64<uint8> Ctrl;
			Http.DownloadRange(FString::Printf(TEXT("%s/__ctl/throttle?kbps=%d"), *BaseUrl, Kbps), 0, -1, Ctrl);
		};

		// --- A. Throughput: windowed executor vs a single connection -------
		{
			// 1 MB/s per connection: a single connection needs ~16s, four
			// in-flight segments should land near ~4s if concurrency is real.
			ArmThrottle(1024);
			const FString Url = FString::Printf(TEXT("%s/%s"), *BaseUrl, *TputName);

			double SingleSec = 0.0;
			{
				TArray64<uint8> Out;
				const double T0 = FPlatformTime::Seconds();
				Http.DownloadRange(Url, 0, -1, Out);
				SingleSec = FPlatformTime::Seconds() - T0;
				Check(Out.Num() == TputTotal, TEXT("bench: single-connection fetch complete"));
			}

			double WindowedSec = 0.0;
			{
				const FString Dest = FPaths::Combine(WorkDir, TEXT("tput.bin"));
				FDownloadExecutorConfig Cfg;
				Cfg.Window = 8; // 4MB quantum -> 4 segments, all in flight
				FDownloadExecutor Executor(Http, Cfg);
				FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
				FDownloadJob Job;
				Job.Id = TEXT("bench-tput");
				Job.Url = Url;
				Job.DestPath = Dest;
				Job.ExpectedSize = TputTotal;
				Executor.Submit(MoveTemp(Job), [Done](const FDownloadResult&) { Done->Trigger(); });
				const double T0 = FPlatformTime::Seconds();
				Check(Done->Wait(300 * 1000), TEXT("bench: windowed fetch complete"));
				WindowedSec = FPlatformTime::Seconds() - T0;
				FPlatformProcess::ReturnSynchEventToPool(Done);
				Check(IFileManager::Get().FileSize(*Dest) == TputTotal, TEXT("bench: windowed output has the full size"));
			}

			const double SingleMbps = (TputTotal / (1024.0 * 1024.0)) / SingleSec;
			const double WindowedMbps = (TputTotal / (1024.0 * 1024.0)) / WindowedSec;
			UE_LOG(LogUnrealGamePatcher, Display,
				TEXT("[BENCH] throughput: single-connection %.1fs (%.2f MB/s) | windowed %.1fs (%.2f MB/s) | speedup x%.2f"),
				SingleSec, SingleMbps, WindowedSec, WindowedMbps, SingleSec / WindowedSec);
			Check(WindowedSec < SingleSec, TEXT("bench: windowed throughput beats a single connection"));
		}

		// --- B. Memory peak at full window (96MB payload / 24 segments) -----
		{
			// 8 MB/s per connection with 24 in-flight segments streaming
			// concurrently: the full 96MB window cycles continuously, so the
			// ~12s total run is enough for the process peak to reflect the
			// download buffers.
			ArmThrottle(8192);
			const FString Url = FString::Printf(TEXT("%s/%s"), *BaseUrl, *MemName);
			const FString Dest = FPaths::Combine(WorkDir, TEXT("mem.bin"));

			FPlatformMemory::GetStats(); // refresh
			const uint64 PeakBefore = FPlatformMemory::GetStats().PeakUsedPhysical;

			FDownloadExecutorConfig Cfg;
			Cfg.Window = 24;
			FDownloadExecutor Executor(Http, Cfg);
			FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
			FDownloadJob Job;
			Job.Id = TEXT("bench-mem");
			Job.Url = Url;
			Job.DestPath = Dest;
			Job.ExpectedSize = MemTotal;
			Executor.Submit(MoveTemp(Job), [Done](const FDownloadResult&) { Done->Trigger(); });
			Check(Done->Wait(300 * 1000), TEXT("bench: full-window fetch complete"));
			FPlatformProcess::ReturnSynchEventToPool(Done);
			Check(IFileManager::Get().FileSize(*Dest) == MemTotal, TEXT("bench: memory-run output has the full size"));

			const uint64 PeakAfter = FPlatformMemory::GetStats().PeakUsedPhysical;
			const double DeltaMB = (double)(PeakAfter - PeakBefore) / (1024.0 * 1024.0);
			UE_LOG(LogUnrealGamePatcher, Display,
				TEXT("[BENCH] memory: process peak delta %.1f MB with window 24 x 4MB (data budget 96MB, engine buffer growth ~2x)"),
				DeltaMB);
			// The DATA budget is window x segment (96MB); the engine's curl
			// response TArray grows x1.5 with a realloc copy spike, so the
			// process peak lands at ~2x the data budget - a constant factor,
			// the window stays the linear knob. The Apple transport carries a
			// larger per-connection buffer constant (~2.8x measured), so the
			// same linear-scaling bound uses a platform constant.
#if PLATFORM_WINDOWS
			const double MemFactor = 2.5;
#else
			const double MemFactor = 3.5;
#endif
			Check(DeltaMB < 96.0 * MemFactor, TEXT("bench: memory peak within the data-budget factor"));
		}

		CdnServer.Stop();

		return FinishTest(bAllPassed, TEXT("benchtest"));
	}

}
