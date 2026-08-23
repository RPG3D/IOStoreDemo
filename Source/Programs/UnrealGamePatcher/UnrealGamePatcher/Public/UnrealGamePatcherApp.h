// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Entry point for the UnrealGamePatcher program (GUI by default, -cli for console mode).
 * The actual WinMain lives in UnrealGamePatcherApp.cpp; this header keeps the module
 * self-contained so the CLI / GUI modules can share dispatch types if needed.
 */
