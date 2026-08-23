// Copyright Epic Games, Inc. All Rights Reserved.

#include "FBuildDiffer.h"
#include "UnrealGamePatcherLog.h"
#include "UnrealGamePatcherPaths.h"
#include "Hash/FPatcherHash.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UnrealGamePatcher
{
	namespace
	{
		// Same strict readers as FIndexManifest.cpp.
		bool ReadInt64Field(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, int64& Out)
		{
			const TSharedPtr<FJsonValue>* Value = Obj->Values.Find(Key);
			if (!Value || !(*Value).IsValid() || (*Value)->Type != EJson::Number)
			{
				return false;
			}
			Out = (int64)(*Value)->AsNumber();
			return true;
		}

		bool ReadStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, FString& Out)
		{
			const TSharedPtr<FJsonValue>* Value = Obj->Values.Find(Key);
			if (!Value || !(*Value).IsValid() || (*Value)->Type != EJson::String)
			{
				return false;
			}
			Out = (*Value)->AsString();
			return true;
		}

		bool ReadObjectField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, TSharedPtr<FJsonObject>& Out)
		{
			const TSharedPtr<FJsonValue>* Value = Obj->Values.Find(Key);
			if (!Value || !(*Value).IsValid() || (*Value)->Type != EJson::Object)
			{
				return false;
			}
			Out = (*Value)->AsObject();
			return true;
		}

		bool IsHex40(const FString& Str)
		{
			if (Str.Len() != 40) return false;
			for (const TCHAR C : Str)
			{
				if (!(C >= TEXT('0') && C <= TEXT('9')) &&
					!(C >= TEXT('a') && C <= TEXT('f')) &&
					!(C >= TEXT('A') && C <= TEXT('F')))
				{
					return false;
				}
			}
			return true;
		}

		/** Canonical body for the delta manifest; fixed key order so the
		 *  self-check hash is deterministic (same scheme as the index). */
		bool SerializeDeltaBody(const FDeltaManifest& D, bool bIncludeSelfCheck, const FString& SelfCheckHex, FString& OutJson)
		{
			TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("format"), FDeltaManifest::FormatName);
			Root->SetNumberField(TEXT("version"), FDeltaManifest::FormatVersion);
			Root->SetStringField(TEXT("appId"), D.AppId);
			Root->SetStringField(TEXT("baseVersion"), D.BaseVersion);
			Root->SetStringField(TEXT("targetVersion"), D.TargetVersion);
	
			// Files: sorted keys.
			{
				TSharedPtr<FJsonObject> FilesObj = MakeShared<FJsonObject>();
				TArray<FString> Keys;
				D.Files.GetKeys(Keys);
				Keys.Sort();
				for (const FString& Key : Keys)
				{
					const FIndexFileEntry& E = D.Files[Key];
					TSharedPtr<FJsonObject> EObj = MakeShared<FJsonObject>();
					EObj->SetStringField(TEXT("sha1"), E.SHA1);
					EObj->SetNumberField(TEXT("size"), static_cast<double>(E.Size));
					FilesObj->SetObjectField(Key, EObj);
				}
				Root->SetObjectField(TEXT("files"), FilesObj);
			}

			// Removed files: sorted array.
			{
				TArray<FString> Removed = D.RemovedFiles;
				Removed.Sort();
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (const FString& F : Removed)
				{
					Arr.Add(MakeShared<FJsonValueString>(F));
				}
				Root->SetArrayField(TEXT("removedFiles"), Arr);
			}

			// Chunks: sorted keys.
			{
				TSharedPtr<FJsonObject> ChunksObj = MakeShared<FJsonObject>();
				TArray<FString> Keys;
				D.Chunks.GetKeys(Keys);
				Keys.Sort();
				for (const FString& Key : Keys)
				{
					const FIndexChunkEntry& E = D.Chunks[Key];
					TSharedPtr<FJsonObject> EObj = MakeShared<FJsonObject>();
					EObj->SetStringField(TEXT("sha1"), E.SHA1);
					EObj->SetNumberField(TEXT("size"), static_cast<double>(E.Size));
					EObj->SetStringField(TEXT("file"), E.File);
					ChunksObj->SetObjectField(Key, EObj);
				}
				Root->SetObjectField(TEXT("chunks"), ChunksObj);
			}

			if (bIncludeSelfCheck)
			{
				Root->SetStringField(TEXT("selfCheck"), SelfCheckHex);
			}

			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
			return FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		}
	}

	bool FDeltaManifestFile::Save(const FString& Path, const FDeltaManifest& Delta)
	{
		FString Body;
		if (!SerializeDeltaBody(Delta, false, FString(), Body))
		{
			return false;
		}
		const FString SelfCheckHex = FPatcherHash::XXH64ToHex(Body.GetCharArray().GetData(), Body.Len() * sizeof(TCHAR));

		FString Full;
		if (!SerializeDeltaBody(Delta, true, SelfCheckHex, Full))
		{
			return false;
		}

		const FString TempPath = Path + TEXT(".tmp");
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (PF.FileExists(*TempPath))
		{
			PF.DeleteFile(*TempPath);
		}
		if (!FFileHelper::SaveStringToFile(Full, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDeltaManifest: failed writing '%s'"), *TempPath);
			return false;
		}
		if (!Paths::AtomicReplaceFile(TempPath, Path))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDeltaManifest: failed moving '%s' to '%s'"), *TempPath, *Path);
			return false;
		}
		return true;
	}

	bool FDeltaManifestFile::Load(const FString& Path, FDeltaManifest& OutDelta)
	{
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *Path))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDeltaManifest: cannot read '%s'"), *Path);
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDeltaManifest: '%s' is not valid JSON"), *Path);
			return false;
		}

		FString Format;
		if (!ReadStringField(Root, TEXT("format"), Format) || !Format.Equals(FDeltaManifest::FormatName))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDeltaManifest: '%s' has missing/invalid 'format'"), *Path);
			return false;
		}
		int64 Version = 0;
		if (!ReadInt64Field(Root, TEXT("version"), Version) || Version != FDeltaManifest::FormatVersion)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDeltaManifest: '%s' has unsupported 'version'"), *Path);
			return false;
		}
		if (!ReadStringField(Root, TEXT("appId"), OutDelta.AppId) || OutDelta.AppId.IsEmpty() ||
			!ReadStringField(Root, TEXT("baseVersion"), OutDelta.BaseVersion) ||
			!ReadStringField(Root, TEXT("targetVersion"), OutDelta.TargetVersion) || OutDelta.TargetVersion.IsEmpty())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDeltaManifest: '%s' missing header fields"), *Path);
			return false;
		}
		// NOTE: updateStrategy was removed from the format (write-only legacy);
		// old delta manifests carrying it load fine - unknown fields are skipped.

		OutDelta.Files.Empty();
		{
			TSharedPtr<FJsonObject> FilesObj;
			if (!ReadObjectField(Root, TEXT("files"), FilesObj))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDeltaManifest: '%s' missing 'files'"), *Path);
				return false;
			}
			for (const auto& Pair : FilesObj->Values)
			{
				const TSharedPtr<FJsonObject> EObj = Pair.Value->AsObject();
				FIndexFileEntry E;
				if (!EObj.IsValid() || !ReadStringField(EObj, TEXT("sha1"), E.SHA1) || !IsHex40(E.SHA1) ||
					!ReadInt64Field(EObj, TEXT("size"), E.Size) || E.Size < 0)
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDeltaManifest: bad file entry '%s' in '%s'"), *FString(Pair.Key), *Path);
					return false;
				}
				OutDelta.Files.Add(FString(Pair.Key), E);
			}
		}

		OutDelta.RemovedFiles.Empty();
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Root->TryGetArrayField(TEXT("removedFiles"), Arr))
			{
				for (const TSharedPtr<FJsonValue>& V : *Arr)
				{
					FString F;
					if (!V->TryGetString(F))
					{
						UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDeltaManifest: bad removedFiles entry in '%s'"), *Path);
						return false;
					}
					OutDelta.RemovedFiles.Add(F);
				}
			}
		}

		OutDelta.Chunks.Empty();
		{
			TSharedPtr<FJsonObject> ChunksObj;
			if (!ReadObjectField(Root, TEXT("chunks"), ChunksObj))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDeltaManifest: '%s' missing 'chunks'"), *Path);
				return false;
			}
			for (const auto& Pair : ChunksObj->Values)
			{
				const TSharedPtr<FJsonObject> EObj = Pair.Value->AsObject();
				FIndexChunkEntry E;
				if (!EObj.IsValid() ||
					!ReadStringField(EObj, TEXT("sha1"), E.SHA1) || !IsHex40(E.SHA1) ||
					!ReadInt64Field(EObj, TEXT("size"), E.Size) || E.Size < 0)
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDeltaManifest: bad chunk entry '%s' in '%s'"), *FString(Pair.Key), *Path);
					return false;
				}
				ReadStringField(EObj, TEXT("file"), E.File);
				OutDelta.Chunks.Add(FString(Pair.Key), E);
			}
		}

		// Self-check over the same canonical body.
		FString SelfCheckHex;
		if (!ReadStringField(Root, TEXT("selfCheck"), SelfCheckHex) || SelfCheckHex.Len() != 16)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDeltaManifest: '%s' missing/invalid 'selfCheck'"), *Path);
			return false;
		}
		FString Body;
		if (!SerializeDeltaBody(OutDelta, false, FString(), Body))
		{
			return false;
		}
		const FString Recomputed = FPatcherHash::XXH64ToHex(Body.GetCharArray().GetData(), Body.Len() * sizeof(TCHAR));
		if (!Recomputed.Equals(SelfCheckHex, ESearchCase::IgnoreCase))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FDeltaManifest: '%s' FAILED self-check (embedded %s, recomputed %s)"),
				*Path, *SelfCheckHex, *Recomputed);
			return false;
		}

		return true;
	}

	bool FBuildDiffer::Diff(
		const FIndexManifest& Base,
		const FIndexManifest& Target,
		const FString& TargetCdnRoot,
		const FString& OutDir,
		bool bCopyChunks,
		FDiffOutput& Out)
	{
		if (!Base.AppId.Equals(Target.AppId, ESearchCase::IgnoreCase))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FBuildDiffer: app mismatch ('%s' vs '%s')"), *Base.AppId, *Target.AppId);
			return false;
		}

		Out.Raw = FDiffAnalyzer::Diff(Base, Target);

		// Summary counters.
		FDeltaSummary& S = Out.Summary;
		for (const FDiffEntry& E : Out.Raw.Files)
		{
			switch (E.Kind)
			{
			case EChangeKind::Same: ++S.NumFilesSame; break;
			case EChangeKind::Changed: ++S.NumFilesChanged; break;
			case EChangeKind::Added: ++S.NumFilesAdded; break;
			case EChangeKind::Removed: ++S.NumFilesRemoved; break;
			}
		}
		for (const FDiffEntry& E : Out.Raw.Chunks)
		{
			switch (E.Kind)
			{
			case EChangeKind::Same: ++S.NumChunksSame; break;
			case EChangeKind::Changed: ++S.NumChunksChanged; break;
			case EChangeKind::Added: ++S.NumChunksAdded; break;
			case EChangeKind::Removed: ++S.NumChunksRemoved; break;
			}
		}

		// Delta manifest: target state of added/changed files + removed list
		// + every chunk the client must download.
		FDeltaManifest& Delta = Out.Delta;
		Delta.AppId = Target.AppId;
		Delta.BaseVersion = Base.ContentVersion;
		Delta.TargetVersion = Target.ContentVersion;

		for (const FDiffEntry& E : Out.Raw.Files)
		{
			if (E.Kind == EChangeKind::Added || E.Kind == EChangeKind::Changed)
			{
				Delta.Files.Add(E.Id, Target.Files[E.Id]);
			}
			else if (E.Kind == EChangeKind::Removed)
			{
				Delta.RemovedFiles.Add(E.Id);
			}
		}

		for (const FDiffEntry& E : Out.Raw.Chunks)
		{
			if (E.Kind == EChangeKind::Added || E.Kind == EChangeKind::Changed)
			{
				const FIndexChunkEntry& Entry = Target.Chunks[E.Id];
				Delta.Chunks.Add(E.Id, Entry);
				S.DownloadBytes += Entry.Size;
			}
		}

		// Write the delta manifest (deterministic) AFTER the chunk staging
		// below - a self-consistent delta.json must never reference chunks
		// that failed to stage into OutDir.
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (!PF.DirectoryExists(*OutDir))
		{
			PF.CreateDirectoryTree(*OutDir);
		}

		// Copy the referenced chunk files next to the delta.
		if (bCopyChunks)
		{
			for (const TPair<FString, FIndexChunkEntry>& Pair : Delta.Chunks)
			{
				const FIndexChunkEntry& E = Pair.Value;
				if (E.File.IsEmpty())
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FBuildDiffer: chunk '%s' has no CDN file path (re-ingest target version)"), *Pair.Key);
					return false;
				}
				// Chunk entities are GLOBAL (content-addressed at the CDN root):
				// copy from <targetcdn>/chunks/... (no version subdir).
				const FString Src = FPaths::Combine(TargetCdnRoot, E.File);
				const FString Dst = FPaths::Combine(OutDir, E.File);
				if (!PF.DirectoryExists(*FPaths::GetPath(Dst)))
				{
					PF.CreateDirectoryTree(*FPaths::GetPath(Dst));
				}
				const int64 SrcSize = IFileManager::Get().FileSize(*Src);
				if (SrcSize == INDEX_NONE || (IFileManager::Get().FileSize(*Dst) != SrcSize &&
					IFileManager::Get().Copy(*Dst, *Src) != ECopyResult::COPY_OK))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FBuildDiffer: failed copying chunk '%s'"), *Src);
					return false;
				}
			}
		}

		const FString DeltaPath = FPaths::Combine(OutDir, TEXT("UnrealGamePatcher.delta.json"));
		if (!FDeltaManifestFile::Save(DeltaPath, Delta))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FBuildDiffer: failed writing delta '%s'"), *DeltaPath);
			return false;
		}

		UE_LOG(LogUnrealGamePatcher, Display,
			TEXT("FBuildDiffer: %s -> %s: files %d same / %d changed / %d added / %d removed; chunks %d same / %d changed / %d added / %d removed; download %lld bytes"),
			*Base.ContentVersion, *Target.ContentVersion,
			S.NumFilesSame, S.NumFilesChanged, S.NumFilesAdded, S.NumFilesRemoved,
			S.NumChunksSame, S.NumChunksChanged, S.NumChunksAdded, S.NumChunksRemoved,
			S.DownloadBytes);
		return true;
	}

	bool FBuildDiffer::DiffIndexFiles(
		const FString& BaseIndexPath,
		const FString& TargetIndexPath,
		const FString& TargetCdnRoot,
		const FString& OutDir,
		bool bCopyChunks,
		FDiffOutput& Out)
	{
		FIndexManifest Base, Target;
		if (!FIndexManifestFile::Load(BaseIndexPath, Base))
		{
			return false;
		}
		if (!FIndexManifestFile::Load(TargetIndexPath, Target))
		{
			return false;
		}
		return Diff(Base, Target, TargetCdnRoot, OutDir, bCopyChunks, Out);
	}
}
