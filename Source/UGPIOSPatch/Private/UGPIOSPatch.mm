// Copyright Epic Games, Inc. All Rights Reserved.

#include "UGPIOSDownload.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "IOS/IOSAppDelegate.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "UnrealGamePatcherPaths.h"
#include "Update/FUpdateOrchestrator.h"

#include <cerrno>
#include <copyfile.h>
#include <stdio.h>

@interface UGPIOSStartupView : UIView
@property (nonatomic, strong) UILabel* statusLabel;
@property (nonatomic, strong) UIProgressView* progress;
@property (nonatomic, strong) UIView* failureDialog;
@property (nonatomic, strong) UILabel* failureLabel;
@property (nonatomic, strong) UILabel* retryStatusLabel;
@property (nonatomic, strong) UIButton* retryButton;
@property (nonatomic, strong) UIActivityIndicatorView* retryActivity;
@property (nonatomic, strong) dispatch_semaphore_t retrySignal;
@property (nonatomic) NSUInteger failedRetryCount;
@property (nonatomic) BOOL retryInFlight;
- (void)showFailure:(NSString*)Message;
- (void)updateStatus:(NSString*)Status progress:(float)Progress;
@end

@implementation UGPIOSStartupView

- (instancetype)initWithFrame:(CGRect)Frame
{
	self = [super initWithFrame:Frame];
	if (!self)
	{
		return nil;
	}
	self.backgroundColor = UIColor.systemBackgroundColor;
	UILabel* Title = [UILabel new];
	Title.text = @"IOStoreDemo Content Update";
	Title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
	Title.textAlignment = NSTextAlignmentCenter;
	Title.translatesAutoresizingMaskIntoConstraints = NO;
	[self addSubview:Title];
	self.statusLabel = [UILabel new];
	self.statusLabel.textAlignment = NSTextAlignmentCenter;
	self.statusLabel.numberOfLines = 0;
	self.progress = [UIProgressView new];
	self.retryActivity = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
	self.retryActivity.hidesWhenStopped = YES;
	self.retrySignal = dispatch_semaphore_create(0);
	UIStackView* Stack = [[UIStackView alloc] initWithArrangedSubviews:@[self.retryActivity, self.statusLabel, self.progress]];
	Stack.axis = UILayoutConstraintAxisVertical;
	Stack.spacing = 16;
	Stack.translatesAutoresizingMaskIntoConstraints = NO;
	[self addSubview:Stack];
	[NSLayoutConstraint activateConstraints:@[
		[Title.centerXAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.centerXAnchor],
		[Title.centerYAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.centerYAnchor],
		[Stack.bottomAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.bottomAnchor constant:-24],
		[Stack.leadingAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.leadingAnchor constant:24],
		[Stack.trailingAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.trailingAnchor constant:-24],
	]];

	self.failureDialog = [[UIView alloc] initWithFrame:self.bounds];
	self.failureDialog.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	self.failureDialog.backgroundColor = [UIColor colorWithWhite:0 alpha:0.35];
	self.failureDialog.accessibilityViewIsModal = YES;
	self.failureDialog.hidden = YES;
	[self addSubview:self.failureDialog];

	UILabel* FailureTitle = [UILabel new];
	FailureTitle.text = @"Content Update Failed";
	FailureTitle.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
	FailureTitle.textAlignment = NSTextAlignmentCenter;
	self.failureLabel = [UILabel new];
	self.failureLabel.numberOfLines = 0;
	self.failureLabel.textAlignment = NSTextAlignmentCenter;
	self.retryStatusLabel = [UILabel new];
	self.retryStatusLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
	self.retryStatusLabel.numberOfLines = 0;
	self.retryStatusLabel.textAlignment = NSTextAlignmentCenter;
	self.retryButton = [UIButton buttonWithType:UIButtonTypeSystem];
	[self.retryButton setTitle:@"Retry" forState:UIControlStateNormal];
	[self.retryButton addTarget:self action:@selector(retry) forControlEvents:UIControlEventTouchUpInside];

	UIStackView* Dialog = [[UIStackView alloc] initWithArrangedSubviews:@[
		FailureTitle, self.failureLabel, self.retryStatusLabel, self.retryButton
	]];
	Dialog.axis = UILayoutConstraintAxisVertical;
	Dialog.spacing = 12;
	Dialog.layoutMargins = UIEdgeInsetsMake(20, 24, 12, 24);
	Dialog.layoutMarginsRelativeArrangement = YES;
	Dialog.backgroundColor = UIColor.secondarySystemBackgroundColor;
	Dialog.layer.cornerRadius = 14;
	Dialog.translatesAutoresizingMaskIntoConstraints = NO;
	[self.failureDialog addSubview:Dialog];
	[NSLayoutConstraint activateConstraints:@[
		[Dialog.centerXAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.centerXAnchor],
		[Dialog.centerYAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.centerYAnchor],
		[Dialog.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.safeAreaLayoutGuide.leadingAnchor constant:24],
		[Dialog.trailingAnchor constraintLessThanOrEqualToAnchor:self.safeAreaLayoutGuide.trailingAnchor constant:-24],
		[Dialog.widthAnchor constraintLessThanOrEqualToConstant:360],
		[self.retryButton.heightAnchor constraintGreaterThanOrEqualToConstant:44],
	]];
	return self;
}

