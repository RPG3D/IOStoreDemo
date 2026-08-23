// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * UnrealGamePatcherBuild module: build-side tooling (ingest / diff / CDN artifact layout).
 */
class FUnrealGamePatcherBuildModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
