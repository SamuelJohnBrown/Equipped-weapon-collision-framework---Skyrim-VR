#pragma once

#include "skse64/GameReferences.h"
#include "skse64_common/Relocation.h"
#include "skse64_common/BranchTrampoline.h"

namespace FalseEdgeVR
{
  // ============================================
    // Activate Hook - Block player from activating grabbed weapons
    // when other hand has weapon equipped
    // ============================================
    
    // Function signature for TESObjectREFR::Activate
    // bool Activate(TESObjectREFR* activatee, TESObjectREFR* activator, UInt32 unk01, UInt32 unk02, UInt32 count, bool defaultProcessingOnly)
    typedef bool(*_TESObjectREFR_Activate)(TESObjectREFR* activatee, TESObjectREFR* activator, UInt32 unk01, UInt32 unk02, UInt32 count, bool defaultProcessingOnly);
    
    // Original activate function pointer (set during hook setup)
    extern _TESObjectREFR_Activate OriginalActivate;
    
    // Bypass flag - when true, our code is calling activate and should not be blocked
    extern bool g_bypassActivateBlock;
    
  // The hook function that intercepts ALL activate calls
    bool __fastcall ActivateHook(TESObjectREFR* activatee, TESObjectREFR* activator, UInt32 unk01, UInt32 unk02, UInt32 count, bool defaultProcessingOnly);
    
    // Sets up the activate hook - call this during mod initialization
    void SetupActivateHook();
    
    // Safe activate function - sets bypass flag and calls original
    // Use this in our code instead of calling OriginalActivate directly
    bool SafeActivate(TESObjectREFR* activatee, TESObjectREFR* activator, UInt32 unk01, UInt32 unk02, UInt32 count, bool defaultProcessingOnly);
  
// Check if an object is currently grabbed by HIGGS
    bool IsObjectGrabbedByHiggs(TESObjectREFR* obj);
    
    // Check if we should block activation of this object
 bool ShouldBlockActivation(TESObjectREFR* activatee, TESObjectREFR* activator);
}
