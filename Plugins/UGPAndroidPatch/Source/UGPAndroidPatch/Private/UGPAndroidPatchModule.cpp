// Copyright Epic Games, Inc. All Rights Reserved.

// Runtime module shell for the packaging-time UPL (see UGPAndroidPatch_APL.xml).
// The game module owns the startup updater. This plugin supplies packaging rules.

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, UGPAndroidPatch)
