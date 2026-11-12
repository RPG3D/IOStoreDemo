// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * GUI mode entry. Creates a Slate window and drives the shared orchestrator (Core) on a worker thread.
 * Returns the process exit code.
 */
int32 RunUnrealGamePatcherGUI(const TCHAR* CommandLine, const FString& LaunchArgs);
