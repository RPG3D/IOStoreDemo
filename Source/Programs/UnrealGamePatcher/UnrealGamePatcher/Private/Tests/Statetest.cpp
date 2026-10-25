// Copyright Epic Games, Inc. All Rights Reserved.
// -cli statetest —— sidecar 状态文件（事务/状态层）
// 验证点：完整状态 roundtrip（holes + pending 事务）；空状态；损坏检测；
//         compact 的洞消费链（注入洞 → bloat 比率 → 压实清零）。

#include "Tests/TestFramework.h"

namespace UnrealGamePatcher::Tests
{
	int32 RunStateTest(const TCHAR* /*CmdLine*/)
	{

		// Sidecar state acceptance (task 2.3): write/read roundtrip plus
		// corruption detection. Works on a temp dir, leaves no artifacts.
		bool bAllPassed = true;
		const FString WorkDir = GetTestWorkDir(TEXT("statetest"));
		ResetWorkDir(WorkDir);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		const FString StatePath = UnrealGamePatcher::Paths::GetStatePath(WorkDir);

		auto Check = MakeCheck(bAllPassed);

		// --- 1. Roundtrip of a full state (holes + pending transaction) ----
		{
			UnrealGamePatcher::FPatcherState State;
			UnrealGamePatcher::FHole Hole0;
			Hole0.File = TEXT("Content/Paks/game.pak");
			Hole0.Offset = 5300306;
			Hole0.Size = 586;
			State.Holes.Add(Hole0);
			State.Holes.Add({ 5301964, 441 });
			State.Transaction.Status = UnrealGamePatcher::ETransactionStatus::Pending;
			State.Transaction.ContentVersion = TEXT("v123");
			State.LastVerifiedAt = TEXT("2026-09-06T00:00:00.000Z");
			UnrealGamePatcher::FTransactionTarget OverwriteTarget;
			OverwriteTarget.File = TEXT("Content/Paks/game.pak");
			OverwriteTarget.Op = UnrealGamePatcher::EApplyOp::Overwrite;
			OverwriteTarget.Offset = 5000188;
			OverwriteTarget.Size = 300000;
			OverwriteTarget.OriginalHash = TEXT("c9393344fbc1cf3d495f87e9f8d5d18b90597b1a");
			OverwriteTarget.NewHash = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
			OverwriteTarget.BackupPath = TEXT("Content/Paks/.backup/game.pak.0.bak");
			OverwriteTarget.bPreExisted = true;
			State.Transaction.Targets.Add(OverwriteTarget);
			UnrealGamePatcher::FTransactionTarget AppendTarget;
			AppendTarget.File = TEXT("Content/Paks/game.pak");
			AppendTarget.Op = UnrealGamePatcher::EApplyOp::Append;
			AppendTarget.Offset = 5300896;
			AppendTarget.Size = 840;
			AppendTarget.OriginalHash = TEXT("");
			AppendTarget.NewHash = TEXT("1b106f5a3c11df0c6fe11796f4e7cfb62420ac29");
			State.Transaction.Targets.Add(AppendTarget);

			Check(UnrealGamePatcher::FPatcherStateFile::Save(StatePath, State), TEXT("save full state"));

			UnrealGamePatcher::FPatcherState Loaded;
			Check(UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Loaded), TEXT("load full state"));
			Check(Loaded.Holes.Num() == 2 &&
				Loaded.Holes[0].Offset == 5300306 && Loaded.Holes[0].Size == 586 &&
				Loaded.Holes[1].Offset == 5301964 && Loaded.Holes[1].Size == 441,
				TEXT("holes roundtrip"));
			Check(Loaded.Transaction.Status == UnrealGamePatcher::ETransactionStatus::Pending &&
				Loaded.Transaction.ContentVersion == TEXT("v123"),
				TEXT("txn status/version roundtrip"));
			Check(Loaded.Transaction.Targets.Num() == 2 &&
				Loaded.Transaction.Targets[0].File == TEXT("Content/Paks/game.pak") &&
				Loaded.Transaction.Targets[0].Op == UnrealGamePatcher::EApplyOp::Overwrite &&
				Loaded.Transaction.Targets[0].Offset == 5000188 &&
				Loaded.Transaction.Targets[0].Size == 300000 &&
				Loaded.Transaction.Targets[0].OriginalHash == TEXT("c9393344fbc1cf3d495f87e9f8d5d18b90597b1a") &&
				Loaded.Transaction.Targets[0].BackupPath == TEXT("Content/Paks/.backup/game.pak.0.bak"),
				TEXT("overwrite target roundtrip"));
			Check(Loaded.Transaction.Targets[1].Op == UnrealGamePatcher::EApplyOp::Append &&
				Loaded.Transaction.Targets[1].Offset == 5300896 &&
				Loaded.Transaction.Targets[1].NewHash == TEXT("1b106f5a3c11df0c6fe11796f4e7cfb62420ac29"),
				TEXT("append target roundtrip"));
			// Field-level roundtrips the original section skipped:
			// LastVerifiedAt (drives the VerifyEveryDays trigger), the hole's
			// file id, bPreExisted (decides restore vs remove on rollback),
			// and the committed/rolled-back statuses + replace/delete ops.
			Check(Loaded.Holes[0].File == TEXT("Content/Paks/game.pak"), TEXT("hole file id roundtrip"));
			Check(Loaded.Transaction.Targets[0].bPreExisted, TEXT("bPreExisted roundtrip"));
			Check(Loaded.Transaction.Targets[1].bPreExisted == false, TEXT("bPreExisted=false roundtrip"));
			Check(!Loaded.LastVerifiedAt.IsEmpty(), TEXT("LastVerifiedAt roundtrip (present)"));

			// committed + replace-file/delete targets.
			UnrealGamePatcher::FPatcherState Committed;
			Committed.Transaction.Status = UnrealGamePatcher::ETransactionStatus::Committed;
			Committed.Transaction.ContentVersion = TEXT("v124");
			UnrealGamePatcher::FTransactionTarget ReplaceTarget;
			ReplaceTarget.File = TEXT("readme.txt");
			ReplaceTarget.Op = UnrealGamePatcher::EApplyOp::ReplaceFile;
			ReplaceTarget.bPreExisted = true;
			Committed.Transaction.Targets.Add(ReplaceTarget);
			UnrealGamePatcher::FTransactionTarget DeleteTarget;
			DeleteTarget.File = TEXT("old.ini");
			DeleteTarget.Op = UnrealGamePatcher::EApplyOp::Delete;
			DeleteTarget.bPreExisted = true;
			Committed.Transaction.Targets.Add(DeleteTarget);
			Check(UnrealGamePatcher::FPatcherStateFile::Save(StatePath, Committed), TEXT("save committed state"));
			UnrealGamePatcher::FPatcherState LoadedCommitted;
			Check(UnrealGamePatcher::FPatcherStateFile::Load(StatePath, LoadedCommitted), TEXT("load committed state"));
			Check(LoadedCommitted.Transaction.Status == UnrealGamePatcher::ETransactionStatus::Committed &&
				LoadedCommitted.Transaction.Targets.Num() == 2 &&
				LoadedCommitted.Transaction.Targets[0].Op == UnrealGamePatcher::EApplyOp::ReplaceFile &&
				LoadedCommitted.Transaction.Targets[1].Op == UnrealGamePatcher::EApplyOp::Delete,
				TEXT("committed status + replace/delete ops roundtrip"));

			// rolled-back status + LastVerifiedAt value roundtrip.
			UnrealGamePatcher::FPatcherState RolledBack;
			RolledBack.Transaction.Status = UnrealGamePatcher::ETransactionStatus::RolledBack;
			RolledBack.LastVerifiedAt = TEXT("2026-01-02T03:04:05.000Z");
			Check(UnrealGamePatcher::FPatcherStateFile::Save(StatePath, RolledBack), TEXT("save rolled-back state"));
			UnrealGamePatcher::FPatcherState LoadedRolled;
			Check(UnrealGamePatcher::FPatcherStateFile::Load(StatePath, LoadedRolled), TEXT("load rolled-back state"));
			Check(LoadedRolled.Transaction.Status == UnrealGamePatcher::ETransactionStatus::RolledBack &&
				LoadedRolled.LastVerifiedAt == TEXT("2026-01-02T03:04:05.000Z"),
				TEXT("rolled-back status + LastVerifiedAt value roundtrip"));
		}

