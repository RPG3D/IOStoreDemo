// Copyright Epic Games, Inc. All Rights Reserved.

// S1 全链：仅承载 Android 打包期 UPL（UGPAndroidPatch_APL.xml）——把
// libugp.so 装进 APK、切换 LAUNCHER 到 PatchActivity。运行时无任何代码。
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
