// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformFileManager.h"
#include "Hash/FPatcherHash.h"
#include "Misc/Char.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Stats/FIOStats.h"
#include "UnrealGamePatcherLog.h"
#include "UnrealGamePatcherPaths.h"

namespace UnrealGamePatcher
{
	// All hand-written JSON mechanics shared by the index / latest / state /
	// delta persistence, in one place. The per-format contracts stay in their
	// own loaders: the canonical SerializeBody key order (the selfCheck hash
	// is bound to its exact bytes) and the per-field required/format
	// validation.
	//
	// NOTE (2026-09-21 spike): these loaders deliberately do NOT use
	// FJsonObjectConverter + USTRUCT reflection. UHT and JsonUtilities work
	// fine for this Program (verified by spike build), but measured against
	// this codebase the converter route is net-negative:
	//   1. The selfCheck hash is bound to the hand-written canonical
	//      SerializeBody bytes - that writer must stay byte-stable anyway.
	//   2. Wire keys and member names diverge semantically ("offset" maps to
	//      Offset / RawOffset / OffsetInFile depending on the containing
	//      array), so reflection import needs a context-aware key-rewrite
	//      table plus a recursive walker.
	//   3. The converter silently defaults missing fields; the strict
	//      required-key/format checks (pinned by indextest/statetest, e.g.
	//      a renamed "storedSize" key must be rejected) would remain as a
	//      hand-written layer over the raw JSON regardless.
	// Net result measured at design time: ~280 new lines (tables + walker +
	// validation) replacing ~230 lines of tight extraction.

	// --- Strict field readers ------------------------------------------------
	// A wrong-typed or missing field returns false so a damaged file is
	// rejected instead of silently defaulted.

	inline bool ReadInt64Field(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, int64& Out)
	{
		const TSharedPtr<FJsonValue>* Value = Obj->Values.Find(Key);
		if (!Value || !(*Value).IsValid() || (*Value)->Type != EJson::Number)
		{
			return false;
		}
		Out = (int64)(*Value)->AsNumber();
		return true;
	}

