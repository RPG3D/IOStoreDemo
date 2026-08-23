// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UnrealGamePatcher
{
	/**
	 * Self test for the hash wrappers, comparing against known-good reference
	 * vectors (task 1.3 acceptance).
	 *
	 * XXH3-64 vectors were generated with the official Python xxhash package
	 * (xxh3_64_hexdigest, seed 0) and match xxhsum -H2 output. The FIoHash
	 * vector is BLAKE3-256 of the empty input (leading 160 bits), the standard
	 * published test vector.
	 *
	 * Returns true when every check passes; logs failures otherwise.
	 */
	struct FPatcherHashSelfTest
	{
		UNREALGAMEPATCHERCORE_API static bool Run();
	};
}
