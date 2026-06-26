#include "Engine.h"
#include "EquipManager.h"
#include "VRInputHandler.h"
#include "WeaponGeometry.h"
#include "skse64/GameObjects.h"
#include <skse64/PapyrusActor.cpp>
#include "skse64/GameRTTI.h"
#include "skse64/PapyrusVM.h"
#include "skse64/GameExtraData.h"
#include <thread>
#include <chrono>

namespace FalseEdgeVR
{
	SKSETrampolineInterface* g_trampolineInterface = nullptr;

	HiggsPluginAPI::IHiggsInterface001* higgsInterface;
	vrikPluginApi::IVrikInterface001* vrikInterface;

	SkyrimVRESLPluginAPI::ISkyrimVRESLInterface001* skyrimVRESLInterface;

	// ============================================
	// Spell Casting (from SpellWheelVR - Papyrus Spell.Cast)
	// ============================================
	typedef bool(*_CastSpell)(VMClassRegistry* registry, UInt32 stackId, SpellItem* spell, TESObjectREFR* akSource, TESObjectREFR* akTarget);
	RelocAddr<_CastSpell> CastSpell_Native(0x009BB6B0);

	// Task to cast spell on main game thread
	class CastSpellOnPlayerTask : public TaskDelegate
	{
	public:
		UInt32 m_formId;

		CastSpellOnPlayerTask(UInt32 formId) : m_formId(formId) {}

		virtual void Run() override
		{
			Actor* player = *g_thePlayer;
			if (!player)
			{
				_MESSAGE("[CastSpell] ERROR: Player not available");
				return;
			}

			TESForm* form = LookupFormByID(m_formId);
			if (!form)
			{
				_MESSAGE("[CastSpell] ERROR: Spell form %08X not found", m_formId);
				return;
			}

			SpellItem* spell = DYNAMIC_CAST(form, TESForm, SpellItem);
			if (!spell)
			{
				_MESSAGE("[CastSpell] ERROR: Form %08X is not a SpellItem", m_formId);
				return;
			}

			// Cast the spell on the player (source = player, target = player for self-cast spells)
			bool result = CastSpell_Native((*g_skyrimVM)->GetClassRegistry(), 0, spell, player, player);
		}

		virtual void Dispose() override
		{
			delete this;
		}
	};

	void CastSpellOnPlayer(UInt32 formId)
	{
		if (formId == 0)
		{
			_MESSAGE("[CastSpell] ERROR: Invalid formId 0");
			return;
		}

		extern SKSETaskInterface* g_task;
		if (g_task)
		{
			g_task->AddTask(new CastSpellOnPlayerTask(formId));
		}
		else
		{
			_MESSAGE("[CastSpell] ERROR: g_task not available!");
		}
	}

	// ============================================
	// Sound Playing
	// ============================================

	// PlaySoundEffect function signature - for playing TESSound records
	typedef void(*_PlaySoundEffect)(VMClassRegistry* VMinternal, UInt32 stackId, TESSound* sound, TESObjectREFR* source);
	static RelocAddr<_PlaySoundEffect> PlaySoundEffect(0x009EF150);

	// Play a sound at the player's location
	// soundFormId is the full FormID of the SOUN record
	void PlaySoundAtPlayer(UInt32 soundFormId)
	{
		PlayerCharacter* player = *g_thePlayer;
		if (!player)
		{
			_MESSAGE("[PlaySound] ERROR: Player not available");
			return;
		}

		// Look up the sound form (SOUN type)
		TESForm* form = LookupFormByID(soundFormId);
		if (!form)
		{
			_MESSAGE("[PlaySound] ERROR: Failed to find sound form %08X", soundFormId);
			return;
		}

		// Cast to TESSound (SOUN record)
		TESSound* sound = DYNAMIC_CAST(form, TESForm, TESSound);
		if (!sound)
		{
			_MESSAGE("[PlaySound] ERROR: Form %08X is not a TESSound (type=%d, expected=%d)",
				soundFormId, form->formType, kFormType_Sound);
			return;
		}

		// Play the sound using the Papyrus native function
		PlaySoundEffect((*g_skyrimVM)->GetClassRegistry(), 0, sound, player);
	}

