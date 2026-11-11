// Copyright Epic Games, Inc. All Rights Reserved.

#include "Formats/Index/FIndexManifest.h"
#include "Formats/JsonIo.h"
#include "UnrealGamePatcherLog.h"
#include "UnrealGamePatcherPaths.h"
#include "Hash/FPatcherHash.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"

namespace UnrealGamePatcher
{
	namespace
	{
		bool IsContentPath(const FString& Path)
		{
			// Manifest paths are portable, relative file names. On Windows,
			// trailing dots and spaces can alias "." and "..".
			if (Path.IsEmpty() || Path.StartsWith(TEXT("/")) || Path.EndsWith(TEXT("/")) ||
				Path.Contains(TEXT("\\")) || Path.Contains(TEXT(":")))
			{
				return false;
			}
			for (TCHAR Character : Path)
			{
				if (Character < 32)
				{
					return false;
				}
			}
			TArray<FString> Components;
			Path.ParseIntoArray(Components, TEXT("/"), false);
			for (const FString& Part : Components)
			{
				if (Part.IsEmpty() || Part.EndsWith(TEXT(".")) || Part.EndsWith(TEXT(" ")))
				{
					return false;
				}
			}
			return !Components[0].Equals(TEXT(".ugp"), ESearchCase::IgnoreCase) &&
				!Components[0].Equals(TEXT("UnrealGamePatcher.state.json"), ESearchCase::IgnoreCase) &&
				!Components[0].StartsWith(TEXT("UnrealGamePatcher.state.json."), ESearchCase::IgnoreCase);
		}

		/** Builds the canonical JSON body for a manifest. The key order is
		 *  fixed here, so the XXH3-64 self-check hash is deterministic and
		 *  both Save and Load use this exact same function to (re)compute it. */
		bool SerializeBody(const FIndexManifest& M, bool bIncludeSelfCheck, const FString& SelfCheckHex, FString& OutJson)
		{
			TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("format"), FIndexManifest::FormatName);
			Root->SetNumberField(TEXT("version"), FIndexManifest::FormatVersion);
			Root->SetStringField(TEXT("appId"), M.AppId);
			Root->SetStringField(TEXT("contentVersion"), M.ContentVersion);
			Root->SetStringField(TEXT("baseVersion"), M.BaseVersion);
				Root->SetBoolField(TEXT("verifyOnUpdate"), M.bVerifyOnUpdate);

			// Files: sorted keys so the body is canonical.
			TSharedPtr<FJsonObject> FilesObj = MakeShared<FJsonObject>();
			{
				TArray<FString> Keys;
				M.Files.GetKeys(Keys);
				Keys.Sort();
				for (const FString& Key : Keys)
				{
					const FIndexFileEntry& E = M.Files[Key];
					TSharedPtr<FJsonObject> EObj = MakeShared<FJsonObject>();
					EObj->SetStringField(TEXT("sha1"), E.SHA1);
					EObj->SetNumberField(TEXT("size"), static_cast<double>(E.Size));
					{
						TArray<TSharedPtr<FJsonValue>> PartsArr;
						for (const FIndexFileChunkRef& Part : E.Parts)
						{
							TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
							PObj->SetStringField(TEXT("chunk"), Part.Chunk);
							PObj->SetNumberField(TEXT("offset"), static_cast<double>(Part.Offset));
							PObj->SetNumberField(TEXT("size"), static_cast<double>(Part.Size));
							PartsArr.Add(MakeShared<FJsonValueObject>(PObj));
						}
						EObj->SetArrayField(TEXT("parts"), PartsArr);
					}
					if (E.PakEntries.Num() > 0)
					{
						TArray<TSharedPtr<FJsonValue>> EntriesArr;
						for (const FIndexPakEntry& PE : E.PakEntries)
						{
							TSharedPtr<FJsonObject> PEObj = MakeShared<FJsonObject>();
							PEObj->SetStringField(TEXT("path"), PE.Path);
							PEObj->SetNumberField(TEXT("offset"), static_cast<double>(PE.OffsetInFile));
							PEObj->SetNumberField(TEXT("storedSize"), static_cast<double>(PE.StoredSize));
							PEObj->SetNumberField(TEXT("uncompressedSize"), static_cast<double>(PE.UncompressedSize));
							PEObj->SetStringField(TEXT("sha1"), PE.SHA1);
							EntriesArr.Add(MakeShared<FJsonValueObject>(PEObj));
						}
						EObj->SetArrayField(TEXT("entries"), EntriesArr);
					}
					if (E.IoStoreChunks.Num() > 0)
					{
						TArray<TSharedPtr<FJsonValue>> IoArr;
						for (const FIndexIoStoreChunk& IC : E.IoStoreChunks)
						{
							TSharedPtr<FJsonObject> IoObj = MakeShared<FJsonObject>();
							IoObj->SetStringField(TEXT("id"), IC.Id);
							IoObj->SetNumberField(TEXT("offset"), static_cast<double>(IC.RawOffset));
							IoObj->SetNumberField(TEXT("size"), static_cast<double>(IC.RawSize));
						// The content signal (FIoHash of the raw data) - see
						// FIndexIoStoreChunk::MetaHash. Always written so the
						// canonical body stays deterministic.
						IoObj->SetStringField(TEXT("metaHash"), IC.MetaHash);
						// Physical block span inside the .ucas (download-phase window
						// targeting). Always written (deterministic).
						IoObj->SetNumberField(TEXT("fileOffset"), static_cast<double>(IC.FileOffset));
						IoObj->SetNumberField(TEXT("fileSize"), static_cast<double>(IC.FileSize));
							IoArr.Add(MakeShared<FJsonValueObject>(IoObj));
						}
						EObj->SetArrayField(TEXT("ioChunks"), IoArr);
					}
					FilesObj->SetObjectField(Key, EObj);
				}
			}
			Root->SetObjectField(TEXT("files"), FilesObj);

