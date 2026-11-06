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

		// M1 (android-port-feasibility-2026-11-02.md 2.2): the APK ships with ZERO
		// game containers - all content is installed by UGP into the sandbox
		// (Content/Paks) at first run. Force the pak environment on even when
		// the APK has no pak files.
		GlobalDefinitions.Add("UE_FORCE_USE_PAKS=1");
    }
}
