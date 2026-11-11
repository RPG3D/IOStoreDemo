// Copyright Epic Games, Inc. All Rights Reserved.

// libugp.so launch module: JNI bootstrap + engine-lite lifecycle driver.
// Pure Core-level code, no UI; the update engine comes from
// UnrealGamePatcherCore (which pulls UnrealGamePatcherDownload -> HTTP).

using UnrealBuildTool;

public class UnrealGamePatcherLib : ModuleRules
{
	public UnrealGamePatcherLib(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;
		PrivateDefinitions.Add("UGP_BUILD_CONFIGURATION=" + Target.Configuration);

		// RequiredProgramMainCPPInclude.h (FEngineLoop/entry plumbing) is
		// granted via the Launch module's include path - same as the CLI.
		PrivateIncludePathModuleNames.Add("Launch");

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// LaunchEngineLoop.cpp is compiled INTO this module's TU via
				// RequiredProgramMainCPPInclude.h - it needs these include
				// paths on top of Launch's own (same set the CLI module uses).
				"Projects",
				// FUpdateOrchestrator.h includes Download/IChunkDownloader.h;
				// Download is a PRIVATE dep of Core, so its public include
				// paths don't propagate - depend on it directly (same as CLI).
				"UnrealGamePatcherDownload",
				// FAndroidAppEntry glue stubs reference ApplicationCore's
				// AndroidApplication.h declarations.
				"ApplicationCore",
				// ApplicationCore's Android media code (compiled because
				// Launch pulls ApplicationCore) references RenderCore/RHI
				// symbols (FRenderResource base, shader plumbing).
				"RenderCore",
				"RHI",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealGamePatcherCore",
				"Json",
				"HTTP",
			}
		);
	}
}
