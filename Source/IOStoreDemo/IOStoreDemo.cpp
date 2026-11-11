// Fill out your copyright notice in the Description page of Project Settings.

#include "IOStoreDemo.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_ANDROID
extern bool (*GAndroidPreInitContent)();
extern bool RunUGPAndroidBeforeEnginePreInit();

namespace
{
	struct FRegisterAndroidContentStartup
	{
		FRegisterAndroidContentStartup()
		{
			GAndroidPreInitContent = &RunUGPAndroidBeforeEnginePreInit;
		}
	} RegisterAndroidContentStartup;
}
#endif

IMPLEMENT_PRIMARY_GAME_MODULE( FDefaultGameModuleImpl, IOStoreDemo, "IOStoreDemo" );