- (void)showFailure:(NSString*)Message
{
	if (self.retryInFlight)
	{
		++self.failedRetryCount;
	}
	self.retryInFlight = NO;
	self.failureLabel.text = Message;
	self.retryStatusLabel.text = [NSString stringWithFormat:@"Failed retries: %lu", (unsigned long)self.failedRetryCount];
	[self.retryActivity stopAnimating];
	self.retryButton.enabled = YES;
	self.failureDialog.hidden = NO;
	NSLog(@"UGP: showing retry dialog %@\n%@", Message, self.retryStatusLabel.text);
}

- (void)updateStatus:(NSString*)Status progress:(float)Progress
{
	self.statusLabel.text = self.retryInFlight
		? [NSString stringWithFormat:@"Retry %lu: %@", (unsigned long)(self.failedRetryCount + 1), Status]
		: Status;
	self.progress.progress = Progress;
}

- (void)retry
{
	if (self.failureDialog.hidden || self.retryInFlight)
	{
		return;
	}
	self.retryInFlight = YES;
	self.retryButton.enabled = NO;
	self.failureDialog.hidden = YES;
	[self.retryActivity startAnimating];
	[self updateStatus:@"Starting..." progress:0];
	NSLog(@"UGP: retry %lu started", (unsigned long)(self.failedRetryCount + 1));
	dispatch_semaphore_signal(self.retrySignal);
}

@end

namespace
{
	using namespace UnrealGamePatcher;

	FString ResolveWritablePath(IPlatformFile& File, const FString& Path)
	{
		FString PhysicalPath = File.ConvertToAbsolutePathForExternalAppForWrite(*Path);
		if (PhysicalPath.StartsWith(TEXT("/private/var/")))
		{
			PhysicalPath.RemoveFromStart(TEXT("/private")); // iOS platform file's absolute path prefix
		}
		return PhysicalPath;
	}

	void SetStatus(UGPIOSStartupView* View, NSString* Text, float Progress = 0)
	{
		dispatch_async(dispatch_get_main_queue(), ^{
			[View updateStatus:Text progress:Progress];
		});
	}

	void ShowRetryDialog(UGPIOSStartupView* View, EPatcherError Error, NSString* Detail)
	{
		NSString* Message = [NSString stringWithFormat:@"UGP-%04d\n%@", static_cast<int32>(Error), Detail];
		dispatch_async(dispatch_get_main_queue(), ^{
			[View showFailure:Message];
		});
	}

	bool MigrateLegacyInstall(IPlatformFile& File, const FString& LegacyDir, const FString& InstallDir)
	{
		if (!File.DirectoryExists(*(LegacyDir / TEXT(".ugp"))))
		{
			return true;
		}

		FString LegacyVersion;
		const bool bCanReuse = !File.DirectoryExists(*InstallDir) && FUpdateOrchestrator::ReadCommittedVersion(LegacyDir, LegacyVersion);
		// Keep interrupted installs outside the engine's Pak search paths. Their
		// transaction records still refer to the old absolute paths.
		const FString Destination = bCanReuse ? InstallDir : LegacyDir + TEXT(".ugp-legacy-") + FString(NSUUID.UUID.UUIDString);
		NSError* Error = nil;
		if (!File.CreateDirectoryTree(*FPaths::GetPath(Destination)) ||
			![NSFileManager.defaultManager moveItemAtPath:LegacyDir.GetNSString() toPath:Destination.GetNSString() error:&Error])
		{
			NSLog(@"UGP: could not move legacy Pak directory: %@", Error);
			return false;
		}
		NSLog(@"UGP: moved legacy Pak directory to %@", Destination.GetNSString());
		return true;
	}

