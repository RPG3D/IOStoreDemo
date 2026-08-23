// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Patcher program version constants.
 * Used by the CLI --version output and by the generated index (generator info).
 */
namespace UnrealGamePatcher
{
	// Program version (semver-ish, bumped by release).
	static const TCHAR* const ProgramVersion = TEXT("0.1.0");

	// NOTE: the index format version lives with its format
	// (FIndexManifest::FormatVersion = 2) - do not duplicate it here.
	// The default download concurrency lives in FDownloadManager (8).
}
