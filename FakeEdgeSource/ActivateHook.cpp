#include "ActivateHook.h"
#include "EquipManager.h"
#include "Engine.h"
#include "VRInputHandler.h"
#include "SkyrimEventSinkCompat.h"
#include "config.h"
#include "skse64/GameReferences.h"
#include "skse64/GameRTTI.h"
#include "skse64/GameData.h"
#include "skse64/PapyrusEvents.h"
#include "skse64_common/SafeWrite.h"
#include <chrono>
#include <cstring>

namespace FalseEdgeVR
{
    // Forward declaration from VRInputHandler.cpp
    bool IsControllerInShoulderZone(bool isLeftVRController);
    
    // ============================================
    // Globals
    // ============================================
    
    // Address for TESObjectREFR::Activate - 0x2A8300 for Skyrim VR
    RelocAddr<_TESObjectREFR_Activate> OriginalActivateFunc(0x2A8300);
    
    // Original function pointer - will point to trampoline after hook setup
    _TESObjectREFR_Activate OriginalActivate = nullptr;
    
    // Bypass flag - our code sets this to true before calling activate
    bool g_bypassActivateBlock = false;

    static UInt32 s_promptCrosshairRefHandle = 0;
    static UInt32 s_lastPromptActivationTargetID = 0;
    static bool s_lastPromptActivationWasSynthetic = false;
    static std::chrono::steady_clock::time_point s_lastPromptActivationTime{};

