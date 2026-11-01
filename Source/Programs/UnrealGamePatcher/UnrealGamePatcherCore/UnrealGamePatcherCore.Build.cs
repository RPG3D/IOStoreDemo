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

		// Json is a PUBLIC dependency: the module's public header JsonIo.h
		// includes Dom/JsonObject.h, so every module that includes JsonIo.h
		// needs the Json include paths propagated (UBT only forwards them
		// through public dependencies).
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
			"Core",
			"CoreUObject",
			"ApplicationCore",
			"Json",
			}
		);
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
			// Download layer: FDownloadManager / orchestrator drive the
			// generic download component (transport primitive + executor).
			"UnrealGamePatcherDownload",
			}
		);
	}
}
