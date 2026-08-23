// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class UnrealGamePatcherTarget : TargetRules
{
	public UnrealGamePatcherTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Program;
		LinkType = TargetLinkType.Monolithic;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		LaunchModuleName = "UnrealGamePatcher";

		// The patcher runs standalone (GUI by default, -cli for console mode); it must not
		// link against the Engine. Only Core / CoreUObject / ApplicationCore / Slate are used.
		bCompileAgainstEngine = false;
		bCompileAgainstCoreUObject = true;
		bCompileAgainstApplicationCore = true;

		bBuildDeveloperTools = false;
		bIsBuildingConsoleApplication = false;
		bUseLoggingInShipping = true;
		bLoggingToMemoryEnabled = false;

		// Place the target under the "Programs" group in the generated solution.
		// Reference: UnrealBuildTool/Configuration/Rules/TestTargetRules.cs
		SolutionDirectory = "/Games/Source/Programs/UnrealGamePatcher";
	}
}
