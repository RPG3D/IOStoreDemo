// Copyright Epic Games, Inc. All Rights Reserved.

// Registers the packaging rules for the in-process startup updater.
//
// UBT 不会自动发现 APL 文件：必须像引擎的 AndroidPermission 插件那样，在
// Build.cs 里把 APL 登记进 target receipt（AndroidPlugin 属性），
// UEDeployAndroid 才会在打包时执行它。

using UnrealBuildTool;
using System.IO;

public class UGPAndroidPatch : ModuleRules
{
	public UGPAndroidPatch(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.Add("Core");

		if (Target.Platform == UnrealTargetPlatform.Android)
		{
			string PluginPath = Utils.MakePathRelativeTo(ModuleDirectory, Target.RelativeEnginePath);
			AdditionalPropertiesForReceipt.Add("AndroidPlugin", Path.Combine(PluginPath, "UGPAndroidPatch_APL.xml"));
		}
	}
}
