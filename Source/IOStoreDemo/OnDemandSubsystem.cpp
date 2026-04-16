#include "OnDemandSubsystem.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY(LogOnDemand);

static TArray<IConsoleObject*> RegisteredConsoleCommands;

void UOnDemandSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	
	UE_LOG(LogOnDemand, Log, TEXT("OnDemandSubsystem initialized"));
	
	RegisterConsoleCommands();
}

void UOnDemandSubsystem::Deinitialize()
{
	if (CurrentInstallRequest)
	{
		CurrentInstallRequest->Cancel();
		CurrentInstallRequest.Reset();
	}
	
	DLCContentHandle.Reset();
	UnregisterConsoleCommands();
	
	Super::Deinitialize();
}

void UOnDemandSubsystem::RegisterConsoleCommands()
{
	IConsoleManager& ConsoleManager = IConsoleManager::Get();
	
	auto RegisterCmd = [&ConsoleManager](const TCHAR* Name, const TCHAR* Help, const FConsoleCommandWithArgsDelegate& Delegate) -> IConsoleObject*
	{
		return ConsoleManager.RegisterConsoleCommand(Name, Help, Delegate);
	};

	RegisteredConsoleCommands.Add(RegisterCmd(TEXT("installdlc"), TEXT("Install DLC by tagset (e.g., installdlc NewMap)"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() == 0)
			{
				UE_LOG(LogOnDemand, Warning, TEXT("Usage: installdlc <tagset>"));
				return;
			}
			if (UGameInstance* GI = GWorld->GetGameInstance())
			{
				if (UOnDemandSubsystem* Subsystem = GI->GetSubsystem<UOnDemandSubsystem>())
				{
					Subsystem->InstallDLC(Args[0]);
				}
			}
		})));

	RegisteredConsoleCommands.Add(RegisterCmd(TEXT("ondemandstatus"), TEXT("Show OnDemand cache status"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (UGameInstance* GI = GWorld->GetGameInstance())
			{
				if (UOnDemandSubsystem* Subsystem = GI->GetSubsystem<UOnDemandSubsystem>())
				{
					Subsystem->OnDemandStatus();
				}
			}
		})));

	RegisteredConsoleCommands.Add(RegisterCmd(TEXT("getinstallsize"), TEXT("Get install size for tagset"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() == 0)
			{
				UE_LOG(LogOnDemand, Warning, TEXT("Usage: getinstallsize <tagset>"));
				return;
			}
			if (UGameInstance* GI = GWorld->GetGameInstance())
			{
				if (UOnDemandSubsystem* Subsystem = GI->GetSubsystem<UOnDemandSubsystem>())
				{
					Subsystem->GetInstallSize(Args[0]);
				}
			}
		})));
}

void UOnDemandSubsystem::UnregisterConsoleCommands()
{
	IConsoleManager& ConsoleManager = IConsoleManager::Get();
	for (IConsoleObject* Cmd : RegisteredConsoleCommands)
	{
		ConsoleManager.UnregisterConsoleObject(Cmd);
	}
	RegisteredConsoleCommands.Empty();
}

void UOnDemandSubsystem::InstallDLC(const FString& TagSet)
{
	UE_LOG(LogOnDemand, Log, TEXT("InstallDLC: %s"), *TagSet);

	UE::IoStore::IOnDemandIoStore* OnDemandStore = UE::IoStore::TryGetOnDemandIoStore();
	if (!OnDemandStore)
	{
		UE_LOG(LogOnDemand, Error, TEXT("OnDemandIoStore not available"));
		return;
	}

	MountDLCToc(TagSet);
}

void UOnDemandSubsystem::MountDLCToc(const FString& TagSet)
{
	UE::IoStore::IOnDemandIoStore* OnDemandStore = UE::IoStore::TryGetOnDemandIoStore();
	if (!OnDemandStore)
	{
		UE_LOG(LogOnDemand, Error, TEXT("OnDemandIoStore not available"));
		return;
	}

	DLCMountId = TEXT("DLC_InstallOnDemand");
	
	UE::IoStore::FOnDemandMountArgs MountArgs;
	MountArgs.MountId = DLCMountId;
	MountArgs.TocRelativeUrl = TEXT("ias-content/dlc-install.iochunktoc");
	MountArgs.Options = UE::IoStore::EOnDemandMountOptions::InstallOnDemand;
	
	UE_LOG(LogOnDemand, Log, TEXT("Mounting DLC TOC: %s"), *MountArgs.TocRelativeUrl);
	
	OnDemandStore->Mount(MoveTemp(MountArgs),
		[this, TagSet](UE::IoStore::FOnDemandMountResult Result)
		{
			OnMountComplete(MoveTemp(Result), TagSet);
		});
}

