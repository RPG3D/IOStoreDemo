// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealGamePatcherGUI.h"

#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HttpModule.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "StandaloneRenderer.h"
#include "Stats/StatsSystem.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Misc/Parse.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"

#include "Download/FHttpChunkDownloader.h"
#include "Update/FUpdateEvent.h"
#include "Update/FUpdateOrchestrator.h"
#include "UnrealGamePatcherVersion.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealGamePatcherGUI, Log, All);

namespace
{
	FString PhaseToString(UnrealGamePatcher::EUpdatePhase Phase)
	{
		switch (Phase)
		{
		case UnrealGamePatcher::EUpdatePhase::Manifest: return TEXT("加载清单");
		case UnrealGamePatcher::EUpdatePhase::Diff: return TEXT("差异分析");
		case UnrealGamePatcher::EUpdatePhase::Download: return TEXT("下载");
		case UnrealGamePatcher::EUpdatePhase::Apply: return TEXT("应用");
		case UnrealGamePatcher::EUpdatePhase::Commit: return TEXT("提交");
		case UnrealGamePatcher::EUpdatePhase::Done: return TEXT("完成");
		case UnrealGamePatcher::EUpdatePhase::Failed: return TEXT("失败");
		default: return TEXT("");
		}
	}

	/** State shared between the update worker thread and the UI thread.
	 *  The worker only writes (via OnEvent, under the lock); the Slate tick
	 *  reads through the getters. */
	struct FPatcherUIState
	{
		std::atomic<bool> bRunning{ false };
		std::atomic<bool> bDone{ false };
		std::atomic<bool> bSuccess{ false };
		std::atomic<bool> bError{ false };

		mutable FCriticalSection Lock;
		UnrealGamePatcher::EUpdatePhase Phase = UnrealGamePatcher::EUpdatePhase::None;
		UnrealGamePatcher::EPatcherError Error = UnrealGamePatcher::EPatcherError::Ok;
		int32 Current = 0;
		int32 Total = 0;
		UnrealGamePatcher::FUpdateStats Stats;
		FString StatusText;

		/** Worker-thread entry: store the latest broadcast event (stage /
		 *  progress / error) under the lock; the UI tick reads it. */
		void OnEvent(const UnrealGamePatcher::FUpdateEvent& E)
		{
			FScopeLock Scope(&Lock);
			Phase = E.Phase;
			Current = E.Current;
			Total = E.Total;
			if (E.bIsError || E.Phase == UnrealGamePatcher::EUpdatePhase::Failed)
			{
				bError = true;
				Error = E.Error;
				StatusText = E.Message.IsEmpty() ? TEXT("更新失败") : E.Message;
			}
			else if (E.Phase == UnrealGamePatcher::EUpdatePhase::Done)
			{
				bError = false;
				Error = UnrealGamePatcher::EPatcherError::Ok;
				StatusText = E.Message;
			}
			else if (!E.Message.IsEmpty())
			{
				StatusText = E.Message;
			}
		}

		void SetStatus(const FString& Text)
		{
			FScopeLock Scope(&Lock);
			StatusText = Text;
		}

		FString GetStatus() const
		{
			FScopeLock Scope(&Lock);
			return StatusText;
		}

		FString GetPhaseName() const
		{
			FScopeLock Scope(&Lock);
			return PhaseToString(Phase);
		}

		UnrealGamePatcher::EPatcherError GetError() const
		{
			FScopeLock Scope(&Lock);
			return Error;
		}

		void GetProgress(int32& OutCurrent, int32& OutTotal) const
		{
			FScopeLock Scope(&Lock);
			OutCurrent = Current;
			OutTotal = Total;
		}
	};

	/** Runs the update off the UI thread; every stage/progress/error event from
	 *  the orchestrator is forwarded to the shared UI state (the Slate tick
	 *  reads it each frame). */
	class FUpdateWorker : public FRunnable
	{
	public:
		FUpdateWorker(
			FPatcherUIState& InState,
			const FString& InCdn,
			const FString& InVersion,
			const FString& InInstall)
			: State(InState)
			, Cdn(InCdn)
			, Version(InVersion)
			, Install(InInstall)
		{
		}

