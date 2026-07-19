#include "SpellWheelTwoHandLog.h"

#include "config.h"
#include "Engine.h"
#include "spellwheelinterface001.h"

#include "skse64/GameData.h"
#include "skse64/GameEvents.h"
#include "skse64/GameExtraData.h"
#include "skse64/GameObjects.h"
#include "skse64/GameReferences.h"
#include "skse64/GameRTTI.h"
#include "skse64/GameForms.h"
#include "skse64/GameVR.h"
#include "skse64/NiNodes.h"
#include "skse64/PapyrusVM.h"
#include "skse64/gamethreads.h"

#include <atomic>
#include <chrono>
#include <unordered_map>

namespace BarebonesVR
{
	extern SKSETaskInterface* g_task;

	namespace
	{
		struct VanillaTwoHandFormState
		{
			BGSEquipSlot* equipSlot = nullptr;
			UInt8 animType = 0;
		};

		struct SwappedWeaponState
		{
			BGSEquipSlot* originalEquipSlot = nullptr;
			UInt8 originalAnimType = 0;
			bool animTypeSwapped = false;
		};

		spellwheelPluginApi::ISpellWheelInterface001* s_spellwheelInterface = nullptr;

		std::unordered_map<UInt32, VanillaTwoHandFormState> s_vanillaTwoHandForms;
		std::unordered_map<UInt32, SwappedWeaponState> s_swappedWeapons;

		bool s_wasMainWheelOpen = false;
		bool s_wasSecondaryWheelOpen = false;
		bool s_hasSpellwheelActivity = false;
		std::chrono::steady_clock::time_point s_spellwheelActivityTime{};

		enum class SpellwheelHandIntent
		{
			None,
			SecondaryWheelOffHand,
			MainWheelCrossHand,
			MainWheelMainHand
		};

		SpellwheelHandIntent s_spellwheelHandIntent = SpellwheelHandIntent::None;
		std::chrono::steady_clock::time_point s_spellwheelHandIntentTime{};
		bool s_mainWheelMainHandSelectionPending = false;

		bool s_internalEquipInProgress = false;

		constexpr int kSpellwheelActivityWindowMs = 5000;
		constexpr int kHandIntentWindowMs = 2500;

		bool s_offhandTriggerWasDown = false;
		bool s_manualOffhandConcentrationCasting = false;
		bool s_manualOffhandFireForgetPrimed = false;
		bool s_manualOffhandIsLeftGameHand = true;
		std::chrono::steady_clock::time_point s_lastManualOffhandMagickaDrainTime{};

		typedef void(*_SetAnimationVariableBool)(VMClassRegistry* registry, UInt64 stackID, TESObjectREFR* refr, BSFixedString* variableName, bool value);
		RelocAddr<_SetAnimationVariableBool> sub_SetAnimationVariableBool(0x9D1AF0);

		constexpr UInt32 kMagickaActorValue = 25;

		bool IsTwoHandedMeleeWeapon(TESForm* form)
		{
			if (!form || form->formType != kFormType_Weapon)
			{
				return false;
			}

			TESObjectWEAP* weapon = DYNAMIC_CAST(form, TESForm, TESObjectWEAP);
			if (!weapon)
			{
				return false;
			}

			switch (weapon->gameData.type)
			{
			case TESObjectWEAP::GameData::kType_TwoHandSword:
			case TESObjectWEAP::GameData::kType_2HS:
			case TESObjectWEAP::GameData::kType_TwoHandAxe:
			case TESObjectWEAP::GameData::kType_2HA:
				return true;
			default:
				return false;
			}
		}

		UInt8 GetVanillaTwoHandAnimType(TESObjectWEAP* weapon);

		bool SharesTwoHandAnimFamily(UInt8 a, UInt8 b)
		{
			const bool aSword = a == TESObjectWEAP::GameData::kType_TwoHandSword || a == TESObjectWEAP::GameData::kType_2HS;
			const bool aAxe = a == TESObjectWEAP::GameData::kType_TwoHandAxe || a == TESObjectWEAP::GameData::kType_2HA;
			const bool bSword = b == TESObjectWEAP::GameData::kType_TwoHandSword || b == TESObjectWEAP::GameData::kType_2HS;
			const bool bAxe = b == TESObjectWEAP::GameData::kType_TwoHandAxe || b == TESObjectWEAP::GameData::kType_2HA;
			return (aSword && bSword) || (aAxe && bAxe);
		}

		TESObjectWEAP* FindSubstituteTwoHandWeapon(TESObjectWEAP* conflictWeapon)
		{
			if (!conflictWeapon)
			{
				return nullptr;
			}

			DataHandler* dataHandler = *g_dataHandler;
			if (!dataHandler)
			{
				return nullptr;
			}

			const UInt8 conflictAnim = GetVanillaTwoHandAnimType(conflictWeapon);
			const UInt16 conflictDamage = conflictWeapon->damage.GetAttackDamage();

			TESObjectWEAP* best = nullptr;
			UInt16 bestDamage = 0;

			for (UInt32 i = 0; i < dataHandler->weapons.count; ++i)
			{
				TESObjectWEAP* candidate = nullptr;
				if (!dataHandler->weapons.GetNthItem(i, candidate) || !candidate)
				{
					continue;
				}

				if (candidate->formID == conflictWeapon->formID)
				{
					continue;
				}

				if (!IsTwoHandedMeleeWeapon(candidate))
				{
					continue;
				}

				const UInt8 candAnim = GetVanillaTwoHandAnimType(candidate);
				if (!SharesTwoHandAnimFamily(conflictAnim, candAnim))
				{
					continue;
				}

				const UInt16 candDamage = candidate->damage.GetAttackDamage();
				if (candDamage <= conflictDamage)
				{
					continue;
				}

				if (!best || candDamage > bestDamage)
				{
					best = candidate;
					bestDamage = candDamage;
				}
			}

			return best;
		}

		void ForceNpcEquipSubstituteAndSuppressConflict(Actor* actor, TESObjectWEAP* conflictWeapon, TESObjectWEAP* substituteWeapon)
		{
			if (!actor || !conflictWeapon || !substituteWeapon)
			{
				return;
			}

			EquipManager* equipMan = EquipManager::GetSingleton();
			if (!equipMan || !(*g_skyrimVM))
			{
				return;
			}

			VMClassRegistry* registry = (*g_skyrimVM)->GetClassRegistry();
			if (!registry)
			{
				return;
			}

			// Add substitute to NPC silently (Papyrus native helper).
			AddItem_Native(registry, 0, actor, substituteWeapon, 1, true);

			// Equip substitute (let engine decide hands for NPC).
			s_internalEquipInProgress = true;
			CALL_MEMBER_FN(equipMan, EquipItem)(actor, substituteWeapon, nullptr, 1, nullptr, false, true, false, nullptr);

			// Try to unequip the conflicting weapon if it is currently worn.
			ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
				actor->extraData.GetByType(kExtraData_ContainerChanges));
			if (containerChanges && containerChanges->data)
			{
				InventoryEntryData* entryData = containerChanges->data->FindItemEntry(conflictWeapon);
				if (entryData)
				{
					BaseExtraList* rightList = nullptr;
					BaseExtraList* leftList = nullptr;
					entryData->GetExtraWornBaseLists(&rightList, &leftList);

					if (rightList)
					{
						CALL_MEMBER_FN(equipMan, UnequipItem)(actor, conflictWeapon, rightList, 1, GetRightHandSlot(), false, false, false, false, nullptr);
					}
					if (leftList)
					{
						CALL_MEMBER_FN(equipMan, UnequipItem)(actor, conflictWeapon, leftList, 1, GetLeftHandSlot(), false, false, false, false, nullptr);
					}
				}
			}