void UOnDemandSubsystem::OnMountComplete(UE::IoStore::FOnDemandMountResult Result, FString TagSet)
{
	if (!Result.Status.IsOk())
	{
		UE_LOG(LogOnDemand, Error, TEXT("Mount failed"));
		return;
	}

	UE_LOG(LogOnDemand, Log, TEXT("Mount succeeded, installing tag: %s"), *TagSet);

	UE::IoStore::IOnDemandIoStore* OnDemandStore = UE::IoStore::TryGetOnDemandIoStore();
	if (!OnDemandStore)
	{
		UE_LOG(LogOnDemand, Error, TEXT("OnDemandIoStore lost"));
		return;
	}

	DLCContentHandle = UE::IoStore::FOnDemandContentHandle::Create(TEXT("DLC_Install"));
	
	UE::IoStore::FOnDemandInstallArgs InstallArgs;
	InstallArgs.MountId = DLCMountId;
	InstallArgs.TagSets = { TagSet };
	InstallArgs.ContentHandle = DLCContentHandle;
	InstallArgs.Options = UE::IoStore::EOnDemandInstallOptions::InstallSoftReferences 
		| UE::IoStore::EOnDemandInstallOptions::CallbackOnGameThread;
	InstallArgs.Priority = 10;
	
	CurrentInstallRequest = MakeShared<UE::IoStore::FOnDemandInstallRequest>(
		OnDemandStore->Install(
			MoveTemp(InstallArgs),
			[this](UE::IoStore::FOnDemandInstallResult InstallResult)
			{
				OnInstallComplete(MoveTemp(InstallResult));
			},
			[this](UE::IoStore::FOnDemandInstallProgress Progress)
			{
				OnInstallProgress(MoveTemp(Progress));
			}
		)
	);
}

void UOnDemandSubsystem::OnInstallComplete(UE::IoStore::FOnDemandInstallResult Result)
{
	if (Result.IsOk())
	{
		UE_LOG(LogOnDemand, Log, TEXT("Install complete! Downloaded: %llu bytes"), 
			Result.Progress.CurrentInstallSize);
	}
	else
	{
		UE_LOG(LogOnDemand, Error, TEXT("Install failed"));
	}
	
	CurrentInstallRequest.Reset();
}

void UOnDemandSubsystem::OnInstallProgress(UE::IoStore::FOnDemandInstallProgress Progress)
{
	float Pct = Progress.GetRelativeProgress() * 100.0f;
	UE_LOG(LogOnDemand, Log, TEXT("Progress: %.1f%% (%llu / %llu)"), 
		Pct, Progress.GetCachedSize(), Progress.GetTotalSize());
}

void UOnDemandSubsystem::UninstallDLC(const FString& TagSet)
{
	UE_LOG(LogOnDemand, Warning, TEXT("UninstallDLC not yet implemented"));
}

void UOnDemandSubsystem::GetInstallSize(const FString& TagSet)
{
	UE::IoStore::IOnDemandIoStore* OnDemandStore = UE::IoStore::TryGetOnDemandIoStore();
	if (!OnDemandStore)
	{
		UE_LOG(LogOnDemand, Error, TEXT("OnDemandIoStore not available"));
		return;
	}

	UE::IoStore::FOnDemandGetInstallSizeArgs Args;
	Args.MountId = DLCMountId;
	Args.TagSets = { TagSet };
	Args.ContentHandle = DLCContentHandle;
	
	TIoStatusOr<UE::IoStore::FOnDemandInstallSizeResult> Result = OnDemandStore->GetInstallSize(Args);
	if (Result.IsOk())
	{
		UE_LOG(LogOnDemand, Log, TEXT("Install size for '%s': %llu bytes"), 
			*TagSet, Result.ValueOrDie().InstallSize);
	}
	else
	{
		UE_LOG(LogOnDemand, Warning, TEXT("Could not get size for '%s'"), *TagSet);
	}
}

void UOnDemandSubsystem::OnDemandStatus()
{
	UE::IoStore::IOnDemandIoStore* OnDemandStore = UE::IoStore::TryGetOnDemandIoStore();
	if (!OnDemandStore)
	{
		UE_LOG(LogOnDemand, Warning, TEXT("OnDemandIoStore not available"));
		return;
	}

	UE::IoStore::FOnDemandCacheUsage Usage = OnDemandStore->GetCacheUsage({});
	
	UE_LOG(LogOnDemand, Log, TEXT("=== OnDemand Cache Status ==="));
	UE_LOG(LogOnDemand, Log, TEXT("Install: %llu / %llu bytes"), 
		Usage.InstallCache.TotalSize, Usage.InstallCache.MaxSize);
	UE_LOG(LogOnDemand, Log, TEXT("Streaming: %llu / %llu bytes"), 
		Usage.StreamingCache.TotalSize, Usage.StreamingCache.MaxSize);
	UE_LOG(LogOnDemand, Log, TEXT("Referenced: %llu bytes"), Usage.InstallCache.ReferencedSize);
	UE_LOG(LogOnDemand, Log, TEXT("Fragmented: %llu bytes"), Usage.InstallCache.FragmentedChunksSize);
}