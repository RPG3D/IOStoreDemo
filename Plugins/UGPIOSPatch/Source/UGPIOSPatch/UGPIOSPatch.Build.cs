// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UGPIOSPatch : ModuleRules
{
	public UGPIOSPatch(ReadOnlyTargetRules Target) : base(Target)
	{
		// UE's shared PCH is compiled without ARC.
		PCHUsage = PCHUsageMode.NoPCHs;
		CppStandard = CppStandardVersion.Cpp20;
		bEnableObjCAutomaticReferenceCounting = true;
		PublicFrameworks.AddRange(new[] { "Foundation", "UIKit" });
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"ApplicationCore",
			"UnrealGamePatcherCore",
			"UnrealGamePatcherDownload",
		});
	}
}