	// Play a sound at any actor's location (NPC or player)
	// soundFormId is the full FormID of the SOUN record
	void PlaySoundAtActor(UInt32 soundFormId, Actor* actor)
	{
		if (!actor)
		{
			_MESSAGE("[PlaySound] ERROR: Actor not available");
			return;
		}

		// Look up the sound form (SOUN type)
		TESForm* form = LookupFormByID(soundFormId);
		if (!form)
		{
			_MESSAGE("[PlaySound] ERROR: Failed to find sound form %08X", soundFormId);
			return;
		}

		// Cast to TESSound (SOUN record)
		TESSound* sound = DYNAMIC_CAST(form, TESForm, TESSound);
		if (!sound)
		{
			_MESSAGE("[PlaySound] ERROR: Form %08X is not a TESSound (type=%d, expected=%d)",
				soundFormId, form->formType, kFormType_Sound);
			return;
		}

		// Play the sound using the Papyrus native function
		PlaySoundEffect((*g_skyrimVM)->GetClassRegistry(), 0, sound, actor);
	}

	// Assign player ownership on an extra-data list (inventory stack or world ref).
	static TESForm* GetPlayerOwnershipForm(PlayerCharacter* player)
	{
		if (!player)
			return nullptr;
		// ExtraOwnership uses the actor base (TESNPC), not the live reference.
		if (player->baseForm)
			return player->baseForm;
		return player;
	}

	void SetPlayerOwnership(BaseExtraList* extraList)
	{
		if (!extraList)
			return;

		PlayerCharacter* player = *g_thePlayer;
		TESForm* ownerForm = GetPlayerOwnershipForm(player);
		if (!ownerForm)
			return;

		static const RelocPtr<uintptr_t> s_ExtraOwnershipVtbl(0x015A32D0);

		ExtraOwnership* xOwnership = static_cast<ExtraOwnership*>(extraList->GetByType(kExtraData_Ownership));
		if (xOwnership)
		{
			xOwnership->owner = ownerForm;
			return;
		}

		xOwnership = (ExtraOwnership*)BSExtraData::Create(sizeof(ExtraOwnership), s_ExtraOwnershipVtbl.GetUIntPtr());
		if (!xOwnership)
			return;

		// BaseExtraList::Add requires m_presence; freshly spawned refs may not be ready yet.
		struct ExtraListProbe
		{
			void* m_data;
			void* m_presence;
		};
		if (!static_cast<ExtraListProbe*>(static_cast<void*>(extraList))->m_presence)
			return;

		xOwnership->owner = ownerForm;
		extraList->Add(kExtraData_Ownership, xOwnership);
	}

	void ClearItemOwnership(BaseExtraList* extraList)
	{
		if (!extraList || !extraList->HasType(kExtraData_Ownership))
			return;

		BSExtraData* xOwnership = extraList->GetByType(kExtraData_Ownership);
		if (xOwnership)
			extraList->Remove(kExtraData_Ownership, xOwnership);
	}

	void EnsurePlayerOwnsWeaponInInventory(PlayerCharacter* player, TESForm* weaponForm)
	{
		if (!player || !weaponForm)
			return;

		ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
			player->extraData.GetByType(kExtraData_ContainerChanges));
		if (!containerChanges || !containerChanges->data)
			return;

		InventoryEntryData* entryData = containerChanges->data->FindItemEntry(weaponForm);
		if (!entryData || !entryData->extendDataList)
			return;

		for (ExtendDataList::Iterator it = entryData->extendDataList->Begin(); !it.End(); ++it)
		{
			BaseExtraList* extraList = it.Get();
			if (extraList)
				SetPlayerOwnership(extraList);
		}

