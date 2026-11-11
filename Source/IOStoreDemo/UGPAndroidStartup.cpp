// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if PLATFORM_ANDROID

#include "Android/AndroidJavaEnv.h"
#include "Async/Async.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "State/FPatcherState.h"
#include "UnrealGamePatcherLog.h"
#include "UnrealGamePatcherPaths.h"
#include "Update/FUpdateOrchestrator.h"

#include <cerrno>
#include <cstdio>

extern FString GInternalFilePath;

namespace
{
	using namespace UnrealGamePatcher;

	class FAndroidStartup
	{
	public:
		FAndroidStartup()
			: Env(AndroidJavaEnv::GetJavaEnv())
			, Class(AndroidJavaEnv::FindJavaClassGlobalRef("com/epicgames/unreal/patch/UGPStartupView"))
		{
			const jmethodID Constructor = Env->GetMethodID(Class, "<init>", "(Lcom/epicgames/unreal/GameActivity;)V");
			View = Env->NewObject(Class, Constructor, AndroidJavaEnv::GetGameActivityThis());
			Status = Env->GetMethodID(Class, "status", "(Ljava/lang/String;II)V");
			Failure = Env->GetMethodID(Class, "failure", "(ILjava/lang/String;)V");
			Retry = Env->GetMethodID(Class, "awaitRetry", "()V");
			Dismiss = Env->GetMethodID(Class, "dismiss", "()V");
			Download = Env->GetStaticMethodID(Class, "download", "(Ljava/lang/String;JJ)[B");
		}

		~FAndroidStartup()
		{
			Env->CallVoidMethod(View, Dismiss);
			Env->DeleteLocalRef(View);
			Env->DeleteGlobalRef(Class);
		}

		void Report(const FUpdateEvent& Event)
		{
			const auto Text = FJavaHelper::ToJavaString(Env, Event.Message);
			const bool bDownloading = Event.Phase == EUpdatePhase::Download;
			Env->CallVoidMethod(View, Status, *Text,
				bDownloading ? Event.Current : 0, bDownloading ? Event.Total : 0);
		}

		void WaitForRetry(const FUpdateEvent& Event)
		{
			const auto Text = FJavaHelper::ToJavaString(Env, Event.Message);
			Env->CallVoidMethod(View, Failure, static_cast<jint>(Event.Error), *Text);
			Env->CallVoidMethod(View, Retry);
		}

		bool DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& Data) const
		{
			JNIEnv* ThreadEnv = AndroidJavaEnv::GetJavaEnv();
			const auto Resource = FJavaHelper::ToJavaString(ThreadEnv, Url);
			const jbyteArray Bytes = static_cast<jbyteArray>(
				ThreadEnv->CallStaticObjectMethod(Class, Download, *Resource, static_cast<jlong>(Offset), static_cast<jlong>(Size)));
			if (ThreadEnv->ExceptionCheck())
			{
				ThreadEnv->ExceptionDescribe();
				ThreadEnv->ExceptionClear();
				return false;
			}
			if (!Bytes)
			{
				return false;
			}
			const jsize Length = ThreadEnv->GetArrayLength(Bytes);
			Data.SetNumUninitialized(Length);
			ThreadEnv->GetByteArrayRegion(Bytes, 0, Length, reinterpret_cast<jbyte*>(Data.GetData()));
			ThreadEnv->DeleteLocalRef(Bytes);
			return true;
		}

