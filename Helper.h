#pragma once
#include "skse64/PapyrusSpell.h"
#include "skse64/PapyrusGame.h"
#include "skse64/PapyrusActor.h"
#include "skse64/PapyrusPotion.h"
#include "skse64/GameMenus.h"
#include "skse64_common/SafeWrite.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <xbyak/xbyak.h>
#include "skse64/NiExtraData.h"
#include "skse64_common/BranchTrampoline.h"
#include <skse64/GameRTTI.h>
#include <skse64/GameData.h>
#include <skse64/NiTypes.h>
#include <skse64/NiGeometry.h>
#include <skse64/GameExtraData.h>
#include <skse64/GameHandlers.h>

#include "skse64/NiExtraData.h"
#include <skse64/NiControllers.h>
#include "skse64/InternalTasks.h"

#include <deque>
#include <queue>
#include <array>
#include "skse64\GameVR.h"
#include <skse64/PapyrusEvents.h>

#include "config.h"

namespace FalseEdgeVR
{
	UInt32 GetFullFormIdMine(const char* espName, UInt32 baseFormId);
	void ShowErrorBoxAndTerminate(const char* errorString);
	void GameLoad();
	void PostLoadGame();

	// Remove item from inventory (silent = no pickup/drop sound)
	void RemoveItemFromInventory(TESObjectREFR* target, TESForm* item, SInt32 count, bool silent);
}