    static bool IsRecentPromptActivation(UInt32 targetID, bool synthetic)
    {
        if (targetID == 0 || s_lastPromptActivationTargetID != targetID ||
            s_lastPromptActivationWasSynthetic != synthetic)
            return false;

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - s_lastPromptActivationTime).count();
        return elapsed >= 0 && elapsed <= 200;
    }

    static void RecordPromptActivation(UInt32 targetID, bool synthetic)
    {
        s_lastPromptActivationTargetID = targetID;
        s_lastPromptActivationWasSynthetic = synthetic;
        s_lastPromptActivationTime = std::chrono::steady_clock::now();
    }

    class PromptCrosshairEventHandler : public BSTEventSink<SKSECrosshairRefEvent>
    {
    public:
        virtual EventResult ReceiveEvent(
            SKSECrosshairRefEvent* evn,
            EventDispatcher<SKSECrosshairRefEvent>* /*dispatcher*/) override
        {
            s_promptCrosshairRefHandle =
                (evn && evn->crosshairRef) ? evn->crosshairRef->CreateRefHandle() : 0;
            return kEvent_Continue;
        }

        static PromptCrosshairEventHandler* GetSingleton()
        {
            static PromptCrosshairEventHandler instance;
            return MakeSkyrimEventSinkCompatible(&instance);
        }
    };

    typedef void (*_EquipManager_EquipItem)(::EquipManager*, Actor*, TESForm*, BaseExtraList*, SInt32, BGSEquipSlot*, bool, bool, bool, void*);
    _EquipManager_EquipItem OriginalEquipItem = nullptr;
  
    // ============================================
    // Helper Functions
    // ============================================

    static int AnalyzeFunctionPrologSize(unsigned char* funcStart)
    {
        int prologSize = 0;
        for (int i = 0; i < 20 && prologSize < 14; )
        {
            unsigned char b = funcStart[i];

            if (b >= 0x40 && b <= 0x4F)
            {
                unsigned char nextB = funcStart[i + 1];

                if (nextB >= 0x50 && nextB <= 0x57)
                {
                    prologSize += 2;
                    i += 2;
                    continue;
                }
                else if (nextB == 0x83 || nextB == 0x81)
                {
                    if (nextB == 0x83)
                    {
                        prologSize += 4;
                        i += 4;
                    }
                    else
                    {
                        prologSize += 7;
                        i += 7;
                    }
                    continue;
                }
                else if (nextB == 0x89 || nextB == 0x8B)
                {
                    prologSize += 5;
                    i += 5;
                    continue;
                }
                else if (nextB == 0x8D)
                {
                    prologSize += 5;
                    i += 5;
                    continue;
                }
                else
                {
                    prologSize += 2;
                    i += 2;
                    continue;
                }
            }
            else if (b >= 0x50 && b <= 0x57)
            {
                prologSize += 1;
                i += 1;
                continue;
            }
            else
            {
                break;
            }
        }

        if (prologSize < 5)
            prologSize = 14;

        return prologSize;
    }

    static bool InstallDetourHook(uintptr_t funcAddr, void* hookFunc, void** outTrampolineFunc, const char* hookName)
    {
        unsigned char* funcStart = (unsigned char*)funcAddr;
        int prologSize = AnalyzeFunctionPrologSize(funcStart);

        void* trampMem = g_localTrampoline.Allocate(prologSize + 14);
        if (!trampMem)
        {
            _MESSAGE("%s: ERROR - Failed to allocate trampoline memory!", hookName);
            return false;
        }

        unsigned char* tramp = (unsigned char*)trampMem;
        memcpy(tramp, funcStart, prologSize);

        int offset = prologSize;
        tramp[offset++] = 0xFF;
        tramp[offset++] = 0x25;
        tramp[offset++] = 0x00;
        tramp[offset++] = 0x00;
        tramp[offset++] = 0x00;
        tramp[offset++] = 0x00;

        uintptr_t jumpBack = funcAddr + prologSize;
        memcpy(&tramp[offset], &jumpBack, 8);

        *outTrampolineFunc = trampMem;
        g_branchTrampoline.Write5Branch(funcAddr, (uintptr_t)hookFunc);
        return true;
    }
  
    // ============================================
    // Helper Functions
    // ============================================
    
    bool IsObjectGrabbedByHiggs(TESObjectREFR* obj)
    {
        if (!obj || !higgsInterface)
     return false;
        
        // Check if either hand is grabbing this object
  TESObjectREFR* leftGrabbed = higgsInterface->GetGrabbedObject(true);
        TESObjectREFR* rightGrabbed = higgsInterface->GetGrabbedObject(false);
    
   return (leftGrabbed == obj) || (rightGrabbed == obj);
  }
  
    // Get which VR controller is holding this object (returns true for left, false for right)
    // Returns false if not held by either hand
    bool GetHoldingVRController(TESObjectREFR* obj, bool& outIsLeftController)
    {
 if (!obj || !higgsInterface)
   return false;
        
    TESObjectREFR* leftGrabbed = higgsInterface->GetGrabbedObject(true);
    TESObjectREFR* rightGrabbed = higgsInterface->GetGrabbedObject(false);
    
    if (leftGrabbed == obj)
        {
outIsLeftController = true;
 return true;
        }
   else if (rightGrabbed == obj)
   {
       outIsLeftController = false;
            return true;
        }
      
        return false;
    }
    
    bool ShouldBlockActivation(TESObjectREFR* activatee, TESObjectREFR* activator)
    {
        // Only block player activations
        PlayerCharacter* player = *g_thePlayer;
 if (!player || activator != player)
    return false;
     
   // Only block if the object is grabbed by HIGGS
        if (!IsObjectGrabbedByHiggs(activatee))
   return false;
 
        // Only block if it's a weapon
        if (!activatee->baseForm || activatee->baseForm->formType != kFormType_Weapon)
return false;
        
        // ============================================
// CHECK DROP PROTECTION OVERRIDE
// If player spammed grip, allow the drop
// ============================================
        bool isLeftVRController = false;
        if (GetHoldingVRController(activatee, isLeftVRController))
        {
            if (VRInputHandler::IsDropProtectionDisabled(isLeftVRController))
            {
                return false;  // Don't block - player wants to drop
            }

            // ============================================
            // CHECK SHOULDER ZONE
            // If controller is in shoulder zone, allow activation (holstering)
            // ============================================
            if (IsControllerInShoulderZone(isLeftVRController))
            {
                return false;  // Don't block - player is holstering
            }
        }
     
        // Check if this grabbed weapon is from our trigger system (dropped weapon refs)
     // If so, ALWAYS block - the player uses trigger to equip, not activate
        TESObjectREFR* droppedLeft = EquipManager::GetSingleton()->GetDroppedWeaponRef(true);
      TESObjectREFR* droppedRight = EquipManager::GetSingleton()->GetDroppedWeaponRef(false);
        
      if (activatee == droppedLeft || activatee == droppedRight)
        {
  return true;  // Block - this weapon is managed by our trigger system
   }
        
    // Check if EITHER hand has a weapon equipped (legacy check)
    const PlayerEquipState& equipState = EquipManager::GetSingleton()->GetEquipState();
        
        if (equipState.leftHand.isEquipped || equipState.rightHand.isEquipped)
        {
    return true;  // Block
        }

        return false;  // Don't block - allow normal pickup
    }
    
    // ============================================
    // Hook Function
    // ============================================
    
    bool __fastcall ActivateHook(TESObjectREFR* activatee, TESObjectREFR* activator, UInt32 unk01, UInt32 unk02, UInt32 count, bool defaultProcessingOnly)
    {
        // If bypass flag is set, our code is calling - allow it through
        if (g_bypassActivateBlock)
        {
            return OriginalActivate(activatee, activator, unk01, unk02, count, defaultProcessingOnly);
        }

        PlayerCharacter* player = *g_thePlayer;
        if (player && activator == player && activatee && activatee != player &&
            activatee->baseForm && activatee->baseForm->formType != kFormType_Weapon)
        {
            // The raw VR binding may deliver its normal activation just after
            // our owner-grip fallback.  Suppress only that same-target duplicate
            // in a very short window so doors and other toggles do not fire twice.
            if (IsRecentPromptActivation(activatee->formID, true))
            {
                _MESSAGE(
                    "[FalseEdgeVR] Suppressed duplicate native prompt activation: target=0x%08X",
                    activatee->formID);
                return false;
            }

            RecordPromptActivation(activatee->formID, false);
        }
  
        // Log all weapon activations for debugging
        if (activatee && activatee->baseForm && activatee->baseForm->formType == kFormType_Weapon)
        {
            PlayerCharacter* player = *g_thePlayer;
            bool isPlayer = (player && activator == player);
    bool isGrabbed = IsObjectGrabbedByHiggs(activatee);
 
  }
    
  // Check if we should block this activation
        if (ShouldBlockActivation(activatee, activator))
        {
    return false;  // Block activation
      }

        player = *g_thePlayer;
        if (player && activator == player && activatee && activatee->baseForm &&
            activatee->baseForm->formType == kFormType_Door && !g_bypassActivateBlock)
        {
            NotifyDoorOrTransitionActivated();
        }
        
        // Allow activation
        return OriginalActivate(activatee, activator, unk01, unk02, count, defaultProcessingOnly);
    }

    void __fastcall EquipItemHook(::EquipManager* thisPtr, Actor* actor, TESForm* item, BaseExtraList* extraData, SInt32 count, BGSEquipSlot* equipSlot, bool withEquipSound, bool preventUnequip, bool showMsg, void* unk)
    {
        PlayerCharacter* player = *g_thePlayer;
        if (player && actor == player && item && equipSlot && EquipManager::IsWeapon(item))
        {
            BGSEquipSlot* leftSlot = GetLeftHandSlot();
            BGSEquipSlot* rightSlot = GetRightHandSlot();
            if (leftSlot && rightSlot && (equipSlot == leftSlot || equipSlot == rightSlot))
            {
                bool isLeftHand = (equipSlot == leftSlot);
                EquipManager* mgr = EquipManager::GetSingleton();
                if (mgr->HasConflictingGrabbedWeaponInHand(isLeftHand, item))
                    mgr->SchedulePickUpGrabbedWeaponBeforeEquip(isLeftHand);
            }
        }

        OriginalEquipItem(thisPtr, actor, item, extraData, count, equipSlot, withEquipSound, preventUnequip, showMsg, unk);
    }
    
    // ============================================
    // Safe Activate - For our code to use
    // ============================================
    
    bool SafeActivate(TESObjectREFR* activatee, TESObjectREFR* activator, UInt32 unk01, UInt32 unk02, UInt32 count, bool defaultProcessingOnly)
    {
        TESForm* pickedUpWeaponForm = nullptr;
        if (activatee && activatee->baseForm && activatee->baseForm->formType == kFormType_Weapon)
        {
            pickedUpWeaponForm = activatee->baseForm;
            SetOwnerToPlayer(activatee);
        }

      // Set bypass flag
  g_bypassActivateBlock = true;
      
 // Call the original function (via trampoline)
        bool result = OriginalActivate(activatee, activator, unk01, unk02, count, defaultProcessingOnly);
        
        // Clear bypass flag
        g_bypassActivateBlock = false;

        if (result && pickedUpWeaponForm && activator == *g_thePlayer)
            EnsurePlayerOwnsWeaponInInventory(static_cast<PlayerCharacter*>(activator), pickedUpWeaponForm);
        
     return result;
}

    void RegisterPromptActivationSupport(SKSEMessagingInterface* messaging)
    {
        if (!messaging || !messaging->GetEventDispatcher)
        {
            _ERROR("[FalseEdgeVR] Prompt target tracking unavailable: SKSE messaging dispatcher missing");
            return;
        }

        auto* dispatcher = static_cast<EventDispatcher<SKSECrosshairRefEvent>*>(
            messaging->GetEventDispatcher(SKSEMessagingInterface::kDispatcher_CrosshairEvent));
        if (!dispatcher)
        {
            _ERROR("[FalseEdgeVR] Prompt target tracking unavailable: crosshair dispatcher missing");
            return;
        }

        dispatcher->AddEventSink(PromptCrosshairEventHandler::GetSingleton());
        _MESSAGE("[FalseEdgeVR] Registered SKSE crosshair target tracker for grip prompt activation");
    }

    bool TryActivateTrackedPrompt(const char* sourceLabel)
    {
        if (!sourceLabel)
            sourceLabel = "grip";

        PlayerCharacter* player = *g_thePlayer;
        if (!player || !OriginalActivate || s_promptCrosshairRefHandle == 0)
        {
            _MESSAGE(
                "[FalseEdgeVR] Grip prompt activation skipped (%s): no tracked target",
                sourceLabel);
            return false;
        }

        UInt32 handle = s_promptCrosshairRefHandle;
        NiPointer<TESObjectREFR> target;
        if (!LookupREFRByHandle(handle, target) || !target || target.m_pObject == player ||
            !target->baseForm || target->baseForm->formType == kFormType_Weapon)
        {
            _MESSAGE(
                "[FalseEdgeVR] Grip prompt activation skipped (%s): target unavailable or weapon",
                sourceLabel);
            return false;
        }

        if (IsRecentPromptActivation(target->formID, false))
        {
            _MESSAGE(
                "[FalseEdgeVR] Native prompt activation already handled %s: target=0x%08X",
                sourceLabel, target->formID);
            return true;
        }

        if (target->baseForm->formType == kFormType_Door)
            NotifyDoorOrTransitionActivated();

        RecordPromptActivation(target->formID, true);
        const bool result = SafeActivate(target.m_pObject, player, 0, 0, 1, false);
        if (!result)
        {
            s_lastPromptActivationTargetID = 0;
            s_lastPromptActivationWasSynthetic = false;
        }

        _MESSAGE(
            "[FalseEdgeVR] Grip prompt activation (%s): target=0x%08X base=0x%08X result=%d",
            sourceLabel,
            target->formID,
            target->baseForm->formID,
            result ? 1 : 0);
        return result;
    }
    
    // ============================================
    // Hook Setup
    // ============================================
    
    void SetupActivateHook()
    {
        uintptr_t funcAddr = OriginalActivateFunc.GetUIntPtr();
        void* trampoline = nullptr;

        if (InstallDetourHook(funcAddr, (void*)ActivateHook, &trampoline, "SetupActivateHook"))
            OriginalActivate = (_TESObjectREFR_Activate)trampoline;
    }

    void SetupEquipItemHook()
    {
        RelocAddr<uintptr_t> equipItemAddr(0x00640A90);
        uintptr_t funcAddr = equipItemAddr.GetUIntPtr();
        void* trampoline = nullptr;

        if (InstallDetourHook(funcAddr, (void*)EquipItemHook, &trampoline, "SetupEquipItemHook"))
        {
            OriginalEquipItem = (_EquipManager_EquipItem)trampoline;
            _MESSAGE("SetupEquipItemHook: EquipItem hook installed");
        }
    }
}
