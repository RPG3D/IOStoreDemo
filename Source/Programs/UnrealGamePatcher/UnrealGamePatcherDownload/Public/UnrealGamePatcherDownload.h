// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * Generic download component (reusable): transport primitive + window-driven
 * executor + journal resume. See Docs/DOWNLOAD.md (component spec) and
 * Docs/ROADMAP.md §2 (module plan).
 */
// Own log category (the component is reusable standalone - it does not
// depend on UnrealGamePatcherCore, which itself depends on this module).
DECLARE_LOG_CATEGORY_EXTERN(LogUnrealGamePatcherDownload, Log, All);

class FUnrealGamePatcherDownloadModule : public FDefaultModuleImpl
{
};
