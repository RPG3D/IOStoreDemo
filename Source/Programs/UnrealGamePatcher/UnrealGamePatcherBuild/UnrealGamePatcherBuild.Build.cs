// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UnrealGamePatcherBuild : ModuleRules
{
	public UnrealGamePatcherBuild(ReadOnlyTargetRules Target) : base(Target)
	{
		// Build-side tooling: ingest (packaging output -> CDN index + chunks) and diff.
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"ApplicationCore",
				"UnrealGamePatcherCore",
			}
		);
	}
}