	bool DiscardUpdateDirectory(IPlatformFile& File, const FString& UpdateDir)
	{
		if (File.DirectoryExists(*UpdateDir) && !File.DeleteDirectoryRecursively(*UpdateDir))
		{
			NSLog(@"UGP: [UGP-%04d] could not remove update directory %@",
				static_cast<int32>(EPatcherError::StagingCleanupFailed), UpdateDir.GetNSString());
			return false;
		}
		return true;
	}

	bool ApplyUpdate(const FString& Cdn, const FString& Version, const FString& UpdateDir, const FUpdateOptions& Options, FUpdateEvent& Failure)
	{
#if !UE_BUILD_SHIPPING
		TGuardValue<int32> InjectFailure(FUpdateOrchestrator::TestFailAfterWrites, 0);
		FParse::Value(FCommandLine::Get(), TEXT("-ugpinjectfail="), FUpdateOrchestrator::TestFailAfterWrites);
#endif
		FUpdateStats Stats;
		if (!FUpdateOrchestrator::UpdateWithOptions(Cdn, Version, UpdateDir, true, Options, Stats))
		{
			return false;
		}

		FString InstalledVersion;
		if (!FUpdateOrchestrator::ReadCommittedVersion(UpdateDir, InstalledVersion) || InstalledVersion != Version)
		{
			Failure.Error = EPatcherError::InstalledContentInvalid;
			Failure.Message = TEXT("The installed content is missing a mountable Pak or has an invalid version.");
			return false;
		}
		return true;
	}

	bool RunUpdate(UGPIOSStartupView* View, const FString& Cdn, const FString& InstallDir)
	{
		SetStatus(View, @"Checking content version...");
		Paths::FInstallLock StartupLock;
		if (!StartupLock.Acquire(InstallDir, FPaths::GetPath(InstallDir)))
		{
			ShowRetryDialog(View, EPatcherError::InstallLockFailed, @"Could not lock the game content directory.");
			return false;
		}

		IPlatformFile& File = FPlatformFileManager::Get().GetPlatformFile();
		// A sibling of Paks is outside UE's recursive Pak scan. After an
		// interruption it contains either an unpublished update or the old
		// install left over after a successful atomic exchange.
		const FString UpdateDir = InstallDir + TEXT(".ugp-update");
		const bool bUpdateDirAvailable = DiscardUpdateDirectory(File, UpdateDir);
		FString LocalVersion;
		const bool bInstalled = FUpdateOrchestrator::ReadCommittedVersion(InstallDir, LocalVersion);
		FUGPIOSDownload Downloader;
		FUpdateOptions Options;
		Options.Downloader = &Downloader;
		Options.BaseUrl = Cdn;
		Options.bForceVerify = !bInstalled;
		FUpdateEvent Failure;
		Failure.Error = EPatcherError::Internal;
		Failure.Message = TEXT("Check the network and CDN, then retry.");
		Options.Events = [View, &Failure](const FUpdateEvent& Event)
		{
			if (Event.bIsError)
			{
				Failure = Event;
			}
			else
			{
				SetStatus(View, Event.Message.GetNSString(), Event.Total > 0 ? float(Event.Current) / Event.Total : 0);
			}
		};

		FString Latest;
		if (!FUpdateOrchestrator::ResolveLatest(Cdn, Options, Latest))
		{
			if (bInstalled)
			{
				NSLog(@"UGP: [UGP-%04d] cannot resolve CDN version, using committed content %@",
					static_cast<int32>(Failure.Error), LocalVersion.GetNSString());
				return true;
			}
			Failure.Message = TEXT("Could not read the content version. Check the network and CDN, then retry.");
		}
		else if (bInstalled && LocalVersion == Latest)
		{
			return true;
		}
		else if (!bInstalled)
		{
			if (ApplyUpdate(Cdn, Latest, InstallDir, Options, Failure))
			{
				return true;
			}
		}
		else
		{
			if (bUpdateDirAvailable)
			{
				ON_SCOPE_EXIT { DiscardUpdateDirectory(File, UpdateDir); };
				// APFS clones share blocks until the staged copy is modified.
				// The active index and every active container remain untouched.
				if (copyfile(TCHAR_TO_UTF8(*InstallDir), TCHAR_TO_UTF8(*UpdateDir), nullptr,
					COPYFILE_RECURSIVE | COPYFILE_CLONE | COPYFILE_NOFOLLOW) != 0)
				{
					Failure.Error = EPatcherError::StagingSnapshotFailed;
					NSLog(@"UGP: could not clone installed content for update (errno %d)", errno);
				}
				else if (ApplyUpdate(Cdn, Latest, UpdateDir, Options, Failure))
				{
					if (renamex_np(TCHAR_TO_UTF8(*UpdateDir), TCHAR_TO_UTF8(*InstallDir), RENAME_SWAP) == 0)
					{
						NSLog(@"UGP: activated content %@ in %@", Latest.GetNSString(), InstallDir.GetNSString());
						return true;
					}
					Failure.Error = EPatcherError::ActivateContentFailed;
					NSLog(@"UGP: could not activate content %@ (errno %d)", Latest.GetNSString(), errno);
				}
			}
			else
			{
				Failure.Error = EPatcherError::StagingCleanupFailed;
			}
			NSLog(@"UGP: [UGP-%04d] update to %@ failed, using committed content %@",
				static_cast<int32>(Failure.Error), Latest.GetNSString(), LocalVersion.GetNSString());
			return true;
		}
		ShowRetryDialog(View, Failure.Error, Failure.Message.GetNSString());
		return false;
	}
}

