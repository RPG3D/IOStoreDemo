// Copyright Epic Games, Inc. All Rights Reserved.

#include "Formats/Diff/FDiffAnalyzer.h"

namespace UnrealGamePatcher
{
	namespace
	{
		/** Union of both maps' ids, sorted (deterministic iteration order). */
		TArray<FString> UnionIds(
			const TMap<FString, FIndexFileEntry>& Local,
			const TMap<FString, FIndexFileEntry>& Remote)
		{
			TArray<FString> Ids;
			Local.GetKeys(Ids);
			for (const auto& Pair : Remote)
			{
				if (!Local.Contains(Pair.Key))
				{
					Ids.Add(Pair.Key);
				}
			}
			Ids.Sort();
			return Ids;
		}

		/** True when every remote pak entry exists locally with a matching
		 *  built-in SHA1 (the container's physical layout/order is irrelevant -
		 *  entries are the truth). */
		bool PaksLogicallyEqual(const FIndexFileEntry& Local, const FIndexFileEntry& Remote)
		{
			// Count equality first: the loop below only validates REMOTE
			// entries against the local set, so a pak entry REMOVED between
			// versions (local-only leftover) would otherwise classify the
			// container as Same and the stale asset would stay mounted
			// forever - no update, check or repair ever converged it.
			if (Local.PakEntries.Num() != Remote.PakEntries.Num())
			{
				return false;
			}
			TMap<FString, FString> LocalByPath;
			for (const FIndexPakEntry& E : Local.PakEntries)
			{
				LocalByPath.Add(E.Path, E.SHA1);
			}
			for (const FIndexPakEntry& R : Remote.PakEntries)
			{
				const FString* H = LocalByPath.Find(R.Path);
				if (!H || !H->Equals(R.SHA1, ESearchCase::IgnoreCase))
				{
					return false;
				}
			}
			return true;
		}

		/** True when both containers carry the same chunk multiset (order
		 *  agnostic - appends reorder nothing already present). The identity
		 *  is (chunk id, meta hash): engine chunk ids are PACKAGE-derived
		 *  (FPackageId::FromName) so a content edit inside an existing
		 *  package keeps its id while the meta hash (FIoHash of the raw
		 *  data) changes - id-only comparison would hide the edit. When
		 *  either side lacks meta hashes (manifests predating the field)
		 *  the comparison falls back to ids only. */
		bool UcasChunkSetsEqual(const FIndexFileEntry& Local, const FIndexFileEntry& Remote)
		{
			if (Local.IoStoreChunks.Num() != Remote.IoStoreChunks.Num())
			{
				return false;
			}
			bool bMeta = true;
			for (const FIndexIoStoreChunk& C : Local.IoStoreChunks)
			{
				if (C.MetaHash.IsEmpty()) { bMeta = false; break; }
			}
			if (bMeta)
			{
				for (const FIndexIoStoreChunk& C : Remote.IoStoreChunks)
				{
					if (C.MetaHash.IsEmpty()) { bMeta = false; break; }
				}
			}
			TArray<FString> L, R;
			for (const FIndexIoStoreChunk& C : Local.IoStoreChunks)
			{
				L.Add(bMeta ? C.Id + TEXT("|") + C.MetaHash : C.Id);
			}
			for (const FIndexIoStoreChunk& C : Remote.IoStoreChunks)
			{
				R.Add(bMeta ? C.Id + TEXT("|") + C.MetaHash : C.Id);
			}
			L.Sort(); R.Sort();
			for (int32 I = 0; I < L.Num(); ++I)
			{
				if (!L[I].Equals(R[I], ESearchCase::IgnoreCase))
				{
					return false;
				}
			}
			return true;
		}

		/** Generic diff over two maps of id -> (sha1, size). The equality
		 *  criterion is the content hash only; size is carried along for
		 *  diagnostics and for the apply phase (task 5.x). */
		template <typename TEntry>
		void DiffMap(
			const TMap<FString, TEntry>& Local,
			const TMap<FString, TEntry>& Remote,
			TArray<FDiffEntry>& OutDiff)
		{
			OutDiff.Reset();

			TArray<FString> Ids;
			{
				TArray<FString> LocalIds;
				Local.GetKeys(LocalIds);
				TArray<FString> RemoteIds;
				Remote.GetKeys(RemoteIds);
				Ids = MoveTemp(LocalIds);
				for (const FString& Id : RemoteIds)
				{
					if (!Local.Contains(Id))
					{
						Ids.Add(Id);
					}
				}
				Ids.Sort();
			}

			for (const FString& Id : Ids)
			{
				FDiffEntry E;
				E.Id = Id;

				const TEntry* L = Local.Find(Id);
				const TEntry* R = Remote.Find(Id);

				if (L && R)
				{
					E.LocalSha1 = L->SHA1;
					E.RemoteSha1 = R->SHA1;
					E.LocalSize = L->Size;
					E.RemoteSize = R->Size;
					E.Kind = (L->SHA1 == R->SHA1) ? EChangeKind::Same : EChangeKind::Changed;
				}
				else if (L)
				{
					E.LocalSha1 = L->SHA1;
					E.LocalSize = L->Size;
					E.Kind = EChangeKind::Removed;
				}
				else
				{
					E.RemoteSha1 = R->SHA1;
					E.RemoteSize = R->Size;
					E.Kind = EChangeKind::Added;
				}

				OutDiff.Add(E);
			}
		}
	}