	private:
		JNIEnv* Env;
		jclass Class;
		jobject View;
		jmethodID Status;
		jmethodID Failure;
		jmethodID Retry;
		jmethodID Dismiss;
		jmethodID Download;
	};

	class FAndroidStartupDownload final : public IChunkDownloader
	{
	public:
		explicit FAndroidStartupDownload(FAndroidStartup& InStartup)
			: Startup(InStartup), Pool(FQueuedThreadPool::Allocate())
		{
			bAsync = Pool->Create(8, 0, TPri_Normal, TEXT("UGPDownload"));
		}

		~FAndroidStartupDownload()
		{
			Pool->Destroy();
		}

		bool DownloadRange(const FString& Url, int64 Offset, int64 Size, TArray64<uint8>& Data) override
		{
			return Startup.DownloadRange(Url, Offset, Size, Data);
		}

		FCancelHandle DownloadRangeAsync(const FString& Url, int64 Offset, int64 Size,
			TFunction<void(bool, const TArray64<uint8>&, const FString&)>&& Done) override
		{
			if (!bAsync)
			{
				return IChunkDownloader::DownloadRangeAsync(Url, Offset, Size, MoveTemp(Done));
			}
			AsyncPool(*Pool, [this, Url, Offset, Size, Done = MoveTemp(Done)]()
			{
				TArray64<uint8> Data;
				const bool bOk = DownloadRange(Url, Offset, Size, Data);
				Done(bOk, Data, bOk ? FString() : TEXT("Android HTTP request failed"));
			});
			return nullptr;
		}

	private:
		FAndroidStartup& Startup;
		TUniquePtr<FQueuedThreadPool> Pool;
		bool bAsync;
	};

	bool MoveDirectory(const FString& From, const FString& To)
	{
		if (rename(TCHAR_TO_UTF8(*From), TCHAR_TO_UTF8(*To)) == 0)
		{
			return true;
		}
		UE_LOG(LogUnrealGamePatcher, Error, TEXT("UGP Android: could not rename %s to %s (errno %d)"), *From, *To, errno);
		return false;
	}

	bool DiscardDirectory(IPlatformFile& File, const FString& Directory)
	{
		return !File.DirectoryExists(*Directory) || File.DeleteDirectoryRecursively(*Directory);
	}

	bool IsContainer(const FString& Path)
	{
		const FString Extension = FPaths::GetExtension(Path);
		return Extension == TEXT("pak") || Extension == TEXT("utoc") ||
			Extension == TEXT("ucas") || Extension == TEXT("sig");
	}

	bool ToSavedPakIndex(FIndexManifest& Index, const FString& LegacyPrefix)
	{
		TMap<FString, FIndexFileEntry> Containers;
		bool bHasPak = false;
		for (auto& Entry : Index.Files)
		{
			const FString Name = FPaths::GetCleanFilename(Entry.Key);
			if (!IsContainer(Name) || (Entry.Key != Name && Entry.Key != LegacyPrefix + Name))
			{
				continue;
			}
			if (Containers.Contains(Name))
			{
				return false;
			}
			bHasPak |= Name.EndsWith(TEXT(".pak"));
			Containers.Add(Name, MoveTemp(Entry.Value));
		}
		if (!bHasPak)
		{
			return false;
		}
		Index.Files = MoveTemp(Containers);
		return true;
	}

	bool MigrateLegacyPaks(IPlatformFile& File, const FString& StageRoot, const FString& LegacyPrefix, const FString& InstallDir)
	{
		const FString LegacyPaks = StageRoot / LegacyPrefix.LeftChop(1);
		const FString PreviousStage = StageRoot + TEXT(".ugp-previous");
		const FString Candidate = InstallDir + TEXT(".ugp-migrate");
		// Recover old staged publication if it died before installing the new root.
		if (File.DirectoryExists(*PreviousStage) && !File.DirectoryExists(*StageRoot) && !MoveDirectory(PreviousStage, StageRoot))
		{
			return false;
		}
		FString Version;
		if (!File.DirectoryExists(*InstallDir) && !File.DirectoryExists(*(InstallDir + TEXT(".ugp-previous"))))
		{
			FString Source;
			if (FUpdateOrchestrator::ReadCommittedVersion(StageRoot, Version))
			{
				Source = StageRoot;
			}
			else if (FUpdateOrchestrator::ReadCommittedVersion(PreviousStage, Version))
			{
				Source = PreviousStage;
			}
			if (!Source.IsEmpty())
			{
				FPatcherState State;
				FIndexManifest Index;
				if (!FPatcherStateFile::Load(Paths::GetStatePath(Source), State) ||
					!FIndexManifestFile::Load(Source / TEXT(".ugp/index.json"), Index) ||
					!ToSavedPakIndex(Index, LegacyPrefix) || !DiscardDirectory(File, Candidate) ||
					!File.CreateDirectoryTree(*(Candidate / TEXT(".ugp"))))
				{
					return false;
				}
				for (const auto& Entry : Index.Files)
				{
					if (!File.CopyFile(*(Candidate / Entry.Key), *(Source / LegacyPrefix / Entry.Key)))
					{
						return false;
					}
				}
				State.Holes.RemoveAll([&](FHole& Hole)
				{
					return !Hole.File.RemoveFromStart(LegacyPrefix) || !Index.Files.Contains(Hole.File);
				});
				State.Transaction.Targets.Reset();
				if (!FPatcherStateFile::Save(Paths::GetStatePath(Candidate), State) ||
					!FIndexManifestFile::Save(Candidate / TEXT(".ugp/index.json"), Index) ||
					!FUpdateOrchestrator::ReadCommittedVersion(Candidate, Version) ||
					!MoveDirectory(Candidate, InstallDir))
				{
					return false;
				}
				UE_LOG(LogUnrealGamePatcher, Display, TEXT("UGP Android: migrated %s into %s"), *Version, *InstallDir);
			}
		}

		// Exclude the old containers from UE's Pak scan while retaining their bytes.
		if (!File.DirectoryExists(*LegacyPaks))
		{
			return true;
		}
		FString Archive = LegacyPaks + TEXT(".ugp-legacy");
		if (File.DirectoryExists(*Archive))
		{
			Archive += TEXT("-") + FGuid::NewGuid().ToString();
		}
		return MoveDirectory(LegacyPaks, Archive);
	}

	bool UpdateSavedPaks(const FString& Cdn, const FString& InstallDir, const FString& LegacyPrefix,
		const FString& CacheDir, FAndroidStartup& Startup, FAndroidStartupDownload& Downloader, FUpdateEvent& Failure)
	{
		IPlatformFile& File = FPlatformFileManager::Get().GetPlatformFile();
		const FString Candidate = InstallDir + TEXT(".ugp-update");
		const FString Previous = InstallDir + TEXT(".ugp-previous");
		const auto Fail = [&Failure](EPatcherError Error, const TCHAR* Message)
		{
			Failure = { EUpdatePhase::Failed, Error, true, 0, 0, Message };
			return false;
		};

		FString LocalVersion;
		bool bInstalled = FUpdateOrchestrator::ReadCommittedVersion(InstallDir, LocalVersion);
		if (!bInstalled && File.DirectoryExists(*Previous))
		{
			if (!FUpdateOrchestrator::ReadCommittedVersion(Previous, LocalVersion) || !DiscardDirectory(File, Candidate) ||
				(File.DirectoryExists(*InstallDir) && !MoveDirectory(InstallDir, Candidate)) ||
				!MoveDirectory(Previous, InstallDir))
			{
				return Fail(EPatcherError::RollbackFailed, TEXT("Could not restore the previous game content."));
			}
			bInstalled = true;
		}

		FUpdateOptions Options;
		Options.Downloader = &Downloader;
		Options.BaseUrl = Cdn;
		Options.ChunkCacheDir = CacheDir;
		Options.InstallLockRootDir = GInternalFilePath;
		Options.bForceVerify = !bInstalled;
		Options.PrepareManifest = [&LegacyPrefix](FIndexManifest& Index) { return ToSavedPakIndex(Index, LegacyPrefix); };
		Options.Events = [&Startup, &Failure](const FUpdateEvent& Event)
		{
			if (Event.bIsError)
			{
				Failure = Event;
			}
			else
			{
				Startup.Report(Event);
			}
		};

		FString Latest;
		const auto UseOldVersion = [&]()
		{
			UE_LOG(LogUnrealGamePatcher, Warning, TEXT("UGP Android: [UGP-%04d] using committed content %s"),
				static_cast<int32>(Failure.Error), *LocalVersion);
			return true;
		};
		if (!FUpdateOrchestrator::ResolveLatest(Cdn, Options, Latest))
		{
			return bInstalled && UseOldVersion();
		}
		if (bInstalled && LocalVersion == Latest)
		{
			return true;
		}

		if (!DiscardDirectory(File, Candidate) || !DiscardDirectory(File, Previous))
		{
			Fail(EPatcherError::StagingCleanupFailed, TEXT("Could not clear the update directory."));
			return bInstalled && UseOldVersion();
		}
		ON_SCOPE_EXIT { DiscardDirectory(File, Candidate); };

		if (bInstalled && !File.CopyDirectoryTree(*Candidate, *InstallDir, false))
		{
			Fail(EPatcherError::StagingSnapshotFailed, TEXT("Could not prepare installed game content."));
			return UseOldVersion();
		}
		FUpdateStats Stats;
		if (!FUpdateOrchestrator::UpdateWithOptions(Cdn, Latest, Candidate, true, Options, Stats))
		{
			return bInstalled && UseOldVersion();
		}
		FString CandidateVersion;
		if (!FUpdateOrchestrator::ReadCommittedVersion(Candidate, CandidateVersion) || CandidateVersion != Latest)
		{
			Fail(EPatcherError::InstalledContentInvalid, TEXT("The updated game content did not pass validation."));
			return bInstalled && UseOldVersion();
		}
		if (File.DirectoryExists(*InstallDir) && !MoveDirectory(InstallDir, Previous))
		{
			Fail(EPatcherError::ActivateContentFailed, TEXT("Could not activate updated game content."));
			return bInstalled && UseOldVersion();
		}
		if (!MoveDirectory(Candidate, InstallDir))
		{
			if (File.DirectoryExists(*Previous) && !MoveDirectory(Previous, InstallDir))
			{
				return Fail(EPatcherError::RollbackFailed, TEXT("Could not restore the previous game content."));
			}
			Fail(EPatcherError::ActivateContentFailed, TEXT("Could not activate updated game content."));
			return bInstalled && UseOldVersion();
		}
		DiscardDirectory(File, Previous);
		UE_LOG(LogUnrealGamePatcher, Display, TEXT("UGP Android: activated %s in %s"), *Latest, *InstallDir);
		return true;
	}
}

