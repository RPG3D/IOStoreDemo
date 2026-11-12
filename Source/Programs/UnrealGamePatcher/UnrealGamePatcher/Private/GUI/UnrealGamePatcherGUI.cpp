// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealGamePatcherGUI.h"

#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HttpModule.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "StandaloneRenderer.h"
#include "Stats/StatsSystem.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"

#include "Download/FHttpChunkDownloader.h"
#include "Update/FUpdateOrchestrator.h"
#include "UnrealGamePatcherVersion.h"
#include "UnrealGamePatcherPaths.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealGamePatcherGUI, Log, All);

namespace
{
	using namespace UnrealGamePatcher;

	enum class ELauncherState
	{
		Idle,
		Updating,
		Ready,
		UpdateFailed,
		Playing,
	};

	class FUpdateWorker final : public FRunnable
	{
	public:
		FUpdateWorker(const FString& InCdn, const FString& InVersion, const FString& InInstall)
			: Cdn(InCdn), Version(InVersion), Install(InInstall)
		{
		}

		uint32 Run() override
		{
			FHttpChunkDownloader Downloader;
			FUpdateOptions Options;
			if (Cdn.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase) ||
				Cdn.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
			{
				Options.Downloader = &Downloader;
				Options.BaseUrl = Cdn;
			}
			Options.Events = [this](const FUpdateEvent& Event)
			{
				FScopeLock Scope(&Lock);
				if (Event.bIsError || !Progress.bIsError)
				{
					Progress = Event;
				}
			};

			FString TargetVersion = Version;
			if (TargetVersion.IsEmpty() && !FUpdateOrchestrator::ResolveLatest(Cdn, Options, TargetVersion))
			{
				bFinished = true;
				return 0;
			}

			FUpdateStats Stats;
			bSuccess = FUpdateOrchestrator::UpdateWithOptions(Cdn, TargetVersion, Install, true, Options, Stats);
			bFinished = true;
			return 0;
		}

		FUpdateEvent GetProgress() const
		{
			FScopeLock Scope(&Lock);
			return Progress;
		}

		std::atomic<bool> bFinished{ false };
		bool bSuccess = false;

	private:
		const FString& Cdn;
		const FString& Version;
		const FString& Install;
		mutable FCriticalSection Lock;
		FUpdateEvent Progress{ EUpdatePhase::None, EPatcherError::Ok, false, 0, 0,
			TEXT("Checking content version...") };
	};

	FString FormatUpdateFailure(const FUpdateEvent& Event)
	{
		const EPatcherError Error = Event.Error == EPatcherError::Ok ? EPatcherError::Internal : Event.Error;
		const TCHAR* Detail = Event.bIsError && !Event.Message.IsEmpty() ? *Event.Message : TEXT("The content update could not finish.");
		return FString::Printf(TEXT("UGP-%04d\n%s"), static_cast<int32>(Error), Detail);
	}

#if PLATFORM_MAC
	FString FindMacBundle(FString Path)
	{
		while (!Path.IsEmpty())
		{
			if (Path.EndsWith(TEXT(".app"), ESearchCase::IgnoreCase))
			{
				return Path;
			}
			Path = FPaths::GetPath(Path);
		}
		return FString();
	}

	bool MacBundleOverlapsInstall(const FString& Bundle, const FString& Install)
	{
		if (Bundle.IsEmpty())
		{
			return false;
		}
		const FString BundlePath = Bundle.ToLower();
		const FString InstallPath = Install.ToLower();
		return FPaths::IsUnderDirectory(InstallPath, BundlePath) || FPaths::IsUnderDirectory(BundlePath, InstallPath);
	}
#endif
}

