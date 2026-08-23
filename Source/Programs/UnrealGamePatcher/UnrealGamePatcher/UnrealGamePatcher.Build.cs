// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UnrealGamePatcher : ModuleRules
{
	public UnrealGamePatcher(ReadOnlyTargetRules Target) : base(Target)
	{
		// Frontend module (was three: UnrealGamePatcher / UnrealGamePatcherCLI /
		// UnrealGamePatcherGUI): WinMain + CLI dispatch + tests + Slate GUI.
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;
		PrivateIncludePathModuleNames.Add("Launch");
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"ApplicationCore",
				"Projects",
				"Slate",
				"SlateCore",
				"StandaloneRenderer",
				// FHttpModule init for the unified -cdn http(s):// mode.
				"HTTP",
				// Internal logic layers
				"UnrealGamePatcherCore",
				"UnrealGamePatcherDownload",
				"UnrealGamePatcherBuild",
			}
		);
	}
}
