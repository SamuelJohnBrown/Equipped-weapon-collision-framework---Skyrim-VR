#include "ActivateHook.h"
#include "EquipManager.h"
#include "Engine.h"
#include "VRInputHandler.h"
#include "config.h"
#include "skse64/GameReferences.h"
#include "skse64/GameRTTI.h"
#include "skse64_common/SafeWrite.h"

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
                _MESSAGE("ShouldBlockActivation: Drop protection DISABLED for %s VR controller - allowing drop",
                    isLeftVRController ? "LEFT" : "RIGHT");
                return false;  // Don't block - player wants to drop
            }

            // ============================================
            // CHECK SHOULDER ZONE
            // If controller is in shoulder zone, allow activation (holstering)
            // ============================================
            if (IsControllerInShoulderZone(isLeftVRController))
            {
                _MESSAGE("ShouldBlockActivation: %s VR controller is in SHOULDER ZONE - allowing activation (holster)",
                    isLeftVRController ? "LEFT" : "RIGHT");
                return false;  // Don't block - player is holstering
            }
        }
     
        // Check if this grabbed weapon is from our trigger system (dropped weapon refs)
     // If so, ALWAYS block - the player uses trigger to equip, not activate
        TESObjectREFR* droppedLeft = EquipManager::GetSingleton()->GetDroppedWeaponRef(true);
      TESObjectREFR* droppedRight = EquipManager::GetSingleton()->GetDroppedWeaponRef(false);
        
      if (activatee == droppedLeft || activatee == droppedRight)
        {
     _MESSAGE("ShouldBlockActivation: Weapon is from trigger system - blocking activation");
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
 
            _MESSAGE("ActivateHook: Weapon activation - FormID: %08X, IsPlayer: %s, IsGrabbed: %s",
       activatee->formID,
   isPlayer ? "YES" : "NO",
        isGrabbed ? "YES" : "NO");
  }
    
  // Check if we should block this activation
        if (ShouldBlockActivation(activatee, activator))
        {
            _MESSAGE("ActivateHook: BLOCKED player from activating grabbed weapon (FormID: %08X)",
     activatee ? activatee->formID : 0);
    return false;  // Block activation
      }
        
        // Allow activation
        return OriginalActivate(activatee, activator, unk01, unk02, count, defaultProcessingOnly);
    }
    
    // ============================================
    // Safe Activate - For our code to use
    // ============================================
    
    bool SafeActivate(TESObjectREFR* activatee, TESObjectREFR* activator, UInt32 unk01, UInt32 unk02, UInt32 count, bool defaultProcessingOnly)
    {
      // Set bypass flag
  g_bypassActivateBlock = true;
      
 // Call the original function (via trampoline)
        bool result = OriginalActivate(activatee, activator, unk01, unk02, count, defaultProcessingOnly);
        
        // Clear bypass flag
        g_bypassActivateBlock = false;
        
     return result;
}
    
    // ============================================
    // Hook Setup
    // ============================================
    
    void SetupActivateHook()
    {
        _MESSAGE("SetupActivateHook: Initializing Activate Hook...");
        
        uintptr_t funcAddr = OriginalActivateFunc.GetUIntPtr();
        _MESSAGE("SetupActivateHook: Activate function address: 0x%llX", funcAddr);
   
        // Log first bytes for debugging
        unsigned char* funcStart = (unsigned char*)funcAddr;
        _MESSAGE("SetupActivateHook: First 16 bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
     funcStart[0], funcStart[1], funcStart[2], funcStart[3],
     funcStart[4], funcStart[5], funcStart[6], funcStart[7],
            funcStart[8], funcStart[9], funcStart[10], funcStart[11],
   funcStart[12], funcStart[13], funcStart[14], funcStart[15]);
    
    // Store original function address (before we overwrite it)
        OriginalActivate = (_TESObjectREFR_Activate)funcAddr;
        
        // Analyze prolog to determine safe size to copy
        int prologSize = 0;
 for (int i = 0; i < 20 && prologSize < 14; )
        {
  unsigned char b = funcStart[i];
     
      // REX prefix (40-4F)
    if (b >= 0x40 && b <= 0x4F)
    {
 unsigned char nextB = funcStart[i + 1];
    
   // REX + push (50-57)
 if (nextB >= 0x50 && nextB <= 0x57)
       {
    prologSize += 2;
       i += 2;
        continue;
    }
           // REX + sub rsp
      else if (nextB == 0x83 || nextB == 0x81)
   {
        if (nextB == 0x83) // imm8
       {
      prologSize += 4;
            i += 4;
            }
       else // imm32
    {
          prologSize += 7;
      i += 7;
     }
          continue;
        }
       // REX + mov
    else if (nextB == 0x89 || nextB == 0x8B)
   {
           prologSize += 5;
        i += 5;
  continue;
      }
    // REX + lea
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
          // Single byte push (50-57)
  else if (b >= 0x50 && b <= 0x57)
            {
       prologSize += 1;
                i += 1;
                continue;
  }
  // Unknown - break
       else
    {
       break;
     }
        }
        
    _MESSAGE("SetupActivateHook: Detected prolog size: %d bytes", prologSize);
        
   // Ensure minimum size for our jump
        if (prologSize < 5)
        {
    _MESSAGE("SetupActivateHook: WARNING - Prolog too small, using 14 bytes");
  prologSize = 14;
        }
      
        // Allocate trampoline memory
    void* trampMem = g_localTrampoline.Allocate(prologSize + 14);
        if (!trampMem)
 {
  _MESSAGE("SetupActivateHook: ERROR - Failed to allocate trampoline memory!");
   return;
 }
        
        unsigned char* tramp = (unsigned char*)trampMem;
        
        // Copy the original prolog bytes
        memcpy(tramp, funcStart, prologSize);
        
        // Add jump back to original function + prologSize
    int offset = prologSize;
    tramp[offset++] = 0xFF;  // JMP [rip+0]
        tramp[offset++] = 0x25;
        tramp[offset++] = 0x00;
  tramp[offset++] = 0x00;
 tramp[offset++] = 0x00;
        tramp[offset++] = 0x00;
        
     uintptr_t jumpBack = funcAddr + prologSize;
  memcpy(&tramp[offset], &jumpBack, 8);
     
        // Update OriginalActivate to point to trampoline
        OriginalActivate = (_TESObjectREFR_Activate)trampMem;
      
        _MESSAGE("SetupActivateHook: Trampoline at 0x%llX, jumps back to 0x%llX", (uintptr_t)trampMem, jumpBack);
        _MESSAGE("SetupActivateHook: Copied %d bytes to trampoline", prologSize);
        
        // Write jump at original function to our hook
     g_branchTrampoline.Write5Branch(funcAddr, (uintptr_t)ActivateHook);

        _MESSAGE("SetupActivateHook: Hook installed successfully!");
    }
}
