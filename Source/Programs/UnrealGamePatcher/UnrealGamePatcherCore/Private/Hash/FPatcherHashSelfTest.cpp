// Copyright Epic Games, Inc. All Rights Reserved.

#include "Hash/FPatcherHashSelfTest.h"
#include "Hash/FPatcherHash.h"
#include "UnrealGamePatcherLog.h"

namespace UnrealGamePatcher
{
	namespace
	{
		bool Check(bool bCondition, const TCHAR* What)
		{
			if (bCondition)
			{
				UE_LOG(LogUnrealGamePatcher, Display, TEXT("[PASS] %s"), What);
			}
			else
			{
				UE_LOG(LogUnrealGamePatcher, Error, TEXT("[FAIL] %s"), What);
			}
			return bCondition;
		}

		bool CheckEq(const FString& Actual, const TCHAR* Expected, const TCHAR* What)
		{
			if (Actual.Equals(Expected, ESearchCase::IgnoreCase))
			{
				UE_LOG(LogUnrealGamePatcher, Display, TEXT("[PASS] %s (%s)"), What, *Actual);
				return true;
			}
			UE_LOG(LogUnrealGamePatcher, Error, TEXT("[FAIL] %s: got %s, expected %s"), What, *Actual, Expected);
			return false;
		}
	}

	bool FPatcherHashSelfTest::Run()
	{
		bool bAllPassed = true;

		// --- XXH3-64 reference vectors (official Python xxhash, seed 0) -------
		{
			const ANSICHAR Empty[] = "";
			bAllPassed &= CheckEq(
				FPatcherHash::XXH64ToHex(Empty, 0), TEXT("2d06800538d394c2"), TEXT("XXH64(\"\")"));

			const ANSICHAR A[] = "a";
			bAllPassed &= CheckEq(
				FPatcherHash::XXH64ToHex(A, 1), TEXT("e6c632b61e964e1f"), TEXT("XXH64(\"a\")"));

			const ANSICHAR Abc[] = "abc";
			bAllPassed &= CheckEq(
				FPatcherHash::XXH64ToHex(Abc, 3), TEXT("78af5f94892f3950"), TEXT("XXH64(\"abc\")"));

			const ANSICHAR AppName[] = "UnrealGamePatcher";
			bAllPassed &= CheckEq(
				FPatcherHash::XXH64ToHex(AppName, FCStringAnsi::Strlen(AppName)),
				TEXT("8e673d6b5bcb5011"), TEXT("XXH64(\"UnrealGamePatcher\")"));

			const ANSICHAR Fox[] = "The quick brown fox jumps over the lazy dog";
			bAllPassed &= CheckEq(
				FPatcherHash::XXH64ToHex(Fox, FCStringAnsi::Strlen(Fox)),
				TEXT("ce7d19a5418fb365"), TEXT("XXH64(fox)"));
		}

		// --- XXH3-64: deterministic + byte-order round trip -------------------
		{
			const ANSICHAR Data[] = "roundtrip";
			const FString Hex = FPatcherHash::XXH64ToHex(Data, 9);
			const FXxHash64 Parsed = FPatcherHash::XXH64FromHex(Hex);
			const FString ReEncoded = FPatcherHash::XXH64ToHex(Parsed);
			bAllPassed &= Check(Hex.Len() == 16, TEXT("XXH64 hex is 16 chars"));
			bAllPassed &= CheckEq(ReEncoded, *Hex, TEXT("XXH64 hex round trip"));
		}

		// --- FIoHash: BLAKE3-256 of empty input (leading 160 bits) ------------
		{
			const ANSICHAR Empty[] = "";
			const FIoHash Hash = FIoHash::HashBuffer(Empty, 0);
			const FString Hex = FPatcherHash::FIoHashToHex(Hash);
			// Standard BLAKE3-256 test vector for the empty input; FIoHash keeps
			// the leading 160 bits.
			bAllPassed &= CheckEq(Hex, TEXT("af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9"),
				TEXT("FIoHash(\"\") = BLAKE3-160 empty vector"));

			// Hex parse round trip.
			const FIoHash Parsed = FPatcherHash::FIoHashFromHex(Hex);
			bAllPassed &= Check(Parsed == Hash, TEXT("FIoHash hex round trip"));
		}

		// --- FIoChunkId: hex round trip ---------------------------------------
		{
			// ExternalFile / bulk-data types are rejected by engine
			// CreateIoChunkId (IoChunkId checkSlow asserts, Debug-only); the
			// hex round trip is type-agnostic.
			const FIoChunkId ChunkId = CreateIoChunkId(0x1234, 0, EIoChunkType::ExportBundleData);
			const FString Hex = FPatcherHash::FIoChunkIdToHex(ChunkId);
			const FIoChunkId Parsed = FPatcherHash::FIoChunkIdFromHex(Hex);
			bAllPassed &= Check(Parsed == ChunkId, TEXT("FIoChunkId hex round trip"));
		}

		return bAllPassed;
	}
}
