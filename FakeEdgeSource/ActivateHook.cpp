#include "ActivateHook.h"
#include "EquipManager.h"
#include "Engine.h"
#include "VRInputHandler.h"
#include "config.h"
#include "skse64/GameReferences.h"
#include "skse64/GameRTTI.h"
#include "skse64/GameData.h"
#include "skse64_common/SafeWrite.h"
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

        PlayerCharacter* player = *g_thePlayer;
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