	inline bool ReadStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, FString& Out)
	{
		const TSharedPtr<FJsonValue>* Value = Obj->Values.Find(Key);
		if (!Value || !(*Value).IsValid() || (*Value)->Type != EJson::String)
		{
			return false;
		}
		Out = (*Value)->AsString();
		return true;
	}

	inline bool ReadObjectField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, TSharedPtr<FJsonObject>& Out)
	{
		const TSharedPtr<FJsonValue>* Value = Obj->Values.Find(Key);
		if (!Value || !(*Value).IsValid() || (*Value)->Type != EJson::Object)
		{
			return false;
		}
		Out = (*Value)->AsObject();
		return true;
	}

	inline bool ReadArrayField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, TArray<TSharedPtr<FJsonValue>>& Out)
	{
		const TSharedPtr<FJsonValue>* Value = Obj->Values.Find(Key);
		if (!Value || !(*Value).IsValid() || (*Value)->Type != EJson::Array)
		{
			return false;
		}
		Out = (*Value)->AsArray();
		return true;
	}

	inline bool IsHex40(const FString& Str)
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

	// --- Compact serialization ----------------------------------------------

	/** Serializes a built object tree to compact single-line JSON (the only
	 *  shape these files use). */
	inline bool SerializeJsonCompact(const TSharedRef<FJsonObject>& Root, FString& OutJson)
	{
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		return FJsonSerializer::Serialize(Root, Writer);
	}

	// --- Atomic JSON-file write ---------------------------------------------

	/** Writes Json to Path as UTF-8 (no BOM) through temp + recoverable
	 *  replace, so a crash mid-write (or a failed rename under AV/lock)
	 *  leaves the previous good file untouched. LogTag prefixes the failure
	 *  logs ("FIndexManifest", "FPatcherState", ...). */
	inline bool WriteJsonFileAtomic(const FString& Path, const FString& Json, const TCHAR* LogTag)
	{
		const FString TempPath = Path + TEXT(".tmp");
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (PF.FileExists(*TempPath))
		{
			PF.DeleteFile(*TempPath);
		}
		if (!FFileHelper::SaveStringToFile(Json, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("%s: failed writing '%s'"), LogTag, *TempPath);
			return false;
		}
		// Approximate byte count (JSON is ASCII-dominant; the exact UTF-8 size
		// would require a throwaway conversion) - good enough for the
		// volume-accounting purpose.
		FIOStats::Get().AddDiskWrite(Json.Len());
		if (!Paths::AtomicReplaceFile(TempPath, Path))
		{
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("%s: failed moving '%s' to '%s'"), LogTag, *TempPath, *Path);
			return false;
		}
		return true;
	}

	// --- Canonical-body self-check (index + delta manifests) -----------------

	/** XXH3-64 over the UTF-16 code units of the canonical body (exactly the
	 *  bytes SerializeBody produces without the selfCheck field), as 16 hex.
	 *  Save embeds it; Load recomputes it over the re-serialized body and
	 *  compares case-insensitively - any tamper changes the hash. */
	inline FString JsonSelfCheckHex(const FString& CanonicalBody)
	{
		return FPatcherHash::XXH64ToHex(CanonicalBody.GetCharArray().GetData(), CanonicalBody.Len() * sizeof(TCHAR));
	}

	/** Legacy-layout fallback for the canonical-body self-check. Indexes/deltas
	 *  published before the condensed serializer (pretty-printed writer, pre-
	 *  metaHash ioChunks) embed the hash over their own raw bytes, so the
	 *  canonical re-serialization can never reproduce it. Verifies the hash
	 *  against the raw text with the embedded "selfCheck" key-value stripped.
	 *  Tamper strength is unchanged: any body byte change breaks the raw hash. */
	inline bool MatchesRawBodySelfCheck(const FString& Text, const FString& SelfCheckHex)
	{
		// Scan every "selfCheck" token from the end (root writes it last); a
		// token inside an escaped value can't match the strict shape below.
		// SearchFrom strictly decreases: Find's FromEnd StartPos=-1 would mean
		// INDEX_NONE (= search from the end again, looping forever).
		const TCHAR Needle[] = TEXT("\"selfCheck\"");
		constexpr int32 NeedleLen = sizeof(Needle) / sizeof(TCHAR) - 1;
		const FString Quoted = TEXT("\"") + SelfCheckHex + TEXT("\"");
		int32 SearchFrom = Text.Len();
		while (SearchFrom > 0)
		{
			const int32 KeyIdx = Text.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromEnd, SearchFrom - 1);
			if (KeyIdx == INDEX_NONE) { break; }
			SearchFrom = KeyIdx;
			int32 Cursor = KeyIdx + NeedleLen;
			const auto SkipWs = [&Text, &Cursor]()
			{
				while (Cursor < Text.Len() && TChar<TCHAR>::IsWhitespace(Text[Cursor])) { ++Cursor; }
			};
			SkipWs();
			if (Cursor >= Text.Len() || Text[Cursor] != TEXT(':')) { continue; }
			++Cursor;
			SkipWs();
			if (!Text.Mid(Cursor, Quoted.Len()).Equals(Quoted, ESearchCase::IgnoreCase)) { continue; }
			const int32 ValueEnd = Cursor + Quoted.Len();
			int32 CommaIdx = KeyIdx - 1;
			while (CommaIdx >= 0 && TChar<TCHAR>::IsWhitespace(Text[CommaIdx])) { --CommaIdx; }
			if (CommaIdx < 0 || Text[CommaIdx] != TEXT(',')) { continue; }
			const FString RawBody = Text.Mid(0, CommaIdx) + Text.Mid(ValueEnd);
			if (JsonSelfCheckHex(RawBody).Equals(SelfCheckHex, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}
}
