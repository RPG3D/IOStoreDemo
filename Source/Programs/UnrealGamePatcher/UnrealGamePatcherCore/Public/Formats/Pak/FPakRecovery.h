// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Formats/Pak/FPakReader.h"

namespace UnrealGamePatcher
{
	/**
	 * pak 恢复扫描器（WAL 式尾部修复，ROADMAP Phase 3 §5.2）。
	 *
	 * 写入序 "payload → flush → index → flush → footer(单扇区原子写) → flush"
	 * 保证任意断电时刻盘上存在至少一个完整自洽的版本。本扫描器从文件尾
	 * 向前找回"最后一个合法 footer"，并把尾部垃圾（半截 v2 追加）截掉：
	 *
	 *   - Clean：物理末尾的 footer 完整通过校验，什么都不用做；
	 *   - Truncated：找到更早的合法 footer，文件已截断到它的末尾——
	 *     对 FPakReader/引擎而言即"完整旧版本"；
	 *   - Unrecoverable：全盘找不到合法 footer（不是本工具写坏的容器，
	 *     或损坏过于严重），调用方必须走整文件重建兜底。
	 *
	 * 候选 footer 的校验 = magic + version + IndexOffset/Size 边界 +
	 * IndexHash（对 index blob 实算 SHA1），不能只匹配 magic——payload
	 * 字节型里可能出现 magic 串，全靠完整校验过滤假命中。
	 *
	 * 幂等：重复调用无害（已修复的容器第二次走 Clean 快路径）。
	 * 是 update / check / repair / compact 的公共前置。
	 */
	struct FPakRecovery
	{
		enum class EResult
		{
			Clean,
			Truncated,
			Unrecoverable,
		};

		struct FOutcome
		{
			EResult Result = EResult::Unrecoverable;
			// Truncated 时的新文件大小（= 幸存 footer 末尾）；其余为 -1。
			int64 TruncatedToBytes = -1;
			// 幸存版本的 footer（Clean/Truncated 时有效）。
			FPakInfo Info;
		};

		/** 扫描并在需要时截断尾部。失败（打不开文件等）按 Unrecoverable 返回。 */
		UNREALGAMEPATCHERCORE_API static FOutcome ScanAndRepair(const FString& PakFilePath);
	};
}