	void FDiffAnalyzer::DiffFiles(
		const TMap<FString, FIndexFileEntry>& Local,
		const TMap<FString, FIndexFileEntry>& Remote,
		TArray<FDiffEntry>& OutDiff)
	{
		DiffMap(Local, Remote, OutDiff);
	}

	void FDiffAnalyzer::DiffFilesVirtualDisk(
		const TMap<FString, FIndexFileEntry>& Local,
		const TMap<FString, FIndexFileEntry>& Remote,
		TArray<FDiffEntry>& OutDiff)
	{
		OutDiff.Reset();
		const TArray<FString> Ids = UnionIds(Local, Remote);

		for (const FString& Id : Ids)
		{
			FDiffEntry E;
			E.Id = Id;

			const FIndexFileEntry* L = Local.Find(Id);
			const FIndexFileEntry* R = Remote.Find(Id);

			if (L && R)
			{
				E.LocalSha1 = L->SHA1;
				E.RemoteSha1 = R->SHA1;
				E.LocalSize = L->Size;
				E.RemoteSize = R->Size;

				if (Id.EndsWith(TEXT(".pak")) && L->PakEntries.Num() > 0 && R->PakEntries.Num() > 0)
				{
					// Pak truth = per-entry hashes, not the container hash.
					E.Kind = PaksLogicallyEqual(*L, *R) ? EChangeKind::Same : EChangeKind::Changed;
				}
				else if (Id.EndsWith(TEXT(".ucas")) && L->IoStoreChunks.Num() > 0 && R->IoStoreChunks.Num() > 0)
				{
					// Ucas truth = chunk-id multiset, not the container hash.
					E.Kind = UcasChunkSetsEqual(*L, *R) ? EChangeKind::Same : EChangeKind::Changed;
				}
				else
				{
					E.Kind = (L->SHA1 == R->SHA1) ? EChangeKind::Same : EChangeKind::Changed;
				}
				if (E.Kind == EChangeKind::Same)
				{
					E.LocalSha1 = E.RemoteSha1;
				}
			}
			else if (L)
			{
				E.LocalSha1 = L->SHA1;
				E.LocalSize = L->Size;
				E.Kind = EChangeKind::Removed;
			}
			else
			{
				E.RemoteSha1 = R->SHA1;
				E.RemoteSize = R->Size;
				E.Kind = EChangeKind::Added;
			}

			OutDiff.Add(E);
		}

		// A .utoc is container metadata with NO byte truth of its own: the
		// truth extractor emits it size-only (an in-place patch rewrites the
		// utoc against the LOCAL ucas layout, so its bytes legitimately differ
		// from the CDN copy - the main diff pass therefore always hands the
		// utoc a phantom Changed here). Its status is derived entirely from
		// the sibling ucas, with ONE exception that matters: a utoc that is
		// genuinely MISSING on disk (Added) must never be mirrored into Same -
		// the old mirror did exactly that and stranded the install on a
		// container that cannot mount (no update, check or repair ever
		// converged it; the committed sidecar kept claiming the utoc exists).
		// For that case the SIBLING ucas is forced to Changed so the pair is
		// routed through the in-place patcher / whole-file rebuild: the
		// patcher fails on the unreadable utoc and falls back to a CDN-layout
		// rebuild of both files, restoring a consistent pair. A broken-but-
		// present utoc surfaces through the sibling instead: verify mode's
		// ucas extraction reads the PAIR, so it classifies the ucas Changed.
		for (FDiffEntry& E : OutDiff)
		{
			if (!E.Id.EndsWith(TEXT(".utoc")))
			{
				continue;
			}
			const FString UcasId = E.Id.LeftChop(5) + TEXT(".ucas");
			for (FDiffEntry& U : OutDiff)
			{
				if (U.Id != UcasId)
				{
					continue;
				}
				if (U.Kind == EChangeKind::Removed)
				{
					// Stray utoc whose ucas is gone: delete it with the ucas.
					E.Kind = EChangeKind::Removed;
				}
				else if (U.Kind == EChangeKind::Changed)
				{
					// Pair is handled jointly (the ucas patch rewrites the
					// utoc). An Added utoc stays Added - it is fetched whole.
					if (E.Kind != EChangeKind::Added)
					{
						E.Kind = EChangeKind::Changed;
					}
				}
				else if (E.Kind == EChangeKind::Added)
				{
					// Sibling Same but the utoc is missing: repair the pair.
					U.Kind = EChangeKind::Changed;
				}
				else
				{
					// Phantom Changed (the extractor provides no utoc byte
					// truth on purpose) or genuine Same: the pair is intact -
					// mirror the sibling's Same.
					E.Kind = EChangeKind::Same;
					E.LocalSha1 = E.RemoteSha1;
				}
				break;
			}
		}
	}

	void FDiffAnalyzer::DiffChunks(
		const TMap<FString, FIndexChunkEntry>& Local,
		const TMap<FString, FIndexChunkEntry>& Remote,
		TArray<FDiffEntry>& OutDiff)
	{
		DiffMap(Local, Remote, OutDiff);
	}

	FDiffResult FDiffAnalyzer::Diff(
		const FIndexManifest& LocalTruth,
		const FIndexManifest& RemoteManifest)
	{
		FDiffResult Result;
		DiffMap(LocalTruth.Files, RemoteManifest.Files, Result.Files);
		DiffMap(LocalTruth.Chunks, RemoteManifest.Chunks, Result.Chunks);
		return Result;
	}
}
