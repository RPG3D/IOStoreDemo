#pragma once

#include "CoreMinimal.h"
#include "IO/IoStoreOnDemand.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "OnDemandSubsystem.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOnDemand, Log, All);

UCLASS()
class UOnDemandSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "OnDemand")
	void InstallDLC(const FString& TagSet);

	UFUNCTION(BlueprintCallable, Category = "OnDemand")
	void UninstallDLC(const FString& TagSet);

	UFUNCTION(BlueprintCallable, Category = "OnDemand")
	void GetInstallSize(const FString& TagSet);

	UFUNCTION(BlueprintCallable, Category = "OnDemand")
	void OnDemandStatus();

	UFUNCTION(BlueprintCallable, Category = "OnDemand")
	bool IsInitialized() const { return bInitialized; }

private:
	bool bInitialized = false;
	FString DLCMountId;
	UE::IoStore::FOnDemandContentHandle DLCContentHandle;
	TSharedPtr<UE::IoStore::FOnDemandInstallRequest> CurrentInstallRequest;

	void MountDLCToc(const FString& TagSet);
	void OnMountComplete(UE::IoStore::FOnDemandMountResult Result, FString TagSet);
	void OnInstallComplete(UE::IoStore::FOnDemandInstallResult Result);
	void OnInstallProgress(UE::IoStore::FOnDemandInstallProgress Progress);

	static void RegisterConsoleCommands();
	static void UnregisterConsoleCommands();
};