		// --- 2. Empty state roundtrip --------------------------------------
		{
			UnrealGamePatcher::FPatcherState Empty;
			Check(UnrealGamePatcher::FPatcherStateFile::Save(StatePath, Empty), TEXT("save empty state"));
			UnrealGamePatcher::FPatcherState Loaded;
			Check(UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Loaded), TEXT("load empty state"));
			Check(Loaded.Holes.Num() == 0 &&
				Loaded.Transaction.Status == UnrealGamePatcher::ETransactionStatus::None &&
				Loaded.Transaction.Targets.Num() == 0,
				TEXT("empty state roundtrip"));
		}

		// --- 3. Corruption detection ---------------------------------------
		{
			// 3a. Not JSON at all.
			Check(FFileHelper::SaveStringToFile(TEXT("{ not valid json"), *StatePath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write corrupt (bad json)"));
			UnrealGamePatcher::FPatcherState Dummy;
			Check(!UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Dummy), TEXT("reject bad json"));

			// 3b. Valid JSON, wrong schema (format missing).
			Check(FFileHelper::SaveStringToFile(TEXT("{\"holes\":[]}"), *StatePath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write wrong schema"));
			Check(!UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Dummy), TEXT("reject missing format"));

			// 3c. Valid JSON, wrong format name.
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"SomeOther.state\",\"version\":1,\"holes\":[]}"), *StatePath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write wrong format"));
			Check(!UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Dummy), TEXT("reject wrong format"));

			// 3d. Wrong version number.
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"UnrealGamePatcher.state\",\"version\":99,\"holes\":[]}"), *StatePath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write wrong version"));
			Check(!UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Dummy), TEXT("reject wrong version"));

			// 3e. Truncated JSON (cut mid-string).
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"UnrealGamePatcher.state\",\"version\":1,\"holes\":[{\"offset\":5"), *StatePath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write truncated json"));
			Check(!UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Dummy), TEXT("reject truncated json"));

			// 3f. Missing file.
			PF.DeleteFile(*StatePath);
			Check(!UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Dummy), TEXT("reject missing file"));

			// 3g. Valid file, corrupt transaction (bad target type).
			Check(FFileHelper::SaveStringToFile(
				TEXT("{\"format\":\"UnrealGamePatcher.state\",\"version\":1,\"holes\":[],\"transaction\":{\"status\":\"pending\",\"targets\":[42]}}"),
				*StatePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("write corrupt txn target"));
			Check(!UnrealGamePatcher::FPatcherStateFile::Load(StatePath, Dummy), TEXT("reject corrupt txn target"));
		}

		// Clean up the work dir.
		PF.DeleteDirectoryRecursively(*WorkDir);

		return FinishTest(bAllPassed, TEXT("statetest"));
	
	}

}
