// Fill out your copyright notice in the Description page of Project Settings.

#include "IOStoreDemo.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_MAC && !WITH_EDITOR
#include "Misc/CoreDelegates.h"

extern void LockUGPMacContentBeforeMount();

namespace
{
	const FDelegateHandle MacContentLockRegistration =
		FCoreDelegates::GetPreMainInitDelegate().AddStatic(&LockUGPMacContentBeforeMount);
}
#endif

IMPLEMENT_PRIMARY_GAME_MODULE( FDefaultGameModuleImpl, IOStoreDemo, "IOStoreDemo" );
