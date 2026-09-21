// Copyright Epic Games, Inc. All Rights Reserved.

#include "State/FPatcherState.h"
#include "Formats/JsonIo.h"
#include "UnrealGamePatcherLog.h"
#include "UnrealGamePatcherPaths.h"
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
		const TCHAR* StatusToString(ETransactionStatus Status)
		{
			switch (Status)
			{
			case ETransactionStatus::None: return TEXT("none");
			case ETransactionStatus::Pending: return TEXT("pending");
			case ETransactionStatus::Committed: return TEXT("committed");
			case ETransactionStatus::RolledBack: return TEXT("rolled-back");
			}
			return TEXT("none");
		}

		ETransactionStatus StringToStatus(const FString& Str)
		{
			if (Str == TEXT("pending")) return ETransactionStatus::Pending;
			if (Str == TEXT("committed")) return ETransactionStatus::Committed;
			if (Str == TEXT("rolled-back")) return ETransactionStatus::RolledBack;
			return ETransactionStatus::None;
		}

		const TCHAR* OpToString(EApplyOp Op)
		{
			switch (Op)
			{
			case EApplyOp::Overwrite: return TEXT("overwrite");
			case EApplyOp::Append: return TEXT("append");
			case EApplyOp::ReplaceFile: return TEXT("replace-file");
			case EApplyOp::Delete: return TEXT("delete");
			}
			return TEXT("overwrite");
		}

		EApplyOp StringToOp(const FString& Str)
		{
			if (Str == TEXT("append")) return EApplyOp::Append;
			if (Str == TEXT("replace-file")) return EApplyOp::ReplaceFile;
			if (Str == TEXT("delete")) return EApplyOp::Delete;
			return EApplyOp::Overwrite;
		}

		TSharedPtr<FJsonObject> SerializeTransaction(const FTransactionRecord& Txn)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("status"), StatusToString(Txn.Status));
			Obj->SetStringField(TEXT("contentVersion"), Txn.ContentVersion);

			TArray<TSharedPtr<FJsonValue>> Targets;
			for (const FTransactionTarget& T : Txn.Targets)
			{
				TSharedPtr<FJsonObject> TObj = MakeShared<FJsonObject>();
				TObj->SetStringField(TEXT("file"), T.File);
				TObj->SetStringField(TEXT("op"), OpToString(T.Op));
				TObj->SetNumberField(TEXT("offset"), static_cast<double>(T.Offset));
				TObj->SetNumberField(TEXT("size"), static_cast<double>(T.Size));
				TObj->SetStringField(TEXT("originalHash"), T.OriginalHash);
				TObj->SetStringField(TEXT("newHash"), T.NewHash);
				TObj->SetStringField(TEXT("backupPath"), T.BackupPath);
				if (T.bPreExisted) { TObj->SetBoolField(TEXT("preExisted"), true); }
				Targets.Add(MakeShared<FJsonValueObject>(TObj));
			}
			Obj->SetArrayField(TEXT("targets"), Targets);
			return Obj;
		}

		bool DeserializeTransaction(const TSharedPtr<FJsonObject>& Obj, FTransactionRecord& OutTxn)
		{
			FString StatusStr;
			if (!ReadStringField(Obj, TEXT("status"), StatusStr))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: transaction missing/invalid 'status'"));
				return false;
			}
			OutTxn.Status = StringToStatus(StatusStr);
			ReadStringField(Obj, TEXT("contentVersion"), OutTxn.ContentVersion); // optional, defaults to empty

			TArray<TSharedPtr<FJsonValue>> Targets;
			if (!ReadArrayField(Obj, TEXT("targets"), Targets))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: transaction missing/invalid 'targets'"));
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : Targets)
			{
				if (Value->Type != EJson::Object)
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: transaction target is not an object"));
					return false;
				}
				const TSharedPtr<FJsonObject> TObj = Value->AsObject();
				FTransactionTarget T;
				if (!ReadStringField(TObj, TEXT("file"), T.File) || T.File.IsEmpty())
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: target missing/invalid 'file'"));
					return false;
				}
				FString OpStr;
				if (!ReadStringField(TObj, TEXT("op"), OpStr))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: target '%s' missing 'op'"), *T.File);
					return false;
				}
				T.Op = StringToOp(OpStr);
				if (!ReadInt64Field(TObj, TEXT("offset"), T.Offset))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: target '%s' missing/invalid 'offset'"), *T.File);
					return false;
				}
				if (!ReadInt64Field(TObj, TEXT("size"), T.Size))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: target '%s' missing/invalid 'size'"), *T.File);
					return false;
				}
				ReadStringField(TObj, TEXT("originalHash"), T.OriginalHash); // optional
				ReadStringField(TObj, TEXT("newHash"), T.NewHash);           // optional
				ReadStringField(TObj, TEXT("backupPath"), T.BackupPath);     // optional
				T.bPreExisted = false; // default: preserves the old heuristic for legacy sidecars
				if (TObj->HasField(TEXT("preExisted"))) { T.bPreExisted = TObj->GetBoolField(TEXT("preExisted")); }
				OutTxn.Targets.Add(T);
			}
			return true;
		}
	}

	bool FPatcherStateFile::Save(const FString& StatePath, const FPatcherState& State)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("format"), FPatcherState::FormatName);
		Root->SetNumberField(TEXT("version"), FPatcherState::FormatVersion);

		TArray<TSharedPtr<FJsonValue>> Holes;
		for (const FHole& Hole : State.Holes)
		{
			TSharedPtr<FJsonObject> HObj = MakeShared<FJsonObject>();
			HObj->SetNumberField(TEXT("offset"), static_cast<double>(Hole.Offset));
			HObj->SetNumberField(TEXT("size"), static_cast<double>(Hole.Size));
			if (!Hole.File.IsEmpty())
			{
				HObj->SetStringField(TEXT("file"), Hole.File);
			}
			Holes.Add(MakeShared<FJsonValueObject>(HObj));
		}
		Root->SetArrayField(TEXT("holes"), Holes);
		Root->SetObjectField(TEXT("transaction"), SerializeTransaction(State.Transaction));
		if (!State.LastVerifiedAt.IsEmpty())
		{
			Root->SetStringField(TEXT("lastVerifiedAt"), State.LastVerifiedAt);
		} // optional; omitted when never verified

		FString Json;
		if (!SerializeJsonCompact(Root.ToSharedRef(), Json))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: failed to serialize state to JSON"));
			return false;
		}

		// Write atomically via WriteJsonFileAtomic (temp + recoverable
		// replace): the sidecar IS the recovery record, losing it must not be
		// possible.
		return WriteJsonFileAtomic(StatePath, Json, TEXT("FPatcherState"));
	}

	bool FPatcherStateFile::Load(const FString& StatePath, FPatcherState& OutState)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (!PF.FileExists(*StatePath))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: state file not found: '%s'"), *StatePath);
			return false;
		}

		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *StatePath))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: cannot read state file '%s'"), *StatePath);
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: '%s' is not valid JSON (corrupt or truncated)"), *StatePath);
			return false;
		}

		// Schema validation: format/version must match exactly; a file that
		// claims a different format is not ours to interpret.
		FString Format;
		if (!ReadStringField(Root, TEXT("format"), Format) || !Format.Equals(FPatcherState::FormatName))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: '%s' has missing/invalid 'format' (got '%s')"), *StatePath, *Format);
			return false;
		}
		int64 Version = 0;
		if (!ReadInt64Field(Root, TEXT("version"), Version) || Version != FPatcherState::FormatVersion)
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: '%s' has unsupported 'version' %lld (expect %d)"),
				*StatePath, Version, FPatcherState::FormatVersion);
			return false;
		}

		// Holes.
		OutState.Holes.Empty();
		TArray<TSharedPtr<FJsonValue>> Holes;
		if (ReadArrayField(Root, TEXT("holes"), Holes))
		{
			for (const TSharedPtr<FJsonValue>& Value : Holes)
			{
				if (Value->Type != EJson::Object)
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: hole entry is not an object in '%s'"), *StatePath);
					return false;
				}
				const TSharedPtr<FJsonObject> HObj = Value->AsObject();
				FHole Hole;
				if (!ReadInt64Field(HObj, TEXT("offset"), Hole.Offset) ||
					!ReadInt64Field(HObj, TEXT("size"), Hole.Size))
				{
					UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: hole missing offset/size in '%s'"), *StatePath);
					return false;
				}
				ReadStringField(HObj, TEXT("file"), Hole.File); // optional (legacy holes are global)
				OutState.Holes.Add(Hole);
			}
		}

		// Transaction (optional at top level, but when present must be valid).
		OutState.Transaction = FTransactionRecord();
		const TSharedPtr<FJsonValue>* TxnValue = Root->Values.Find(TEXT("transaction"));
		if (TxnValue && (*TxnValue).IsValid())
		{
			if ((*TxnValue)->Type != EJson::Object)
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: 'transaction' is not an object in '%s'"), *StatePath);
				return false;
			}
			if (!DeserializeTransaction((*TxnValue)->AsObject(), OutState.Transaction))
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("FPatcherState: invalid transaction in '%s'"), *StatePath);
				return false;
			}
		}

		// Optional verify timestamp (absent = never verified).
		ReadStringField(Root, TEXT("lastVerifiedAt"), OutState.LastVerifiedAt);

		UE_LOG(LogUnrealGamePatcher, Display, TEXT("FPatcherState: loaded '%s' (%d holes, txn status %s)"),
			*StatePath, OutState.Holes.Num(), StatusToString(OutState.Transaction.Status));
		return true;
	}
}
