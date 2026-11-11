// Copyright Epic Games, Inc. All Rights Reserved.

#include "Download/FDownloadExecutor.h"
#include "UnrealGamePatcherDownload.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Templates/SharedPointer.h"

namespace UnrealGamePatcher
{
	static constexpr const TCHAR* JournalMagic = TEXT("UGPJRN1");

	static FString JournalPathFor(const FString& DestPath)
	{
		return DestPath + TEXT(".journal");
	}
	static FString StagingPathFor(const FString& DestPath)
	{
		return DestPath + TEXT(".download");
	}

	static bool FileSha1Matches(const FString& Path, const FString& ExpectedSha1)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		IFileHandle* Handle = PF.OpenRead(*Path);
		if (!Handle)
		{
			return false;
		}
		FSHA1 Hasher;
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(1 << 20);
		int64 Remaining = Handle->Size();
		while (Remaining > 0)
		{
			const int32 ToRead = (int32)FMath::Min<int64>(Remaining, Buffer.Num());
			if (!Handle->Read(Buffer.GetData(), ToRead))
			{
				delete Handle;
				return false;
			}
			Hasher.Update(Buffer.GetData(), ToRead);
			Remaining -= ToRead;
		}
		delete Handle;
		Hasher.Final();
		uint8 Hash[20] = { 0 };
		Hasher.GetHash(Hash);
		FString Hex;
		Hex.Reserve(40);
		for (int32 I = 0; I < 20; ++I)
		{
			Hex += FString::Printf(TEXT("%02x"), Hash[I]);
		}
		return Hex.Equals(ExpectedSha1, ESearchCase::IgnoreCase);
	}

	// One lock per staging file: segment settles are concurrent, and the
	// hole-fill + positioned write below must be serialized per file or a
	// concurrent filler working from a stale file size can zero out another
	// segment's already-written data. Network downloads stay parallel - only
	// the local writes to the SAME file serialize. Entries are bounded by the
	// run's job count and intentionally live for the process lifetime.
	static FCriticalSection& StagingLockFor(const FString& Staging)
	{
		static FCriticalSection RegistryCS;
		static TMap<FString, FCriticalSection*> Registry;
		FScopeLock RegistryLock(&RegistryCS);
		FCriticalSection*& Lock = Registry.FindOrAdd(Staging, nullptr);
		if (!Lock)
		{
			Lock = new FCriticalSection();
		}
		return *Lock;
	}

	static void JournalReset(const FString& DestPath, int64 SegmentSize, int64 Total)
	{
		const FString Text = FString::Printf(TEXT("%s %lld %lld\n"), JournalMagic, SegmentSize, Total);
		FFileHelper::SaveStringToFile(Text, *JournalPathFor(DestPath),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	static void JournalAppendSegment(const FString& DestPath, int32 SegmentIndex)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (IFileHandle* Handle = PF.OpenWrite(*JournalPathFor(DestPath), /*bAppend=*/true))
		{
			const FString Line = FString::Printf(TEXT("SEG %d\n"), SegmentIndex);
			const FTCHARToUTF8 Conv(*Line);
			Handle->Write((const uint8*)Conv.Get(), Conv.Length());
			delete Handle;
		}
	}

	static bool JournalReplay(const FString& DestPath, int64 SegmentSize, int64 Total,
		int32 NumSegments, TSet<int32>& OutDone)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *JournalPathFor(DestPath)))
		{
			return false;
		}
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		if (Lines.Num() < 1)
		{
			return false;
		}
		TArray<FString> Header;
		Lines[0].ParseIntoArray(Header, TEXT(" "));
		if (Header.Num() < 3 || Header[0] != JournalMagic ||
			FCString::Atoi64(*Header[1]) != SegmentSize || FCString::Atoi64(*Header[2]) != Total)
		{
			return false;
		}
		for (int32 I = 1; I < Lines.Num(); ++I)
		{
			TArray<FString> Fields;
			Lines[I].ParseIntoArray(Fields, TEXT(" "));
			if (Fields.Num() == 2 && Fields[0] == TEXT("SEG"))
			{
				const int32 Idx = FCString::Atoi(*Fields[1]);
				if (Idx >= 0 && Idx < NumSegments)
				{
					OutDone.Add(Idx);
				}
			}
		}
		return true;
	}

	struct FSegment
	{
		int64 Offset = 0;
		int64 Size = 0;
		int32 Index = 0;
		bool bDone = false;
		bool bInFlight = false;
		bool bWholeFile = false;   // probe fallback: one whole-body GET, no Range
		int32 Attempts = 0;
		IChunkDownloader::FCancelHandle CancelHandle;
	};

	struct FJob
	{
		FDownloadJob Job;
		FDownloadResult Result;
		int64 Handle = 0;
		int32 EnqueueOrder = 0;

		int64 Total = -1;
		bool bRangeOk = true;
		bool bProbing = false;
		bool bPlanned = false;
		bool bPreallocated = false;
		bool bFinished = false;
		int32 VerifyAttempts = 0;
		// Effective segment size for THIS job (no-Range servers fall back to
		// one whole-file segment) and whether the on-disk journal replayed
		// successfully for the current plan - journal (re)initialization and
		// resume both key off these.
		int64 EffectiveSegSize = 0;
		bool bJournalValid = false;

		TArray<FSegment> Segments;

		int32 InFlight = 0;
		int32 SegmentsDone = 0;
		int32 Retries = 0;
		int64 BytesDone = 0;
		bool bCanceled = false;

		TFunction<void(const FDownloadResult&)> OnDone;

		int32 UndoneCount() const { return Segments.Num() - SegmentsDone; }
		bool AllSegmentsDone() const { return Total >= 0 && SegmentsDone >= Segments.Num(); }
	};

	/** The whole executor state machine. Every mutation happens under one
	 *  lock, from exactly two places: the caller thread (Submit/Cancel) and
	 *  the engine HTTP thread (settle callbacks, serialized by the engine).
	 *  There is no dedicated download thread - the engine HTTP stack already
	 *  provides the connection pool, I/O thread and stall/timeout detection;
	 *  we only split files into range tasks and schedule them. */
	struct FDownloadExecutor::FExecutorState : public TSharedFromThis<FExecutorState>
	{
		IChunkDownloader& Backend;
		FDownloadExecutorConfig Cfg;

		FCriticalSection Lock;
		TArray<TUniquePtr<FJob>> Jobs;
		TMap<int64, FJob*> JobsByHandle;
		int32 NextEnqueueOrder = 0;
		int32 WindowInFlight = 0;
		int64 NextHandle = 0;
		int64 ProgressTick = 0;

		FExecutorState(IChunkDownloader& InBackend, const FDownloadExecutorConfig& InConfig)
			: Backend(InBackend), Cfg(InConfig)
		{
		}

		// ---- state transitions (Lock held) --------------------------------

		int64 Submit(FDownloadJob&& InJob, TFunction<void(const FDownloadResult&)> OnDone)
		{
			FJob* J = new FJob();
			J->Job = MoveTemp(InJob);
			J->Result.Id = J->Job.Id;
			J->EnqueueOrder = NextEnqueueOrder++;
			J->Handle = NextHandle++;
			J->Total = J->Job.ExpectedSize;
			J->OnDone = MoveTemp(OnDone);
			Jobs.Add(TUniquePtr<FJob>(J));
			JobsByHandle.Add(J->Handle, J);
			Refill();
			return J->Handle;
		}

		void Cancel(int64 Handle)
		{
			FJob** Found = JobsByHandle.Find(Handle);
			if (!Found)
			{
				return;
			}
			FJob& J = **Found;
			if (J.bFinished || J.bCanceled)
			{
				return;
			}
			J.bCanceled = true;
			for (FSegment& S : J.Segments)
			{
				if (S.bInFlight)
				{
					Backend.CancelDownload(S.CancelHandle);
				}
			}
			// No in-flight request to settle into the finish path - finish now.
			if (J.InFlight == 0)
			{
				Finish(J, false, TEXT("canceled"));
			}
			// With in-flight segments, the first settle callback finishes the job.
		}

		void OnProbeSettled(FJob& J, int64 TotalSize, bool bRangeOk)
		{
			J.bProbing = false;
			if (J.bCanceled)
			{
				Finish(J, false, TEXT("canceled"));
				return;
			}
			if (TotalSize < 0)
			{
				Finish(J, false, TEXT("size probe failed"));
				return;
			}
			J.Total = TotalSize;
			J.bRangeOk = bRangeOk;
			// Segment planning happens once, in Refill.
			Refill();
		}

		void OnSegmentSettled(FJob& J, int32 SegmentIndex, bool bOk,
			const TArray64<uint8>& Data, const FString& Error)
		{
			WindowInFlight -= 1;
			// Paired with IssueSegment's += 1: every issued segment settles
			// exactly once, so the decrement must happen before the early
			// returns below or a canceled/paused job's count goes stale and
			// Cancel's InFlight==0 check never fires.
			J.InFlight -= 1;
			ProgressTick += 1;

			if (J.bFinished)
			{
				return; // canceled job whose in-flight request still settled
			}
			if (J.bCanceled)
			{
				Finish(J, false, TEXT("canceled"));
				return;
			}

			if (SegmentIndex < 0 || SegmentIndex >= J.Segments.Num())
			{
				return;
			}
			FSegment& S = J.Segments[SegmentIndex];
			S.bInFlight = false;

			if (!bOk || Data.Num() != S.Size)
			{
				if (S.Attempts <= Cfg.MaxRetries)
				{
					UE_LOG(LogUnrealGamePatcherDownload, Warning,
						TEXT("FDownloadExecutor: '%s' segment %d failed (%s) - retry %d/%d"),
						*J.Job.Id, S.Index, *Error, S.Attempts, Cfg.MaxRetries);
					return; // Refill reissues the segment
				}
				Finish(J, false, FString::Printf(TEXT("segment %d failed: %s"), S.Index, *Error));
				return;
			}

			IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
			const FString Staging = StagingPathFor(J.Job.DestPath);
			PF.CreateDirectoryTree(*FPaths::GetPath(Staging));
			// Segments settle out of order: a later segment may reach the file
			// before the earlier ones. POSIX allows seek-past-EOF writes (the
			// gap becomes a hole), but Android's FUSE external storage REJECTS
			// them - fill the hole with zeros first so every segment write
			// lands at or before the current EOF. Fill + write are serialized
			// per staging file: concurrent fillers working from a stale file
			// size could otherwise zero out another segment's written data.
			// Network transfers stay parallel - only same-file disk writes
			// serialize here.
			FScopeLock StagingLock(&StagingLockFor(Staging));
			{
				const int64 CurSize = IFileManager::Get().FileSize(*Staging);
				if (CurSize >= 0 && CurSize < S.Offset)
				{
					static const uint8 Zeros[4096] = {};
					if (FArchive* Filler = IFileManager::Get().CreateFileWriter(*Staging, FILEWRITE_Append))
					{
						Filler->Seek(CurSize);
						int64 Remaining = S.Offset - CurSize;
						while (Remaining > 0 && !Filler->IsError())
						{
							const int32 ZeroChunk = (int32)FMath::Min<int64>(sizeof(Zeros), Remaining);
							Filler->Serialize((void*)Zeros, ZeroChunk);
							Remaining -= ZeroChunk;
						}
						delete Filler;
					}
				}
			}
			// FILEWRITE_Append opens WITHOUT truncating (plain OpenWrite would
			// zero the already-written segments of a multi-segment file);
			// Seek() then positions absolutely for the in-place segment write.
			bool bWritten = false;
			if (FArchive* Writer = IFileManager::Get().CreateFileWriter(*Staging, FILEWRITE_Append))
			{
				Writer->Seek(S.Offset);
				Writer->Serialize((void*)Data.GetData(), Data.Num());
				bWritten = !Writer->IsError();
				delete Writer;
			}
			StagingLock.Unlock();
			if (!bWritten)
			{
				Finish(J, false, FString::Printf(TEXT("cannot write segment %d"), S.Index));
				return;
			}
			JournalAppendSegment(J.Job.DestPath, S.Index);
			S.bDone = true;
			J.SegmentsDone += 1;
			J.BytesDone += Data.Num();

			if (J.AllSegmentsDone())
			{
				VerifyAndPromote(J);
			}
		}

		// ---- scheduling (Lock held) ---------------------------------------

		/** Fills the in-flight window. Two rules, in order: (0) finishing jobs -
		 *  a job with <= 2 undone segments gets the window first (capped at 4
		 *  in-flight segments per job) so files complete (and verify/promote)
		 *  early; (1) any job, unlimited share, plus size probes for
		 *  unknown-size jobs. Called after every submit/cancel/settle - the
		 *  single scheduling point. */
		void Refill()
		{
			while (WindowInFlight < Cfg.Window)
			{
				FJob* PickJob = nullptr;
				FSegment* PickSeg = nullptr;
				bool bProbe = false;
				for (int32 Rule = 0; Rule < 2 && !PickJob; ++Rule)
				{
					for (TUniquePtr<FJob>& JobPtr : Jobs)
					{
						FJob& J = *JobPtr;
						if (J.bFinished || J.bCanceled || J.bProbing)
						{
							continue;
						}
						if (J.Total < 0)
						{
							if (Rule == 1)
							{
								J.bProbing = true;
								PickJob = &J;
								bProbe = true;
								break;
							}
							continue;
						}
						if (!J.bPlanned)
						{
							J.bPlanned = true;
							PlanSegments(J);
							if (J.Total > 0)
							{
								Preallocate(J);
							}
							else if (J.Total == 0)
							{
								// A zero-size resource plans zero segments;
								// create the empty staging file so the
								// verify-promote below succeeds - otherwise
								// the verify-fail retry path would wait for
								// segment settles that can never come and
								// the job (and its batch) would hang.
								if (IFileHandle* Handle = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*StagingPathFor(J.Job.DestPath)))
								{
									delete Handle;
								}
							}
							if (J.AllSegmentsDone())
							{
								VerifyAndPromote(J);
							}
							continue;
						}
						if (J.Segments.Num() == 0)
						{
							continue;
						}
						const bool bFinishing = J.UndoneCount() <= 2;
						const bool bCapOk = Rule == 1 || J.InFlight < 4;
						if ((Rule == 0 && !bFinishing) || !bCapOk)
						{
							continue;
						}
						for (FSegment& S : J.Segments)
						{
							if (!S.bDone && !S.bInFlight)
							{
								PickJob = &J;
								PickSeg = &S;
								break;
							}
						}
						if (PickJob)
						{
							break;
						}
					}
				}
				if (!PickJob)
				{
					return;
				}

				if (bProbe)
				{
					IssueProbe(*PickJob);
				}
				else
				{
					IssueSegment(*PickJob, *PickSeg);
				}
			}
		}

		void IssueSegment(FJob& J, FSegment& S)
		{
			J.InFlight += 1;
			WindowInFlight += 1;
			S.bInFlight = true;
			S.Attempts += 1;
			if (S.Attempts > 1)
			{
				J.Retries += 1;
			}
			FJob* JobPtr = &J;
			const int32 SegIndex = S.Index;
			// Wire semantics: a request for the WHOLE resource is a plain GET
			// (no Range header - single-segment files and the probe fallback);
			// any proper sub-range is an exact Range request - including
			// segment 0 of a multi-segment file, which must not fetch the
			// whole body.
			const bool bWholeResource = S.bWholeFile || (S.Offset == 0 && S.Size >= J.Total);
			const int64 WireSize = bWholeResource ? -1 : S.Size;
			TSharedRef<FExecutorState> ExecutorState = SharedThis(this);
			// The handle MUST be stored: Cancel() and ~FDownloadExecutor abort
			// in-flight requests through it. Dropping the return value (review
			// 2026-10-09 H2) left S.CancelHandle forever null, so "cancel" was
			// bookkeeping-only and the HTTP transfer ran to completion.
			// (Safe even for the synchronous fallback backends, which fire
			// OnDone inline before the assignment lands - the settle does not
			// read the handle, and a completed request ignores CancelDownload.)
			S.CancelHandle = Backend.DownloadRangeAsync(J.Job.Url, S.Offset, WireSize,
				[ExecutorState, JobPtr, SegIndex](bool bOk, const TArray64<uint8>& Data, const FString& Error)
			{
				// Engine HTTP thread, serialized with every other settle. The
				// state ref keeps the executor alive until in-flight requests
				// have drained (their settles are guarded no-ops by then).
				FScopeLock Scope(&ExecutorState->Lock);
				ExecutorState->OnSegmentSettled(*JobPtr, SegIndex, bOk, Data, Error);
				ExecutorState->Refill();
			});
		}

		void IssueProbe(FJob& J)
		{
			FJob* JobPtr = &J;
			TSharedRef<FExecutorState> ExecutorState = SharedThis(this);
			Backend.ProbeSizeAsync(J.Job.Url,
				[ExecutorState, JobPtr](int64 TotalSize, bool bRangeOk)
			{
				FScopeLock Scope(&ExecutorState->Lock);
				ExecutorState->OnProbeSettled(*JobPtr, TotalSize, bRangeOk);
			});
		}

		// ---- file plumbing (Lock held) ------------------------------------

		void PlanSegments(FJob& J)
		{
			// A server without Range support answers 200 with the FULL body for
			// any range request - segmenting would corrupt every segment after
			// the first. Fall back to one whole-file request (the segment is
			// issued with Size<0 so no Range header is sent). The Max(1)
			// guards the division for a zero-size resource on such a server.
			const int64 Seg = J.bRangeOk ? Cfg.SegmentSize : FMath::Max<int64>(J.Total, 1);
			const int32 Num = (int32)((J.Total + Seg - 1) / Seg);
			J.EffectiveSegSize = Seg;
			J.Segments.Reset();
			J.Segments.SetNum(Num);
			if (Num > 0 && !J.bRangeOk)
			{
				J.Segments[0].bWholeFile = true;
			}
			TSet<int32> Done;
			J.bJournalValid = false;
			if (Num > 0)
			{
				// An invalid/absent journal simply means nothing is resumed -
				// but the distinction matters to Preallocate, which must write
				// a fresh header so THIS run's settled segments become
				// replayable (a journal without the UGPJRN1 header line is
				// unusable: crash-resume would silently re-download all).
				J.bJournalValid = JournalReplay(J.Job.DestPath, Seg, J.Total, Num, Done);
			}
			for (int32 I = 0; I < Num; ++I)
			{
				FSegment& S = J.Segments[I];
				S.Index = I;
				S.Offset = (int64)I * Seg;
				S.Size = FMath::Min<int64>(Seg, J.Total - S.Offset);
				S.bDone = Done.Contains(I);
				if (S.bDone)
				{
					J.SegmentsDone += 1;
				}
			}
		}

		void Preallocate(FJob& J)
		{
			IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
			const FString Staging = StagingPathFor(J.Job.DestPath);
			PF.CreateDirectoryTree(*FPaths::GetPath(Staging));

			// Resume path: a full-size staging file is exactly what an
			// interrupted run leaves behind - OpenWrite would TRUNCATE it and
			// destroy the journaled segments, so keep it as-is. Only with a
			// journal that replayed for the current plan, though: replay
			// success is what tells us the SEG marks match this download.
			const int64 OnDisk = IFileManager::Get().FileSize(*Staging);
			if (OnDisk == J.Total && J.bJournalValid)
			{
				J.bPreallocated = true;
				return;
			}

			// (Re)initialize the journal whenever it is not valid for THIS
			// download. A fresh download has no journal at all - without
			// writing the header HERE, the SEG lines appended by settled
			// segments could never be replayed and every crash-resume would
			// silently re-download everything. A staging/journal mismatch
			// (crash mid-preallocation or mid-segment-write) means the resume
			// state is not trustworthy - drop it and start over. A full-size
			// staging with an invalid journal is simply truncated below: its
			// SEG marks are unverifiable without the header.
			JournalReset(J.Job.DestPath, J.EffectiveSegSize, J.Total);
			if (J.SegmentsDone > 0)
			{
				for (FSegment& S : J.Segments)
				{
					S.bDone = false;
				}
				J.SegmentsDone = 0;
			}

			if (IFileHandle* Handle = PF.OpenWrite(*Staging))
			{
				if (J.Total > 0 && Handle->Seek(J.Total - 1))
				{
					uint8 Zero = 0;
					Handle->Write(&Zero, 1);
				}
				delete Handle;
				J.bPreallocated = true;
			}
		}

		void VerifyAndPromote(FJob& J)
		{
			IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
			const FString Staging = StagingPathFor(J.Job.DestPath);
			const int64 OnDisk = IFileManager::Get().FileSize(*Staging);
			bool bOk = OnDisk == J.Total;
			if (bOk && !J.Job.ExpectedSha1.IsEmpty())
			{
				bOk = FileSha1Matches(Staging, J.Job.ExpectedSha1);
			}
			if (!bOk)
			{
				if (J.VerifyAttempts < Cfg.MaxRetries)
				{
					++J.VerifyAttempts;
					UE_LOG(LogUnrealGamePatcherDownload, Warning,
						TEXT("FDownloadExecutor: '%s' verify failed (attempt %d) - re-downloading all segments"),
						*J.Job.Id, J.VerifyAttempts);
					JournalReset(J.Job.DestPath, J.EffectiveSegSize, J.Total);
					for (FSegment& S : J.Segments)
					{
						S.bDone = false;
						S.bInFlight = false;
						S.Attempts = 0;
					}
					J.SegmentsDone = 0;
					J.bPreallocated = false;
					PF.DeleteFile(*Staging);
					return; // Refill reissues every segment
				}
				Finish(J, false, TEXT("whole-file verification failed"));
				return;
			}

			if (PF.FileExists(*J.Job.DestPath))
			{
				PF.SetReadOnly(*J.Job.DestPath, false);
				PF.DeleteFile(*J.Job.DestPath);
			}
			if (!PF.MoveFile(*J.Job.DestPath, *Staging))
			{
				Finish(J, false, TEXT("cannot promote staged file"));
				return;
			}
			PF.DeleteFile(*JournalPathFor(J.Job.DestPath));
			Finish(J, true, FString());
		}

		void Finish(FJob& J, bool bSuccess, const FString& Error)
		{
			if (J.bFinished)
			{
				return;
			}
			J.bFinished = true;
			J.Result.bSuccess = bSuccess;
			J.Result.Error = Error;
			J.Result.BytesDownloaded = J.BytesDone;
			J.Result.Retries = J.Retries;
			if (!bSuccess)
			{
				IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
				PF.DeleteFile(*StagingPathFor(J.Job.DestPath));
				PF.DeleteFile(*JournalPathFor(J.Job.DestPath));
			}
			if (J.OnDone)
			{
				J.OnDone(J.Result);
			}
		}
	};
}