		virtual uint32 Run() override
		{
			State.bRunning = true;
			State.bDone = false;
			State.bSuccess = false;
			State.bError = false;
			State.SetStatus(FString::Printf(TEXT("更新到 %s ..."), *Version));

			UnrealGamePatcher::FUpdateStats Stats;
			UnrealGamePatcher::FUpdateOptions Options;
			Options.Events = [this](const UnrealGamePatcher::FUpdateEvent& E) { State.OnEvent(E); };
			// -cdn is ONE base: an http(s):// URL fetches everything over HTTP,
			// a local directory (bare path or file://...) is read from disk.
			const bool bCdnIsHttp = Cdn.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase) ||
				Cdn.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase);
			// Function scope on purpose: Options.Downloader stays in use by the
			// orchestrator below - a block-scoped downloader here would dangle.
			UnrealGamePatcher::FHttpChunkDownloader Downloader;
			if (bCdnIsHttp)
			{
				FHttpModule::Get(); // force-load on the game thread (see downloadtest)
				Options.Downloader = &Downloader;
				Options.BaseUrl = Cdn;
			}
			const bool bOk = UnrealGamePatcher::FUpdateOrchestrator::UpdateWithOptions(Cdn, Version, Install, true, Options, Stats);

			{
				FScopeLock Scope(&State.Lock);
				State.Stats = Stats;
			}
			State.bSuccess = bOk;
			State.bDone = true;
			State.bRunning = false;
			State.bError = State.bError.load() || !bOk; // the Failed event already carried the detail
			return 0;
		}

	private:
		FPatcherUIState& State;
		FString Cdn;
		FString Version;
		FString Install;
	};
}