			// Chunks: sorted keys.
			TSharedPtr<FJsonObject> ChunksObj = MakeShared<FJsonObject>();
			{
				TArray<FString> Keys;
				M.Chunks.GetKeys(Keys);
				Keys.Sort();
				for (const FString& Key : Keys)
				{
					const FIndexChunkEntry& E = M.Chunks[Key];
					TSharedPtr<FJsonObject> EObj = MakeShared<FJsonObject>();
					EObj->SetStringField(TEXT("sha1"), E.SHA1);
					EObj->SetNumberField(TEXT("size"), static_cast<double>(E.Size));
					EObj->SetStringField(TEXT("file"), E.File);
					ChunksObj->SetObjectField(Key, EObj);
				}
			}
			Root->SetObjectField(TEXT("chunks"), ChunksObj);

			if (bIncludeSelfCheck)
			{
				Root->SetStringField(TEXT("selfCheck"), SelfCheckHex);
			}

			return SerializeJsonCompact(Root.ToSharedRef(), OutJson);
		}
	}

	bool FIndexManifestFile::Save(const FString& Path, const FIndexManifest& Manifest)
	{
		// Canonical body without selfCheck -> XXH3-64 -> embed and re-serialize.
		FString Body;
		if (!SerializeBody(Manifest, false, FString(), Body))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: failed to serialize body"));
			return false;
		}
		const FString SelfCheckHex = JsonSelfCheckHex(Body);

		FString Full;
		if (!SerializeBody(Manifest, true, SelfCheckHex, Full))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: failed to serialize manifest"));
			return false;
		}

		return WriteJsonFileAtomic(Path, Full, TEXT("FIndexManifest"));
	}

	bool FIndexManifestFile::Load(const FString& Path, FIndexManifest& OutManifest)
	{
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *Path))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: cannot read index file '%s'"), *Path);
			return false;
		}
		if (!LoadFromString(Json, OutManifest))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: failed to load '%s'"), *Path);
			return false;
		}
		return true;
	}

	bool FIndexManifestFile::LoadFromString(const FString& Json, FIndexManifest& OutManifest)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: '%s' is not valid JSON (corrupt or truncated)"), TEXT("<index>"));
			return false;
		}

		// Schema: format/version must match exactly; a file claiming a
		// different format is not ours to interpret.
		FString Format;
		if (!ReadStringField(Root, TEXT("format"), Format) || !Format.Equals(FIndexManifest::FormatName))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: '%s' has missing/invalid 'format' (got '%s')"), TEXT("<index>"), *Format);
			return false;
		}
		int64 Version = 0;
		// v1 (BPT-fed, guid/hash/group) and v2 (self-designed, SHA1 keys) both load.
		if (!ReadInt64Field(Root, TEXT("version"), Version) ||
			(Version != 1 && Version != FIndexManifest::FormatVersion))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: '%s' has unsupported 'version' %lld (expect %d)"),
				TEXT("<index>"), Version, FIndexManifest::FormatVersion);
			return false;
		}

		// Required fields.
		if (!ReadStringField(Root, TEXT("appId"), OutManifest.AppId) || OutManifest.AppId.IsEmpty())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: '%s' missing/invalid 'appId'"), TEXT("<index>"));
			return false;
		}
		if (!ReadStringField(Root, TEXT("contentVersion"), OutManifest.ContentVersion) || OutManifest.ContentVersion.IsEmpty())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: '%s' missing/invalid 'contentVersion'"), TEXT("<index>"));
			return false;
		}
		ReadStringField(Root, TEXT("baseVersion"), OutManifest.BaseVersion); // optional, defaults to empty
		// NOTE: updateStrategy was removed from the format (write-only legacy).
		// Old manifests carrying it load fine - unknown fields are skipped.
		if (Root->HasField(TEXT("verifyOnUpdate")))
		{
			OutManifest.bVerifyOnUpdate = Root->GetBoolField(TEXT("verifyOnUpdate"));
		} // optional, defaults to false

		// Files: every value must be an object with sha1 (40-hex) and size.
		OutManifest.Files.Empty();
		{
			TSharedPtr<FJsonObject> FilesObj;
			if (!ReadObjectField(Root, TEXT("files"), FilesObj))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: '%s' missing/invalid 'files'"), TEXT("<index>"));
				return false;
			}
			for (const auto& Pair : FilesObj->Values)
			{
				if (!IsContentPath(Pair.Key))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: invalid file path '%s'"), *Pair.Key);
					return false;
				}
				if (Pair.Value->Type != EJson::Object)
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: file '%s' is not an object in '%s'"), *Pair.Key, TEXT("<index>"));
					return false;
				}
				const TSharedPtr<FJsonObject> EObj = Pair.Value->AsObject();
				FIndexFileEntry E;
				if (!ReadStringField(EObj, TEXT("sha1"), E.SHA1) || !IsHex40(E.SHA1))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: file '%s' has invalid 'sha1' in '%s'"), *Pair.Key, TEXT("<index>"));
					return false;
				}
				if (!ReadInt64Field(EObj, TEXT("size"), E.Size) || E.Size < 0)
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: file '%s' has invalid 'size' in '%s'"), *Pair.Key, TEXT("<index>"));
					return false;
				}
				// Parts is optional (whole-file-download entries carry none).
				const TArray<TSharedPtr<FJsonValue>>* PartsArr = nullptr;
				if (EObj->TryGetArrayField(TEXT("parts"), PartsArr))
				{
					for (const TSharedPtr<FJsonValue>& PartValue : *PartsArr)
					{
						const TSharedPtr<FJsonObject> PObj = PartValue->AsObject();
						FIndexFileChunkRef Part;
						// Validate the object BEFORE any field read - a non-object
						// element ("parts":[42]) yields a null PObj and reading a
						// field off it would crash instead of failing cleanly.
						if (!PObj.IsValid())
						{
							UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: file '%s' has a non-object 'parts' element in '%s'"), *Pair.Key, TEXT("<index>"));
							return false;
						}
						// v2 writes "chunk" (the SHA1 content address); v1 used
						// "guid" - accept both so old manifests still load.
						const bool bHasV2Key = ReadStringField(PObj, TEXT("chunk"), Part.Chunk);
						const bool bHasV1Key = !bHasV2Key && ReadStringField(PObj, TEXT("guid"), Part.Chunk);
						if ((!bHasV2Key && !bHasV1Key) ||
							!ReadInt64Field(PObj, TEXT("offset"), Part.Offset) ||
							!ReadInt64Field(PObj, TEXT("size"), Part.Size) ||
							Part.Chunk.IsEmpty() || Part.Size < 0 || Part.Offset < 0)
						{
							UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: file '%s' has malformed 'parts' in '%s'"), *Pair.Key, TEXT("<index>"));
							return false;
						}
						E.Parts.Add(Part);
					}
				}
				// Pak entry metadata is optional (present for pak files).
				const TArray<TSharedPtr<FJsonValue>>* EntriesArr = nullptr;
				if (EObj->TryGetArrayField(TEXT("entries"), EntriesArr))
				{
					for (const TSharedPtr<FJsonValue>& EntryValue : *EntriesArr)
					{
						const TSharedPtr<FJsonObject> PEObj = EntryValue->AsObject();
						FIndexPakEntry PE;
						if (!PEObj.IsValid() ||
							!ReadStringField(PEObj, TEXT("path"), PE.Path) ||
							!ReadInt64Field(PEObj, TEXT("offset"), PE.OffsetInFile) ||
							!ReadInt64Field(PEObj, TEXT("storedSize"), PE.StoredSize) ||
							!ReadInt64Field(PEObj, TEXT("uncompressedSize"), PE.UncompressedSize) ||
							!ReadStringField(PEObj, TEXT("sha1"), PE.SHA1) ||
							PE.Path.IsEmpty() || PE.StoredSize < 0 || PE.OffsetInFile < 0)
						{
							UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: file '%s' has malformed 'entries' in '%s'"), *Pair.Key, TEXT("<index>"));
							return false;
						}
						E.PakEntries.Add(PE);
					}
				}
				// IoStore chunk metadata is optional (present for ucas files).
				double NumberField = 0.0;
				const TArray<TSharedPtr<FJsonValue>>* IoArr = nullptr;
				if (EObj->TryGetArrayField(TEXT("ioChunks"), IoArr))
				{
					for (const TSharedPtr<FJsonValue>& IoValue : *IoArr)
					{
						const TSharedPtr<FJsonObject> IoObj = IoValue->AsObject();
						FIndexIoStoreChunk IC;
						if (!IoObj.IsValid() ||
							!ReadStringField(IoObj, TEXT("id"), IC.Id) ||
							!ReadInt64Field(IoObj, TEXT("offset"), IC.RawOffset) ||
							!ReadInt64Field(IoObj, TEXT("size"), IC.RawSize) ||
							IC.Id.IsEmpty() || IC.RawSize < 0 || IC.RawOffset < 0)
						{
							UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: file '%s' has malformed 'ioChunks' in '%s'"), *Pair.Key, TEXT("<index>"));
							return false;
						}
							// Optional (older manifests predate it): when present it must
							// be a well-formed 40-hex FIoHash (empty = legacy, no meta).
							if (IoObj->TryGetStringField(TEXT("metaHash"), IC.MetaHash) &&
								!IC.MetaHash.IsEmpty() && !IsHex40(IC.MetaHash))
							{
								UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: file '%s' has malformed 'ioChunks.metaHash' in '%s'"), *Pair.Key, TEXT("<index>"));
								return false;
							}
						// Optional physical span (older manifests predate it).
						if (IoObj->TryGetNumberField(TEXT("fileOffset"), NumberField))
						{
							IC.FileOffset = static_cast<int64>(NumberField);
						}
						if (IoObj->TryGetNumberField(TEXT("fileSize"), NumberField))
						{
							IC.FileSize = static_cast<int64>(NumberField);
						}
						if ((IC.FileOffset < 0) != (IC.FileSize <= 0))
						{
							UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: file '%s' has inconsistent 'ioChunks.fileOffset/fileSize' in '%s'"), *Pair.Key, TEXT("<index>"));
							return false;
						}
						E.IoStoreChunks.Add(IC);
					}
				}
				OutManifest.Files.Add(FString(Pair.Key), E);
			}
		}

		// Chunks: every value must be an object with hash/sha1/size/group.
		OutManifest.Chunks.Empty();
		{
			TSharedPtr<FJsonObject> ChunksObj;
			if (!ReadObjectField(Root, TEXT("chunks"), ChunksObj))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: '%s' missing/invalid 'chunks'"), TEXT("<index>"));
				return false;
			}
			for (const auto& Pair : ChunksObj->Values)
			{
				if (Pair.Value->Type != EJson::Object)
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: chunk '%s' is not an object in '%s'"), *Pair.Key, TEXT("<index>"));
					return false;
				}
				const TSharedPtr<FJsonObject> EObj = Pair.Value->AsObject();
				FIndexChunkEntry E;
				// v1 carried rolling "hash" and "group" - ignored in v2.
				if (!ReadStringField(EObj, TEXT("sha1"), E.SHA1) || !IsHex40(E.SHA1))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: chunk '%s' has invalid 'sha1' in '%s'"), *Pair.Key, TEXT("<index>"));
					return false;
				}
				if (!ReadInt64Field(EObj, TEXT("size"), E.Size) || E.Size < 0)
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: chunk '%s' has invalid 'size' in '%s'"), *Pair.Key, TEXT("<index>"));
					return false;
				}
				ReadStringField(EObj, TEXT("file"), E.File); // optional (legacy manifests)
				if (!E.File.IsEmpty() && (!IsContentPath(E.File) || E.File.Contains(TEXT("?")) ||
					E.File.Contains(TEXT("#")) || E.File.Contains(TEXT("%"))))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: invalid chunk path '%s'"), *E.File);
					return false;
				}
				OutManifest.Chunks.Add(FString(Pair.Key), E);
			}
		}

		// Self-check: strip the embedded hash and recompute over the same
		// canonical body used by Save; any tamper changes the hash.
		FString SelfCheckHex;
		if (!ReadStringField(Root, TEXT("selfCheck"), SelfCheckHex) || SelfCheckHex.Len() != 16)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: '%s' missing/invalid 'selfCheck'"), TEXT("<index>"));
			return false;
		}
		FString Body;
		if (!SerializeBody(OutManifest, false, FString(), Body))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: failed to re-serialize body for self-check"));
			return false;
		}
		const FString Recomputed = JsonSelfCheckHex(Body);
		if (!Recomputed.Equals(SelfCheckHex, ESearchCase::IgnoreCase) &&
			!MatchesRawBodySelfCheck(Json, SelfCheckHex))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FIndexManifest: '%s' FAILED self-check (tampered or corrupt; embedded %s, recomputed %s)"),
				TEXT("<index>"), *SelfCheckHex, *Recomputed);
			return false;
		}

		UE_LOG(LogUnrealGamePatcher, Display, TEXT("FIndexManifest: loaded '%s' (app %s, content %s, %d files, %d chunks)"),
			TEXT("<index>"), *OutManifest.AppId, *OutManifest.ContentVersion,
			OutManifest.Files.Num(), OutManifest.Chunks.Num());
		return true;
	}

	// --- Latest-version announcement (latest.json) ---------------------------

	namespace
	{
		bool SerializeLatestBody(const FLatestVersion& Latest, FString& OutJson)
		{
			TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("format"), FLatestVersionFile::FormatName);
			Root->SetNumberField(TEXT("version"), FLatestVersionFile::FormatVersion);
			Root->SetStringField(TEXT("currentVersion"), Latest.Version);
			if (!Latest.MinVersion.IsEmpty()) { Root->SetStringField(TEXT("minVersion"), Latest.MinVersion); }
			if (Latest.bForce) { Root->SetBoolField(TEXT("force"), true); }
		if (!Latest.Channel.IsEmpty()) { Root->SetStringField(TEXT("channel"), Latest.Channel); }
		return SerializeJsonCompact(Root.ToSharedRef(), OutJson);
	}
	}

	FString FLatestVersionFile::GetPath(const FString& CdnDir)
	{
		return FPaths::Combine(*CdnDir, TEXT("index"), TEXT("latest.json"));
	}

	bool FLatestVersionFile::Save(const FString& Path, const FLatestVersion& Latest)
	{
		FString Full;
		if (!SerializeLatestBody(Latest, Full))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FLatestVersion: failed to serialize '%s'"), *Path);
			return false;
		}
		return WriteJsonFileAtomic(Path, Full, TEXT("FLatestVersion"));
	}

	bool FLatestVersionFile::Load(const FString& Path, FLatestVersion& OutLatest)
	{
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *Path))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FLatestVersion: cannot read '%s'"), *Path);
			return false;
		}
		if (!LoadFromString(Json, OutLatest))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FLatestVersion: failed to load '%s'"), *Path);
			return false;
		}
		return true;
	}

	bool FLatestVersionFile::LoadFromString(const FString& Json, FLatestVersion& OutLatest)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FLatestVersion: '%s' is not valid JSON"), TEXT("<latest>"));
			return false;
		}
		FString Format;
		if (!ReadStringField(Root, TEXT("format"), Format) || !Format.Equals(FLatestVersionFile::FormatName))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FLatestVersion: '%s' missing/invalid 'format'"), TEXT("<latest>"));
			return false;
		}
		int64 FmtVersion = 0;
		if (!ReadInt64Field(Root, TEXT("version"), FmtVersion) || FmtVersion != FLatestVersionFile::FormatVersion)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FLatestVersion: '%s' unsupported 'version'"), TEXT("<latest>"));
			return false;
		}
		if (!ReadStringField(Root, TEXT("currentVersion"), OutLatest.Version) || OutLatest.Version.IsEmpty())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FLatestVersion: '%s' missing/invalid 'currentVersion'"), TEXT("<latest>"));
			return false;
		}
		ReadStringField(Root, TEXT("minVersion"), OutLatest.MinVersion);
		ReadStringField(Root, TEXT("channel"), OutLatest.Channel);
		const TSharedPtr<FJsonValue>* Force = Root->Values.Find(TEXT("force"));
		OutLatest.bForce = Force && (*Force).IsValid() && (*Force)->AsBool();
		return true;
	}
}
