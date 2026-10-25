#include "Tests/TestFramework.h"

namespace UnrealGamePatcher::Tests
{
	/** Builds a small staged fixture + ingests it into Cdn, loading the
	 *  resulting index. The fixture is the real small containers (pakchunk1:
	 *  pak/ucas/utoc) plus a plain file - the same input the new build command
	 *  consumes. Returns false on any failure. */
	bool BuildFixtureCdn(
		const FString& WorkDir,
		const FString& Cdn,
		UnrealGamePatcher::FIndexManifest& OutIndex)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		const FString ProjectRoot = FPaths::GetPath(FPaths::GetPath(FPaths::GetPath(FPlatformProcess::BaseDir())));
		const FString StagedSrc = FPaths::Combine(ProjectRoot, TEXT(".manifesttest"), TEXT("chunkinstall"),
			TEXT("Windows"), TEXT("1.0.0"), TEXT("pakchunk1"));
		const FString StagedDir = FPaths::Combine(WorkDir, TEXT("staged"));
		if (!PF.DirectoryExists(*StagedDir) && !PF.CreateDirectoryTree(*StagedDir))
		{
			return false;
		}
		for (const TCHAR* Name : { TEXT("pakchunk1-Windows.pak"), TEXT("pakchunk1-Windows.ucas"), TEXT("pakchunk1-Windows.utoc") })
		{
			if (!PF.CopyFile(*(FPaths::Combine(StagedDir, Name)), *(FPaths::Combine(StagedSrc, Name))))
			{
				return false;
			}
		}
		FFileHelper::SaveStringToFile(TEXT("plain file content"), *(FPaths::Combine(StagedDir, TEXT("boot.txt"))));

		UnrealGamePatcher::FIngestResult Ingest;
		if (!UnrealGamePatcher::FBuildIngestor::Ingest(StagedDir, TEXT("IOStoreDemo"), TEXT("1.0.0"), Cdn, Ingest))
		{
			return false;
		}
		return UnrealGamePatcher::FIndexManifestFile::Load(Ingest.IndexPath, OutIndex);
	}

	/** Verifies a container file is LOGICALLY equivalent to the remote metadata
	 *  (virtual-disk model: per-entry / per-chunk hashes). The whole-file hash
	 *  legitimately differs after an in-place patch - per-entry equality is the
	 *  correct post-repair assertion. Verify-mode truth re-reads the container
	 *  and recomputes payload hashes, so this also proves the damage is gone. */
	bool CheckContainerLogicallyEqual(
		const UnrealGamePatcher::FIndexManifest& Remote,
		const FString& InstallDir,
		const FString& FileId)
	{
		UnrealGamePatcher::FIndexManifest Truth;
		if (!UnrealGamePatcher::FLocalTruthExtractor::ExtractInstall(InstallDir, Truth, nullptr, /*bVerify=*/true))
		{
			return false;
		}
		UnrealGamePatcher::FDiffResult D;
		UnrealGamePatcher::FDiffAnalyzer::DiffFilesVirtualDisk(Truth.Files, Remote.Files, D.Files);
		for (const UnrealGamePatcher::FDiffEntry& E : D.Files)
		{
			if (E.Id == FileId)
			{
				return E.Kind == UnrealGamePatcher::EChangeKind::Same;
			}
		}
		return false;
	}
}
