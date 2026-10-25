// Copyright Epic Games, Inc. All Rights Reserved.
// -cli selftest —— 哈希封装自测（哈希工具层）
// 验证点：XXH3-64 / FIoHash(BLAKE3-160) / FIoChunkId 的 hex 往返与已知向量；
//         严格 hex 解析负例（奇数长度、非 hex、错误长度 → 零值/无效哈希，
//         N-F1/F2/F3 修复钉——引擎解析器永不吃到非法输入）；
//         FileSHA1Hex 流式分块（>2GB 不截断）。

#include "Tests/TestFramework.h"

namespace UnrealGamePatcher::Tests
{
	int32 RunSelftest(const TCHAR* /*CmdLine*/)
	{

		return UnrealGamePatcher::FPatcherHashSelfTest::Run() ? ExitSuccess : ExitFailure;
	
	}

}
