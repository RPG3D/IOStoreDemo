// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * CLI mode entry. Runs the patcher without a Slate window; prints text and returns an exit code.
 * Receives the command line with the leading executable token already removed
 * (see FCommandLine::RemoveExeName); arguments are read via FParse::Value /
 * FParse::Param straight off this line.
 *
 * Exit codes:
 *   0  success
 *   1  failure
 *   2  argument/usage error
 */
int32 RunUnrealGamePatcherCLI(const TCHAR* CommandLine);
