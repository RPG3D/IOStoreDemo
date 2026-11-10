// Copyright Epic Games, Inc. All Rights Reserved.

// Spike (2026-11-02): UnrealGamePatcher as a standalone Android shared library
// (libugp.so) - PatchActivity loads it BEFORE GameActivity ever boots the
// engine, runs the update against writable containers, then hands off.
// Feasibility doc: Docs/android-port-feasibility-2026-11-02.md

using UnrealBuildTool;
using System.Collections.Generic;

[SupportedPlatforms("Android")]
public class UnrealGamePatcherLibTarget : TargetRules
{
	public UnrealGamePatcherLibTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Program;
		LinkType = TargetLinkType.Monolithic;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		LaunchModuleName = "UnrealGamePatcherLib";

		// Same capability set as the standalone patcher: engine Core only,
		// no Engine module, no Slate/UI. The JNI bootstrap drives the engine
		// loop explicitly (PreInit -> update -> AppExit).
		bCompileAgainstEngine = false;
		bCompileAgainstCoreUObject = true;
		bCompileAgainstApplicationCore = true;

		bBuildDeveloperTools = false;
		bUseLoggingInShipping = true;
		bLoggingToMemoryEnabled = false;

		SolutionDirectory = "/Games/Source/Programs/UnrealGamePatcher";
	}
}
