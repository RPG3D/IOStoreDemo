using UnrealBuildTool;

public class IOStoreDemo : ModuleRules
{
	public IOStoreDemo(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore" });

		PrivateDependencyModuleNames.AddRange(new string[] { 
			"IoStoreOnDemand",
			"IoStoreOnDemandCore",
		});

		if (Target.Platform == UnrealTargetPlatform.Android)
		{
			PrivateDependencyModuleNames.AddRange(new[] { "UnrealGamePatcherCore", "UnrealGamePatcherDownload" });
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac && !Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealGamePatcherCore");
		}
	}
}