			s_internalEquipInProgress = false;

			_MESSAGE(
				"BarebonesVR: NPC 0x%08X conflict swap: equipped substitute 0x%08X (dmg=%u) and suppressed conflict 0x%08X",
				actor->formID,
				substituteWeapon->formID,
				substituteWeapon->damage.GetAttackDamage(),
				conflictWeapon->formID);
		}

		bool GetWornState(Actor* actor, TESForm* weaponForm, bool& outWornRight, bool& outWornLeft)
		{
			outWornRight = false;
			outWornLeft = false;

			ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
				actor->extraData.GetByType(kExtraData_ContainerChanges));
			if (!containerChanges || !containerChanges->data)
			{
				return false;
			}

			InventoryEntryData* entryData = containerChanges->data->FindItemEntry(weaponForm);
			if (!entryData)
			{
				return false;
			}

			BaseExtraList* rightList = nullptr;
			BaseExtraList* leftList = nullptr;
			entryData->GetExtraWornBaseLists(&rightList, &leftList);

			outWornRight = rightList != nullptr;
			outWornLeft = leftList != nullptr;
			return true;
		}

		bool IsLeftHandedMode()
		{
			return leftHandedMode != 0;
		}

		bool IsMainHandLeftGameHand()
		{
			return IsLeftHandedMode();
		}

		bool IsOffHandLeftGameHand()
		{
			return !IsLeftHandedMode();
		}

		bool VRControllerToGameHand(bool isLeftVRController)
		{
			if (IsLeftHandedMode())
			{
				return !isLeftVRController;
			}

			return isLeftVRController;
		}

		bool IsOffHandVRController(bool isLeftVRController)
		{
			return VRControllerToGameHand(isLeftVRController) != IsLeftHandedMode();
		}

		bool GetControllerTrigger(bool isLeftVRController, bool& outTrigger)
		{
			outTrigger = false;

			BSOpenVR* openVR = *g_openVR;
			if (!openVR || !openVR->vrSystem)
			{
				return false;
			}

			vr_1_0_12::ETrackedControllerRole role = isLeftVRController
				? vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_LeftHand
				: vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_RightHand;

			vr_1_0_12::TrackedDeviceIndex_t device = openVR->vrSystem->GetTrackedDeviceIndexForControllerRole(role);
			if (device == vr_1_0_12::k_unTrackedDeviceIndexInvalid)
			{
				return false;
			}

			vr_1_0_12::VRControllerState_t state{};
			if (!openVR->vrSystem->GetControllerState(device, &state, sizeof(state)))
			{
				return false;
			}

			const uint64_t triggerMask = 1ull << vr_1_0_12::k_EButton_SteamVR_Trigger;
			outTrigger = (state.ulButtonPressed & triggerMask) != 0 || state.rAxis[1].x > 0.5f;
			return true;
		}

		UInt8 OneHandedAnimTypeFor(UInt8 twoHandedType)
		{
			switch (twoHandedType)
			{
			case TESObjectWEAP::GameData::kType_TwoHandSword:
			case TESObjectWEAP::GameData::kType_2HS:
				return TESObjectWEAP::GameData::kType_OneHandSword;
			case TESObjectWEAP::GameData::kType_TwoHandAxe:
			case TESObjectWEAP::GameData::kType_2HA:
				return TESObjectWEAP::GameData::kType_OneHandAxe;
			default:
				return twoHandedType;
			}
		}

		void RecordVanillaTwoHandFormIfNeeded(TESObjectWEAP* weapon)
		{
			if (!weapon || s_vanillaTwoHandForms.count(weapon->formID) != 0)
			{
				return;
			}

			s_vanillaTwoHandForms[weapon->formID] = { weapon->equipType.GetEquipSlot(), weapon->gameData.type };
		}

		UInt8 GetVanillaTwoHandAnimType(TESObjectWEAP* weapon)
		{
			if (!weapon)
			{
				return 0;
			}

			const auto it = s_vanillaTwoHandForms.find(weapon->formID);
			if (it != s_vanillaTwoHandForms.end())
			{
				return it->second.animType;
			}

			return weapon->gameData.type;
		}

		BGSEquipSlot* GetVanillaTwoHandEquipSlot(TESObjectWEAP* weapon)
		{
			if (!weapon)
			{
				return nullptr;
			}

			const auto it = s_vanillaTwoHandForms.find(weapon->formID);
			if (it != s_vanillaTwoHandForms.end())
			{
				return it->second.equipSlot;
			}

			return weapon->equipType.GetEquipSlot();
		}

		void EnsureEitherHandEquipSlot(TESObjectWEAP* weapon)
		{
			BGSEquipSlot* eitherHand = GetEitherHandSlot();
			if (weapon && eitherHand && weapon->equipType.GetEquipSlot() != eitherHand)
			{
				weapon->equipType.SetEquipSlot(eitherHand);
			}
		}

		bool IsFormEquippedInGameHand(PlayerCharacter* player, UInt32 formId, bool isLeftGameHand)
		{
			if (!player || !formId)
			{
				return false;
			}

			TESForm* equipped = player->GetEquippedObject(isLeftGameHand);
			return equipped && equipped->formID == formId;
		}

		bool ShouldSkipOffHandAnimSwapForSharedForm(PlayerCharacter* player, UInt32 weaponFormId)
		{
			return player && IsFormEquippedInGameHand(player, weaponFormId, IsLeftHandedMode());
		}

		void ApplyOffHandWeaponAnimSwap(TESObjectWEAP* weapon, SwappedWeaponState& state, PlayerCharacter* player)
		{
			if (!weapon)
			{
				return;
			}

			if (ShouldSkipOffHandAnimSwapForSharedForm(player, weapon->formID))
			{
				weapon->gameData.type = state.originalAnimType;
				state.animTypeSwapped = false;
				return;
			}

			const UInt8 oneHandType = OneHandedAnimTypeFor(state.originalAnimType);
			if (oneHandType == state.originalAnimType)
			{
				return;
			}

			weapon->gameData.type = oneHandType;
			state.animTypeSwapped = true;
		}

		void ApplyMainHandWeaponAnimRestore(TESObjectWEAP* weapon, SwappedWeaponState& state)
		{
			if (!weapon)
			{
				return;
			}

			if (weapon->gameData.type != state.originalAnimType)
			{
				weapon->gameData.type = state.originalAnimType;
			}

			state.animTypeSwapped = false;
		}

		void RestorePlayerTwoHandWeaponForm(TESObjectWEAP* weapon, const SwappedWeaponState& state)
		{
			if (!weapon)
			{
				return;
			}

			EnsureEitherHandEquipSlot(weapon);
			weapon->gameData.type = state.originalAnimType;
		}

		void RestoreVanillaTwoHandWeaponForm(TESObjectWEAP* weapon)
		{
			if (!weapon)
			{
				return;
			}

			BGSEquipSlot* vanillaSlot = GetVanillaTwoHandEquipSlot(weapon);
			if (vanillaSlot)
			{
				weapon->equipType.SetEquipSlot(vanillaSlot);
			}

			weapon->gameData.type = GetVanillaTwoHandAnimType(weapon);
		}

		bool IsWeaponFormPatched(TESObjectWEAP* weapon)
		{
			if (!weapon)
			{
				return false;
			}

			if (s_swappedWeapons.count(weapon->formID) != 0)
			{
				return true;
			}

			const auto vanilla = s_vanillaTwoHandForms.find(weapon->formID);
			if (vanilla == s_vanillaTwoHandForms.end())
			{
				return false;
			}

			BGSEquipSlot* eitherHand = GetEitherHandSlot();
			if (eitherHand && weapon->equipType.GetEquipSlot() == eitherHand)
			{
				return true;
			}

			return weapon->gameData.type != vanilla->second.animType;
		}

		bool IsKnownTwoHandWeaponForm(UInt32 formId)
		{
			return s_vanillaTwoHandForms.count(formId) != 0;
		}

		void ClearSpellwheelHandIntent();

		void RestoreUnequippedTwoHandWeapon(TESObjectWEAP* weapon)
		{
			if (!weapon || !IsKnownTwoHandWeaponForm(weapon->formID) || !IsWeaponFormPatched(weapon))
			{
				return;
			}

			RestoreVanillaTwoHandWeaponForm(weapon);
			s_swappedWeapons.erase(weapon->formID);
			ClearSpellwheelHandIntent();

			_MESSAGE(
				"BarebonesVR: Restored vanilla records for unequipped 2H weapon 0x%08X (animType=%u, eitherHand=0)",
				weapon->formID,
				weapon->gameData.type);
		}

		void PruneUnwornSwappedWeapons(PlayerCharacter* player)
		{
			if (!player)
			{
				return;
			}

			for (auto it = s_swappedWeapons.begin(); it != s_swappedWeapons.end(); )
			{
				TESForm* form = LookupFormByID(it->first);
				TESObjectWEAP* weapon = form ? DYNAMIC_CAST(form, TESForm, TESObjectWEAP) : nullptr;

				bool wornRight = false;
				bool wornLeft = false;
				const bool stillWorn = form && GetWornState(player, form, wornRight, wornLeft) && (wornRight || wornLeft);
				if (!stillWorn)
				{
					if (weapon)
					{
						RestoreVanillaTwoHandWeaponForm(weapon);
						ClearSpellwheelHandIntent();
						_MESSAGE(
							"BarebonesVR: Restored vanilla records for unequipped 2H weapon 0x%08X (prune, animType=%u, eitherHand=0)",
							weapon->formID,
							weapon->gameData.type);
					}

					it = s_swappedWeapons.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		void RefreshWeaponAnimForEquippedHand(PlayerCharacter* player, bool isLeftGameHand)
		{
			if (!player)
			{
				return;
			}

			TESForm* equippedForm = player->GetEquippedObject(isLeftGameHand);
			if (!equippedForm || !IsKnownTwoHandWeaponForm(equippedForm->formID))
			{
				return;
			}

			const auto it = s_swappedWeapons.find(equippedForm->formID);
			if (it == s_swappedWeapons.end())
			{
				return;
			}

			TESObjectWEAP* weapon = DYNAMIC_CAST(equippedForm, TESForm, TESObjectWEAP);
			if (!weapon)
			{
				return;
			}

			weapon->equipType.SetEquipSlot(GetEitherHandSlot());

			const bool mainHandIsLeftGameHand = IsLeftHandedMode();
			const bool inOffHand = (isLeftGameHand != mainHandIsLeftGameHand);
			if (inOffHand)
			{
				ApplyOffHandWeaponAnimSwap(weapon, it->second, player);
			}
			else
			{
				ApplyMainHandWeaponAnimRestore(weapon, it->second);
			}
		}

		bool IsTrackedPlayerTwoHand(TESForm* form)
		{
			return form && s_swappedWeapons.count(form->formID) != 0;
		}

		bool GameHandToVRController(bool isLeftGameHand)
		{
			if (IsLeftHandedMode())
			{
				return !isLeftGameHand;
			}

			return isLeftGameHand;
		}

		bool PlayerHasSameWeaponEquippedOrGrabbed(PlayerCharacter* player, UInt32 weaponFormId)
		{
			if (!player || !weaponFormId)
			{
				return false;
			}

			const bool mainHandIsLeftGameHand = IsLeftHandedMode();
			const bool offHandIsLeftGameHand = !mainHandIsLeftGameHand;

			TESForm* mainHandItem = player->GetEquippedObject(mainHandIsLeftGameHand);
			if (mainHandItem && mainHandItem->formID == weaponFormId)
			{
				return true;
			}

			TESForm* offHandItem = player->GetEquippedObject(offHandIsLeftGameHand);
			if (offHandItem && offHandItem->formID == weaponFormId)
			{
				return true;
			}

			if (higgsInterface)
			{
				const bool mainHandIsLeftVRController = GameHandToVRController(mainHandIsLeftGameHand);
				const bool offHandIsLeftVRController = GameHandToVRController(offHandIsLeftGameHand);

				TESObjectREFR* mainGrabbed = higgsInterface->GetGrabbedObject(mainHandIsLeftVRController);
				if (mainGrabbed && mainGrabbed->baseForm && mainGrabbed->baseForm->formID == weaponFormId)
				{
					return true;
				}

				TESObjectREFR* offGrabbed = higgsInterface->GetGrabbedObject(offHandIsLeftVRController);
				if (offGrabbed && offGrabbed->baseForm && offGrabbed->baseForm->formID == weaponFormId)
				{
					return true;
				}
			}

			return false;
		}

		void UpdateDualWieldWeaponCollision(bool forceRebuild = false)
		{
			if (!higgsInterface || !g_thePlayer || !*g_thePlayer)
			{
				return;
			}

			PlayerCharacter* player = *g_thePlayer;
			const bool mainHandIsLeftGameHand = IsLeftHandedMode();
			const bool offHandIsLeftGameHand = !mainHandIsLeftGameHand;

			auto ensureCollisionForGameHand = [&](bool isLeftGameHand)
			{
				const bool isLeftVRController = GameHandToVRController(isLeftGameHand);
				if (forceRebuild)
				{
					higgsInterface->DisableWeaponCollision(isLeftVRController);
				}
				if (higgsInterface->IsWeaponCollisionDisabled(isLeftVRController))
				{
					higgsInterface->EnableWeaponCollision(isLeftVRController);
				}
				higgsInterface->ForceWeaponCollisionEnabled(isLeftVRController);
			};

			TESForm* mainHandItem = player->GetEquippedObject(mainHandIsLeftGameHand);
			TESForm* offHandItem = player->GetEquippedObject(offHandIsLeftGameHand);

			const bool mainIsTracked2H = IsTrackedPlayerTwoHand(mainHandItem);
			const bool offIsTracked2H = IsTrackedPlayerTwoHand(offHandItem);

			if (mainIsTracked2H)
			{
				ensureCollisionForGameHand(mainHandIsLeftGameHand);
			}
			if (offIsTracked2H)
			{
				ensureCollisionForGameHand(offHandIsLeftGameHand);
			}

			if (mainIsTracked2H && !offIsTracked2H && offHandItem && offHandItem->formType == kFormType_Weapon)
			{
				ensureCollisionForGameHand(offHandIsLeftGameHand);
			}
			if (offIsTracked2H && !mainIsTracked2H && mainHandItem && mainHandItem->formType == kFormType_Weapon)
			{
				ensureCollisionForGameHand(mainHandIsLeftGameHand);
			}
		}

		void RefreshPlayerTwoHandWeapons()
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player)
			{
				return;
			}

			PruneUnwornSwappedWeapons(player);

			const bool mainHandIsLeftGameHand = IsLeftHandedMode();
			RefreshWeaponAnimForEquippedHand(player, mainHandIsLeftGameHand);
			RefreshWeaponAnimForEquippedHand(player, !mainHandIsLeftGameHand);
			UpdateDualWieldWeaponCollision(true);
		}

		void SetManualOffhandCastingAnim(PlayerCharacter* player, bool isLeftGameHand, bool isCasting)
		{
			if (!player)
			{
				return;
			}

			TESObjectREFR* playerRef = DYNAMIC_CAST(player, Actor, TESObjectREFR);
			VMClassRegistry* registry = (*g_skyrimVM) ? (*g_skyrimVM)->GetClassRegistry() : nullptr;
			if (playerRef && registry)
			{
				BSFixedString castingVar(isLeftGameHand ? "IsCastingLeft" : "IsCastingRight");
				sub_SetAnimationVariableBool(registry, 0, playerRef, &castingVar, isCasting);
			}
		}

		void StopManualOffhandConcentrationCast(PlayerCharacter* player, ActorMagicCaster* caster)
		{
			if (!player || !caster)
			{
				return;
			}

			if (s_manualOffhandConcentrationCasting)
			{
				CALL_MEMBER_FN(caster, InterruptCast)();
			}

			if (s_manualOffhandConcentrationCasting || s_manualOffhandFireForgetPrimed)
			{
				SetManualOffhandCastingAnim(player, s_manualOffhandIsLeftGameHand, false);
			}

			s_manualOffhandConcentrationCasting = false;
			s_manualOffhandFireForgetPrimed = false;
		}

		bool CastImmediateOffhandSpell(ActorMagicCaster* spellCaster, SpellItem* spell, PlayerCharacter* player, float alchStrength, bool hostileEffectivenessOnly)
		{
			if (!spellCaster || !spell || !player)
			{
				return false;
			}

			return spellCaster->CastSpellImmediate(
				spell,
				false,
				(spell->data.delivery == SpellItem::Delivery::kSelf) ? player : nullptr,
				alchStrength,
				hostileEffectivenessOnly,
				0.0f,
				player);
		}

		float GetManualOffhandSpellMagickaCost(SpellItem* spell, PlayerCharacter* player)
		{
			if (!spell || !player)
			{
				return 0.0f;
			}

			MagicItem* magicItem = spell;
			float cost = CALL_MEMBER_FN(magicItem, GetEffectiveMagickaCost)(player);
			return (cost > 0.0f) ? cost : 0.0f;
		}

		void ConsumeManualOffhandMagicka(PlayerCharacter* player, float cost)
		{
			if (!player || cost <= 0.0f)
			{
				return;
			}

			player->actorValueOwner.RestoreActorValue(Actor::kDamage, kMagickaActorValue, -cost);
		}

		bool HasManualOffhandMagicka(PlayerCharacter* player, float cost)
		{
			if (!player || cost <= 0.0f)
			{
				return true;
			}

			return player->actorValueOwner.GetCurrent(kMagickaActorValue) >= cost;
		}

		void UpdateManualOffhandCasting()
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player)
			{
				return;
			}

			const bool mainHandIsLeftGameHand = IsLeftHandedMode();
			const bool offHandIsLeftGameHand = !mainHandIsLeftGameHand;

			TESForm* mainHandObject = player->GetEquippedObject(mainHandIsLeftGameHand);
			if (!IsTrackedPlayerTwoHand(mainHandObject))
			{
				ActorMagicCaster* offCaster = player->magicCasters[offHandIsLeftGameHand ? Actor::SlotTypes::kLeftHand : Actor::SlotTypes::kRightHand];
				StopManualOffhandConcentrationCast(player, offCaster);
				s_offhandTriggerWasDown = false;
				return;
			}

			SpellItem* offSpell = offHandIsLeftGameHand ? player->leftHandSpell : player->rightHandSpell;
			ActorMagicCaster* spellCaster = player->magicCasters[offHandIsLeftGameHand ? Actor::SlotTypes::kLeftHand : Actor::SlotTypes::kRightHand];
			if (!offSpell || !spellCaster)
			{
				StopManualOffhandConcentrationCast(player, spellCaster);
				s_offhandTriggerWasDown = false;
				return;
			}

			bool triggerDown = false;
			GetControllerTrigger(GameHandToVRController(offHandIsLeftGameHand), triggerDown);

			const bool pressedEdge = triggerDown && !s_offhandTriggerWasDown;
			const bool releasedEdge = !triggerDown && s_offhandTriggerWasDown;

			if (pressedEdge)
			{
				const bool concentration = offSpell->data.castingType == SpellItem::CastingType::kConcentration;
				const float spellCost = GetManualOffhandSpellMagickaCost(offSpell, player);
				const float currentMagicka = player->actorValueOwner.GetCurrent(kMagickaActorValue);
				const bool hasMagicka = concentration ? (currentMagicka > 0.0f) : HasManualOffhandMagicka(player, spellCost);

				float alchStrength = 1.0f;
				CannotCastReason castReason = CannotCastReason::kOK;
				const bool canCast = spellCaster->CheckCast(offSpell, false, &alchStrength, &castReason, false);

				if (hasMagicka && canCast)
				{
					bool castResult = false;
					if (concentration)
					{
						castResult = CastImmediateOffhandSpell(spellCaster, offSpell, player, alchStrength, true);
					}
					else
					{
						s_manualOffhandIsLeftGameHand = offHandIsLeftGameHand;
						SetManualOffhandCastingAnim(player, offHandIsLeftGameHand, true);
						s_manualOffhandFireForgetPrimed = true;
						castResult = true;
					}

					if (castResult && concentration)
					{
						s_manualOffhandIsLeftGameHand = offHandIsLeftGameHand;
						SetManualOffhandCastingAnim(player, offHandIsLeftGameHand, true);
						s_manualOffhandConcentrationCasting = true;
						s_lastManualOffhandMagickaDrainTime = std::chrono::steady_clock::now();
					}
				}
			}
			else if (releasedEdge)
			{
				if (s_manualOffhandFireForgetPrimed)
				{
					const float spellCost = GetManualOffhandSpellMagickaCost(offSpell, player);
					if (!HasManualOffhandMagicka(player, spellCost))
					{
						CALL_MEMBER_FN(spellCaster, InterruptCast)();
					}
					else
					{
						float alchStrength = 1.0f;
						CannotCastReason castReason = CannotCastReason::kOK;
						if (spellCaster->CheckCast(offSpell, false, &alchStrength, &castReason, false))
						{
							const bool castResult = CastImmediateOffhandSpell(spellCaster, offSpell, player, alchStrength, false);
							if (castResult)
							{
								ConsumeManualOffhandMagicka(player, spellCost);
							}
							else
							{
								CALL_MEMBER_FN(spellCaster, InterruptCast)();
							}
						}
					}
				}
				StopManualOffhandConcentrationCast(player, spellCaster);
			}

			if (s_manualOffhandConcentrationCasting && triggerDown)
			{
				const auto now = std::chrono::steady_clock::now();
				const float costPerSec = GetManualOffhandSpellMagickaCost(offSpell, player);
				if (costPerSec > 0.0f)
				{
					const float dtSec = std::chrono::duration_cast<std::chrono::duration<float>>(
						now - s_lastManualOffhandMagickaDrainTime).count();
					s_lastManualOffhandMagickaDrainTime = now;

					const float drain = costPerSec * (dtSec > 0.0f ? dtSec : 0.0f);
					const float currentMagicka = player->actorValueOwner.GetCurrent(kMagickaActorValue);

					if (currentMagicka <= 0.0f || drain >= currentMagicka)
					{
						if (currentMagicka > 0.0f)
						{
							player->actorValueOwner.RestoreActorValue(Actor::kDamage, kMagickaActorValue, -currentMagicka);
						}
						StopManualOffhandConcentrationCast(player, spellCaster);
					}
					else if (drain > 0.0f)
					{
						player->actorValueOwner.RestoreActorValue(Actor::kDamage, kMagickaActorValue, -drain);
					}
				}
			}

			s_offhandTriggerWasDown = triggerDown;
		}

		std::atomic_bool s_manualOffhandCastingTaskQueued{ false };

		class ManualOffhandCastingTask : public TaskDelegate
		{
		public:
			virtual void Run() override
			{
				// Run on the game's task thread (safer than HIGGS pre-physics callback thread).
				UpdateManualOffhandCasting();
				s_manualOffhandCastingTaskQueued.store(false, std::memory_order_release);
			}

			virtual void Dispose() override
			{
				delete this;
			}
		};

		bool IsSpellwheelOpenNow()
		{
			if (!s_spellwheelInterface)
			{
				return false;
			}

			return s_spellwheelInterface->IsMainWheelOpen() || s_spellwheelInterface->IsSecondaryWheelOpen();
		}

		void MarkSpellwheelActivity()
		{
			s_hasSpellwheelActivity = true;
			s_spellwheelActivityTime = std::chrono::steady_clock::now();
		}

		bool HasRecentSpellwheelActivity()
		{
			if (IsSpellwheelOpenNow())
			{
				return true;
			}

			if (!s_hasSpellwheelActivity)
			{
				return false;
			}

			const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - s_spellwheelActivityTime).count();

			if (ageMs > kSpellwheelActivityWindowMs)
			{
				s_hasSpellwheelActivity = false;
				return false;
			}

			return true;
		}

		void MarkSpellwheelHandIntent(SpellwheelHandIntent intent)
		{
			s_spellwheelHandIntent = intent;
			s_spellwheelHandIntentTime = std::chrono::steady_clock::now();
		}

		void ClearSpellwheelHandIntent()
		{
			s_spellwheelHandIntent = SpellwheelHandIntent::None;
			s_mainWheelMainHandSelectionPending = false;
		}

		void ClearOffHandSpellwheelIntent()
		{
			ClearSpellwheelHandIntent();
		}

		bool IsOffHandSpellwheelIntent(SpellwheelHandIntent intent)
		{
			return intent == SpellwheelHandIntent::SecondaryWheelOffHand ||
				intent == SpellwheelHandIntent::MainWheelCrossHand;
		}

		bool IsMainHandSpellwheelIntent(SpellwheelHandIntent intent)
		{
			return intent == SpellwheelHandIntent::MainWheelMainHand;
		}

		bool HasSpellwheelHandIntent(bool (*intentMatcher)(SpellwheelHandIntent))
		{
			if (!intentMatcher(s_spellwheelHandIntent))
			{
				return false;
			}

			if (s_spellwheelInterface)
			{
				if (s_spellwheelInterface->IsSecondaryWheelOpen())
				{
					return IsOffHandSpellwheelIntent(s_spellwheelHandIntent);
				}

				if (s_spellwheelInterface->IsMainWheelOpen())
				{
					bool leftTrigger = false;
					bool rightTrigger = false;
					GetControllerTrigger(true, leftTrigger);
					GetControllerTrigger(false, rightTrigger);

					const bool offHandTrigger =
						(leftTrigger && IsOffHandVRController(true)) ||
						(rightTrigger && IsOffHandVRController(false));
					const bool mainHandTrigger =
						(leftTrigger && !IsOffHandVRController(true)) ||
						(rightTrigger && !IsOffHandVRController(false));

					if (IsOffHandSpellwheelIntent(s_spellwheelHandIntent) && offHandTrigger)
					{
						return true;
					}

					if (IsMainHandSpellwheelIntent(s_spellwheelHandIntent) && mainHandTrigger)
					{
						return true;
					}

					if (offHandTrigger || mainHandTrigger)
					{
						return false;
					}
				}
			}

			if (!HasRecentSpellwheelActivity())
			{
				ClearSpellwheelHandIntent();
				return false;
			}

			const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - s_spellwheelHandIntentTime).count();

			if (ageMs > kHandIntentWindowMs)
			{
				ClearSpellwheelHandIntent();
				return false;
			}

			return true;
		}

		bool HasOffHandSpellwheelIntent()
		{
			return HasSpellwheelHandIntent(IsOffHandSpellwheelIntent);
		}

		bool HasMainHandSpellwheelIntent()
		{
			return HasSpellwheelHandIntent(IsMainHandSpellwheelIntent);
		}

		void ConsumeOffHandSpellwheelIntent()
		{
			if (IsOffHandSpellwheelIntent(s_spellwheelHandIntent))
			{
				ClearSpellwheelHandIntent();
			}
		}

		void ConsumeMainHandSpellwheelIntent()
		{
			if (IsMainHandSpellwheelIntent(s_spellwheelHandIntent))
			{
				s_mainWheelMainHandSelectionPending = false;
				ClearSpellwheelHandIntent();
			}
		}

		void RecordVanillaTwoHandEquipSlots()
		{
			DataHandler* dataHandler = *g_dataHandler;
			if (!dataHandler)
			{
				return;
			}

			for (UInt32 i = 0; i < dataHandler->weapons.count; ++i)
			{
				TESObjectWEAP* weapon = nullptr;
				if (!dataHandler->weapons.GetNthItem(i, weapon) || !weapon)
				{
					continue;
				}

				if (!IsTwoHandedMeleeWeapon(weapon))
				{
					continue;
				}

				RecordVanillaTwoHandFormIfNeeded(weapon);
			}
		}

		class ApplyTwoHandEitherHandSwapTask : public TaskDelegate
		{
		public:
			UInt32 m_actorFormId;
			UInt32 m_weaponFormId;
			bool m_toLeftGameHand;
			bool m_targetIsMainHand;

			ApplyTwoHandEitherHandSwapTask(UInt32 actorFormId, UInt32 weaponFormId, bool toLeftGameHand, bool targetIsMainHand)
				: m_actorFormId(actorFormId)
				, m_weaponFormId(weaponFormId)
				, m_toLeftGameHand(toLeftGameHand)
				, m_targetIsMainHand(targetIsMainHand)
			{
			}

			bool IsInTargetHand(Actor* actor, TESForm* weaponForm)
			{
				bool wornRight = false;
				bool wornLeft = false;
				GetWornState(actor, weaponForm, wornRight, wornLeft);
				return m_toLeftGameHand ? (wornLeft && !wornRight) : (wornRight && !wornLeft);
			}

			void PatchWeaponForTargetHand(TESObjectWEAP* weapon, SwappedWeaponState& swapState, PlayerCharacter* player, bool targetIsOffHand)
			{
				weapon->equipType.SetEquipSlot(GetEitherHandSlot());
				if (targetIsOffHand)
				{
					ApplyOffHandWeaponAnimSwap(weapon, swapState, player);
				}
				else
				{
					ApplyMainHandWeaponAnimRestore(weapon, swapState);
				}
			}

			virtual void Run() override
			{
				TESForm* actorForm = LookupFormByID(m_actorFormId);
				Actor* actor = actorForm ? DYNAMIC_CAST(actorForm, TESForm, Actor) : nullptr;
				if (!actor)
				{
					return;
				}

				TESForm* weaponForm = LookupFormByID(m_weaponFormId);
				TESObjectWEAP* weapon = weaponForm ? DYNAMIC_CAST(weaponForm, TESForm, TESObjectWEAP) : nullptr;
				if (!weapon)
				{
					return;
				}

				bool wornRight = false;
				bool wornLeft = false;
				if (!GetWornState(actor, weaponForm, wornRight, wornLeft) || (!wornRight && !wornLeft))
				{
					return;
				}

				EquipManager* equipMan = EquipManager::GetSingleton();
				if (!equipMan)
				{
					return;
				}

				RecordVanillaTwoHandFormIfNeeded(weapon);

				SwappedWeaponState swapState;
				swapState.originalEquipSlot = GetVanillaTwoHandEquipSlot(weapon);
				swapState.originalAnimType = GetVanillaTwoHandAnimType(weapon);
				swapState.animTypeSwapped = false;

				const bool targetIsOffHand = !m_targetIsMainHand;
				PlayerCharacter* player = DYNAMIC_CAST(actor, Actor, PlayerCharacter);

				s_internalEquipInProgress = true;

				if (IsInTargetHand(actor, weaponForm) && targetIsOffHand)
				{
					PatchWeaponForTargetHand(weapon, swapState, player, targetIsOffHand);
					s_swappedWeapons[m_weaponFormId] = swapState;
					_MESSAGE(
						"BarebonesVR: Applied EitherHand swap to 2H weapon 0x%08X in %s hand (animType=%u, eitherHand=1)",
						m_weaponFormId,
						m_targetIsMainHand ? "main" : "off",
						weapon->gameData.type);
					s_internalEquipInProgress = false;
					RefreshPlayerTwoHandWeapons();
					return;
				}

				ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
					actor->extraData.GetByType(kExtraData_ContainerChanges));
				if (!containerChanges || !containerChanges->data)
				{
					s_internalEquipInProgress = false;
					return;
				}

				InventoryEntryData* entryData = containerChanges->data->FindItemEntry(weaponForm);
				if (!entryData)
				{
					s_internalEquipInProgress = false;
					return;
				}

				BaseExtraList* rightList = nullptr;
				BaseExtraList* leftList = nullptr;
				entryData->GetExtraWornBaseLists(&rightList, &leftList);

				if (wornRight)
				{
					CALL_MEMBER_FN(equipMan, UnequipItem)(actor, weaponForm, rightList, 1, GetRightHandSlot(), false, false, false, false, nullptr);
				}
				else
				{
					CALL_MEMBER_FN(equipMan, UnequipItem)(actor, weaponForm, leftList, 1, GetLeftHandSlot(), false, false, false, false, nullptr);
				}

				weapon->equipType.SetEquipSlot(GetEitherHandSlot());
				if (targetIsOffHand)
				{
					ApplyOffHandWeaponAnimSwap(weapon, swapState, player);
				}

				BGSEquipSlot* targetSlot = m_toLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
				CALL_MEMBER_FN(equipMan, EquipItem)(actor, weaponForm, nullptr, 1, targetSlot, false, true, false, nullptr);

				if (IsInTargetHand(actor, weaponForm))
				{
					if (!targetIsOffHand)
					{
						ApplyMainHandWeaponAnimRestore(weapon, swapState);
					}

					s_swappedWeapons[m_weaponFormId] = swapState;
					_MESSAGE(
						"BarebonesVR: Re-equipped 2H weapon 0x%08X to %s hand (animType=%u, eitherHand=1)",
						m_weaponFormId,
						m_targetIsMainHand ? "main" : "off",
						weapon->gameData.type);
				}
				else
				{
					RestorePlayerTwoHandWeaponForm(weapon, swapState);
					s_swappedWeapons.erase(m_weaponFormId);
					_ERROR(
						"BarebonesVR: Failed to re-equip 2H weapon 0x%08X to %s hand.",
						m_weaponFormId,
						m_targetIsMainHand ? "main" : "off");
				}

				s_internalEquipInProgress = false;
				RefreshPlayerTwoHandWeapons();
			}

			virtual void Dispose() override
			{
				delete this;
			}
		};

		void ScheduleTwoHandEitherHandSwap(PlayerCharacter* player, TESForm* weaponForm, bool toLeftGameHand, bool targetIsMainHand)
		{
			if (!player || !weaponForm || !g_task)
			{
				return;
			}

			g_task->AddTask(new ApplyTwoHandEitherHandSwapTask(
				player->formID,
				weaponForm->formID,
				toLeftGameHand,
				targetIsMainHand));
		}

		void ScheduleMainHandTwoHandRebake(PlayerCharacter* player)
		{
			if (!player)
			{
				return;
			}

			const bool mainHandIsLeftGameHand = IsLeftHandedMode();
			TESForm* mainHandObject = player->GetEquippedObject(mainHandIsLeftGameHand);
			if (!IsTrackedPlayerTwoHand(mainHandObject))
			{
				return;
			}

			ScheduleTwoHandEitherHandSwap(player, mainHandObject, mainHandIsLeftGameHand, true);
		}

		void RouteTwoHandEquipToHand(
			PlayerCharacter* player,
			TESForm* item,
			bool targetLeftGameHand,
			bool targetIsMainHand,
			bool weaponInMainHand,
			bool weaponInOffHand,
			const char* sourceLabel)
		{
			const bool landedMainOnly = weaponInMainHand && !weaponInOffHand;
			const bool landedOffOnly = weaponInOffHand && !weaponInMainHand;

			if (targetIsMainHand && landedOffOnly)
			{
				_MESSAGE(
					"BarebonesVR: %s 2H equip for weapon 0x%08X; engine placed it in off hand instead - re-equipping to main hand",
					sourceLabel,
					item->formID);
			}
			else if (!targetIsMainHand && landedMainOnly)
			{
				_MESSAGE(
					"BarebonesVR: %s 2H equip for weapon 0x%08X; engine placed it in main hand instead - re-equipping to off hand",
					sourceLabel,
					item->formID);
			}
			else if (targetIsMainHand && landedMainOnly)
			{
				_MESSAGE(
					"BarebonesVR: %s 2H equip for weapon 0x%08X; landed in main hand",
					sourceLabel,
					item->formID);
			}
			else if (!targetIsMainHand && landedOffOnly)
			{
				_MESSAGE(
					"BarebonesVR: %s 2H equip for weapon 0x%08X; landed in off hand",
					sourceLabel,
					item->formID);
			}
			else
			{
				_MESSAGE(
					"BarebonesVR: %s 2H equip for weapon 0x%08X; equip completed with unsettled worn state (wornRight=%d, wornLeft=%d)",
					sourceLabel,
					item->formID,
					weaponInMainHand ? 1 : 0,
					weaponInOffHand ? 1 : 0);
				return;
			}

			ScheduleTwoHandEitherHandSwap(player, item, targetLeftGameHand, targetIsMainHand);
		}

		void OnHiggsGrabbed(bool isLeft, TESObjectREFR* grabbedRefr)
		{
			if (!grabbedRefr || !grabbedRefr->baseForm)
			{
				return;
			}

			const bool isKnownTwoHand = IsKnownTwoHandWeaponForm(grabbedRefr->baseForm->formID);
			if (!IsTwoHandedMeleeWeapon(grabbedRefr->baseForm) && !isKnownTwoHand)
			{
				return;
			}

			TESObjectWEAP* weapon = DYNAMIC_CAST(grabbedRefr->baseForm, TESForm, TESObjectWEAP);
			if (!weapon)
			{
				return;
			}

			RecordVanillaTwoHandFormIfNeeded(weapon);

			BGSEquipSlot* eitherHand = GetEitherHandSlot();
			if (eitherHand && weapon->equipType.GetEquipSlot() != eitherHand)
			{
				weapon->equipType.SetEquipSlot(eitherHand);
			}

			_MESSAGE(
				"BarebonesVR: HIGGS grip transfer suppressed for 2H weapon 0x%08X (%s VR controller); grip input preserved",
				weapon->formID,
				isLeft ? "left" : "right");
		}

		void UpdateSpellWheelTracking()
		{
			const int lh = static_cast<int>(vlibGetSetting("bLeftHandedMode:VRInput"));
			if (lh >= 0 && lh != leftHandedMode)
			{
				leftHandedMode = lh;
			}

			if (!s_spellwheelInterface)
			{
				return;
			}

			const bool mainOpen = s_spellwheelInterface->IsMainWheelOpen();
			const bool secondaryOpen = s_spellwheelInterface->IsSecondaryWheelOpen();

			if (mainOpen || secondaryOpen)
			{
				MarkSpellwheelActivity();
			}
			else if (s_wasMainWheelOpen || s_wasSecondaryWheelOpen)
			{
				MarkSpellwheelActivity();
			}

			if (secondaryOpen)
			{
				s_mainWheelMainHandSelectionPending = false;
				MarkSpellwheelHandIntent(SpellwheelHandIntent::SecondaryWheelOffHand);
			}
			else if (mainOpen)
			{
				bool leftTrigger = false;
				bool rightTrigger = false;
				GetControllerTrigger(true, leftTrigger);
				GetControllerTrigger(false, rightTrigger);

				const bool offHandTrigger =
					(leftTrigger && IsOffHandVRController(true)) ||
					(rightTrigger && IsOffHandVRController(false));
				const bool mainHandTrigger =
					(leftTrigger && !IsOffHandVRController(true)) ||
					(rightTrigger && !IsOffHandVRController(false));

				if (offHandTrigger)
				{
					s_mainWheelMainHandSelectionPending = false;
					MarkSpellwheelHandIntent(SpellwheelHandIntent::MainWheelCrossHand);
				}
				else if (mainHandTrigger)
				{
					s_mainWheelMainHandSelectionPending = true;
					MarkSpellwheelHandIntent(SpellwheelHandIntent::MainWheelMainHand);
				}
			}
			else
			{
				// Main-hand selection pending survives wheel close until the equip event consumes it.
			}

			s_wasMainWheelOpen = mainOpen;
			s_wasSecondaryWheelOpen = secondaryOpen;
		}

		void OnHiggsPrePhysicsStep(void* /*world*/)
		{
			UpdateSpellWheelTracking();

			if (!s_internalEquipInProgress)
			{
				RefreshPlayerTwoHandWeapons();
				if (g_task && !s_manualOffhandCastingTaskQueued.exchange(true, std::memory_order_acq_rel))
				{
					g_task->AddTask(new ManualOffhandCastingTask());
				}
			}
		}

		class SpellWheelTwoHandEquipEventHandler : public BSTEventSink<TESEquipEvent>
		{
		public:
			static SpellWheelTwoHandEquipEventHandler* GetSingleton()
			{
				static SpellWheelTwoHandEquipEventHandler instance;
				return &instance;
			}

			virtual EventResult ReceiveEvent(TESEquipEvent* evn, EventDispatcher<TESEquipEvent>* /*dispatcher*/) override
			{
				if (s_internalEquipInProgress)
				{
					return kEvent_Continue;
				}

				PlayerCharacter* player = *g_thePlayer;
				if (!evn || !player || !evn->actor)
				{
					return kEvent_Continue;
				}

				Actor* actor = DYNAMIC_CAST(evn->actor, TESObjectREFR, Actor);
				if (!actor)
				{
					return kEvent_Continue;
				}
				TESForm* item = LookupFormByID(evn->baseObject);
				if (!item)
				{
					return kEvent_Continue;
				}

				// If an NPC equips a 2H weapon that the player is main/off-hand wielding or grabbing, swap it.
				if (actor != player)
				{
					if (!evn->equipped)
					{
						return kEvent_Continue;
					}

					if (!actor->IsInCombat() || !player->IsInCombat())
					{
						return kEvent_Continue;
					}

					const bool isKnownTwoHand = IsKnownTwoHandWeaponForm(item->formID);
					if (!IsTwoHandedMeleeWeapon(item) && !isKnownTwoHand)
					{
						return kEvent_Continue;
					}

					if (!PlayerHasSameWeaponEquippedOrGrabbed(player, item->formID))
					{
						return kEvent_Continue;
					}

					_MESSAGE(
						"BarebonesVR: NPC 0x%08X equipped same 2H weapon record as player's main/off-hand wield/grab: weapon=0x%08X",
						actor->formID,
						item->formID);

					TESObjectWEAP* conflictWeapon = DYNAMIC_CAST(item, TESForm, TESObjectWEAP);
					if (!conflictWeapon)
					{
						return kEvent_Continue;
					}

					TESObjectWEAP* substitute = FindSubstituteTwoHandWeapon(conflictWeapon);
					if (!substitute)
					{
						_MESSAGE(
							"BarebonesVR: NPC 0x%08X conflict swap: no substitute found for 0x%08X",
							actor->formID,
							conflictWeapon->formID);
						return kEvent_Continue;
					}

					ForceNpcEquipSubstituteAndSuppressConflict(actor, conflictWeapon, substitute);
					return kEvent_Continue;
				}

				const bool isKnownTwoHand = IsKnownTwoHandWeaponForm(item->formID);
				const bool isPlayerTwoHandWeapon = IsTwoHandedMeleeWeapon(item) || isKnownTwoHand;

				if (!isPlayerTwoHandWeapon)
				{
					if (item->formType == kFormType_Spell)
					{
						ScheduleMainHandTwoHandRebake(player);
					}

					RefreshPlayerTwoHandWeapons();
					return kEvent_Continue;
				}

				TESObjectWEAP* weapon = DYNAMIC_CAST(item, TESForm, TESObjectWEAP);
				if (!weapon)
				{
					return kEvent_Continue;
				}

				if (!evn->equipped)
				{
					if (isKnownTwoHand)
					{
						RestoreUnequippedTwoHandWeapon(weapon);
					}
					return kEvent_Continue;
				}

				bool wornRight = false;
				bool wornLeft = false;
				const bool hasWornState = GetWornState(player, item, wornRight, wornLeft);

				const bool mainHandIsLeftGameHand = IsLeftHandedMode();
				const bool weaponInMainHand = hasWornState && (mainHandIsLeftGameHand ? wornLeft : wornRight);
				const bool weaponInOffHand = hasWornState && (mainHandIsLeftGameHand ? wornRight : wornLeft);

				const bool offHandIntent = HasOffHandSpellwheelIntent();
				const bool mainHandIntent = HasMainHandSpellwheelIntent();

				if (offHandIntent)
				{
					ConsumeOffHandSpellwheelIntent();
					RouteTwoHandEquipToHand(
						player,
						item,
						IsOffHandLeftGameHand(),
						false,
						weaponInMainHand,
						weaponInOffHand,
						"SpellWheel off-hand initiated");
				}
				else if (mainHandIntent)
				{
					ConsumeMainHandSpellwheelIntent();
					RouteTwoHandEquipToHand(
						player,
						item,
						IsMainHandLeftGameHand(),
						true,
						weaponInMainHand,
						weaponInOffHand,
						"SpellWheel main-hand initiated");
				}

				return kEvent_Continue;
			}

		private:
			SpellWheelTwoHandEquipEventHandler() = default;
		};
	}

	void SetupSpellWheelTwoHandLog()
	{
		RecordVanillaTwoHandEquipSlots();

		EventDispatcherList* dispatcherList = GetEventDispatcherList();
		if (dispatcherList)
		{
			dispatcherList->unk4D0.AddEventSink(SpellWheelTwoHandEquipEventHandler::GetSingleton());
			_MESSAGE("BarebonesVR: Registered SpellWheel/HIGGS 2H either-hand equip handler.");
		}
		else
		{
			_ERROR("BarebonesVR: Failed to register SpellWheel off-hand 2H equip handler.");
		}
	}

	void RegisterSpellWheelTwoHandLogCallbacks(PluginHandle pluginHandle, SKSEMessagingInterface* messaging)
	{
		s_spellwheelInterface = spellwheelPluginApi::getSpellWheelInterface001(pluginHandle, messaging);

		if (s_spellwheelInterface)
		{
			_MESSAGE("BarebonesVR: Got SpellWheelVR interface. Buildnumber: %u", s_spellwheelInterface->getBuildNumber());
		}
		else
		{
			_MESSAGE("BarebonesVR: Did not get SpellWheelVR interface (off-hand 2H logging requires SpellWheelVR).");
		}

		if (higgsInterface)
		{
			higgsInterface->AddGrabbedCallback(OnHiggsGrabbed);
			higgsInterface->AddPrePhysicsStepCallback(OnHiggsPrePhysicsStep);
			_MESSAGE("BarebonesVR: Registered HIGGS grab 2H equip handler (Fake Edge VR backup).");
		}
	}
}
