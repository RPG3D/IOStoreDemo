// Copyright Epic Games, Inc. All Rights Reserved.

// Runtime module shell for the packaging-time UPL (see UGPAndroidPatch_APL.xml).
// No behavior: the patch flow lives in libugp.so and PatchActivity.java.

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, UGPAndroidPatch)
