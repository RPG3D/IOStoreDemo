// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Shared log category for the patcher. Defined in UnrealGamePatcherCore.cpp,
// declared here so CLI / GUI / Build modules can log on the same category.
UNREALGAMEPATCHERCORE_API DECLARE_LOG_CATEGORY_EXTERN(LogUnrealGamePatcher, Log, All);
