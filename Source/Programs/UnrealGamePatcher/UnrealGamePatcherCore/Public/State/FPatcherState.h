// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UnrealGamePatcher
{
	/** One reclaimed hole: a data region invalidated by an end append
	 *  (the old directory/footer area or a superseded file chunk). Persisted
	 *  so `compact` can reclaim it and statistics can report bloat. File is
	 *  the target the hole belongs to (manifest id; empty = legacy/global). */
	struct FHole
	{
		int64 Offset = 0;
		int64 Size = 0;
		FString File;
	};

	/** Lifecycle marker of the most recent transaction (design D5). */
	enum class ETransactionStatus : uint8
	{
		None,        // no transaction recorded yet
		Pending,     // APPLY started, not yet verified/committed
		Committed,   // all writes verified, transaction finished
		RolledBack,  // a failure restored the targets
	};

	/** How a target file is modified by the transaction. */
	enum class EApplyOp : uint8
	{
		Overwrite,   // in-place overwrite of a stored block (same size)
		Append,      // new block appended at the end of the target
		ReplaceFile, // full-file atomic replace (plain files: dll/exe/...)
		Delete,      // file removed by the update (rollback restores the backup)
	};

	/** Per-target operation logged by the transaction, sufficient to roll back:
	 *  the original bytes (hash + backup path) and the applied geometry. */
	struct FTransactionTarget
	{
		FString File;          // target file path (absolute, or pak entry path for in-pak ops)
		EApplyOp Op = EApplyOp::Overwrite;
		int64 Offset = 0;      // overwrite offset / append start / ignored for ReplaceFile
		int64 Size = 0;        // bytes written by this op
		FString OriginalHash;  // SHA1 (hex) of the original bytes, for verify-on-rollback
		FString NewHash;       // SHA1 (hex) of the bytes that were written, for VERIFY step
		FString BackupPath;    // where the original block was backed up (empty if none needed)
		/** True when the target file existed BEFORE this transaction (Changed),
		 *  false when this transaction creates it (Added). Containers (pak/ucas)
		 *  legitimately have an empty OriginalHash - the virtual-disk model
		 *  gives them no whole-file hash - so existence must be recorded
		 *  explicitly or a crash-window rollback could DELETE a pre-existing
		 *  container instead of leaving it untouched. Optional in the sidecar
		 *  (older files default to false, which preserves the old heuristic
		 *  for plain files and errs safe for containers). */
		bool bPreExisted = false;
	};

	/** One in-flight (or finished) transaction. */
	struct FTransactionRecord
	{
		ETransactionStatus Status = ETransactionStatus::None;
		FString ContentVersion; // content version being applied to
		TArray<FTransactionTarget> Targets;
	};

	/** The on-disk sidecar state: `UnrealGamePatcher.state.json` (design D5).
	 *  Kept small and deliberately plain so it is easy to inspect and to
	 *  tolerate a broken file (the local pak directory stays the source of
	 *  truth for file data). */
	struct FPatcherState
	{
		static constexpr int32 FormatVersion = 1;
		static constexpr const TCHAR* FormatName = TEXT("UnrealGamePatcher.state");

		TArray<FHole> Holes;
		FTransactionRecord Transaction;
		/** ISO-8601 UTC timestamp of the last VERIFY-mode run (trust-mode
		 *  updates do not touch it). Drives the time-based verify trigger:
		 *  an update older than the configured interval switches to verify
		 *  mode. Empty = never verified (first run verifies). */
		FString LastVerifiedAt;
	};

	/** Loads and saves FPatcherState as JSON. */
	struct FPatcherStateFile
	{
		/** Serializes State and writes it to StatePath (atomic-ish: write
		 *  temp then rename, so a crash mid-write cannot corrupt a good file). */
		UNREALGAMEPATCHERCORE_API static bool Save(const FString& StatePath, const FPatcherState& State);

		/** Loads StatePath. Returns false (with a logged reason) when the file
		 *  is missing, unreadable, malformed JSON, or fails schema validation —
		 *  i.e. damaged state is detected and reported, never silently merged. */
		UNREALGAMEPATCHERCORE_API static bool Load(const FString& StatePath, FPatcherState& OutState);
	};
}
