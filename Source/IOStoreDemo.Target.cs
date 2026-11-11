// Fill out your copyright notice in the Description page of Project Settings.

using UnrealBuildTool;
using System.Collections.Generic;

public class IOStoreDemoTarget : TargetRules
{
	public IOStoreDemoTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

        ExtraModuleNames.AddRange( new string[] { "IOStoreDemo" } );

		bCompileIoStoreOnDemand = true;

		// Mobile packages download their game containers into ProjectSavedDir/Paks.
		// Enable the Pak platform file even when the app ships without containers.
		GlobalDefinitions.Add("UE_FORCE_USE_PAKS=1");

		if (Target.Platform == UnrealTargetPlatform.IOS)
		{
			ExtraModuleNames.Add("UGPIOSPatch");
			GlobalDefinitions.Add("WITH_UGP_IOS_PREINIT=1");
		}
    }
}
