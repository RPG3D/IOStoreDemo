// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UnrealGamePatcherDownload : ModuleRules
{
	public UnrealGamePatcherDownload(ReadOnlyTargetRules Target) : base(Target)
	{
		// Generic download component: transport primitive + window executor +
		// journal resume. No UI, no business dependencies (reusable as-is).
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				// The primitive's public async API exposes engine request
				// handles (FHttpAsyncRequest::WeakRequest) - engine HTTP types
				// are part of the component's public surface.
				"HTTP",
			}
		);
	}
}