int32 RunUnrealGamePatcherGUI(const TCHAR* CommandLine, const FString& LaunchArgs)
{
	FSlateApplication::InitializeAsStandaloneApplication(GetStandardStandaloneRenderer());
	FSlateApplication::InitHighDPI(true);
	FSlateApplication& SlateApp = FSlateApplication::Get();

	FString Cdn, Version, Install, LaunchPath;
	FParse::Value(CommandLine, TEXT("-cdn="), Cdn);
	FParse::Value(CommandLine, TEXT("-version="), Version);
	FParse::Value(CommandLine, TEXT("-install="), Install);
	FParse::Value(CommandLine, TEXT("-launch="), LaunchPath);
	const bool bAutoPlay = FParse::Param(CommandLine, TEXT("autoplay"));
	if (!LaunchPath.IsEmpty())
	{
		LaunchPath = FPaths::ConvertRelativePathToFull(LaunchPath);
	}
	if (Cdn.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase) ||
		Cdn.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
	{
		// The HTTP module must first load on the game thread.
		FHttpModule::Get();
	}

	ELauncherState State = ELauncherState::Idle;
	FString ErrorText;
	TUniquePtr<FUpdateWorker> Worker;
	TUniquePtr<FRunnableThread> WorkerThread;
	FProcHandle GameProcess;
	bool bAutoPlaySucceeded = false;

	const auto FailUpdate = [&](EPatcherError Code, const TCHAR* Message)
	{
		State = ELauncherState::UpdateFailed;
		ErrorText = FormatUpdateFailure({ EUpdatePhase::Failed, Code, true, 0, 0, Message });
		UE_LOG(LogUnrealGamePatcherGUI, Error, TEXT("%s"), *ErrorText);
	};

	const auto StartUpdate = [&]()
	{
		ErrorText.Reset();
		if (Cdn.IsEmpty() || Install.IsEmpty())
		{
			FailUpdate(EPatcherError::InvalidConfiguration, TEXT("Specify the CDN and install directory with -cdn and -install."));
			return;
		}
		if (bAutoPlay && LaunchPath.IsEmpty())
		{
			FailUpdate(EPatcherError::InvalidConfiguration, TEXT("Specify the game path with -launch when using -autoplay."));
			return;
		}
#if PLATFORM_MAC
		FString InstallPath, GamePath, GivenBundlePath;
		const FString GivenBundle = FindMacBundle(LaunchPath);
		if (!Paths::ResolveMacPath(Install, InstallPath) || (!LaunchPath.IsEmpty() && !Paths::ResolveMacPath(LaunchPath, GamePath)) ||
			(!GivenBundle.IsEmpty() && !Paths::ResolveMacPath(GivenBundle, GivenBundlePath)))
		{
			FailUpdate(EPatcherError::InvalidConfiguration, TEXT("Could not resolve the install directory or game path."));
			return;
		}
		if (!FindMacBundle(FPaths::ConvertRelativePathToFull(Install)).IsEmpty() || !FindMacBundle(InstallPath).IsEmpty() ||
			MacBundleOverlapsInstall(FindMacBundle(GamePath), InstallPath) || MacBundleOverlapsInstall(GivenBundlePath, InstallPath))
		{
			FailUpdate(EPatcherError::InvalidConfiguration, TEXT("Choose an install directory separate from the game bundle."));
			return;
		}
		Install = MoveTemp(InstallPath);
		LaunchPath = MoveTemp(GamePath);
#endif
		Worker = MakeUnique<FUpdateWorker>(Cdn, Version, Install);
		WorkerThread.Reset(FRunnableThread::Create(Worker.Get(), TEXT("UGP-GUIUpdate"), 0, TPri_Normal));
		if (!WorkerThread)
		{
			Worker.Reset();
			FailUpdate(EPatcherError::Internal, TEXT("Could not start the update worker."));
			return;
		}
		State = ELauncherState::Updating;
	};

	const auto LaunchGame = [&]()
	{
		uint32 ProcessId = 0;
		const FString WorkingDirectory = FPaths::GetPath(LaunchPath);
		FString GameArgs = LaunchArgs;
#if PLATFORM_MAC
		GameArgs += FString::Printf(TEXT(" -ugpinstall=\"%s\""), *Install);
#endif
		GameProcess = FPlatformProcess::CreateProc(*LaunchPath, *GameArgs, true, false, false,
			&ProcessId, 0, *WorkingDirectory, nullptr);
		if (!GameProcess.IsValid())
		{
			ErrorText = FString::Printf(TEXT("Could not launch the game.\n%s"), *LaunchPath);
			UE_LOG(LogUnrealGamePatcherGUI, Error, TEXT("Could not launch %s"), *LaunchPath);
			return false;
		}
		ErrorText.Reset();
		State = ELauncherState::Playing;
		UE_LOG(LogUnrealGamePatcherGUI, Display, TEXT("Launched %s (PID %u)"), *LaunchPath, ProcessId);
		return true;
	};

	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::Format(NSLOCTEXT("UnrealGamePatcher", "AppTitle", "UnrealGamePatcher {0}"),
			FText::FromString(UnrealGamePatcher::ProgramVersion)))
		.ClientSize(FVector2D(540, 250))
		.SupportsMaximize(false)
		.SupportsMinimize(true);
	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride::CreateLambda([](const TSharedRef<SWindow>&)
	{
		RequestEngineExit(TEXT("Launcher window closed"));
	}));

	Window->SetContent(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
			.FillHeight(1)
			.VAlign(VAlign_Center)
			.Padding(24, 16)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
							.AutoWrapText(true)
							.Text_Lambda([&]()
							{
								if (!ErrorText.IsEmpty())
								{
									return FText::FromString(ErrorText);
								}
								switch (State)
								{
								case ELauncherState::Updating:
									return FText::FromString(Worker->GetProgress().Message);
								case ELauncherState::Ready:
									return NSLOCTEXT("UnrealGamePatcher", "Ready", "Game content is up to date.");
								case ELauncherState::Playing:
									return NSLOCTEXT("UnrealGamePatcher", "Playing", "The game is running.");
								default:
									return NSLOCTEXT("UnrealGamePatcher", "Idle", "Check for game content updates.");
								}
							})
					]
				+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0, 12, 0, 0)
					[
						SNew(SProgressBar)
							.Visibility_Lambda([&]()
							{
								return State == ELauncherState::Updating ? EVisibility::Visible : EVisibility::Collapsed;
							})
							.Percent_Lambda([&]() -> TOptional<float>
							{
								if (State == ELauncherState::Updating)
								{
									const FUpdateEvent Progress = Worker->GetProgress();
									if (Progress.Phase == EUpdatePhase::Download && Progress.Total > 0)
									{
										return FMath::Clamp(float(Progress.Current) / Progress.Total, 0.0f, 1.0f);
									}
								}
								return {};
							})
					]
			]
		+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			.Padding(24, 0, 24, 24)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0, 0, 8, 0)
					[
						SNew(SButton)
							.Text_Lambda([&]()
							{
								return State == ELauncherState::UpdateFailed
									? NSLOCTEXT("UnrealGamePatcher", "Retry", "Retry")
									: NSLOCTEXT("UnrealGamePatcher", "UpdateBtn", "Check for Updates");
							})
							.IsEnabled_Lambda([&]()
							{
								return !IsEngineExitRequested() && State != ELauncherState::Updating && State != ELauncherState::Playing;
							})
							.OnClicked_Lambda([&]()
							{
								StartUpdate();
								return FReply::Handled();
							})
					]
				+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0, 0, 8, 0)
					[
						SNew(SButton)
							.Visibility(LaunchPath.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
							.Text(NSLOCTEXT("UnrealGamePatcher", "LaunchBtn", "Play"))
							.IsEnabled_Lambda([&]() { return !IsEngineExitRequested() && State == ELauncherState::Ready; })
							.OnClicked_Lambda([&]()
							{
								LaunchGame();
								return FReply::Handled();
							})
					]
				+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
							.Text(NSLOCTEXT("UnrealGamePatcher", "QuitBtn", "Quit"))
							.OnClicked_Lambda([]()
							{
								RequestEngineExit(TEXT("User clicked Quit"));
								return FReply::Handled();
							})
					]
			]
	);

	SlateApp.AddWindow(Window);
	if (bAutoPlay || FParse::Param(CommandLine, TEXT("autoupdate")))
	{
		StartUpdate();
	}

	while (!IsEngineExitRequested() || WorkerThread)
	{
		const double FrameStart = FPlatformTime::Seconds();
		BeginExitIfRequested();
		if (Worker && Worker->bFinished.load())
		{
			WorkerThread->WaitForCompletion();
			WorkerThread.Reset();
			State = Worker->bSuccess ? ELauncherState::Ready : ELauncherState::UpdateFailed;
			if (!Worker->bSuccess)
			{
				ErrorText = FormatUpdateFailure(Worker->GetProgress());
			}
			Worker.Reset();
			if (bAutoPlay && State == ELauncherState::Ready && !IsEngineExitRequested())
			{
				bAutoPlaySucceeded = LaunchGame();
				RequestEngineExit(TEXT("Automatic game launch completed"));
			}
		}
		if (GameProcess.IsValid() && !FPlatformProcess::IsProcRunning(GameProcess))
		{
			int32 ExitCode = 0;
			if (FPlatformProcess::GetProcReturnCode(GameProcess, &ExitCode) && ExitCode != 0)
			{
				ErrorText = FString::Printf(TEXT("The game exited with code %d."), ExitCode);
			}
			FPlatformProcess::CloseProc(GameProcess);
			State = ELauncherState::Ready;
		}
		if ((IsEngineExitRequested() && !WorkerThread) || (bAutoPlay && State == ELauncherState::UpdateFailed))
		{
			break;
		}

		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		UE::Stats::FStats::AdvanceFrame(false);
		FTSTicker::GetCoreTicker().Tick(SlateApp.GetDeltaTime());
		SlateApp.PumpMessages();
		SlateApp.Tick();
		constexpr double FrameInterval = 1.0 / 30.0;
		FPlatformProcess::Sleep(static_cast<float>(FMath::Max(0.0, FrameInterval - (FPlatformTime::Seconds() - FrameStart))));
	}

	if (GameProcess.IsValid())
	{
		FPlatformProcess::CloseProc(GameProcess);
	}
	FSlateApplication::Shutdown();
	return State == ELauncherState::UpdateFailed || (bAutoPlay && !bAutoPlaySucceeded) ? 1 : 0;
}