		// If any stack still reads as not player-owned, strip ownership (matches console setownership).
		if (!CALL_MEMBER_FN(entryData, IsOwnedBy)(player, true))
		{
			for (ExtendDataList::Iterator it = entryData->extendDataList->Begin(); !it.End(); ++it)
			{
				BaseExtraList* extraList = it.Get();
				if (extraList)
					ClearItemOwnership(extraList);
			}
		}
	}

	void SetOwnerToPlayer(TESObjectREFR* objRef)
	{
		if (!objRef)
			return;

		SetPlayerOwnership(&objRef->extraData);
	}

	// ============================================
	// Delete World Object (cleanup spawned items)
	// ============================================
	
	// Papyrus ObjectReference.Delete function signature
	typedef void (*_DeleteObject)(VMClassRegistry* registry, UInt32 stackId, TESObjectREFR* obj);
	static RelocAddr<_DeleteObject> DeleteObject_Native(0x009CE380);

	void DeleteWorldObject(TESObjectREFR* objRef)
	{
		if (!objRef)
		{
			_MESSAGE("[DeleteWorldObject] ERROR: Object reference is null");
			return;
		}

		
		// Call the Papyrus Delete function
		DeleteObject_Native((*g_skyrimVM)->GetClassRegistry(), 0, objRef);
		
	}

	// ============================================
	// Delayed Item Removal (cleanup spawned weapon duplicates)
	// ============================================
	
	// Task to check and re-equip weapons after removal has been processed
	// Must be defined BEFORE DelayedRemoveItemTask since it uses this class
	class DelayedReequipCheckTask : public TaskDelegate
	{
	public:
		UInt32 m_itemFormId;
		bool m_leftHadWeapon;
		bool m_rightHadWeapon;

		DelayedReequipCheckTask(UInt32 itemFormId, bool leftHad, bool rightHad) 
			: m_itemFormId(itemFormId), m_leftHadWeapon(leftHad), m_rightHadWeapon(rightHad) {}

		virtual void Run() override
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player)
			{
				_MESSAGE("[ReequipCheck] ERROR: Player not available");
				return;
			}

			TESForm* itemForm = LookupFormByID(m_itemFormId);
			if (!itemForm)
			{
				_MESSAGE("[ReequipCheck] ERROR: Item form %08X not found", m_itemFormId);
				return;
			}

			// Check what's equipped NOW (after the removal was processed)
			TESForm* leftEquippedAfter = player->GetEquippedObject(true);
			TESForm* rightEquippedAfter = player->GetEquippedObject(false);
			
			bool leftStillHasWeapon = (leftEquippedAfter && leftEquippedAfter->formID == m_itemFormId);
			bool rightStillHasWeapon = (rightEquippedAfter && rightEquippedAfter->formID == m_itemFormId);
			
			
			::EquipManager* equipMan = ::EquipManager::GetSingleton();
			if (equipMan)
			{
				// Check LEFT hand
				if (m_leftHadWeapon && !leftStillHasWeapon)
				{
					FalseEdgeVR::EquipManager::s_suppressDrawSound = true;
					
					// Temporarily strip enchantment to prevent enchant VFX/sound
					TESObjectWEAP* weap = DYNAMIC_CAST(itemForm, TESForm, TESObjectWEAP);
					EnchantmentItem* cachedEnchant = nullptr;
					if (weap && weap->enchantable.enchantment)
					{
						cachedEnchant = weap->enchantable.enchantment;
						weap->enchantable.enchantment = nullptr;
					}

					FalseEdgeVR::EquipManager::GetSingleton()->EquipWeaponToGameHand(player, itemForm, true);

					// Restore enchantment immediately
					if (weap && cachedEnchant)
					{
						weap->enchantable.enchantment = cachedEnchant;
					}
					
					FalseEdgeVR::EquipManager::s_suppressDrawSound = false;
				}
				
				// Check RIGHT hand
				if (m_rightHadWeapon && !rightStillHasWeapon)
				{
					FalseEdgeVR::EquipManager::s_suppressDrawSound = true;
					// Temporarily strip enchantment to prevent enchant VFX/sound
					TESObjectWEAP* weap2 = DYNAMIC_CAST(itemForm, TESForm, TESObjectWEAP);
					EnchantmentItem* cachedEnchant2 = nullptr;
					if (weap2 && weap2->enchantable.enchantment)
					{
						cachedEnchant2 = weap2->enchantable.enchantment;
						weap2->enchantable.enchantment = nullptr;
					}

					FalseEdgeVR::EquipManager::GetSingleton()->EquipWeaponToGameHand(player, itemForm, false);

					// Restore enchantment immediately
					if (weap2 && cachedEnchant2)
					{
						weap2->enchantable.enchantment = cachedEnchant2;
					}
					FalseEdgeVR::EquipManager::s_suppressDrawSound = false;
				}
			}
		}

		virtual void Dispose() override
		{
			delete this;
		}
	};

	// Task to remove item from player inventory on game thread
	class DelayedRemoveItemTask : public TaskDelegate
	{
	public:
		UInt32 m_itemFormId;

		DelayedRemoveItemTask(UInt32 itemFormId) : m_itemFormId(itemFormId) {}

		virtual void Run() override
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player)
			{
				_MESSAGE("[DelayedRemove] ERROR: Player not available");
				return;
			}

			TESForm* itemForm = LookupFormByID(m_itemFormId);
			if (!itemForm)
			{
				_MESSAGE("[DelayedRemove] ERROR: Item form %08X not found", m_itemFormId);
				return;
			}

			// Check what the player currently has equipped BEFORE removal
			TESForm* leftEquippedBefore = player->GetEquippedObject(true);
			TESForm* rightEquippedBefore = player->GetEquippedObject(false);
			
			bool leftHadWeapon = (leftEquippedBefore && leftEquippedBefore->formID == m_itemFormId);
			bool rightHadWeapon = (rightEquippedBefore && rightEquippedBefore->formID == m_itemFormId);
			

			// Get container changes to check inventory
			ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
				player->extraData.GetByType(kExtraData_ContainerChanges));
			
			if (!containerChanges || !containerChanges->data)
			{
				return;
			}

			// Find the inventory entry for this item
			InventoryEntryData* entryData = containerChanges->data->FindItemEntry(itemForm);
			if (!entryData)
			{
				return;
			}

			// Count how many are equipped (could be 0, 1, or 2 if dual-wielding same weapon)
			int equippedCount = 0;
			if (leftHadWeapon) equippedCount++;
			if (rightHadWeapon) equippedCount++;

			// Get total count in inventory
			int totalCount = entryData->countDelta;
			
			// Only remove if we have MORE than what's equipped
			if (totalCount > equippedCount)
			{
				RemoveItemFromInventory(player, itemForm, 1, true);
				
				// Schedule a follow-up task to check and re-equip after the game processes the removal
				// We need a small delay to let the unequip event fire
				extern SKSETaskInterface* g_task;
				if (g_task)
				{
					g_task->AddTask(new DelayedReequipCheckTask(m_itemFormId, leftHadWeapon, rightHadWeapon));
				}
			}
			else
			{
			}
		}

		virtual void Dispose() override
		{
			delete this;
		}
	};

	// Task that counts down frames on the game thread before running the removal.
	// Replaces the old detached-thread + sleep approach, which could race a
	// save/load (removal lost if the game saved during the sleep window) and ran
	// off the game thread.
	class FrameDelayedRemoveItemTask : public TaskDelegate
	{
	public:
		UInt32 m_itemFormId;
		int m_framesLeft;

		FrameDelayedRemoveItemTask(UInt32 itemFormId, int framesLeft)
			: m_itemFormId(itemFormId), m_framesLeft(framesLeft) {}

		virtual void Run() override
		{
			extern SKSETaskInterface* g_task;
			if (m_framesLeft > 0 && g_task)
			{
				// Re-queue for next frame
				g_task->AddTask(new FrameDelayedRemoveItemTask(m_itemFormId, m_framesLeft - 1));
				return;
			}

			// Countdown finished - run the removal logic now (on the game thread)
			DelayedRemoveItemTask removal(m_itemFormId);
			removal.Run();
		}

		virtual void Dispose() override
		{
			delete this;
		}
	};

	void DelayedRemoveItemFromInventory(UInt32 itemFormId, int delayMs)
	{
		if (itemFormId == 0)
		{
			_MESSAGE("[DelayedRemove] ERROR: Invalid itemFormId 0");
			return;
		}

		// Convert ms to frames (~90fps = ~11ms per frame)
		int frames = delayMs / 11;
		if (frames < 1)
			frames = 1;

		extern SKSETaskInterface* g_task;
		if (g_task)
		{
			g_task->AddTask(new FrameDelayedRemoveItemTask(itemFormId, frames));
		}
		else
		{
			_MESSAGE("[DelayedRemove] ERROR: g_task not available!");
		}
	}

	// ============================================
	// Blocking (X-Pose) - from dual_wield_block_vr
	// ============================================
	
	// Animation graph function typedefs
	typedef bool(*_IAnimationGraphManagerHolder_NotifyAnimationGraph)(IAnimationGraphManagerHolder* _this, const BSFixedString& a_eventName);
	typedef bool(*_IAnimationGraphManagerHolder_GetAnimationVariableBool)(IAnimationGraphManagerHolder* _this, const BSFixedString& a_variableName, bool& a_out);
	
	inline UInt64* get_vtbl(void* object) { return *((UInt64**)object); }
	
	template<typename T>
	inline T get_vfunc(void* object, UInt64 index) {
		UInt64* vtbl = get_vtbl(object);
		return (T)(vtbl[index]);
	}
	
	void StartBlocking()
	{
		Actor* player = *g_thePlayer;
		if (!player)
		{
			_MESSAGE("[Blocking] ERROR: Player not available");
			return;
		}
		
		static BSFixedString s_blockStart("blockStart");
		get_vfunc<_IAnimationGraphManagerHolder_NotifyAnimationGraph>(&player->animGraphHolder, 0x1)(&player->animGraphHolder, s_blockStart);
	}
	
	void StopBlocking()
	{
		Actor* player = *g_thePlayer;
		if (!player)
		{
			_MESSAGE("[Blocking] ERROR: Player not available");
			return;
		}
		
		static BSFixedString s_blockStop("blockStop");
		get_vfunc<_IAnimationGraphManagerHolder_NotifyAnimationGraph>(&player->animGraphHolder, 0x1)(&player->animGraphHolder, s_blockStop);
	}
	
	bool IsBlocking()
	{
		Actor* player = *g_thePlayer;
		if (!player)
			return false;
		
		static BSFixedString s_IsBlocking("IsBlocking");
		bool isBlocking = false;
		get_vfunc<_IAnimationGraphManagerHolder_GetAnimationVariableBool>(&player->animGraphHolder, 0x12)(&player->animGraphHolder, s_IsBlocking, isBlocking);
		return isBlocking;
	}

	// ============================================
	// Left-Handed Mode Support
	// ============================================
	
	// Local RelocPtr for left-handed mode - the address 0x01E71778 is from GameInput.cpp
	static RelocPtr<bool> s_leftHandedMode(0x01E71778);
	
	bool IsLeftHandedMode()
	{
		// Access the left-handed mode flag from the game
		return *s_leftHandedMode;
	}
	
	bool VRControllerToGameHand(bool isLeftVRController)
	{
		// In left-handed mode, VR controllers are inverted:
		//   - Left VR controller = Right game hand (returns false)
		//   - Right VR controller = Left game hand (returns true)
		// In right-handed (default) mode:
		//   - Left VR controller = Left game hand (returns true)
		//   - Right VR controller = Right game hand (returns false)
		
		if (IsLeftHandedMode())
		{
			return !isLeftVRController;  // Invert
		}
		else
		{
			return isLeftVRController;   // No change
		}
	}
	
	bool GameHandToVRController(bool isLeftGameHand)
	{
		// Reverse of VRControllerToGameHand
		// In left-handed mode:
		//   - Left game hand = Right VR controller (returns false)
		//   - Right game hand = Left VR controller (returns true)
		// In right-handed (default) mode:
		//   - Left game hand = Left VR controller (returns true)
		//   - Right game hand = Right VR controller (returns false)
		
		if (IsLeftHandedMode())
		{
			return !isLeftGameHand;  // Invert
		}
		else
		{
			return isLeftGameHand;   // No change
		}
	}

	bool IsPlayerMounted(Actor* actor)
	{
		PlayerCharacter* player = actor ? DYNAMIC_CAST(actor, Actor, PlayerCharacter) : *g_thePlayer;
		if (!player)
			return false;

		NiPointer<Actor> mountActor;
		return CALL_MEMBER_FN(player, GetMount)(mountActor) && mountActor;
	}

	bool GetCollisionAvoidanceHandIsLeft()
	{
		// Returns which game hand should be unequipped during dual-wield collision
		// collisionAvoidanceHand: 0 = left hand, 1 = right hand
		// 
		// The INI setting refers to which GAME hand gets unequipped.
		// This is independent of left-handed mode - the setting directly controls
		// which game hand (left or right) does the unequip/grab during collision.
		bool result = (collisionAvoidanceHand == 0);
		
		static bool loggedOnce = false;
		if (!loggedOnce)
		{
			loggedOnce = true;
		}
		
		return result;
	}

	void StartMod()
	{
		// Note: Most initialization now happens in main.cpp's InitializeVRSystems()
		// which is called after HIGGS interface is available (PostPostLoad)
		
		// This function is called during DataLoaded, before HIGGS is ready
		// Only do non-HIGGS dependent initialization here
		
		LOG("StartMod: FalseEdgeVR starting...");
		
		// Log initial left-handed mode
		if (IsLeftHandedMode())
		{
		}
	}
}
