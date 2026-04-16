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
	}
}