// Public API: Submit/Cancel run on the caller thread, settles on the engine
// HTTP thread - all through the one state, under the one lock.

UnrealGamePatcher::FDownloadExecutor::FDownloadExecutor(IChunkDownloader& InBackend, const FDownloadExecutorConfig& InConfig)
	: State(MakeShared<FExecutorState>(InBackend, InConfig))
{
}

UnrealGamePatcher::FDownloadExecutor::~FDownloadExecutor()
{
	// Best-effort abort of anything still in flight: cancel the transport
	// requests so their (guarded) settle callbacks drain promptly. The state
	// itself lives until the last callback releases its shared reference.
	FScopeLock Scope(&State->Lock);
	for (TUniquePtr<FJob>& JobPtr : State->Jobs)
	{
		FJob& J = *JobPtr;
		if (J.bFinished || J.bCanceled)
		{
			continue;
		}
		J.bCanceled = true;
		for (FSegment& S : J.Segments)
		{
			if (S.bInFlight)
			{
				State->Backend.CancelDownload(S.CancelHandle);
			}
		}
	}
}

int64 UnrealGamePatcher::FDownloadExecutor::Submit(FDownloadJob&& Job, TFunction<void(const FDownloadResult&)> OnDone)
{
	FScopeLock Scope(&State->Lock);
	return State->Submit(MoveTemp(Job), MoveTemp(OnDone));
}

void UnrealGamePatcher::FDownloadExecutor::Cancel(int64 Handle)
{
	FScopeLock Scope(&State->Lock);
	State->Cancel(Handle);
}

int64 UnrealGamePatcher::FDownloadExecutor::GetProgressTick()
{
	FScopeLock Scope(&State->Lock);
	return State->ProgressTick;
}
