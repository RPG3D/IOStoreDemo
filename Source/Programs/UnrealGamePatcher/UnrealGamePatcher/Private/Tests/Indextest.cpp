// Copyright Epic Games, Inc. All Rights Reserved.
// -cli indextest —— 扩展清单格式（清单格式层）
// 验证点：完整/空/富容器（parts+pak entries+ioChunks）roundtrip；
//         序列化字节确定性（两次 Save 逐字节一致）；
//         篡改拒绝（sha1/contentVersion/chunk size/剥离 selfCheck）；
//         损坏与格式/版本拒绝；§4e 旧序列化布局（pretty）经 raw-body 回退
//         可加载、篡改后双路径均拒（C11 修复钉）。

#include "Tests/TestFramework.h"

namespace UnrealGamePatcher::Tests
{
	int32 RunIndexTest(const TCHAR* /*CmdLine*/)
	{

		// Extension manifest acceptance (task 3.2): roundtrip consistency,
		// deterministic output, and tamper/corruption detection via the
		// embedded XXH3-64 self-check. Works on a temp dir, leaves nothing.
		bool bAllPassed = true;
		const FString WorkDir = GetTestWorkDir(TEXT("indextest"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		const FString IndexPath = FPaths::Combine(WorkDir, TEXT("UnrealGamePatcher.index.json"));

		auto Check = MakeCheck(bAllPassed);

		// Sample data mirrors a real ingest output (the staged fixture's
		// pakchunk1-Windows.pak, 2 entries / 939 bytes).
		UnrealGamePatcher::FIndexManifest Sample;
		Sample.AppId = TEXT("IOStoreDemo");
		Sample.ContentVersion = TEXT("1.0.0");
		Sample.BaseVersion = TEXT("0.9.0");
		Sample.Files.Add(TEXT("pakchunk1-Windows.pak"),
			UnrealGamePatcher::FIndexFileEntry{ TEXT("5a9f1eb0c1fc6268b90ee71f32ce5f7ed9c2c986"), 939 });
		Sample.Files.Add(TEXT("pakchunk1-Windows.ucas"),
			UnrealGamePatcher::FIndexFileEntry{ TEXT("293049e4b3aa98b1814d9f3f18ac3e7aff606436"), 10348176 });
		Sample.Files.Add(TEXT("pakchunk1-Windows.utoc"),
			UnrealGamePatcher::FIndexFileEntry{ TEXT("4c9af782ffcbfe5e524fc6bef14194e6705008d2"), 19405 });
		Sample.Chunks.Add(TEXT("C5C97C8A3F1F1B2F5BB35C0C46A380340408FB2A"),
			UnrealGamePatcher::FIndexChunkEntry{ TEXT("C5C97C8A3F1F1B2F5BB35C0C46A380340408FB2A"), 1048638, TEXT("chunks/C5C97C8A3F1F1B2F5BB35C0C46A380340408FB2A.chunk") });
		Sample.Chunks.Add(TEXT("0206B8FC805A30FECA3BFC686F06E87835134D75"),
			UnrealGamePatcher::FIndexChunkEntry{ TEXT("0206B8FC805A30FECA3BFC686F06E87835134D75"), 895382, TEXT("chunks/0206B8FC805A30FECA3BFC686F06E87835134D75.chunk") });

		// --- 1. Full roundtrip (files + chunks + baseVersion) ----
		{
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("save full manifest"));

			UnrealGamePatcher::FIndexManifest Loaded;
			Check(UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Loaded), TEXT("load full manifest"));
			Check(Loaded.AppId == TEXT("IOStoreDemo") &&
				Loaded.ContentVersion == TEXT("1.0.0") &&
				Loaded.BaseVersion == TEXT("0.9.0"),
				TEXT("header fields roundtrip"));
			Check(Loaded.Files.Num() == 3 &&
				Loaded.Files[TEXT("pakchunk1-Windows.pak")].SHA1 == TEXT("5a9f1eb0c1fc6268b90ee71f32ce5f7ed9c2c986") &&
				Loaded.Files[TEXT("pakchunk1-Windows.pak")].Size == 939 &&
				Loaded.Files[TEXT("pakchunk1-Windows.ucas")].Size == 10348176,
				TEXT("file entries roundtrip"));
			Check(Loaded.Chunks.Num() == 2 &&
				Loaded.Chunks[TEXT("0206B8FC805A30FECA3BFC686F06E87835134D75")].SHA1 == TEXT("0206B8FC805A30FECA3BFC686F06E87835134D75") &&
				Loaded.Chunks[TEXT("0206B8FC805A30FECA3BFC686F06E87835134D75")].Size == 895382 &&
				Loaded.Chunks[TEXT("0206B8FC805A30FECA3BFC686F06E87835134D75")].File == TEXT("chunks/0206B8FC805A30FECA3BFC686F06E87835134D75.chunk"),
				TEXT("chunk entries roundtrip"));
		}

		// --- 2. Empty manifest roundtrip -----------------------------------
		{
			UnrealGamePatcher::FIndexManifest Empty;
			Empty.AppId = TEXT("IOStoreDemo");
			Empty.ContentVersion = TEXT("0.0.1");
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Empty), TEXT("save empty manifest"));
			UnrealGamePatcher::FIndexManifest Loaded;
			Check(UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Loaded), TEXT("load empty manifest"));
			Check(Loaded.Files.Num() == 0 && Loaded.Chunks.Num() == 0,
				TEXT("empty manifest roundtrip"));
		}

		// --- 2b. Container metadata + parts + verifyOnUpdate roundtrip -----
		{
			// The v2 optional per-file fields (parts / pak entries / iostore
			// chunk tables) and the per-version verifyOnUpdate flag are
			// load-bearing (chunk reuse + in-place patching + forced verify)
			// - roundtrip them explicitly.
			UnrealGamePatcher::FIndexManifest Rich;
			Rich.AppId = TEXT("IOStoreDemo");
			Rich.ContentVersion = TEXT("0.0.2");
			Rich.bVerifyOnUpdate = true;

			UnrealGamePatcher::FIndexFileEntry Pak;
			Pak.Size = 1000;
			Pak.SHA1 = TEXT("1111111111111111111111111111111111111111");
			UnrealGamePatcher::FIndexPakEntry PE;
			PE.Path = TEXT("Game/Content/file.uasset");
			PE.OffsetInFile = 176;
			PE.StoredSize = 800;
			PE.UncompressedSize = 800;
			PE.SHA1 = TEXT("2222222222222222222222222222222222222222");
			Pak.PakEntries.Add(PE);

			UnrealGamePatcher::FIndexFileEntry Ucas;
			Ucas.Size = 2000;
			Ucas.SHA1 = TEXT("3333333333333333333333333333333333333333");
			UnrealGamePatcher::FIndexIoStoreChunk IC;
			IC.Id = TEXT("AAAABBBBCCCCDDDDEEEEFFFF");
			IC.RawOffset = 176;
			IC.RawSize = 900;
			Ucas.IoStoreChunks.Add(IC);
			UnrealGamePatcher::FIndexFileChunkRef Part;
			Part.Chunk = TEXT("0206B8FC805A30FECA3BFC686F06E87835134D75");
			Part.Offset = 0;
			Part.Size = 500;
			Ucas.Parts.Add(Part);

			Rich.Files.Add(TEXT("game.pak"), Pak);
			Rich.Files.Add(TEXT("game.ucas"), Ucas);

			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Rich), TEXT("save rich manifest"));
			UnrealGamePatcher::FIndexManifest Loaded;
			Check(UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Loaded), TEXT("load rich manifest"));
			Check(Loaded.bVerifyOnUpdate, TEXT("verifyOnUpdate roundtrip"));
			Check(Loaded.Files.Contains(TEXT("game.pak")) && Loaded.Files[TEXT("game.pak")].PakEntries.Num() == 1 &&
				Loaded.Files[TEXT("game.pak")].PakEntries[0].SHA1 == PE.SHA1 &&
				Loaded.Files[TEXT("game.pak")].PakEntries[0].Path == PE.Path,
				TEXT("pak entries roundtrip"));
			Check(Loaded.Files.Contains(TEXT("game.ucas")) && Loaded.Files[TEXT("game.ucas")].IoStoreChunks.Num() == 1 &&
				Loaded.Files[TEXT("game.ucas")].IoStoreChunks[0].Id == IC.Id &&
				Loaded.Files[TEXT("game.ucas")].Parts.Num() == 1 &&
				Loaded.Files[TEXT("game.ucas")].Parts[0].Chunk == Part.Chunk,
				TEXT("ioChunks + parts roundtrip"));

			// Malformed container metadata must be rejected (strict schema).
			FString RichJson;
			Check(FFileHelper::LoadFileToString(RichJson, *IndexPath), TEXT("read rich manifest json"));
			FString BadJson = RichJson.Replace(TEXT("\"storedSize\":"), TEXT("\"storedSizeX\":"));
			Check(FFileHelper::SaveStringToFile(BadJson, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM),
				TEXT("write malformed entries"));
			UnrealGamePatcher::FIndexManifest Bad;
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Bad), TEXT("reject malformed pak entries"));
		}

		// --- 3. Deterministic output (two saves are byte-identical) --------
		{
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("save full manifest (2nd time)"));
			FString Bytes1, Bytes2;
			Check(FFileHelper::LoadFileToString(Bytes1, *IndexPath), TEXT("read 2nd save"));
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("save full manifest (3rd time)"));
			Check(FFileHelper::LoadFileToString(Bytes2, *IndexPath), TEXT("read 3rd save"));
			Check(Bytes1 == Bytes2, TEXT("deterministic serialization (same bytes across saves)"));
		}

		// --- 4. Tamper detection (self-check catches every modification) ---
		{
			// 4a. Flip one char of a file sha1 (locate by the unique value, so
			//     the test is robust to JSON field ordering).
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("save clean manifest"));
			FString Json;
			Check(FFileHelper::LoadFileToString(Json, *IndexPath), TEXT("read clean manifest"));
			const FString Sha1Val = TEXT("5a9f1eb0c1fc6268b90ee71f32ce5f7ed9c2c986");
			const int32 Sha1Pos = Json.Find(Sha1Val);
			Check(Sha1Pos != INDEX_NONE, TEXT("locate file sha1 value"));
			Json[Sha1Pos + 39] = (Json[Sha1Pos + 39] == TEXT('6')) ? TEXT('7') : TEXT('6');
			Check(FFileHelper::SaveStringToFile(Json, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write tampered sha1"));
			UnrealGamePatcher::FIndexManifest Dummy;
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject tampered file sha1"));

			// 4b. Change contentVersion value (unique substring "1.0.0").
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("re-save clean manifest"));
			Check(FFileHelper::LoadFileToString(Json, *IndexPath), TEXT("re-read clean manifest"));
			Json = Json.Replace(TEXT("1.0.0"), TEXT("1.0.1"));
			Check(FFileHelper::SaveStringToFile(Json, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write tampered contentVersion"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject tampered contentVersion"));

			// 4c. Change a chunk size number (unique value).
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("re-save clean manifest"));
			Check(FFileHelper::LoadFileToString(Json, *IndexPath), TEXT("re-read clean manifest"));
			const int32 SzPos = Json.Find(TEXT("895382"));
			Check(SzPos != INDEX_NONE, TEXT("locate chunk size value"));
			Json = Json.Left(SzPos) + TEXT("895383") + Json.RightChop(SzPos + 6);
			Check(FFileHelper::SaveStringToFile(Json, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write tampered chunk size"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject tampered chunk size"));

			// 4d. Strip the selfCheck field entirely (locate by key, then drop
			//     the field plus its neighbouring comma).
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("re-save clean manifest"));
			Check(FFileHelper::LoadFileToString(Json, *IndexPath), TEXT("re-read clean manifest"));
			const int32 ScPos = Json.Find(TEXT("\"selfCheck\""));
			Check(ScPos != INDEX_NONE, TEXT("locate selfCheck key"));
			const int32 ValStart = Json.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, ScPos + 11);  // opening quote of the value
			Check(ValStart != INDEX_NONE, TEXT("locate selfCheck value start"));
			const int32 ValEnd = Json.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, ValStart + 1);  // closing quote of the value
			Check(ValEnd != INDEX_NONE, TEXT("locate selfCheck value end"));
			int32 FieldStart = ScPos;
			if (FieldStart > 0 && Json[FieldStart - 1] == TEXT(',')) { FieldStart -= 1; }
			int32 FieldEnd = ValEnd + 1;
			if (FieldEnd < Json.Len() && Json[FieldEnd] == TEXT(',')) { FieldEnd += 1; }
			Json = Json.Left(FieldStart) + Json.RightChop(FieldEnd);
			Check(FFileHelper::SaveStringToFile(Json, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write manifest without selfCheck"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject missing selfCheck"));
		}

		// --- 4e. Legacy-layout fallback (a pre-condensed serializer embeds
		//        the hash over its own byte layout, e.g. pretty-printed) ----
		{
			Check(UnrealGamePatcher::FIndexManifestFile::Save(IndexPath, Sample), TEXT("save clean manifest (legacy case)"));
			FString Json;
			Check(FFileHelper::LoadFileToString(Json, *IndexPath), TEXT("read clean manifest (legacy case)"));
			TSharedPtr<FJsonObject> Root;
			{
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
				Check(FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid(), TEXT("parse clean manifest (legacy case)"));
			}
			// Re-serialize without selfCheck under a pretty writer: the byte
			// layout a legacy publisher could have produced (LINE_TERMINATOR
			// + tabs). The hash goes over exactly these bytes.
			Root->Values.Remove(TEXT("selfCheck"));
			FString PrettyBody;
			{
				const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&PrettyBody);
				Check(FJsonSerializer::Serialize(Root.ToSharedRef(), Writer), TEXT("serialize pretty body (legacy case)"));
			}
			Root->SetStringField(TEXT("selfCheck"), UnrealGamePatcher::JsonSelfCheckHex(PrettyBody));
			FString PrettyFull;
			{
				const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&PrettyFull);
				Check(FJsonSerializer::Serialize(Root.ToSharedRef(), Writer), TEXT("serialize pretty full (legacy case)"));
			}
			Check(FFileHelper::SaveStringToFile(PrettyFull, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write legacy-layout manifest"));
			// Canonical re-serialization cannot reproduce the pretty bytes;
			// the raw-body fallback must accept the file.
			UnrealGamePatcher::FIndexManifest LegacyLoaded;
			Check(UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, LegacyLoaded), TEXT("load legacy-layout manifest (raw-body fallback)"));
			Check(LegacyLoaded.Files.Num() == Sample.Files.Num() &&
				LegacyLoaded.Chunks.Num() == Sample.Chunks.Num(),
				TEXT("legacy manifest data intact"));

			// Tampering a legacy-layout file must still be caught: the raw
			// hash over the modified bytes no longer matches.
			Check(FFileHelper::LoadFileToString(Json, *IndexPath), TEXT("re-read legacy manifest"));
			Json = Json.Replace(TEXT("1.0.0"), TEXT("1.0.1"));
			Check(FFileHelper::SaveStringToFile(Json, *IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write tampered legacy manifest"));
			UnrealGamePatcher::FIndexManifest LegacyDummy;
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, LegacyDummy), TEXT("reject tampered legacy-layout manifest"));
		}

		// --- 5. Corruption / schema detection ------------------------------
		{
			UnrealGamePatcher::FIndexManifest Dummy;
			// 5a. Not JSON at all.
			Check(FFileHelper::SaveStringToFile(TEXT("{ not valid json"), *IndexPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write corrupt (bad json)"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject bad json"));

			// 5b. Wrong format name.
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"Other.index\",\"version\":1}"), *IndexPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write wrong format"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject wrong format"));

			// 5c. Wrong version number.
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"UnrealGamePatcher.index\",\"version\":99}"), *IndexPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write wrong version"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject wrong version"));

			// 5d. Missing required appId.
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"UnrealGamePatcher.index\",\"version\":1,\"contentVersion\":\"1\",\"files\":{},\"chunks\":{}}"),
				*IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write missing appId"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject missing appId"));

			// 5e. (updateStrategy removed from the format. A hand-crafted
			// "legacy" file cannot pass the self-check anyway - the embedded
			// hash binds the exact body, and re-serialization drops unknown
			// fields - so there is no meaningful compat case: v2 manifests
			// written by the current publisher simply lack the field, which
			// section 1 covers.)

			// 5f. File entry with non-hex sha1.
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"UnrealGamePatcher.index\",\"version\":1,\"appId\":\"IOStoreDemo\",\"contentVersion\":\"1\",\"files\":{\"a.bin\":{\"sha1\":\"nothex\",\"size\":5}},\"chunks\":{}}"),
				*IndexPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write bad file sha1"));
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject non-hex file sha1"));

			// 5g. Missing file.
			PF.DeleteFile(*IndexPath);
			Check(!UnrealGamePatcher::FIndexManifestFile::Load(IndexPath, Dummy), TEXT("reject missing file"));
		}

		// Clean up the work dir.
		PF.DeleteDirectoryRecursively(*WorkDir);

		return FinishTest(bAllPassed, TEXT("indextest"));
	
	}

}
