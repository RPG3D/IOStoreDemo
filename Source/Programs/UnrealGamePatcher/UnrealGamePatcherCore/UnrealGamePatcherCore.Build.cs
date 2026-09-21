// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UnrealGamePatcherCore : ModuleRules
{
	public UnrealGamePatcherCore(ReadOnlyTargetRules Target) : base(Target)
	{
		// Core layer: format parsing/writing, patch engine, download, transaction, state.
		// No UI dependencies. Engine is used only for low-level capabilities (FCompression, FPaths, FArchive...).
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
			"Core",
			"CoreUObject",
			"ApplicationCore",
			"Json",
			// Download layer: FDownloadManager / orchestrator drive the
			// generic download component (transport primitive + executor).
			"UnrealGamePatcherDownload",
			}
		);
	}
}