// Called once by LaunchIOS on the existing game thread. The main thread
// owns the UIKit view and remains free while the patcher waits for retry.
void RunUGPIOSBeforeEnginePreInit()
{
	@autoreleasepool
	{
		NSString* BundleCdn = [NSBundle.mainBundle objectForInfoDictionaryKey:@"UGPCdnURL"];
		FString Cdn = [BundleCdn isKindOfClass:[NSString class]] ? FString(BundleCdn) : FString();
#if !UE_BUILD_SHIPPING
		FParse::Value(FCommandLine::Get(), TEXT("-ugpcdn="), Cdn);
#endif
		Cdn.TrimStartAndEndInline();
		while (Cdn.RemoveFromEnd(TEXT("/"))) {}
		if (Cdn.IsEmpty())
		{
			return;
		}

		const FString PakDir = FPaths::ProjectSavedDir() / TEXT("Paks");
		IPlatformFile& File = FPlatformFileManager::Get().GetPlatformFile();
		const FString LegacyPakDir = FPaths::ProjectContentDir() / TEXT("Paks");
		const FString LegacyDir = ResolveWritablePath(File, LegacyPakDir);
		const FString InstallDir = ResolveWritablePath(File, PakDir);
		FString BundledPakDir = LegacyPakDir;
		BundledPakDir.ReplaceInline(TEXT("../"), TEXT(""));
		BundledPakDir = TEXT("~/cookeddata/") / BundledPakDir.ToLower();

		__block UGPIOSStartupView* View = nil;
		dispatch_sync(dispatch_get_main_queue(), ^{
			UIWindow* Window = [IOSAppDelegate GetDelegate].Window;
			View = [[UGPIOSStartupView alloc] initWithFrame:Window.bounds];
			View.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
			[Window addSubview:View];
		});
		NSLog(@"UGP: before engine PreInit, install=%@", InstallDir.GetNSString());
		for (;;)
		{
			@autoreleasepool
			{
				if (!Cdn.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase) && !Cdn.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase))
				{
					ShowRetryDialog(View, EPatcherError::InvalidConfiguration, @"UGPCdnURL must be an HTTP or HTTPS URL.");
				}
				else if (File.DirectoryExists(*BundledPakDir))
				{
					ShowRetryDialog(View, EPatcherError::InvalidConfiguration, @"The app bundle must omit the Content/Paks directory.");
				}
				else if (!MigrateLegacyInstall(File, LegacyDir, InstallDir))
				{
					ShowRetryDialog(View, EPatcherError::LegacyMigrationFailed, @"Could not move previously downloaded game content.");
				}
				else if (RunUpdate(View, Cdn, InstallDir))
				{
					break;
				}
			}
			dispatch_semaphore_wait(View.retrySignal, DISPATCH_TIME_FOREVER);
		}
		NSLog(@"UGP: content ready, continuing engine PreInit");
		dispatch_sync(dispatch_get_main_queue(), ^{ [View removeFromSuperview]; });
	}
}

IMPLEMENT_MODULE(FDefaultModuleImpl, UGPIOSPatch)