int32 RunUnrealGamePatcherGUI(const TCHAR* CommandLine)
{
	// crank up a normal Slate application using the platform's standalone renderer
	FSlateApplication::InitializeAsStandaloneApplication(GetStandardStandaloneRenderer());
	FSlateApplication::InitHighDPI(true);
	FSlateApplication& SlateApp = FSlateApplication::Get();

	// Update parameters (same scheme as the CLI): -cdn= -version= -install=
	// (-cdn is ONE base: http(s)://... or a local directory/file://...).
	FString Cdn, Version, Install, LaunchPath;
	FParse::Value(CommandLine, TEXT("-cdn="), Cdn);
	FParse::Value(CommandLine, TEXT("-version="), Version);
	FParse::Value(CommandLine, TEXT("-install="), Install);
	FParse::Value(CommandLine, TEXT("-launch="), LaunchPath);
	// Warm the HTTP module on the GAME thread no matter which path later uses
	// it: FHttpModule::Get() asserts on first use off the game thread, and with
	// -version= given the resolve block below is skipped - the first use would
	// otherwise be the worker thread's downloader.
	const bool bCdnIsHttp = Cdn.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase) ||
		Cdn.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase);
	if (bCdnIsHttp)
	{
		FHttpModule::Get();
	}
	if (Version.IsEmpty())
	{
		// No -version= lock: resolve from the CDN announcement
		// (index/latest.json) when the CDN is given; fall back to the default.
		UnrealGamePatcher::FUpdateOptions ResolveOptions;
		UnrealGamePatcher::FHttpChunkDownloader ResolveDownloader;
		if (bCdnIsHttp)
		{
			ResolveOptions.Downloader = &ResolveDownloader;
			ResolveOptions.BaseUrl = Cdn;
		}
		if (Cdn.IsEmpty() || !UnrealGamePatcher::FUpdateOrchestrator::ResolveLatest(Cdn, ResolveOptions, Version))
		{
			Version = TEXT("1.0.0");
		}
	}

	FPatcherUIState UIState;
	FUpdateWorker* Worker = nullptr;
	FRunnableThread* WorkerThread = nullptr;
	const bool bAutoUpdate = FParse::Param(CommandLine, TEXT("autoupdate"));

	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::Format(NSLOCTEXT("UnrealGamePatcher", "AppTitle", "UnrealGamePatcher {0}"), FText::FromString(UnrealGamePatcher::ProgramVersion)))
		.ClientSize(FVector2D(640, 460))
		.SupportsMaximize(false)
		.SupportsMinimize(true);

	// UI reads the shared state every frame; the worker only writes it (via events).
	Window->SetContent(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
			.Padding(16)
			.AutoHeight()
			[
				SNew(STextBlock)
					.Text(NSLOCTEXT("UnrealGamePatcher", "Header", "UnrealGamePatcher - 增量更新"))
			]
		+ SVerticalBox::Slot()
			.Padding(16)
			.AutoHeight()
			[
				SNew(STextBlock)
					.Text_Lambda([&UIState]() { return FText::FromString(UIState.GetStatus()); })
			]
		+ SVerticalBox::Slot()
			.Padding(16)
			.AutoHeight()
			[
				SNew(SProgressBar)
					.Percent_Lambda([&UIState]() -> TOptional<float>
					{
						int32 C = 0, T = 0;
						UIState.GetProgress(C, T);
						if (T > 0 && !UIState.bDone.load())
						{
							return (float)C / (float)T;
						}
						return UIState.bDone.load() && UIState.bSuccess.load() ? TOptional<float>(1.0f) : TOptional<float>();
					})
			]
		+ SVerticalBox::Slot()
			.Padding(16)
			.AutoHeight()
			[
				SNew(STextBlock)
					.Text_Lambda([&UIState]()
					{
						int32 C = 0, T = 0;
						UIState.GetProgress(C, T);
						return FText::FromString(FString::Printf(TEXT("阶段：%s  %d/%d"), *UIState.GetPhaseName(), C, T));
					})
			]
		+ SVerticalBox::Slot()
			.Padding(16)
			.AutoHeight()
			[
				SNew(STextBlock)
					.ColorAndOpacity(FLinearColor(1.0f, 0.35f, 0.35f))
					.Text_Lambda([&UIState]()
					{
						if (!UIState.bError.load())
						{
							return FText::GetEmpty();
						}
						return FText::FromString(FString::Printf(TEXT("错误码 %d：%s"),
							(int32)UIState.GetError(), *UIState.GetStatus()));
					})
			]
		+ SVerticalBox::Slot()
			.Padding(16)
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0, 0, 8, 0)
					[
						SNew(SButton)
							.Text(NSLOCTEXT("UnrealGamePatcher", "UpdateBtn", "更新"))
							.IsEnabled_Lambda([&UIState]() { return !UIState.bRunning.load(); })
							.OnClicked_Lambda([&]() -> FReply
							{
								if (UIState.bRunning.load())
								{
									return FReply::Handled();
								}
								if (WorkerThread)
								{
									WorkerThread->WaitForCompletion();
									delete WorkerThread;
									WorkerThread = nullptr;
									delete Worker;
									Worker = nullptr;
								}
								Worker = new FUpdateWorker(UIState, Cdn, Version, Install);
								WorkerThread = FRunnableThread::Create(Worker, TEXT("UGP-GUIUpdate"), 0, TPri_Normal);
								return FReply::Handled();
							})
					]
				+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0, 0, 8, 0)
					[
						SNew(SButton)
							.Text(NSLOCTEXT("UnrealGamePatcher", "LaunchBtn", "启动游戏"))
							.IsEnabled_Lambda([&UIState]()
							{
								return UIState.bDone.load() && UIState.bSuccess.load();
							})
							.OnClicked_Lambda([&]() -> FReply
							{
								if (!LaunchPath.IsEmpty())
								{
									FPlatformProcess::CreateProc(*LaunchPath, nullptr, true, false, false, nullptr, 0, nullptr, nullptr);
								}
								return FReply::Handled();
							})
					]
				+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
							.Text(NSLOCTEXT("UnrealGamePatcher", "QuitBtn", "退出"))
							.OnClicked_Lambda([]() -> FReply
							{
								RequestEngineExit(TEXT("User clicked Quit"));
								return FReply::Handled();
							})
					]
			]
	);

	SlateApp.AddWindow(Window);

	if (bAutoUpdate)
	{
		Worker = new FUpdateWorker(UIState, Cdn, Version, Install);
		WorkerThread = FRunnableThread::Create(Worker, TEXT("UGP-GUIUpdate"), 0, TPri_Normal);
	}

	// loop while the patcher does the rest
	while (!IsEngineExitRequested())
	{
		BeginExitIfRequested();

		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		UE::Stats::FStats::AdvanceFrame(false);
		FTSTicker::GetCoreTicker().Tick(FApp::GetDeltaTime());
		SlateApp.PumpMessages();
		SlateApp.Tick();

		FPlatformProcess::Sleep(0.01f);
	}

	if (WorkerThread)
	{
		WorkerThread->WaitForCompletion();
		delete WorkerThread;
		delete Worker;
	}

	FSlateApplication::Shutdown();
	// Propagate the update result as the process exit code (the header
	// documents this contract): launchers can branch on success. No worker =
	// nothing attempted = success.
	const bool bUpdateOk = !Worker || (UIState.bDone.load() && UIState.bSuccess.load());
	return bUpdateOk ? 0 : 1;
}