bool RunUGPAndroidBeforeEnginePreInit()
{
	IPlatformFile& File = FPlatformFileManager::Get().GetPlatformFile();
	FString StageRoot = File.ConvertToAbsolutePathForExternalAppForWrite(TEXT("../../../"));
	FPaths::NormalizeDirectoryName(StageRoot);
	const FString InstallDir = File.ConvertToAbsolutePathForExternalAppForWrite(*(FPaths::ProjectSavedDir() / TEXT("Paks")));
	const FString LegacyPrefix = FString(FApp::GetProjectName()) + TEXT("/Content/Paks/");
	const FString CacheDir = StageRoot + TEXT(".ugp-cache");
	const FString Cdn = TEXT("http://127.0.0.1:18080");
	FAndroidStartup Startup;
	FAndroidStartupDownload Downloader(Startup);
	UE_LOG(LogUnrealGamePatcher, Display, TEXT("UGP Android: before engine PreInit, install=%s"), *InstallDir);

	Paths::FInstallLock StartupLock;
	while (!StartupLock.Acquire(InstallDir, GInternalFilePath / TEXT("UGPStartup")))
	{
		Startup.WaitForRetry({ EUpdatePhase::Failed, EPatcherError::InstallLockFailed, true, 0, 0,
			TEXT("Could not lock the game content directory.") });
	}

	for (;;)
	{
		FUpdateEvent Failure{ EUpdatePhase::Failed, EPatcherError::Internal, true, 0, 0,
			TEXT("The content update could not finish.") };
		if (!MigrateLegacyPaks(File, StageRoot, LegacyPrefix, InstallDir))
		{
			Failure.Error = EPatcherError::LegacyMigrationFailed;
			Failure.Message = TEXT("Could not move previously downloaded game content.");
		}
		else if (UpdateSavedPaks(Cdn, InstallDir, LegacyPrefix, CacheDir, Startup, Downloader, Failure))
		{
			return true;
		}
		Startup.WaitForRetry(Failure);
	}
}

#endif // PLATFORM_ANDROID
