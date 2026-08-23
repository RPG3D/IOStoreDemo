// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * UnrealGamePatcherCore module: format layer / patch engine / download / transaction / state.
 * No UI dependency; shared by CLI and GUI entry points.
 */
class FUnrealGamePatcherCoreModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
