#include "Engine.h"
#include "EquipManager.h"
#include "VRInputHandler.h"
#include "WeaponGeometry.h"
#include "ShieldCollision.h"
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
			_MESSAGE("[CastSpell] Cast spell %08X on player, result: %s", m_formId, result ? "success" : "failed");
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
			_MESSAGE("[CastSpell] Queued spell cast %08X on player", formId);
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
		_MESSAGE("[PlaySound] Played sound %08X at player", soundFormId);
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

	// Set the ownership of an object reference to the player
	// This prevents the item from being flagged as stolen when picked up
	void SetOwnerToPlayer(TESObjectREFR* objRef)
	{
		if (!objRef)
		{
			_MESSAGE("[SetOwner] ERROR: Object reference is null");
			return;
		}

		PlayerCharacter* player = *g_thePlayer;
		if (!player)
		{
			_MESSAGE("[SetOwner] ERROR: Player not available");
			return;
		}

		// Get or create the extra data list
		BaseExtraList* extraList = &objRef->extraData;
		
		// Check if ownership already exists
		if (extraList->HasType(kExtraData_Ownership))
		{
			// Get existing ownership and update it
			BSExtraData* existing = extraList->m_data;
			while (existing)
			{
				if (existing->GetType() == kExtraData_Ownership)
				{
					ExtraOwnership* ownership = static_cast<ExtraOwnership*>(existing);
					ownership->owner = player;
					_MESSAGE("[SetOwner] Updated existing ownership to player (RefID: %08X)", objRef->formID);
					return;
				}
				existing = existing->next;
			}
		}
		
		// Create new ExtraOwnership
		// We use BSExtraData::Create with the vtable for ExtraOwnership
		static RelocPtr<uintptr_t> s_ExtraOwnershipVtbl(0x015A32D0);
		ExtraOwnership* xOwnership = (ExtraOwnership*)BSExtraData::Create(sizeof(ExtraOwnership), s_ExtraOwnershipVtbl.GetUIntPtr());
		if (xOwnership)
		{
			xOwnership->owner = player;
			extraList->Add(kExtraData_Ownership, xOwnership);
			_MESSAGE("[SetOwner] Set ownership to player for RefID: %08X", objRef->formID);
		}
		else
		{
			_MESSAGE("[SetOwner] ERROR: Failed to create ExtraOwnership");
		}
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

		_MESSAGE("[DeleteWorldObject] Deleting world object RefID: %08X", objRef->formID);
		
		// Call the Papyrus Delete function
		DeleteObject_Native((*g_skyrimVM)->GetClassRegistry(), 0, objRef);
		
		_MESSAGE("[DeleteWorldObject] Delete command sent for RefID: %08X", objRef->formID);
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
			
			_MESSAGE("[ReequipCheck] After removal processed - Left equipped: %s, Right equipped: %s",
				leftStillHasWeapon ? "YES" : "NO", rightStillHasWeapon ? "YES" : "NO");
			
			::EquipManager* equipMan = ::EquipManager::GetSingleton();
			if (equipMan)
			{
				// Check LEFT hand
				if (m_leftHadWeapon && !leftStillHasWeapon)
				{
					_MESSAGE("[ReequipCheck] LEFT hand weapon was unequipped - re-equipping!");
					BGSEquipSlot* leftSlot = GetLeftHandSlot();
					FalseEdgeVR::EquipManager::s_suppressDrawSound = true;
					
					// Temporarily strip enchantment to prevent enchant VFX/sound
					TESObjectWEAP* weap = DYNAMIC_CAST(itemForm, TESForm, TESObjectWEAP);
					EnchantmentItem* cachedEnchant = nullptr;
					if (weap && weap->enchantable.enchantment)
					{
						cachedEnchant = weap->enchantable.enchantment;
						weap->enchantable.enchantment = nullptr;
					}

					CALL_MEMBER_FN(equipMan, EquipItem)(player, itemForm, nullptr, 1, leftSlot, false, true, false, nullptr);

					// Restore enchantment immediately
					if (weap && cachedEnchant)
					{
						weap->enchantable.enchantment = cachedEnchant;
					}
					
					FalseEdgeVR::EquipManager::s_suppressDrawSound = false;
					_MESSAGE("[ReequipCheck] Re-equipped weapon to LEFT hand (silent)");
				}
				
				// Check RIGHT hand
				if (m_rightHadWeapon && !rightStillHasWeapon)
				{
					_MESSAGE("[ReequipCheck] RIGHT hand weapon was unequipped - re-equipping!");
					BGSEquipSlot* rightSlot = GetRightHandSlot();
					FalseEdgeVR::EquipManager::s_suppressDrawSound = true;
					// Temporarily strip enchantment to prevent enchant VFX/sound
					TESObjectWEAP* weap2 = DYNAMIC_CAST(itemForm, TESForm, TESObjectWEAP);
					EnchantmentItem* cachedEnchant2 = nullptr;
					if (weap2 && weap2->enchantable.enchantment)
					{
						cachedEnchant2 = weap2->enchantable.enchantment;
						weap2->enchantable.enchantment = nullptr;
					}

					CALL_MEMBER_FN(equipMan, EquipItem)(player, itemForm, nullptr, 1, rightSlot, false, true, false, nullptr);

					// Restore enchantment immediately
					if (weap2 && cachedEnchant2)
					{
						weap2->enchantable.enchantment = cachedEnchant2;
					}
					FalseEdgeVR::EquipManager::s_suppressDrawSound = false;
					_MESSAGE("[ReequipCheck] Re-equipped weapon to RIGHT hand (silent)");
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
			
			_MESSAGE("[DelayedRemove] Before removal - Left equipped: %s, Right equipped: %s",
				leftHadWeapon ? "YES" : "NO", rightHadWeapon ? "YES" : "NO");

			// Get container changes to check inventory
			ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
				player->extraData.GetByType(kExtraData_ContainerChanges));
			
			if (!containerChanges || !containerChanges->data)
			{
				_MESSAGE("[DelayedRemove] No container changes data");
				return;
			}

			// Find the inventory entry for this item
			InventoryEntryData* entryData = containerChanges->data->FindItemEntry(itemForm);
			if (!entryData)
			{
				_MESSAGE("[DelayedRemove] Item %08X not found in inventory", m_itemFormId);
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
				_MESSAGE("[DelayedRemove] Removing 1x %08X from inventory (total: %d, equipped: %d)", 
					m_itemFormId, totalCount, equippedCount);
				RemoveItemFromInventory(player, itemForm, 1, true);
				
				// Schedule a follow-up task to check and re-equip after the game processes the removal
				// We need a small delay to let the unequip event fire
				extern SKSETaskInterface* g_task;
				if (g_task)
				{
					g_task->AddTask(new DelayedReequipCheckTask(m_itemFormId, leftHadWeapon, rightHadWeapon));
					_MESSAGE("[DelayedRemove] Scheduled re-equip check task");
				}
			}
			else
			{
				_MESSAGE("[DelayedRemove] NOT removing %08X - would remove equipped item (total: %d, equipped: %d)", 
					m_itemFormId, totalCount, equippedCount);
			}
		}

		virtual void Dispose() override
		{
			delete this;
		}
	};

	// Thread function to delay then queue the removal task
	static void DelayedRemoveItemThread(UInt32 itemFormId, int delayMs)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
		
		extern SKSETaskInterface* g_task;
		if (g_task)
		{
			g_task->AddTask(new DelayedRemoveItemTask(itemFormId));
			_MESSAGE("[DelayedRemove] Queued item removal after %dms delay for item %08X", delayMs, itemFormId);
		}
		else
		{
			_MESSAGE("[DelayedRemove] ERROR: g_task not available!");
		}
	}

	void DelayedRemoveItemFromInventory(UInt32 itemFormId, int delayMs)
	{
		if (itemFormId == 0)
		{
			_MESSAGE("[DelayedRemove] ERROR: Invalid itemFormId 0");
			return;
		}

		// Start a detached thread to handle the delay
		std::thread(DelayedRemoveItemThread, itemFormId, delayMs).detach();
		_MESSAGE("[DelayedRemove] Started delayed removal thread for item %08X (delay: %dms)", itemFormId, delayMs);
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
		_MESSAGE("[Blocking] Started blocking (X-Pose)");
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
		_MESSAGE("[Blocking] Stopped blocking (X-Pose ended)");
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
	// Weapon Scaling (HIGGS grabbed weapons)
	// ============================================
	
	// Track grabbed weapons for each VR controller hand
	static TESObjectREFR* g_grabbedWeaponLeft = nullptr;
	static TESObjectREFR* g_grabbedWeaponRight = nullptr;
	
	// Helper to check if a reference is still valid
	static bool IsRefrValid(TESObjectREFR* refr)
	{
		if (!refr)
			return false;
		
		// Check formType and formID to see if the reference is valid
		__try {
			return (refr->formType == kFormType_Reference && refr->formID != 0);
		}
		__except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}
	
	void ScaleWorldObject(TESObjectREFR* refr, float targetScale)
	{
		if (!refr)
			return;
			
		if (!IsRefrValid(refr))
			return;
		
		// Get the 3D root node for the reference
		NiNode* rootNode = refr->GetNiNode();
		if (!rootNode)
		{
			return;
		}
		
		// Set the scale on the root node's local transform
		rootNode->m_localTransform.scale = targetScale;
		
		// Also set world transform scale
		rootNode->m_worldTransform.scale = targetScale;
		
		// Recursively set scale on all child nodes as well
		for (UInt32 i = 0; i < rootNode->m_children.m_size; ++i)
		{
			NiAVObject* child = rootNode->m_children.m_data[i];
			if (child)
			{
				child->m_localTransform.scale = targetScale;
				child->m_worldTransform.scale = targetScale;
			}
		}
		
		// Force update of world transforms
		NiAVObject::ControllerUpdateContext ctx;
		ctx.flags = 0;
		ctx.delta = 0;
		rootNode->UpdateWorldData(&ctx);
	}
	
	void TrackGrabbedWeapon(TESObjectREFR* weaponRefr, bool isLeftVRController)
	{
		if (!weaponRefr)
			return;
			
		if (isLeftVRController)
		{
			g_grabbedWeaponLeft = weaponRefr;
			_MESSAGE("Engine: Tracking grabbed weapon in LEFT VR controller (RefID: %08X)", weaponRefr->formID);
		}
		else
		{
			g_grabbedWeaponRight = weaponRefr;
			_MESSAGE("Engine: Tracking grabbed weapon in RIGHT VR controller (RefID: %08X)", weaponRefr->formID);
		}
		
		// Apply initial scale if scaling is enabled
		if (weaponScalingEnabled && weaponGrabbedScale != 1.0f)
		{
			ScaleWorldObject(weaponRefr, weaponGrabbedScale);
			_MESSAGE("Engine: Applied initial scale %.2f to grabbed weapon", weaponGrabbedScale);
		}
	}
	
	void ClearGrabbedWeapon(bool isLeftVRController)
	{
		TESObjectREFR* weaponRefr = isLeftVRController ? g_grabbedWeaponLeft : g_grabbedWeaponRight;
		
		// Restore scale to 1.0 when dropped (if the reference is still valid)
		if (weaponRefr && IsRefrValid(weaponRefr))
		{
			ScaleWorldObject(weaponRefr, 1.0f);
			_MESSAGE("Engine: Restored scale to 1.0 for dropped weapon (RefID: %08X)", weaponRefr->formID);
		}
		
		if (isLeftVRController)
		{
			g_grabbedWeaponLeft = nullptr;
			_MESSAGE("Engine: Cleared grabbed weapon tracking for LEFT VR controller");
		}
		else
		{
			g_grabbedWeaponRight = nullptr;
			_MESSAGE("Engine: Cleared grabbed weapon tracking for RIGHT VR controller");
		}
	}
	
	void UpdateGrabbedWeaponScales()
	{
		// Skip if scaling is disabled
		if (!weaponScalingEnabled)
			return;
			
		// Validate and apply scale to grabbed weapons continuously
		// This ensures scale is maintained even if HIGGS or physics reset it
		
		if (g_grabbedWeaponLeft)
		{
			if (!IsRefrValid(g_grabbedWeaponLeft))
			{
				_MESSAGE("Engine: LEFT grabbed weapon reference is invalid, clearing");
				g_grabbedWeaponLeft = nullptr;
			}
			else
			{
				ScaleWorldObject(g_grabbedWeaponLeft, weaponGrabbedScale);
			}
		}
		
		if (g_grabbedWeaponRight)
		{
			if (!IsRefrValid(g_grabbedWeaponRight))
			{
				_MESSAGE("Engine: RIGHT grabbed weapon reference is invalid, clearing");
				g_grabbedWeaponRight = nullptr;
			}
			else
			{
				ScaleWorldObject(g_grabbedWeaponRight, weaponGrabbedScale);
			}
		}
	}
	
	TESObjectREFR* GetGrabbedWeapon(bool isLeftVRController)
	{
		return isLeftVRController ? g_grabbedWeaponLeft : g_grabbedWeaponRight;
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
			_MESSAGE("GetCollisionAvoidanceHandIsLeft: INI setting=%d, returning %s game hand, IsLeftHandedMode=%s",
				collisionAvoidanceHand, result ? "LEFT" : "RIGHT", IsLeftHandedMode() ? "YES" : "NO");
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
		_MESSAGE("==============================================");
		_MESSAGE("[LeftHandedMode] VR Controller Mode: %s", IsLeftHandedMode() ? "LEFT-HANDED" : "RIGHT-HANDED (default)");
		if (IsLeftHandedMode())
		{
			_MESSAGE("[LeftHandedMode] NOTE: In left-handed mode, VR controllers are inverted!");
			_MESSAGE("[LeftHandedMode]   Left VR controller  -> Right game hand");
			_MESSAGE("[LeftHandedMode]   Right VR controller -> Left game hand");
		}
		_MESSAGE("==============================================");
	}
}
