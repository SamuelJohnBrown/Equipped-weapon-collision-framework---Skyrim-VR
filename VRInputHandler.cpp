#include "VRInputHandler.h"
#include "Engine.h"
#include "WeaponGeometry.h"
#include "ShieldCollision.h"
#include "ActivateHook.h"
#include "skse64/GameReferences.h"
#include "skse64/GameVR.h"
#include <chrono>

namespace FalseEdgeVR
{
    // Forward declaration for trigger polling
    void PollTriggerState();
    
    // ============================================
    // VRInputHandler Implementation
    // ============================================

    VRInputHandler* VRInputHandler::GetSingleton()
    {
        static VRInputHandler instance;
        return &instance;
    }

    void VRInputHandler::Initialize()
    {
        if (m_initialized)
            return;

        _MESSAGE("VRInputHandler: Initializing...");

        // Register HIGGS callbacks if HIGGS is available
        RegisterHiggsCallbacks();

        // Trigger button tracking initialization
        RegisterTriggerCallback();

        m_initialized = true;
        _MESSAGE("VRInputHandler: Initialized successfully");
    }

    void VRInputHandler::RegisterHiggsCallbacks()
    {
        if (m_callbacksRegistered)
            return;

        if (!higgsInterface)
        {
            _MESSAGE("VRInputHandler: HIGGS interface not available, skipping callback registration");
            return;
        }

        _MESSAGE("VRInputHandler: Registering HIGGS callbacks...");

        // Register grab/drop callbacks
        higgsInterface->AddGrabbedCallback(OnGrabbed);
        higgsInterface->AddDroppedCallback(OnDropped);
        higgsInterface->AddPulledCallback(OnPulled);

        // Register collision callback for weapon impacts
        higgsInterface->AddCollisionCallback(OnCollision);

        // Register two-handing callbacks
        higgsInterface->AddStartTwoHandingCallback(OnStartTwoHanding);
        higgsInterface->AddStopTwoHandingCallback(OnStopTwoHanding);
        
        // Register pre-physics step callback for per-frame updates
        higgsInterface->AddPrePhysicsStepCallback(OnPrePhysicsStep);

        m_callbacksRegistered = true;
        _MESSAGE("VRInputHandler: HIGGS callbacks registered successfully");
    }

    void VRInputHandler::UpdateGrabListening()
    {
        bool shouldListen = ShouldListenForGrabs();

    if (shouldListen && !m_isListening)
        {
            m_isListening = true;
            _MESSAGE("VRInputHandler: Started listening for grab events (weapon or shield equipped)");
     }
   else if (!shouldListen && m_isListening)
   {
     m_isListening = false;
            _MESSAGE("VRInputHandler: Stopped listening for grab events (no weapons or shields equipped)");
        }
    }

    bool VRInputHandler::ShouldListenForGrabs() const
    {
    // ALWAYS listen for grabs so we can auto-equip grabbed 1H weapons
        // This allows the trigger system to work with any grabbed weapon
        if (autoEquipGrabbedWeaponEnabled)
     {
        return true;
        }

        // Fallback: Listen for grabs when either hand has a weapon OR shield equipped
 const PlayerEquipState& equipState = EquipManager::GetSingleton()->GetEquipState();
        bool hasWeaponOrShield = equipState.leftHand.isEquipped || equipState.rightHand.isEquipped;
        
        // Also check for shield directly (in case EquipState doesn't track it)
      if (!hasWeaponOrShield)
        {
            hasWeaponOrShield = ShieldCollisionTracker::GetSingleton()->HasShieldEquipped();
    }
        
        return hasWeaponOrShield;
    }

    bool VRInputHandler::IsTwoHanding() const
    {
        if (!higgsInterface)
            return false;

        return higgsInterface->IsTwoHanding();
    }

    // ============================================
    // HIGGS Callback Handlers
    // ============================================
    
    void VRInputHandler::OnPrePhysicsStep(void* world)
    {
   static int frameCount = 0;
        static bool loggedOnce = false;
   
        VRInputHandler* handler = GetSingleton();
  
        // Calculate delta time
 static auto lastTime = std::chrono::high_resolution_clock::now();
        auto currentTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
  lastTime = currentTime;
        
     // Clamp delta time to reasonable values
     if (deltaTime > 0.1f) deltaTime = 0.1f;
        if (deltaTime < 0.0001f) deltaTime = 0.0001f;
      
        frameCount++;
        
        // Log once to confirm callback is working
        if (!loggedOnce)
        {
            _MESSAGE("VRInputHandler::OnPrePhysicsStep - Callback is firing! Frame: %d", frameCount);
         loggedOnce = true;
        }
    
        // Poll trigger button state each frame
        PollTriggerState();
   
        // Check for pending auto-unequip (trigger-based weapon hold system)
        EquipManager::GetSingleton()->CheckPendingAutoUnequip();
    
        // Log every 500 frames to confirm still running
        if (frameCount % 500 == 0)
  {
         _MESSAGE("VRInputHandler::OnPrePhysicsStep - Frame %d, IsListening: %s", 
           frameCount, handler->IsListening() ? "YES" : "NO");
        }
   
        // Update cooldown timers (for re-equip spam prevention)
        if (handler->m_leftHandOnCooldown)
        {
            handler->m_leftHandCooldownTimer += deltaTime;
         if (handler->m_leftHandCooldownTimer >= bladeReequipCooldown)
            {
handler->m_leftHandOnCooldown = false;
          handler->m_leftHandCooldownTimer = 0.0f;
   }
   }
    if (handler->m_rightHandOnCooldown)
    {
   handler->m_rightHandCooldownTimer += deltaTime;
     if (handler->m_rightHandCooldownTimer >= shieldReequipCooldown)
            {
        handler->m_rightHandOnCooldown = false;
  handler->m_rightHandCooldownTimer = 0.0f;
            }
        }

        // ===========================================
        // DISABLED: Old blade collision/distance/timer based re-equip logic
     // The new trigger-based system handles dual-wield blade collision
   // Weapon stays HIGGS grabbed until trigger is pressed
        // ===========================================
  
        // handler->CheckCollisionTimeout(deltaTime);      // DISABLED - trigger system handles this
     // handler->CheckPendingReequip(deltaTime);        // DISABLED - trigger system handles this
   // handler->UpdateCombatTracking();              // DISABLED - not needed for trigger system
        
  // DISABLED: Shield collision timeout also now handled by trigger system
 // handler->CheckShieldCollisionTimeout(deltaTime); // DISABLED - trigger system handles this
   
        // RE-ENABLED: Auto-equip grabbed weapons (needed for world object grab -> equip -> trigger system)
        handler->CheckAutoEquipGrabbedWeapon(deltaTime);
        
        // Update shield bash tracking (still needed for shield bash detection)
        handler->UpdateShieldBashTracking(deltaTime);
      
        // ALWAYS update weapon geometry and shield collision tracking
    // These trackers handle their own equipment checks internally
      UpdateWeaponGeometry(deltaTime);
     UpdateShieldCollision(deltaTime);
    }
    

    void VRInputHandler::PauseTracking(bool pause)
    {
    // Menu pause tracking removed - trackers now run continuously
      // This function is kept for compatibility but does nothing
        if (pause)
        {
            _MESSAGE("VRInputHandler: Menu opened (tracking continues)");
      }
        else
     {
     _MESSAGE("VRInputHandler: Menu closed");
       // Force equipment state refresh when menu closes
 EquipManager::GetSingleton()->UpdateEquipmentState();
            UpdateGrabListening();
  }
    }

    void VRInputHandler::UpdateCombatTracking()
    {
        PlayerCharacter* player = *g_thePlayer;
        if (!player)
        {
            m_isInCombat = false;
  m_closestTargetDistance = 9999.0f;
       m_closestTargetHandle = 0;
            m_closeCombatMode = false;
            return;
  }
   
        // Check if player is in combat
    bool wasInCombat = m_isInCombat;
        m_isInCombat = player->IsInCombat();
    
     // Log combat state changes
        if (m_isInCombat && !wasInCombat)
      {
     _MESSAGE("VRInputHandler: === PLAYER ENTERED COMBAT ===");
          _MESSAGE("VRInputHandler:   currentCombatTarget handle: %08X", player->currentCombatTarget);
        }
   else if (!m_isInCombat && wasInCombat)
  {
            _MESSAGE("VRInputHandler: === PLAYER LEFT COMBAT ===");
    m_closestTargetDistance = 9999.0f;
          m_closestTargetHandle = 0;
       
  // Exit close combat mode when leaving combat
  if (m_closeCombatMode)
            {
   m_closeCombatMode = false;
     _MESSAGE("VRInputHandler: Exited CLOSE COMBAT MODE (left combat)");
            }
        }
   
     // If in combat, get current combat target distance
     if (m_isInCombat)
        {
            m_closestTargetDistance = 9999.0f;
   m_closestTargetHandle = 0;

   NiPoint3 playerPos = player->pos;
 
        // Try to get the current combat target from the Actor's currentCombatTarget handle
       UInt32 combatTargetHandle = player->currentCombatTarget;
         if (combatTargetHandle != 0 && combatTargetHandle != *g_invalidRefHandle)
            {
          NiPointer<TESObjectREFR> targetRefr;
  if (LookupREFRByHandle(combatTargetHandle, targetRefr) && targetRefr)
    {
  Actor* targetActor = DYNAMIC_CAST(targetRefr.get(), TESObjectREFR, Actor);
         if (targetActor && !targetActor->IsDead(1))
       {
    NiPoint3 targetPos = targetActor->pos;
       float dx = playerPos.x - targetPos.x;
 float dy = playerPos.y - targetPos.y;
       float dz = playerPos.z - targetPos.z;
   m_closestTargetDistance = sqrt(dx*dx + dy*dy + dz*dz);
        m_closestTargetHandle = combatTargetHandle;
         }
     }
      }
        else
            {
          // No combat target - log this periodically for debugging
         static int noTargetLogCounter = 0;
  noTargetLogCounter++;
          if (noTargetLogCounter % 200 == 1)
        {
      _MESSAGE("VRInputHandler: In combat but NO COMBAT TARGET! Handle: %08X, InvalidHandle: %08X",
    combatTargetHandle, *g_invalidRefHandle);
    }
            }
    
   // ============================================
     // Close Combat Mode Logic
            // ============================================
     bool wasInCloseCombatMode = m_closeCombatMode;
 
         if (!m_closeCombatMode)
     {
         // Enter close combat mode if within enter threshold
            if (m_closestTargetDistance <= closeCombatEnterDistance)
        {
         m_closeCombatMode = true;
    _MESSAGE("VRInputHandler: === ENTERED CLOSE COMBAT MODE ===");
         _MESSAGE("VRInputHandler:   Target distance: %.1f units (threshold: %.1f)", 
    m_closestTargetDistance, closeCombatEnterDistance);
 _MESSAGE("VRInputHandler:   Collision avoidance DISABLED - auto-equipping any grabbed weapons");
          
   // Auto-equip any grabbed weapons immediately
    ForceEquipGrabbedWeapons();
          }
     }
    else
            {
   // Exit close combat mode if past exit threshold (with buffer)
 if (m_closestTargetDistance > closeCombatExitDistance)
   {
       m_closeCombatMode = false;
        _MESSAGE("VRInputHandler: === EXITED CLOSE COMBAT MODE ===");
             _MESSAGE("VRInputHandler:   Target distance: %.1f units (threshold: %.1f)", 
         m_closestTargetDistance, closeCombatExitDistance);
      _MESSAGE("VRInputHandler:   Collision avoidance RE-ENABLED");
   }
            }
       
       // Log combat status periodically (every 2 seconds)
   m_combatLogTimer += 0.011f; // Approximate frame time at 90fps
  if (m_combatLogTimer >= 2.0f)
            {
         m_combatLogTimer = 0.0f;
    
           if (m_closestTargetHandle != 0)
             {
   NiPointer<TESObjectREFR> targetRefr;
 if (LookupREFRByHandle(m_closestTargetHandle, targetRefr) && targetRefr)
        {
       const char* targetName = CALL_MEMBER_FN(targetRefr.get(), GetReferenceName)();
          _MESSAGE("VRInputHandler: COMBAT STATUS - Target: %s, Distance: %.1f units (%.1f m), CloseCombat: %s", 
 targetName ? targetName : "Unknown",
           m_closestTargetDistance,
      m_closestTargetDistance / 70.0f,
     m_closeCombatMode ? "YES" : "NO");
       }
   }
       else
                {
         _MESSAGE("VRInputHandler: COMBAT STATUS - In combat but no specific target, CloseCombat: %s",
        m_closeCombatMode ? "YES" : "NO");
      }
            }
        }
        else
        {
     // Not in combat - ensure close combat mode is off
      if (m_closeCombatMode)
            {
    m_closeCombatMode = false;
      _MESSAGE("VRInputHandler: Exited CLOSE COMBAT MODE (not in combat)");
      }
    }
    }

    void VRInputHandler::ForceEquipGrabbedWeapons()
    {
        // Check if either hand has a grabbed weapon (from auto-equip pending or collision avoidance)
  // and immediately equip it
        
        PlayerCharacter* player = *g_thePlayer;
        if (!player)
    return;

        // Check left VR controller for grabbed weapon
        if (m_autoEquipWeaponLeft && higgsInterface)
        {
       TESObjectREFR* grabbed = higgsInterface->GetGrabbedObject(true);
            if (grabbed == m_autoEquipWeaponLeft && grabbed->baseForm)
            {
                _MESSAGE("VRInputHandler: Close combat - force equipping LEFT grabbed weapon");
       
     // Suppress pickup sound during internal re-equip
     EquipManager::s_suppressPickupSound = true;
                bool activated = SafeActivate(grabbed, player, 0, 0, 1, true);
                EquipManager::s_suppressPickupSound = false;
     if (activated)
    {
         bool isLeftGameHand = VRControllerToGameHand(true);
           ::EquipManager* equipMan = ::EquipManager::GetSingleton();
       if (equipMan)
            {
       BGSEquipSlot* slot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
    // Suppress draw sound during collision re-equip
         EquipManager::s_suppressDrawSound = true;
         // Temporarily strip enchantment to prevent enchant VFX/sound
         TESObjectWEAP* weap = DYNAMIC_CAST(grabbed->baseForm, TESForm, TESObjectWEAP);
         EnchantmentItem* cachedEnchant = nullptr;
         if (weap && weap->enchantable.enchantment)
         {
             cachedEnchant = weap->enchantable.enchantment;
             weap->enchantable.enchantment = nullptr;
         }

         // Temporarily strip enchantment to prevent enchant VFX/sound
         TESObjectWEAP* weap2 = DYNAMIC_CAST(grabbed->baseForm, TESForm, TESObjectWEAP);
         EnchantmentItem* cachedEnchant2 = nullptr;
         if (weap2 && weap2->enchantable.enchantment)
         {
             cachedEnchant2 = weap2->enchantable.enchantment;
             weap2->enchantable.enchantment = nullptr;
         }
            
            CALL_MEMBER_FN(equipMan, EquipItem)(player, grabbed->baseForm, nullptr, 1, slot, false, true, false, nullptr);
            
            // Restore enchantment immediately
            if (weap2 && cachedEnchant2)
            {
                weap2->enchantable.enchantment = cachedEnchant2;
            }

         // Restore enchantment immediately
         if (weap && cachedEnchant)
         {
             weap->enchantable.enchantment = cachedEnchant;
         }

       EquipManager::s_suppressDrawSound = false;
       _MESSAGE("VRInputHandler: Force equipped weapon to %s game hand", isLeftGameHand ? "LEFT" : "RIGHT");
          }
 }
        
              m_autoEquipPendingLeft = false;
  m_autoEquipTimerLeft = 0.0f;
          m_autoEquipWeaponLeft = nullptr;
         }
 }
        
        // Check right VR controller for grabbed weapon
        if (m_autoEquipWeaponRight && higgsInterface)
        {
            TESObjectREFR* grabbed = higgsInterface->GetGrabbedObject(false);
            if (grabbed == m_autoEquipWeaponRight && grabbed->baseForm)
       {
                _MESSAGE("VRInputHandler: Close combat - force equipping RIGHT grabbed weapon");
          
  // Suppress pickup sound during internal re-equip
     EquipManager::s_suppressPickupSound = true;
   bool activated = SafeActivate(grabbed, player, 0, 0, 1, false);
     EquipManager::s_suppressPickupSound = false;
        if (activated)
   {
 bool isLeftGameHand = VRControllerToGameHand(false);
            ::EquipManager* equipMan = ::EquipManager::GetSingleton();
    if (equipMan)
  {
      BGSEquipSlot* slot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
      // Suppress draw sound during auto-equip
   EquipManager::s_suppressDrawSound = true;
   // Temporarily strip enchantment to prevent enchant VFX/sound
   TESObjectWEAP* weap = DYNAMIC_CAST(grabbed->baseForm, TESForm, TESObjectWEAP);
   EnchantmentItem* cachedEnchant = nullptr;
   if (weap && weap->enchantable.enchantment)
   {
       cachedEnchant = weap->enchantable.enchantment;
       weap->enchantable.enchantment = nullptr;
   }

   CALL_MEMBER_FN(equipMan, EquipItem)(player, grabbed->baseForm, nullptr, 1, slot, false, true, false, nullptr);

   // Restore enchantment immediately
   if (weap && cachedEnchant)
   {
       weap->enchantable.enchantment = cachedEnchant;
   }
       EquipManager::s_suppressDrawSound = false;
                 _MESSAGE("VRInputHandler: Force equipped weapon to %s game hand", isLeftGameHand ? "LEFT" : "RIGHT");
          }
}
         
         m_autoEquipPendingRight = false;
       m_autoEquipTimerRight = 0.0f;
        m_autoEquipWeaponRight = nullptr;
      }
 }
        
        // Also check collision avoidance grabbed weapons (blade collision)
        // Collision avoidance hand determined by INI setting
        bool offHandIsLeft = GetCollisionAvoidanceHandIsLeft();
   bool offHandVRControllerIsLeft = GameHandToVRController(offHandIsLeft);
     TESObjectREFR* bladeDropped = EquipManager::GetSingleton()->GetDroppedWeaponRef(offHandIsLeft);
        if (bladeDropped && higgsInterface)
        {
            TESObjectREFR* grabbed = higgsInterface->GetGrabbedObject(offHandVRControllerIsLeft);
            if (grabbed == bladeDropped && grabbed->baseForm)
            {
                _MESSAGE("VRInputHandler: Close combat - force equipping %s collision-avoidance weapon", offHandIsLeft ? "LEFT" : "RIGHT");

                // Suppress pickup sound during internal re-equip
                EquipManager::s_suppressPickupSound = true;
                bool activated = SafeActivate(grabbed, player, 0, 0, 1, false);
                EquipManager::s_suppressPickupSound = false;
                if (activated)
                {
                    // Re-equip using cached FormID (suppress draw sound)
                    EquipManager::s_suppressDrawSound = true;
                    if (offHandIsLeft)
                        EquipManager::GetSingleton()->ForceReequipLeftHand();
                    else
                        EquipManager::GetSingleton()->ForceReequipRightHand();
                    EquipManager::s_suppressDrawSound = false;
                }

                // Clear collision avoidance state
                EquipManager::GetSingleton()->ClearDroppedWeaponRef(offHandIsLeft);
                EquipManager::GetSingleton()->ClearPendingReequip(offHandIsLeft);
                m_higgsCollisionActive = false;
                m_pendingReequip = false;
            }
        }
        
// Shield collision: weapon hand is LEFT in left-handed, RIGHT in right-handed
bool weaponHandIsLeft = IsLeftHandedMode();
bool weaponVRControllerIsLeft = GameHandToVRController(weaponHandIsLeft);
TESObjectREFR* shieldDropped = EquipManager::GetSingleton()->GetDroppedWeaponRef(weaponHandIsLeft);
if (shieldDropped && higgsInterface)
{
    TESObjectREFR* grabbed = higgsInterface->GetGrabbedObject(weaponVRControllerIsLeft);
    if (grabbed == shieldDropped && grabbed->baseForm)
    {
        _MESSAGE("VRInputHandler: Close combat - force equipping %s collision-avoidance weapon", weaponHandIsLeft ? "LEFT" : "RIGHT");

        // Suppress pickup sound during internal re-equip
        EquipManager::s_suppressPickupSound = true;
        bool activated = SafeActivate(grabbed, player, 0, 0, 1, false);
        EquipManager::s_suppressPickupSound = false;
        if (activated)
        {
            // Re-equip using cached FormID (suppress draw sound)
            EquipManager::s_suppressDrawSound = true;
            if (weaponHandIsLeft)
                EquipManager::GetSingleton()->ForceReequipLeftHand();
            else
                EquipManager::GetSingleton()->ForceReequipRightHand();
            EquipManager::s_suppressDrawSound = false;
        }

        // Clear collision avoidance state
        EquipManager::GetSingleton()->ClearDroppedWeaponRef(weaponHandIsLeft);
        EquipManager::GetSingleton()->ClearPendingReequip(weaponHandIsLeft);
        m_shieldCollisionActive = false;
        m_pendingReequipRight = false;
    }
}
    }

    void VRInputHandler::OnShieldBash()
    {
        // Check if shield bash tracking is enabled
      if (!shieldBashEnabled)
        {
     return;
 }

        
        // Ignore if lockout is active
     if (m_shieldBashLockoutActive)
        {
       _MESSAGE("VRInputHandler: Shield bash detected but LOCKOUT is active (%.0f sec remaining)",
         shieldBashLockoutDuration - m_shieldBashLockoutTimer);
  return;
        }
  
        // If this is the first bash, start the window timer
        if (m_shieldBashCount == 0)
     {
            m_shieldBashWindowTimer = 0.0f;
        }
  
        m_shieldBashCount++;
        _MESSAGE("VRInputHandler: === SHIELD BASH DETECTED === Count: %d/%d (Window: %.1f/%.1f sec)",
            m_shieldBashCount, shieldBashThreshold, m_shieldBashWindowTimer, shieldBashWindow);
   
     // Check if threshold reached
        if (m_shieldBashCount >= shieldBashThreshold)
 {
            _MESSAGE("VRInputHandler: *** SHIELD BASH THRESHOLD REACHED *** %d bashes in %.1f seconds!",
         shieldBashThreshold, m_shieldBashWindowTimer);
      _MESSAGE("VRInputHandler: *** LOCKOUT ACTIVATED *** Duration: %.0f seconds",
      shieldBashLockoutDuration);
     
            // Cast spell on player (Skyrim.esm 0x000AA026)
          const UInt32 SHIELD_BASH_SPELL_FORM_ID = 0x000AA026;
   _MESSAGE("VRInputHandler: Casting shield bash spell %08X on player", SHIELD_BASH_SPELL_FORM_ID);
  CastSpellOnPlayer(SHIELD_BASH_SPELL_FORM_ID);

      // Activate lockout
            m_shieldBashLockoutActive = true;
         m_shieldBashLockoutTimer = 0.0f;
            m_shieldBashCount = 0;
     m_shieldBashWindowTimer = 0.0f;
        }
    }

    void VRInputHandler::OnWeaponSwing(bool isLeftHand, TESForm* weapon)
    {
     // Stub implementation - can be expanded later for swing detection
        // Currently not used
    }

    void VRInputHandler::UpdateShieldBashTracking(float deltaTime)
    {
        // Update lockout timer if active
        if (m_shieldBashLockoutActive)
      {
     m_shieldBashLockoutTimer += deltaTime;
  
        // Log progress every 30 seconds
            static float lockoutLogTimer = 0.0f;
   lockoutLogTimer += deltaTime;
     if (lockoutLogTimer >= 30.0f)
         {
         lockoutLogTimer = 0.0f;
      float remaining = shieldBashLockoutDuration - m_shieldBashLockoutTimer;
       _MESSAGE("VRInputHandler: Shield bash lockout: %.0f sec remaining (%.0f sec elapsed)",
   remaining, m_shieldBashLockoutTimer);
  }
            
  if (m_shieldBashLockoutTimer >= shieldBashLockoutDuration)
  {
   m_shieldBashLockoutActive = false;
        m_shieldBashLockoutTimer = 0.0f;
      lockoutLogTimer = 0.0f;
   _MESSAGE("VRInputHandler: *** SHIELD BASH LOCKOUT EXPIRED *** Bash tracking resumed");
 }
       return;
        }
     
   // Update window timer if we have bashes counted
        if (m_shieldBashCount > 0)
   {
     m_shieldBashWindowTimer += deltaTime;
   
   // Reset if window expired without reaching threshold
   if (m_shieldBashWindowTimer >= shieldBashWindow)
    {
           _MESSAGE("VRInputHandler: Shield bash window expired. Count was %d/%d - resetting",
   m_shieldBashCount, shieldBashThreshold);
      m_shieldBashCount = 0;
     m_shieldBashWindowTimer = 0.0f;
    }
        }
    }

    void VRInputHandler::OnGrabbed(bool isLeftVRController, TESObjectREFR* grabbedRefr)
    {
        VRInputHandler* handler = GetSingleton();

     // Convert VR controller to game hand
        bool isLeftGameHand = VRControllerToGameHand(isLeftVRController);

     const char* vrControllerName = isLeftVRController ? "Left" : "Right";
        const char* gameHandName = isLeftGameHand ? "Left" : "Right";

        if (grabbedRefr)
        {
    _MESSAGE("VRInputHandler: GRAB event - %s VR controller (game %s hand) grabbed object (FormID: %08X)", 
  vrControllerName, gameHandName, grabbedRefr->formID);

            // Get the base form to check what type of object was grabbed
          TESForm* baseForm = grabbedRefr->baseForm;
         if (baseForm)
         {
   _MESSAGE("VRInputHandler:   Base form type: %d, FormID: %08X", 
         baseForm->formType, baseForm->formID);
    
         // Check if grabbed object is a weapon
      if (baseForm->formType == kFormType_Weapon)
     {
    // Check if this is a valid 1H weapon we track (not 2H, bows, staffs, bound weapons, excluded items)
       if (!EquipManager::IsWeapon(baseForm))
   {
     _MESSAGE("VRInputHandler: Grabbed weapon is excluded type (2H/bow/staff/bound/etc) - not registering with trigger system");
  return;
    }
  
     // Check if this grab is from our collision avoidance system
        // If so, skip auto-equip - our system will handle it
  bool isFromCollisionAvoidance = EquipManager::GetSingleton()->HasPendingReequip(isLeftGameHand);
    
    if (isFromCollisionAvoidance)
          {
  _MESSAGE("VRInputHandler: Grabbed weapon is from collision avoidance - skipping auto-equip");
    return;
    }
   
  // Check if the target game hand has a spell/scroll equipped
            // If so, don't auto-equip - player wants to keep their spell
            PlayerCharacter* player = *g_thePlayer;
            if (player)
            {
      TESForm* currentlyEquipped = player->GetEquippedObject(isLeftGameHand);
     if (currentlyEquipped)
             {
            // Check for spell or scroll
        if (currentlyEquipped->formType == kFormType_Spell || 
                 currentlyEquipped->formType == kFormType_ScrollItem)
       {
   _MESSAGE("VRInputHandler: Target hand has SPELL/SCROLL equipped - not auto-equipping grabbed weapon");
      return;
      }
                }
          }
   
         // ============================================
     // AUTO-EQUIP GRABBED 1H WEAPON
     // This will trigger OnEquip which will then HIGGS grab it
      // (unless trigger is held) - registering it with our trigger system
       // ============================================
    _MESSAGE("VRInputHandler: Grabbed valid 1H weapon - auto-equipping to register with trigger system");
          _MESSAGE("VRInputHandler: Auto-equipping to %s VR hand (game %s hand)", 
      vrControllerName, gameHandName);
        
  // Activate (pick up) the grabbed weapon - add to inventory
         if (player)
     {
             // Suppress pickup sound since we're doing internal equip
         EquipManager::s_suppressPickupSound = true;
    SafeActivate(grabbedRefr, player, 0, 0, 1, false);
     EquipManager::s_suppressPickupSound = false;
  
  // Store info for delayed equip using existing system
         UInt32 weaponFormID = baseForm->formID;
     
   // Use the auto-equip timer system
   if (isLeftVRController)
     {
  handler->m_autoEquipPendingLeft = true;
handler->m_autoEquipTimerLeft = 0.0f;
   handler->m_autoEquipWeaponLeft = nullptr;  // Ref was activated, use FormID
          handler->m_autoEquipFormIDLeft = weaponFormID;
    }
        else
           {
     handler->m_autoEquipPendingRight = true;
     handler->m_autoEquipTimerRight = 0.0f;
   handler->m_autoEquipWeaponRight = nullptr;  // Ref was activated, use FormID
             handler->m_autoEquipFormIDRight = weaponFormID;
        }
   }
   }
   }
        }
  else
  {
 _MESSAGE("VRInputHandler: GRAB event - %s VR controller (game %s hand) (null reference)", 
                vrControllerName, gameHandName);
        }
    }

    void VRInputHandler::OnDropped(bool isLeftVRController, TESObjectREFR* droppedRefr)
    {
   if (!droppedRefr)
            return;

    // Convert VR controller to game hand
        bool isLeftGameHand = VRControllerToGameHand(isLeftVRController);

  _MESSAGE("VRInputHandler: DROP event - %s VR controller (game %s hand) dropped object (FormID: %08X)", 
            isLeftVRController ? "Left" : "Right",
            isLeftGameHand ? "Left" : "Right",
        droppedRefr->formID);

 VRInputHandler* handler = GetSingleton();
        
        // ============================================
 // Determine DROP REASON for logging/tracking
        // ============================================
        bool isAutoEquipWeapon = false;
        bool isCollisionAvoidanceWeapon = false;
        
        // Check if this was a weapon we were waiting to auto-equip
        if (isLeftVRController && handler->m_autoEquipWeaponLeft == droppedRefr)
        {
isAutoEquipWeapon = true;
            _MESSAGE("VRInputHandler: === ACCIDENTAL DROP DETECTED (LEFT) ===");
            _MESSAGE("VRInputHandler:   Weapon was pending auto-equip (grabbed from world)");
    _MESSAGE("VRInputHandler:   Cause: Player released grip OR physics collision knocked it away");
            
   handler->m_autoEquipPendingLeft = false;
            handler->m_autoEquipTimerLeft = 0.0f;
    handler->m_autoEquipWeaponLeft = nullptr;
        }
        else if (!isLeftVRController && handler->m_autoEquipWeaponRight == droppedRefr)
        {
            isAutoEquipWeapon = true;
   _MESSAGE("VRInputHandler: === ACCIDENTAL DROP DETECTED (RIGHT) ===");
 _MESSAGE("VRInputHandler:   Weapon was pending auto-equip (grabbed from world)");
            _MESSAGE("VRInputHandler:   Cause: Player released grip OR physics collision knocked it away");
     
 handler->m_autoEquipPendingRight = false;
            handler->m_autoEquipTimerRight = 0.0f;
 handler->m_autoEquipWeaponRight = nullptr;
        }

        // Check if this is the weapon we were tracking for collision avoidance
    TESObjectREFR* trackedWeapon = EquipManager::GetSingleton()->GetDroppedWeaponRef(isLeftGameHand);
    
      if (trackedWeapon && droppedRefr == trackedWeapon)
        {
     isCollisionAvoidanceWeapon = true;
       _MESSAGE("VRInputHandler: === ACCIDENTAL DROP DETECTED (Collision Avoidance Weapon) ===");
 _MESSAGE("VRInputHandler:   Weapon was grabbed by our collision avoidance system");
  _MESSAGE("VRInputHandler:   Cause: Player released grip OR physics collision knocked it away");
            _MESSAGE("VRInputHandler:   Game hand: %s", isLeftGameHand ? "Left" : "Right");
   
       // IMMEDIATELY teleport weapon to hand and force re-grab
      if (higgsInterface)
   {
       PlayerCharacter* player = *g_thePlayer;
      if (player)
     {
               NiNode* rootNode = player->GetNiRootNode(0);
              if (!rootNode)
           rootNode = player->GetNiRootNode(1);
          
  if (rootNode)
    {
                 // Get the VR controller hand node position
    const char* handNodeName = isLeftVRController ? "NPC L Hand [LHnd]" : "NPC R Hand [RHnd]";
 BSFixedString handNodeStr(handNodeName);
         NiAVObject* handNode = rootNode->GetObjectByName(&handNodeStr.data);
           
  if (handNode)
         {
    // Teleport weapon directly to hand position
NiPoint3 handPos = handNode->m_worldTransform.pos;
         droppedRefr->pos = handPos;
       
          // Also update the NiNode position if it exists
       NiNode* weaponNode = droppedRefr->GetNiNode();
      if (weaponNode)
              {
    weaponNode->m_worldTransform.pos = handPos;
        }
          
 _MESSAGE("VRInputHandler: Teleported weapon to hand (%.1f, %.1f, %.1f)",
                  handPos.x, handPos.y, handPos.z);
   }
    }
         }
      
    // Force HIGGS to grab it immediately
    _MESSAGE("VRInputHandler: Force re-grabbing with %s VR controller", 
       isLeftVRController ? "LEFT" : "RIGHT");
    higgsInterface->GrabObject(droppedRefr, isLeftVRController);
    
 // Don't clear tracking state or return - let it continue to be monitored
      // but don't do the normal drop handling below
         return;
   }
     }

        // ============================================
      // Handle re-grab attempt for accidental drops
        // ============================================
     if (droppedRefr->baseForm && droppedRefr->baseForm->formType == kFormType_Weapon)
        {
         const PlayerEquipState& equipState = EquipManager::GetSingleton()->GetEquipState();
            bool otherHandHasWeapon = isLeftGameHand ? 
    equipState.rightHand.isEquipped : equipState.leftHand.isEquipped;
   
        if (otherHandHasWeapon && higgsInterface)
            {
          _MESSAGE("VRInputHandler: Weapon dropped while other hand has weapon - attempting immediate re-grab");
   
            // Check if the hand can grab an object right now
    if (higgsInterface->CanGrabObject(isLeftVRController))
       {
         // Use HIGGS to grab the weapon again
        higgsInterface->GrabObject(droppedRefr, isLeftVRController);
         
       _MESSAGE("VRInputHandler: Re-grab command sent for %s VR controller", 
           isLeftVRController ? "LEFT" : "RIGHT");
         return;
 }
        else
            {
        _MESSAGE("VRInputHandler: Cannot re-grab - hand not in grabbable state");
          }
            }
        }

    // ============================================
    // Clear tracking state for collision avoidance weapon
        // ============================================
        if (isCollisionAvoidanceWeapon)
        {
            _MESSAGE("VRInputHandler: Clearing collision avoidance tracking for game %s hand", 
       isLeftGameHand ? "Left" : "Right");
  
     // Clear the dropped weapon reference (by game hand)
            EquipManager::GetSingleton()->ClearDroppedWeaponRef(isLeftGameHand);
            
         // Clear the pending re-equip (by game hand)
    EquipManager::GetSingleton()->ClearPendingReequip(isLeftGameHand);
   
      // Clear the cached FormID (by game hand)
    EquipManager::GetSingleton()->ClearCachedWeaponFormID(isLeftGameHand);
    
      // Clear collision state in VRInputHandler
     if (isLeftGameHand)
            {
          handler->m_higgsCollisionActive = false;
           handler->m_wasHiggsCollisionActive = false;
      handler->m_timeSinceLastCollision = 0.0f;
         handler->m_pendingReequip = false;
handler->m_pendingReequipTimer = 0.0f;
            }
         else
  {
          handler->m_shieldCollisionActive = false;
       handler->m_wasShieldCollisionActive = false;
          handler->m_timeSinceLastShieldCollision = 0.0f;
      handler->m_pendingReequipRight = false;
  handler->m_pendingReequipRightTimer = 0.0f;
  }
         
 _MESSAGE("VRInputHandler: Cleared all tracking state for game %s hand - weapon was dropped", 
    isLeftGameHand ? "Left" : "Right");
        }
        
    // Log if this was a completely untracked weapon drop (not from our systems)
if (!isAutoEquipWeapon && !isCollisionAvoidanceWeapon && 
 droppedRefr->baseForm && droppedRefr->baseForm->formType == kFormType_Weapon)
{
        _MESSAGE("VRInputHandler: Untracked weapon dropped (not managed by FalseEdgeVR)");
  }
}

    void VRInputHandler::OnPulled(bool isLeftVRController, TESObjectREFR* pulledRefr)
    {
VRInputHandler* handler = GetSingleton();

        if (!handler->IsListening())
    return;

        // Convert VR controller to game hand
        bool isLeftGameHand = VRControllerToGameHand(isLeftVRController);
        const char* vrControllerName = isLeftVRController ? "Left" : "Right";
        const char* gameHandName = isLeftGameHand ? "Left" : "Right";

   if (pulledRefr)
        {
            LOG("VRInputHandler: PULL event - %s VR controller (game %s hand) pulled object (FormID: %08X)", 
 vrControllerName, gameHandName, pulledRefr->formID);
  }
        else
        {
     LOG("VRInputHandler: PULL event - %s VR controller (game %s hand) (null reference)", 
         vrControllerName, gameHandName);
        }
    }

    void VRInputHandler::OnCollision(bool isLeftVRController, float mass, float separatingVelocity)
    {
        VRInputHandler* handler = GetSingleton();

        if (!handler->IsListening())
      return;

        // Convert VR controller to game hand
        bool isLeftGameHand = VRControllerToGameHand(isLeftVRController);

        // Check if this is a collision involving our grabbed weapon
        if (EquipManager::GetSingleton()->HasPendingReequip(isLeftGameHand))
        {
            handler->OnHiggsCollisionDetected(isLeftGameHand);
        }

        // Check for shield bash - high velocity collision from weapon hitting shield
             // Detect when weapon hand collides while shield hand has shield
        const PlayerEquipState& equipState = EquipManager::GetSingleton()->GetEquipState();

        // In left-handed mode: shield = RIGHT, weapon = LEFT
          // In right-handed mode: shield = LEFT, weapon = RIGHT
        bool shieldHandIsLeft = !IsLeftHandedMode();
        bool weaponHandIsLeft = IsLeftHandedMode();
        bool weaponVRControllerIsLeft = GameHandToVRController(weaponHandIsLeft);

        bool hasShield = shieldHandIsLeft ?
            (equipState.leftHand.type == WeaponType::Shield) :
            (equipState.rightHand.type == WeaponType::Shield);
        bool weaponHandHasWeapon = weaponHandIsLeft ?
            equipState.leftHand.isEquipped : equipState.rightHand.isEquipped;

        // Also check if HIGGS is holding a grabbed weapon in weapon VR controller
        bool weaponHandGrabbedWeapon = false;
        if (higgsInterface)
        {
            TESObjectREFR* grabbed = higgsInterface->GetGrabbedObject(weaponVRControllerIsLeft);
            if (grabbed && grabbed->baseForm && grabbed->baseForm->formType == kFormType_Weapon)
            {
                weaponHandGrabbedWeapon = true;
            }
        }

        // Shield bash detection: collision from weapon hand while shield hand has shield
    // The collision comes from the weapon hitting the shield
        bool collisionFromWeaponHand = (isLeftGameHand == weaponHandIsLeft);
        if (collisionFromWeaponHand && hasShield && (weaponHandHasWeapon || weaponHandGrabbedWeapon) && separatingVelocity > 3.0f)
        {
            _MESSAGE("VRInputHandler: Potential SHIELD BASH - Weapon hit shield! Velocity: %.1f, Mass: %.1f",
                separatingVelocity, mass);
            _MESSAGE("VRInputHandler:   Weapon hand equipped: %s, Weapon hand grabbed: %s",
                weaponHandHasWeapon ? "YES" : "NO", weaponHandGrabbedWeapon ? "YES" : "NO");
            handler->OnShieldBash();
        }

      // Only log significant collisions to avoid spam
        if (separatingVelocity > 1.0f)
        {
            const char* vrControllerName = isLeftVRController ? "Left" : "Right";
            const char* gameHandName = isLeftGameHand ? "Left" : "Right";
      LOG("VRInputHandler: COLLISION event - %s VR controller (game %s hand), mass: %.2f, velocity: %.2f", 
      vrControllerName, gameHandName, mass, separatingVelocity);
        }
    }

    void VRInputHandler::OnHiggsCollisionDetected(bool isLeftGameHand)
    {
  if (isLeftGameHand)
        {
    if (!m_higgsCollisionActive)
     {
   m_higgsCollisionActive = true;
      _MESSAGE("VRInputHandler: === GRABBED WEAPON (game LEFT hand) TOUCHING EQUIPPED WEAPON ===");
            }
   m_timeSinceLastCollision = 0.0f;
        }
  else
        {
            if (!m_shieldCollisionActive)
{
             m_shieldCollisionActive = true;
         _MESSAGE("VRInputHandler: === GRABBED WEAPON (game RIGHT hand) TOUCHING SHIELD ===");
  }
      m_timeSinceLastShieldCollision = 0.0f;
     }
    }

    void VRInputHandler::CheckCollisionTimeout(float deltaTime)
    {
        // Skip collision avoidance logic if in close combat mode
              // Skip collision avoidance logic if in close combat mode
        if (m_closeCombatMode)
            return;

        // Skip blade collision logic if player has a shield - shield collision handles that case
        if (ShieldCollisionTracker::GetSingleton()->HasShieldEquipped())
            return;

        // Determine collision avoidance hand from INI setting
        bool offHandIsLeft = GetCollisionAvoidanceHandIsLeft();
        bool offHandVRControllerIsLeft = GameHandToVRController(offHandIsLeft);

        if (!EquipManager::GetSingleton()->HasPendingReequip(offHandIsLeft))
        {
            m_higgsCollisionActive = false;
            m_wasHiggsCollisionActive = false;
            m_timeSinceLastCollision = 0.0f;
            return;
        }

     // TRIGGER OVERRIDE: If trigger is held on EITHER hand, immediately re-equip the weapon
  // This allows the player to force the weapon back to equipped state
        bool leftTrig = IsLeftTriggerPressed();
        bool rightTrig = IsRightTriggerPressed();
        
        if (leftTrig || rightTrig)
        {
 TESObjectREFR* triggerDroppedWeapon = EquipManager::GetSingleton()->GetDroppedWeaponRef(offHandIsLeft);
          if (triggerDroppedWeapon && triggerDroppedWeapon->baseForm)
    {
     _MESSAGE("VRInputHandler: TRIGGER HELD - forcing immediate re-equip of grabbed weapon (Left=%s, Right=%s)",
    leftTrig ? "YES" : "NO", rightTrig ? "YES" : "NO");
            
    // Check if this was a dual-wield same weapon situation
 bool wasDualWieldingSame = EquipManager::GetSingleton()->WasDualWieldingSameWeapon(offHandIsLeft);

      if (wasDualWieldingSame)
   {
          // For dual-wield same weapon: DON'T activate (which would add duplicate to inventory)
       // The re-equip will use the existing inventory item via cached FormID
           _MESSAGE("VRInputHandler: Dual-wield same weapon - SKIPPING activation (will re-equip from existing inventory)");
        _MESSAGE("VRInputHandler: Deleting spawned weapon (RefID: %08X) from world", triggerDroppedWeapon->formID);
         
     // Delete the spawned world object so it doesn't accumulate on the ground
 DeleteWorldObject(triggerDroppedWeapon);
      }
      else
        {
    // Normal case (different weapons): activate to add to inventory
         _MESSAGE("VRInputHandler: Activating grabbed weapon to add to inventory (RefID: %08X, BaseID: %08X)...",
              triggerDroppedWeapon->formID, triggerDroppedWeapon->baseForm->formID);
      
            PlayerCharacter* player = *g_thePlayer;
        if (player)
     {
          // Suppress pickup sound during internal re-equip
       EquipManager::s_suppressPickupSound = true;
           bool activated = SafeActivate(triggerDroppedWeapon, player, 0, 0, 1, false);
          EquipManager::s_suppressPickupSound = false;
     _MESSAGE("VRInputHandler: Activate result: %s", activated ? "SUCCESS" : "FAILED");
             }
       }
          
     // Clear the dropped weapon ref but KEEP the cached FormID for re-equip!
    // Collision avoidance hand from INI setting
    bool offHandIsLeft = GetCollisionAvoidanceHandIsLeft();
  EquipManager::GetSingleton()->ClearDroppedWeaponRef(offHandIsLeft);
 
    // Also clear PendingReequip so CheckCollisionTimeout doesn't clear cached FormID
        EquipManager::GetSingleton()->ClearPendingReequip(offHandIsLeft);
   
         m_pendingReequip = true;
          m_pendingReequipIsLeft = offHandIsLeft;
          m_pendingReequipTimer = 0.0f;
  _MESSAGE("VRInputHandler: Scheduled re-equip for %s hand in %.1f ms (TRIGGER OVERRIDE)", 
           offHandIsLeft ? "left" : "right", reequipDelay * 1000.0f);
      
  // Clear collision state
         m_higgsCollisionActive = false;
        m_wasHiggsCollisionActive = false;
   m_timeSinceLastCollision = 0.0f;
     return;
       }
        }

        TESObjectREFR* droppedWeapon = EquipManager::GetSingleton()->GetDroppedWeaponRef(offHandIsLeft);
        if (!droppedWeapon)
        {
            _MESSAGE("CheckCollisionTimeout: Dropped weapon ref is NULL - clearing state");
            EquipManager::GetSingleton()->ClearPendingReequip(offHandIsLeft);
            EquipManager::GetSingleton()->ClearDroppedWeaponRef(offHandIsLeft);
            EquipManager::GetSingleton()->ClearCachedWeaponFormID(offHandIsLeft);
            m_higgsCollisionActive = false;
            m_wasHiggsCollisionActive = false;
            m_timeSinceLastCollision = 0.0f;
            return;
        }

        if (higgsInterface)
        {
            TESObjectREFR* higgsHeld = higgsInterface->GetGrabbedObject(offHandVRControllerIsLeft);
      if (higgsHeld != droppedWeapon)
   {
            // HIGGS is not holding our weapon - but give it a few frames to actually grab
            // Track how long HIGGS has NOT been holding it
            static float higgsGrabWaitTime = 0.0f;
  static TESObjectREFR* lastDroppedWeapon = nullptr;
         
       // Reset wait time if this is a new dropped weapon
            if (lastDroppedWeapon != droppedWeapon)
            {
     higgsGrabWaitTime = 0.0f;
        lastDroppedWeapon = droppedWeapon;
            }
            
      higgsGrabWaitTime += deltaTime;
          
   // Only clear state if HIGGS hasn't grabbed after 0.3 seconds
     // This gives HIGGS time to actually grab the object
     if (higgsGrabWaitTime >= 0.3f)
     {
        _MESSAGE("CheckCollisionTimeout: HIGGS not holding our weapon after 0.3s (held: %p, dropped: %p) - clearing state",
        higgsHeld, droppedWeapon);
     EquipManager::GetSingleton()->ClearPendingReequip(offHandIsLeft);
         EquipManager::GetSingleton()->ClearDroppedWeaponRef(offHandIsLeft);
   EquipManager::GetSingleton()->ClearCachedWeaponFormID(offHandIsLeft);
    m_higgsCollisionActive = false;
  m_wasHiggsCollisionActive = false;
    m_timeSinceLastCollision = 0.0f;
 higgsGrabWaitTime = 0.0f;
       lastDroppedWeapon = nullptr;
     }
    return;
        }
        else
        {
     // HIGGS is holding our weapon - reset the wait timer
    static float higgsGrabWaitTime = 0.0f;
    static TESObjectREFR* lastDroppedWeapon = nullptr;
     higgsGrabWaitTime = 0.0f;
         lastDroppedWeapon = nullptr;
        }
   }

        float currentDistance = GetGrabbedToEquippedDistance(offHandVRControllerIsLeft);
  bool bladesClose = (currentDistance < bladeReequipThreshold);
        
        static int logCounter = 0;
   logCounter++;
if (logCounter % 100 == 0)
        {
            _MESSAGE("CheckCollisionTimeout: Distance=%.2f, Threshold=%.2f, BladesClose=%s, Timer=%.3f, Timeout=%.3f",
    currentDistance, bladeReequipThreshold, bladesClose ? "YES" : "NO", 
m_timeSinceLastCollision, bladeCollisionTimeout);
        }
   
        if (bladesClose)
        {
       m_timeSinceLastCollision = 0.0f;
            m_higgsCollisionActive = true;
        }
        else
        {
 m_timeSinceLastCollision += deltaTime;
        
       if (m_timeSinceLastCollision >= bladeCollisionTimeout)
    {
     m_higgsCollisionActive = false;
                _MESSAGE("VRInputHandler: === GRABBED WEAPON SAFE TO RE-EQUIP ===");
   _MESSAGE("VRInputHandler: Time separated: %.3f sec, Distance: %.2f (threshold: %.2f)", 
   m_timeSinceLastCollision, currentDistance, bladeReequipThreshold);
  
   droppedWeapon = EquipManager::GetSingleton()->GetDroppedWeaponRef(offHandIsLeft);
        if (droppedWeapon && droppedWeapon->baseForm)
        {
            // Check if this was a dual-wield same weapon situation
            bool wasDualWieldingSame = EquipManager::GetSingleton()->WasDualWieldingSameWeapon(offHandIsLeft);

            if (wasDualWieldingSame)
            {
      // For dual-wield same weapon: DON'T activate (which would add duplicate to inventory)
       // The re-equip will use the existing inventory item via cached FormID
   _MESSAGE("VRInputHandler: Dual-wield same weapon - SKIPPING activation (will re-equip from existing inventory)");
      _MESSAGE("VRInputHandler: Deleting spawned weapon (RefID: %08X) from world", droppedWeapon->formID);
           
         // Delete the spawned world object so it doesn't accumulate on the ground
 DeleteWorldObject(droppedWeapon);
            }
          else
            {
 // Normal case (different weapons): activate to add to inventory
         _MESSAGE("VRInputHandler: Activating grabbed weapon to add to inventory (RefID: %08X, BaseID: %08X)...",
         droppedWeapon->formID, droppedWeapon->baseForm->formID);
     
                PlayerCharacter* player = *g_thePlayer;
          if (player)
           {
        // Suppress pickup sound during internal re-equip
          EquipManager::s_suppressPickupSound = true;
      bool activated = SafeActivate(droppedWeapon, player, 0, 0, 1, false);
           EquipManager::s_suppressPickupSound = false;
        _MESSAGE("VRInputHandler: Activate result: %s", activated ? "SUCCESS" : "FAILED");
      }
            }
        }
        else
    {
   _MESSAGE("VRInputHandler: WARNING - Dropped weapon ref became invalid before activation!");
    }
   
 // Clear the dropped weapon ref but KEEP the cached FormID for re-equip!
    // Collision avoidance hand from INI setting
    bool offHandIsLeft = GetCollisionAvoidanceHandIsLeft();
  EquipManager::GetSingleton()->ClearDroppedWeaponRef(offHandIsLeft);
   
    // Also clear PendingReequip so CheckCollisionTimeout doesn't clear cached FormID
        EquipManager::GetSingleton()->ClearPendingReequip(offHandIsLeft);
   
     m_pendingReequip = true;
  m_pendingReequipIsLeft = offHandIsLeft;
 m_pendingReequipTimer = 0.0f;
  _MESSAGE("VRInputHandler: Scheduled re-equip for %s hand in %.1f ms", 
      offHandIsLeft ? "left" : "right", reequipDelay * 1000.0f);
}
        }
    }

  void VRInputHandler::CheckPendingReequip(float deltaTime)
    {
        if (m_pendingReequip)
        {
            m_pendingReequipTimer += deltaTime;
  
       if (m_pendingReequipTimer >= reequipDelay)
     {
     _MESSAGE("VRInputHandler: Re-equipping weapon to %s hand after %.3f ms delay", 
 m_pendingReequipIsLeft ? "left" : "right", m_pendingReequipTimer * 1000.0f);
 
              m_pendingReequip = false;
         m_pendingReequipTimer = 0.0f;
 
            if (m_pendingReequipIsLeft)
      {
        // Suppress draw sound during collision re-equip
        EquipManager::s_suppressDrawSound = true;
     EquipManager::GetSingleton()->ForceReequipLeftHand();
         EquipManager::s_suppressDrawSound = false;
           m_leftHandOnCooldown = true;
m_leftHandCooldownTimer = 0.0f;
  _MESSAGE("VRInputHandler: Started %.0fms cooldown for left hand", bladeReequipCooldown * 1000.0f);
       }
       else
  {
        // Suppress draw sound during shield collision re-equip
        EquipManager::s_suppressDrawSound = true;
        EquipManager::GetSingleton()->ForceReequipRightHand();
   EquipManager::s_suppressDrawSound = false;
 m_rightHandOnCooldown = true;
 m_rightHandCooldownTimer = 0.0f;
          _MESSAGE("VRInputHandler: Started %.0fms cooldown for right hand (shield)", shieldReequipCooldown * 1000.0f);
        }
        }
        }
  
        if (m_pendingReequipRight)
        {
            m_pendingReequipRightTimer += deltaTime;

            if (m_pendingReequipRightTimer >= shieldReequipDelay)
            {
                bool weaponHandIsLeft = !ShieldCollisionTracker::GetSingleton()->IsShieldInLeftHand();
                _MESSAGE("VRInputHandler: Re-equipping weapon to %s hand after %.3f ms delay (shield collision)",
                    weaponHandIsLeft ? "LEFT" : "RIGHT", m_pendingReequipRightTimer * 1000.0f);

                m_pendingReequipRight = false;
                m_pendingReequipRightTimer = 0.0f;

                // Suppress draw sound during shield collision re-equip
                EquipManager::s_suppressDrawSound = true;
                if (weaponHandIsLeft)
                    EquipManager::GetSingleton()->ForceReequipLeftHand();
                else
                    EquipManager::GetSingleton()->ForceReequipRightHand();
                EquipManager::s_suppressDrawSound = false;
                if (weaponHandIsLeft)
                {
                    m_leftHandOnCooldown = true;
                    m_leftHandCooldownTimer = 0.0f;
                }
                else
                {
                    m_rightHandOnCooldown = true;
                    m_rightHandCooldownTimer = 0.0f;
                }
          _MESSAGE("VRInputHandler: Started %.0fms cooldown for right hand (shield)", shieldReequipCooldown * 1000.0f);
    }
     }
    }

    void VRInputHandler::CheckAutoEquipGrabbedWeapon(float deltaTime)
    {
        // ============================================
        // HANDLE FormID-BASED AUTO-EQUIP (grabbed from world)
        // These are weapons grabbed by HIGGS that we activated and want to equip
      // ============================================
    
        // Left hand FormID equip
     if (m_autoEquipPendingLeft && m_autoEquipFormIDLeft != 0)
        {
         m_autoEquipTimerLeft += deltaTime;
       
   if (m_autoEquipTimerLeft >= 0.05f)  // 50ms delay to let pickup complete
            {
     _MESSAGE("VRInputHandler: Auto-equipping grabbed weapon to LEFT game hand (FormID: %08X)", 
    m_autoEquipFormIDLeft);
  
     bool isLeftGameHand = VRControllerToGameHand(true);
      PlayerCharacter* player = *g_thePlayer;
      TESForm* weaponForm = LookupFormByID(m_autoEquipFormIDLeft);
            
    if (player && weaponForm)
    {
   ::EquipManager* equipMan = ::EquipManager::GetSingleton();
      if (equipMan)
    {
            BGSEquipSlot* slot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
   EquipManager::s_suppressDrawSound = true;
       CALL_MEMBER_FN(equipMan, EquipItem)(player, weaponForm, nullptr, 1, slot, false, true, false, nullptr);
  EquipManager::s_suppressDrawSound = false;
      _MESSAGE("VRInputHandler: Equipped grabbed weapon to %s game hand", 
              isLeftGameHand ? "LEFT" : "RIGHT");
     }
    }
     
      m_autoEquipPendingLeft = false;
   m_autoEquipTimerLeft = 0.0f;
              m_autoEquipFormIDLeft = 0;
       m_autoEquipWeaponLeft = nullptr;
          }
      return;  // Don't process old logic if we handled FormID equip
        }
 
     // Right hand FormID equip
        if (m_autoEquipPendingRight && m_autoEquipFormIDRight != 0)
    {
    m_autoEquipTimerRight += deltaTime;
      
            if (m_autoEquipTimerRight >= 0.05f)  // 50ms delay to let pickup complete
     {
     _MESSAGE("VRInputHandler: Auto-equipping grabbed weapon to RIGHT game hand (FormID: %08X)", 
       m_autoEquipFormIDRight);
    
          bool isLeftGameHand = VRControllerToGameHand(false);
      PlayerCharacter* player = *g_thePlayer;
 TESForm* weaponForm = LookupFormByID(m_autoEquipFormIDRight);
 
      if (player && weaponForm)
              {
        ::EquipManager* equipMan = ::EquipManager::GetSingleton();
          if (equipMan)
          {
             BGSEquipSlot* slot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
            EquipManager::s_suppressDrawSound = true;
          CALL_MEMBER_FN(equipMan, EquipItem)(player, weaponForm, nullptr, 1, slot, false, true, false, nullptr);
      EquipManager::s_suppressDrawSound = false;
        _MESSAGE("VRInputHandler: Equipped grabbed weapon to %s game hand", 
      isLeftGameHand ? "LEFT" : "RIGHT");
    }
 }
             
 m_autoEquipPendingRight = false;
     m_autoEquipTimerRight = 0.0f;
           m_autoEquipFormIDRight = 0;
          m_autoEquipWeaponRight = nullptr;
    }
   return;  // Don't process old logic if we handled FormID equip
        }
        
        // ============================================
        // LEGACY: HIGGS-held object auto-equip (old system, kept for compatibility)
        // This is for when we're still holding the world object via HIGGS
        // ============================================
    
        // If in close combat mode, weapons are force-equipped immediately
  // so skip the normal delayed auto-equip logic
        if (m_closeCombatMode)
      return;
         
        // Skip legacy logic if there's no weapon reference being tracked
     if (!m_autoEquipWeaponLeft && !m_autoEquipWeaponRight)
          return;
          
        // First, check if the OTHER hand still has a weapon equipped
     // If player manually unequipped their main hand weapon, cancel auto-equip
     const PlayerEquipState& equipState = EquipManager::GetSingleton()->GetEquipState();
        
if (m_autoEquipPendingLeft && m_autoEquipWeaponLeft)
        {
      // Left VR controller is holding grabbed weapon - check if RIGHT game hand still has weapon
            bool isLeftGameHand = VRControllerToGameHand(true);
 bool otherHandHasWeapon = isLeftGameHand ? 
   equipState.rightHand.isEquipped : equipState.leftHand.isEquipped;
  
       if (!otherHandHasWeapon)
 {
             _MESSAGE("VRInputHandler: Auto-equip cancelled for LEFT VR hand - other hand no longer has weapon equipped");
     m_autoEquipPendingLeft = false;
       m_autoEquipTimerLeft = 0.0f;
           m_autoEquipWeaponLeft = nullptr;
   }
            else if (!higgsInterface || higgsInterface->GetGrabbedObject(true) != m_autoEquipWeaponLeft)
  {
       _MESSAGE("VRInputHandler: Auto-equip cancelled for LEFT VR hand - weapon no longer held");
  m_autoEquipPendingLeft = false;
     m_autoEquipTimerLeft = 0.0f;
     m_autoEquipWeaponLeft = nullptr;
            }
  else
       {
        // Check distance between GRABBED weapon and EQUIPPED weapon in the other hand
     // This is the key fix - we need to check grabbed-to-equipped distance, not equipped-to-equipped
          float bladeDistance = GetGrabbedToEquippedDistance(true);  // true = left VR controller
        
                // Reset timer if blades are within imminent collision range (friction/sliding)
  if (bladeDistance < bladeImminentThreshold)
            {
       // Blades are close (friction range) - reset timer
   if (m_autoEquipTimerLeft > 0.0f)
      {
            static bool loggedReset = false;
     if (!loggedReset)
  {
       _MESSAGE("VRInputHandler: Auto-equip timer reset - grabbed weapon near equipped (dist: %.2f < %.2f)",
         bladeDistance, bladeImminentThreshold);
              loggedReset = true;
        }
    }
       m_autoEquipTimerLeft = 0.0f;
     }
       else
           {
            // Blades are far enough apart - increment timer
         static bool loggedReset = false;
     loggedReset = false;  // Reset log flag when blades separate
     
       m_autoEquipTimerLeft += deltaTime;
    
  if (m_autoEquipTimerLeft >= autoEquipGrabbedWeaponDelay)
        {
     _MESSAGE("VRInputHandler: Auto-equiping grabbed weapon to LEFT game hand after %.1f sec",
                   autoEquipGrabbedWeaponDelay);
 
             TESForm* weaponForm = m_autoEquipWeaponLeft->baseForm;
     if (weaponForm)
      {
   bool isLeftGameHand = VRControllerToGameHand(true);

            PlayerCharacter* player = *g_thePlayer;
    if (player)
      {
// Suppress pickup sound during internal re-equip
           EquipManager::s_suppressPickupSound = true;
 bool activated = SafeActivate(m_autoEquipWeaponLeft, player, 0, 0, 1, true);
       EquipManager::s_suppressPickupSound = false;
   _MESSAGE("VRInputHandler: Activate grabbed weapon result: %s", activated ? "SUCCESS" : "FAILED");
 
      if (activated)
      {
    ::EquipManager* equipMan = ::EquipManager::GetSingleton();
  if (equipMan)
   {
    BGSEquipSlot* slot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
    // Suppress draw sound during auto-equip
      EquipManager::s_suppressDrawSound = true;
      // Temporarily strip enchantment to prevent enchant VFX/sound
      TESObjectWEAP* weapEnch = DYNAMIC_CAST(weaponForm, TESForm, TESObjectWEAP);
      EnchantmentItem* cachedEnchantAuto = nullptr;
      if (weapEnch && weapEnch->enchantable.enchantment)
      {
          cachedEnchantAuto = weapEnch->enchantable.enchantment;
          weapEnch->enchantable.enchantment = nullptr;
      }

      CALL_MEMBER_FN(equipMan, EquipItem)(player, weaponForm, nullptr, 1, slot, false, true, false, nullptr);

      // Restore enchantment immediately
      if (weapEnch && cachedEnchantAuto)
      {
          weapEnch->enchantable.enchantment = cachedEnchantAuto;
      }
 EquipManager::s_suppressDrawSound = false;
   _MESSAGE("VRInputHandler: Equipped weapon to %s game hand (silent)",
isLeftGameHand ? "LEFT" : "RIGHT");
          
        // Start cooldown to prevent immediate collision detection re-triggering
   if (isLeftGameHand)
     {
         m_leftHandOnCooldown = true;
    m_leftHandCooldownTimer = 0.0f;
  _MESSAGE("VRInputHandler: Started %.0fms cooldown for left hand", bladeReequipCooldown * 1000.0f);
       }
        else
  {
  m_rightHandOnCooldown = true;
         m_rightHandCooldownTimer = 0.0f;
             _MESSAGE("VRInputHandler: Started %.0fms cooldown for right hand (auto-equip)", bladeReequipCooldown * 1000.0f);
     }
    }
    }
        }
   }
        
        m_autoEquipPendingLeft = false;
  m_autoEquipTimerLeft = 0.0f;
         m_autoEquipWeaponLeft = nullptr;
         }
    }
  }
        }
        
      if (m_autoEquipPendingRight && m_autoEquipWeaponRight)
        {
      // Right VR controller is holding grabbed weapon - check if LEFT game hand still has weapon
       bool isLeftGameHand = VRControllerToGameHand(false);
   bool otherHandHasWeapon = isLeftGameHand ? 
   equipState.rightHand.isEquipped : equipState.leftHand.isEquipped;
            
  if (!otherHandHasWeapon)
    {
       _MESSAGE("VRInputHandler: Auto-equipCancelled for RIGHT VR hand - other hand no longer has weapon equipped");
      m_autoEquipPendingRight = false;
      m_autoEquipTimerRight = 0.0f;
           m_autoEquipWeaponRight = nullptr;
 }
     else if (!higgsInterface || higgsInterface->GetGrabbedObject(false) != m_autoEquipWeaponRight)
       {
   _MESSAGE("VRInputHandler: Auto-equipCancelled for RIGHT VR hand - weapon no longer held");
    m_autoEquipPendingRight = false;
      m_autoEquipTimerRight = 0.0f;
    m_autoEquipWeaponRight = nullptr;
     }
      else
       {
  // Check distance between GRABBED weapon and EQUIPPED weapon in the other hand
 float bladeDistance = GetGrabbedToEquippedDistance(false);  // false = right VR controller
    
 // Reset timer if blades are within imminent collision range (friction/sliding)
         if (bladeDistance < bladeImminentThreshold)
   {
    // Blades are close (friction range) - reset timer
        if (m_autoEquipTimerRight > 0.0f)
         {
                 static bool loggedResetRight = false;
         if (!loggedResetRight)
      {
         _MESSAGE("VRInputHandler: Auto-equip timer reset (RIGHT) - grabbed weapon near equipped (dist: %.2f < %.2f)",
      bladeDistance, bladeImminentThreshold);
      loggedResetRight = true;
          }
         }
  m_autoEquipTimerRight = 0.0f;
     }
                else
       {
     // Blades are far enough apart - increment timer
       static bool loggedResetRight = false;
   loggedResetRight = false;  // Reset log flag when blades separate
           
          m_autoEquipTimerRight += deltaTime;
       
      if (m_autoEquipTimerRight >= autoEquipGrabbedWeaponDelay)
              {
    _MESSAGE("VRInputHandler: Auto-equiping grabbed weapon to RIGHT game hand after %.1f sec",
    autoEquipGrabbedWeaponDelay);
  
          TESForm* weaponForm = m_autoEquipWeaponRight->baseForm;
        if (weaponForm)
       {
         bool isLeftGameHand = VRControllerToGameHand(false);
   
  PlayerCharacter* player = *g_thePlayer;
          if (player)
  {
      // Suppress pickup sound during internal re-equip
         EquipManager::s_suppressPickupSound = true;
      bool activated = SafeActivate(m_autoEquipWeaponRight, player, 0, 0, 1, true);
         EquipManager::s_suppressPickupSound = false;
 _MESSAGE("VRInputHandler: Activate grabbed weapon result: %s", activated ? "SUCCESS" : "FAILED");
     
  if (activated)
   {
  ::EquipManager* equipMan = ::EquipManager::GetSingleton();
      if (equipMan)
        {
   BGSEquipSlot* slot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
   // Suppress draw sound during auto-equip
   EquipManager::s_suppressDrawSound = true;
   // Temporarily strip enchantment to prevent enchant VFX/sound
   TESObjectWEAP* weapEnch2 = DYNAMIC_CAST(weaponForm, TESForm, TESObjectWEAP);
   EnchantmentItem* cachedEnchantAuto2 = nullptr;
   if (weapEnch2 && weapEnch2->enchantable.enchantment)
   {
       cachedEnchantAuto2 = weapEnch2->enchantable.enchantment;
       weapEnch2->enchantable.enchantment = nullptr;
   }

   CALL_MEMBER_FN(equipMan, EquipItem)(player, weaponForm, nullptr, 1, slot, false, true, false, nullptr);

   // Restore enchantment immediately
   if (weapEnch2 && cachedEnchantAuto2)
   {
       weapEnch2->enchantable.enchantment = cachedEnchantAuto2;
   }
  EquipManager::s_suppressDrawSound = false;
           _MESSAGE("VRInputHandler: Equipped grabbed weapon to %s game hand (silent)",
   isLeftGameHand ? "LEFT" : "RIGHT");
     
       // Start cooldown to prevent immediate collision detection re-triggering
         if (isLeftGameHand)
  {
            m_leftHandOnCooldown = true;
           m_leftHandCooldownTimer = 0.0f;
   _MESSAGE("VRInputHandler: Started %.0fms cooldown for left hand (auto-equip)", bladeReequipCooldown * 1000.0f);
        }
       else
  {
           m_rightHandOnCooldown = true;
    m_rightHandCooldownTimer = 0.0f;
   _MESSAGE("VRInputHandler: Started %.0fms cooldown for right hand (auto-equip)", bladeReequipCooldown * 1000.0f);
      }
   }
   }
      }
      }
      
      m_autoEquipPendingRight = false;
   m_autoEquipTimerRight = 0.0f;
     m_autoEquipWeaponRight = nullptr;
         }
             }
   }
      }
    }

    float VRInputHandler::GetCurrentBladeDistance() const
    {
      WeaponGeometryTracker* tracker = WeaponGeometryTracker::GetSingleton();
    if (!tracker)
      return 9999.0f;
        
      const BladeGeometry& leftGeom = tracker->GetBladeGeometry(true);
  const BladeGeometry& rightGeom = tracker->GetBladeGeometry(false);
 
        if (!leftGeom.isValid || !rightGeom.isValid)
            return 9999.0f;
     
        NiPoint3 leftMid, rightMid;
    leftMid.x = (leftGeom.basePosition.x + leftGeom.tipPosition.x) * 0.5f;
 leftMid.y = (leftGeom.basePosition.y + leftGeom.tipPosition.y) * 0.5f;
        leftMid.z = (leftGeom.basePosition.z + leftGeom.tipPosition.z) * 0.5f;
        
    rightMid.x = (rightGeom.basePosition.x + rightGeom.tipPosition.x) * 0.5f;
        rightMid.y = (rightGeom.basePosition.y + rightGeom.tipPosition.y) * 0.5f;
    rightMid.z = (rightGeom.basePosition.z + rightGeom.tipPosition.z) * 0.5f;
 
        float dx = leftMid.x - rightMid.x;
   float dy = leftMid.y - rightMid.y;
      float dz = leftMid.z - rightMid.z;
        
 return sqrt(dx*dx + dy*dy + dz*dz);
    }

  float VRInputHandler::GetGrabbedToEquippedDistance(bool isLeftVRController) const
    {
        // Get the HIGGS grabbed weapon position
  if (!higgsInterface)
        return 9999.0f;
        
        TESObjectREFR* grabbedWeapon = higgsInterface->GetGrabbedObject(isLeftVRController);
        if (!grabbedWeapon)
    return 9999.0f;
     
        // Get the grabbed weapon's NiNode for position
        NiNode* grabbedNode = grabbedWeapon->GetNiNode();
        if (!grabbedNode)
            return 9999.0f;
        
        NiPoint3 grabbedPos = grabbedNode->m_worldTransform.pos;
        
        // Get the equipped weapon geometry from the OTHER hand
        WeaponGeometryTracker* tracker = WeaponGeometryTracker::GetSingleton();
        if (!tracker)
     return 9999.0f;
        
        // If left VR controller is grabbing, check against RIGHT hand equipped weapon
        // (Remember: left VR controller = left game hand in standard mode)
        bool isLeftGameHand = VRControllerToGameHand(isLeftVRController);
        const BladeGeometry& equippedGeom = tracker->GetBladeGeometry(!isLeftGameHand);
        
        if (!equippedGeom.isValid)
  return 9999.0f;
        
        // Calculate distance from grabbed weapon to equipped weapon's midpoint
        NiPoint3 equippedMid;
        equippedMid.x = (equippedGeom.basePosition.x + equippedGeom.tipPosition.x) * 0.5f;
        equippedMid.y = (equippedGeom.basePosition.y + equippedGeom.tipPosition.y) * 0.5f;
        equippedMid.z = (equippedGeom.basePosition.z + equippedGeom.tipPosition.z) * 0.5f;
        
        float dx = grabbedPos.x - equippedMid.x;
        float dy = grabbedPos.y - equippedMid.y;
   float dz = grabbedPos.z - equippedMid.z;
     
        return sqrt(dx*dx + dy*dy + dz*dz);
    }

    void VRInputHandler::OnStartTwoHanding()
    {
        _MESSAGE("VRInputHandler: TWO-HANDING started");

        VRInputHandler* handler = GetSingleton();
        if (handler->IsListening())
        {
  _MESSAGE("VRInputHandler:   Player is dual wielding and started two-handing a weapon");
     }
    }

    void VRInputHandler::OnStopTwoHanding()
    {
        _MESSAGE("VRInputHandler: TWO-HANDING stopped");
    }

    void VRInputHandler::OnShieldCollisionDetected()
    {
        if (!m_shieldCollisionActive)
     {
            _MESSAGE("VRInputHandler: === RIGHT HAND WEAPON TOUCHING SHIELD ===");
        }
        m_shieldCollisionActive = true;
      m_timeSinceLastShieldCollision = 0.0f;
    }

    void VRInputHandler::CheckShieldCollisionTimeout(float deltaTime)
    {
        // Skip collision avoidance logic if in close combat mode
        if (m_closeCombatMode)
            return;
            
        // Determine weapon hand based on where the shield is
     // Weapon is ALWAYS in the opposite hand from the shield
  bool weaponHandIsLeft = !ShieldCollisionTracker::GetSingleton()->IsShieldInLeftHand();
 
// Get the VR controller that corresponds to the weapon hand
        bool weaponVRControllerIsLeft = GameHandToVRController(weaponHandIsLeft);

        if (!EquipManager::GetSingleton()->HasPendingReequip(weaponHandIsLeft))
        {
            m_shieldCollisionActive = false;
        m_wasShieldCollisionActive = false;
        m_timeSinceLastShieldCollision = 0.0f;
     return;
     }

        TESObjectREFR* droppedWeapon = EquipManager::GetSingleton()->GetDroppedWeaponRef(weaponHandIsLeft);
 if (!droppedWeapon)
        {
         _MESSAGE("CheckShieldCollisionTimeout: Dropped weapon ref is NULL - clearing state");
   EquipManager::GetSingleton()->ClearPendingReequip(weaponHandIsLeft);
   EquipManager::GetSingleton()->ClearDroppedWeaponRef(weaponHandIsLeft);
     EquipManager::GetSingleton()->ClearCachedWeaponFormID(weaponHandIsLeft);
       m_shieldCollisionActive = false;
        m_wasShieldCollisionActive = false;
          m_timeSinceLastShieldCollision = 0.0f;
    return;
        }
   
  if (higgsInterface)
        {
     TESObjectREFR* higgsHeld = higgsInterface->GetGrabbedObject(weaponVRControllerIsLeft);
 if (higgsHeld != droppedWeapon)
       {
            // HIGGS is not holding our weapon - but give it a few frames to actually grab
 // Track how long HIGGS has NOT been holding it
 static float higgsGrabWaitTimeShield = 0.0f;
       static TESObjectREFR* lastDroppedWeaponShield = nullptr;
            
        // Reset wait time if this is a new dropped weapon
      if (lastDroppedWeaponShield != droppedWeapon)
      {
    higgsGrabWaitTimeShield = 0.0f;
    lastDroppedWeaponShield = droppedWeapon;
 }
      
      higgsGrabWaitTimeShield += deltaTime;
     
// Only clear state if HIGGS hasn't grabbed after 0.3 seconds
    // This gives HIGGS time to actually grab the object
if (higgsGrabWaitTimeShield >= 0.3f)
       {
        _MESSAGE("CheckShieldCollisionTimeout: HIGGS not holding our weapon after 0.3s (held: %p, ours: %p) - clearing state",
   higgsHeld, droppedWeapon);
         EquipManager::GetSingleton()->ClearPendingReequip(weaponHandIsLeft);
     EquipManager::GetSingleton()->ClearDroppedWeaponRef(weaponHandIsLeft);
  EquipManager::GetSingleton()->ClearCachedWeaponFormID(weaponHandIsLeft);
         m_shieldCollisionActive = false;
      m_wasShieldCollisionActive = false;
   m_timeSinceLastShieldCollision = 0.0f;
        higgsGrabWaitTimeShield = 0.0f;
 lastDroppedWeaponShield = nullptr;
        }
          return;
  }
        else
        {
      // HIGGS is holding our weapon - reset the wait timer
       static float higgsGrabWaitTimeShield = 0.0f;
static TESObjectREFR* lastDroppedWeaponShield = nullptr;
            higgsGrabWaitTimeShield = 0.0f;
    lastDroppedWeaponShield = nullptr;
    }
   }

    float currentDistance = GetCurrentWeaponShieldDistance();
        bool weaponClose = (currentDistance < shieldReequipThreshold);
      
     static int logCounter = 0;
  logCounter++;
        if (logCounter % 100 == 0)
 {
  _MESSAGE("CheckShieldCollisionTimeout: Distance=%.2f, Threshold=%.2f, WeaponClose=%s, Timer=%.3f, Timeout=%.3f",
      currentDistance, shieldReequipThreshold, weaponClose ? "YES" : "NO", 
    m_timeSinceLastShieldCollision, shieldCollisionTimeout);
    }
        
  if (weaponClose)
    {
        m_timeSinceLastShieldCollision = 0.0f;
         m_shieldCollisionActive = true;
    }
        else
    {
m_timeSinceLastShieldCollision += deltaTime;
      
    if (m_timeSinceLastShieldCollision >= shieldCollisionTimeout)
       {
     m_shieldCollisionActive = false;
       _MESSAGE("VRInputHandler: === %s HAND WEAPON SAFE TO RE-EQUIP ===", weaponHandIsLeft ? "LEFT" : "RIGHT");
       _MESSAGE("VRInputHandler: Time separated: %.2f sec, Distance: %.2f (threshold: %.2f)", 
 m_timeSinceLastShieldCollision, currentDistance, shieldReequipThreshold);
  
     droppedWeapon = EquipManager::GetSingleton()->GetDroppedWeaponRef(weaponHandIsLeft);
      if (droppedWeapon && droppedWeapon->baseForm)
        {
        // Check if this was a dual-wield same weapon situation
    bool wasDualWieldingSame = EquipManager::GetSingleton()->WasDualWieldingSameWeapon(weaponHandIsLeft);
     
            if (wasDualWieldingSame)
            {
      // For dual-wield same weapon: DON'T activate (which would add duplicate to inventory)
  // The re-equip will use the existing inventory item via cached FormID
   _MESSAGE("VRInputHandler: Dual-wield same weapon - SKIPPING activation (will re-equip from existing inventory)");
      _MESSAGE("VRInputHandler: Deleting spawned weapon (RefID: %08X) from world", droppedWeapon->formID);
           
         // Delete the spawned world object so it doesn't accumulate on the ground
 DeleteWorldObject(droppedWeapon);
            }
          else
            {
 // Normal case (different weapons): activate to add to inventory
         _MESSAGE("VRInputHandler: Activating grabbed RIGHT weapon to add to inventory (RefID: %08X, BaseID: %08X)...",
         droppedWeapon->formID, droppedWeapon->baseForm->formID);
     
                PlayerCharacter* player = *g_thePlayer;
          if (player)
           {
        // Suppress pickup sound during internal re-equip
          EquipManager::s_suppressPickupSound = true;
      bool activated = SafeActivate(droppedWeapon, player, 0, 0, 1, false);
           EquipManager::s_suppressPickupSound = false;
        _MESSAGE("VRInputHandler: Activate result: %s", activated ? "SUCCESS" : "FAILED");
      }
            }
        }
        else
    {
   _MESSAGE("VRInputHandler: WARNING - Dropped weapon ref became invalid before activation!");
    }
   
      // Clear the dropped weapon ref but KEEP the cached FormID for re-equip!
         // For shield collision: weapon hand is LEFT in left-handed, RIGHT in right-handed
      EquipManager::GetSingleton()->ClearDroppedWeaponRef(weaponHandIsLeft);

      // Also clear PendingReequip so CheckShieldCollisionTimeout doesn't clear cached FormID
      EquipManager::GetSingleton()->ClearPendingReequip(weaponHandIsLeft);

      m_pendingReequipRight = true;
      m_pendingReequipRightTimer = 0.0f;
      _MESSAGE("VRInputHandler: Scheduled re-equip for %s hand in %.1f ms", weaponHandIsLeft ? "LEFT" : "RIGHT", shieldReequipDelay *  1000.0f);
  }
        }
    }

float VRInputHandler::GetCurrentWeaponShieldDistance() const
    {
        // Determine weapon hand based on handedness mode
  bool weaponHandIsLeft = IsLeftHandedMode();
     bool weaponVRControllerIsLeft = GameHandToVRController(weaponHandIsLeft);
        
        // First, check if we have a HIGGS-grabbed weapon (shield collision case)
      TESObjectREFR* droppedWeapon = EquipManager::GetSingleton()->GetDroppedWeaponRef(weaponHandIsLeft);
     if (droppedWeapon && higgsInterface)
        {
  TESObjectREFR* higgsHeld = higgsInterface->GetGrabbedObject(weaponVRControllerIsLeft);
      if (higgsHeld == droppedWeapon)
    {
      // Get position of HIGGS grabbed weapon
  NiNode* weaponNode = droppedWeapon->GetNiNode();
         if (weaponNode)
    {
       NiPoint3 weaponPos = weaponNode->m_worldTransform.pos;
       
        // Get shield position from player's left hand
        PlayerCharacter* player = *g_thePlayer;
   if (player)
                {
          NiNode* rootNode = player->GetNiRootNode(0);
            if (!rootNode)
     rootNode = player->GetNiRootNode(1);
         
       if (rootNode)
   {
            BSFixedString shieldNodeStr("SHIELD");
     NiAVObject* shieldNode = rootNode->GetObjectByName(&shieldNodeStr.data);
        if (shieldNode)
{
                NiPoint3 shieldPos = shieldNode->m_worldTransform.pos;
 
       float dx = weaponPos.x - shieldPos.x;
         float dy = weaponPos.y - shieldPos.y;
     float dz = weaponPos.z - shieldPos.z;
          return sqrt(dx*dx + dy*dy + dz*dz);
    }
   }
            }
    }
          }
        }
        
        // Fallback to geometry tracker (for equipped weapons)
        WeaponGeometryTracker* tracker = WeaponGeometryTracker::GetSingleton();
        if (!tracker)
    return 9999.0f;
  
  // Get weapon geometry (RIGHT hand in right-handed, LEFT hand in left-handed)
        const BladeGeometry& weaponGeom = tracker->GetBladeGeometry(weaponHandIsLeft);
        
  if (!weaponGeom.isValid)
    return 9999.0f;
    
  // Get shield geometry (LEFT hand in right-handed, RIGHT hand in left-handed)
     const BladeGeometry& shieldGeom = tracker->GetBladeGeometry(!weaponHandIsLeft);
 
        if (!shieldGeom.isValid)
    return 9999.0f;
  
        NiPoint3 shieldCenter;
        shieldCenter.x = (shieldGeom.basePosition.x + shieldGeom.tipPosition.x) * 0.5f;
        shieldCenter.y = (shieldGeom.basePosition.y + shieldGeom.tipPosition.y) * 0.5f;
        shieldCenter.z = (shieldGeom.basePosition.z + shieldGeom.tipPosition.z) * 0.5f;
     
        NiPoint3 weaponTip = weaponGeom.tipPosition;
    
      float dx = weaponTip.x - shieldCenter.x;
  float dy = weaponTip.y - shieldCenter.y;
        float dz = weaponTip.z - shieldCenter.z;
 
        return sqrt(dx*dx + dy*dy + dz*dz);
    }
    void VRInputHandler::ClearAllState()
    {
   _MESSAGE("VRInputHandler: Clearing all tracking state");
        
        m_higgsCollisionActive = false;
        m_wasHiggsCollisionActive = false;
    m_timeSinceLastCollision = 0.0f;
        
        m_shieldCollisionActive = false;
        m_wasShieldCollisionActive = false;
        m_timeSinceLastShieldCollision = 0.0f;
        
        m_pendingReequip = false;
        m_pendingReequipIsLeft = false;
    m_pendingReequipTimer = 0.0f;
 
        m_pendingReequipRight = false;
  m_pendingReequipRightTimer = 0.0f;
      
        m_leftHandCooldownTimer = 0.0f;
        m_rightHandCooldownTimer = 0.0f;
    m_leftHandOnCooldown = false;
     m_rightHandOnCooldown = false;
        
        m_autoEquipPendingLeft = false;
        m_autoEquipPendingRight = false;
        m_autoEquipTimerLeft = 0.0f;
m_autoEquipTimerRight = 0.0f;
   m_autoEquipWeaponLeft = nullptr;
        m_autoEquipWeaponRight = nullptr;
        
        // Clear combat tracking
        m_isInCombat = false;
      m_closestTargetDistance = 9999.0f;
        m_closestTargetHandle = 0;
        m_closeCombatMode = false;
        m_combatLogTimer = 0.0f;
   
        // Clear shield bash tracking completely on death/load
        m_shieldBashCount = 0;
        m_shieldBashWindowTimer = 0.0f;
        m_shieldBashLockoutActive = false;
        m_shieldBashLockoutTimer = 0.0f;
    
      EquipManager::GetSingleton()->ClearDroppedWeaponRef(true);
        EquipManager::GetSingleton()->ClearDroppedWeaponRef(false);
        EquipManager::GetSingleton()->ClearPendingReequip(true);
        EquipManager::GetSingleton()->ClearPendingReequip(false);
        EquipManager::GetSingleton()->ClearCachedWeaponFormID(true);
EquipManager::GetSingleton()->ClearCachedWeaponFormID(false);
        
   _MESSAGE("VRInputHandler: All tracking state cleared");
    }

    float VRInputHandler::GetGrabbedWeaponVelocity(bool isLeftGameHand) const
    {
        WeaponGeometryTracker* tracker = WeaponGeometryTracker::GetSingleton();
        if (!tracker)
       return 0.0f;
        
     const BladeGeometry& geom = tracker->GetBladeGeometry(isLeftGameHand);
        if (!geom.isValid)
return 0.0f;
        
        float vx = geom.tipVelocity.x;
      float vy = geom.tipVelocity.y;
     float vz = geom.tipVelocity.z;
    
     return sqrt(vx*vx + vy*vy + vz*vz);
    }

    // ============================================
    // Convenience Functions
    // ============================================

    void InitializeVRInput()
 {
        VRInputHandler::GetSingleton()->Initialize();
 }

    // ============================================
    // TRIGGER-BASED WEAPON HOLD SYSTEM
    // Off-hand weapon is HIGGS grabbed by default
    // Trigger HELD = weapon equipped (can attack)
    // Trigger RELEASED = weapon unequipped, HIGGS grabbed
    // ============================================
    
    // Track trigger state for each hand
    static bool s_leftTriggerPressed = false;
    static bool s_rightTriggerPressed = false;
    static bool s_leftTriggerWasPressed = false;
    static bool s_rightTriggerWasPressed = false;
    
    // Pending trigger unequip (delayed unequip on trigger release)
    static bool s_pendingTriggerUnequip = false;
    static float s_triggerUnequipTimer = 0.0f;
    static bool s_pendingUnequipHand = false;  // true = left hand, false = right hand
    
    // Trigger button mask (SteamVR trigger button = button 33)
    static const uint64_t TRIGGER_BUTTON_MASK = (1ull << 33);
    
    // Poll trigger state and handle equip/unequip - call this each frame from OnPrePhysicsStep
    void PollTriggerState()
    {
  BSOpenVR* openVR = (*g_openVR);
        if (!openVR || !openVR->vrSystem)
    return;

        vr_1_0_12::IVRSystem* vrSystem = openVR->vrSystem;
      
        // Get controller indices
        vr_1_0_12::TrackedDeviceIndex_t leftController = vrSystem->GetTrackedDeviceIndexForControllerRole(
       vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_LeftHand);
     vr_1_0_12::TrackedDeviceIndex_t rightController = vrSystem->GetTrackedDeviceIndexForControllerRole(
       vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_RightHand);
        
        // Get controller state for left hand
        vr_1_0_12::VRControllerState_t leftState;
        if (vrSystem->GetControllerState(leftController, &leftState, sizeof(leftState)))
        {
       s_leftTriggerWasPressed = s_leftTriggerPressed;
    // Check both ulButtonPressed (digital) and rAxis[1] (analog trigger)
  // Button 33 is k_EButton_SteamVR_Trigger
       const uint64_t TRIGGER_BUTTON_MASK = (1ull << 33);
      bool digitalPressed = (leftState.ulButtonPressed & TRIGGER_BUTTON_MASK) != 0;
  bool analogPressed = (leftState.rAxis[1].x > 0.5f);  // Trigger axis
    s_leftTriggerPressed = digitalPressed || analogPressed;
        }
   
        // Get controller state for right hand
        vr_1_0_12::VRControllerState_t rightState;
      if (vrSystem->GetControllerState(rightController, &rightState, sizeof(rightState)))
        {
            s_rightTriggerWasPressed = s_rightTriggerPressed;
   const uint64_t TRIGGER_BUTTON_MASK = (1ull << 33);
     bool digitalPressed = (rightState.ulButtonPressed & TRIGGER_BUTTON_MASK) != 0;
            bool analogPressed = (rightState.rAxis[1].x > 0.5f);  // Trigger axis
      s_rightTriggerPressed = digitalPressed || analogPressed;
        }
 
    // Debug logging every 100 frames
 static int debugCounter = 0;
    debugCounter++;
        if (debugCounter % 100 == 0)
        {
          bool offHandIsLeft = GetCollisionAvoidanceHandIsLeft();
            TESObjectREFR* droppedWeapon = EquipManager::GetSingleton()->GetDroppedWeaponRef(offHandIsLeft);
            bool offHandVRControllerIsLeft = GameHandToVRController(offHandIsLeft);
        bool offHandTrigger = offHandVRControllerIsLeft ? s_leftTriggerPressed : s_rightTriggerPressed;

          _MESSAGE("PollTriggerState DEBUG: LeftTrig=%s RightTrig=%s OffHandIsLeft=%s DroppedWeapon=%p OffHandTrigger=%s",
        s_leftTriggerPressed ? "YES" : "NO",
          s_rightTriggerPressed ? "YES" : "NO",
       offHandIsLeft ? "YES" : "NO",
  droppedWeapon,
                offHandTrigger ? "YES" : "NO");
  }
   
        // ============================================
   // TRIGGER-BASED WEAPON EQUIP/UNEQUIP LOGIC
    // Works for ANY weapon: single weapon, dual-wield, or shield+weapon
        // ============================================
        
        // Check BOTH hands for grabbed weapons
      TESObjectREFR* droppedWeaponLeft = EquipManager::GetSingleton()->GetDroppedWeaponRef(true);
        TESObjectREFR* droppedWeaponRight = EquipManager::GetSingleton()->GetDroppedWeaponRef(false);
        
        // Process LEFT hand
        // Add validity checks to prevent CTD on stale references
    if (droppedWeaponLeft)
  {
    // Verify the reference is still valid before accessing it
 bool isValid = false;
       __try {
          isValid = (droppedWeaponLeft->baseForm != nullptr && 
         droppedWeaponLeft->formType == kFormType_Reference);
  }
     __except(EXCEPTION_EXECUTE_HANDLER) {
           _MESSAGE("VRInputHandler: LEFT hand dropped weapon reference is INVALID (exception) - clearing");
     EquipManager::GetSingleton()->ClearDroppedWeaponRef(true);
                EquipManager::GetSingleton()->ClearPendingReequip(true);
     droppedWeaponLeft = nullptr;
  }
      
         if (droppedWeaponLeft && isValid)
            {
            bool leftVRTrigger = GameHandToVRController(true) ? s_leftTriggerPressed : s_rightTriggerPressed;

            if (leftVRTrigger)
 {
                _MESSAGE("VRInputHandler: LEFT hand TRIGGER HELD - Re-equipping grabbed weapon");
    
        bool wasDualWieldingSame = EquipManager::GetSingleton()->WasDualWieldingSameWeapon(true);
   
   PlayerCharacter* player = *g_thePlayer;
                if (player)
      {
    if (wasDualWieldingSame)
    {
    DeleteWorldObject(droppedWeaponLeft);
 }
        else
       {
          EquipManager::s_suppressPickupSound = true;
       SafeActivate(droppedWeaponLeft, player, 0, 0, 1, false);
     EquipManager::s_suppressPickupSound = false;
    }

        EquipManager::GetSingleton()->ClearDroppedWeaponRef(true);
    EquipManager::GetSingleton()->ClearPendingReequip(true);
     
    EquipManager::s_suppressDrawSound = true;
   EquipManager::GetSingleton()->ForceReequipLeftHand();
    EquipManager::s_suppressDrawSound = false;
        
     _MESSAGE("VRInputHandler: Weapon re-equipped to LEFT hand (trigger held)");
      }
 }
        }
    }
   
        // Process RIGHT hand
    // Add validity checks to prevent CTD on stale references
        if (droppedWeaponRight)
  {
            // Verify the reference is still valid before accessing it
   bool isValid = false;
      __try {
      isValid = (droppedWeaponRight->baseForm != nullptr && 
           droppedWeaponRight->formType == kFormType_Reference);
     }
        __except(EXCEPTION_EXECUTE_HANDLER) {
        _MESSAGE("VRInputHandler: RIGHT hand dropped weapon reference is INVALID (exception) - clearing");
     EquipManager::GetSingleton()->ClearDroppedWeaponRef(false);
         EquipManager::GetSingleton()->ClearPendingReequip(false);
         droppedWeaponRight = nullptr;
   }
        
     if (droppedWeaponRight && isValid)
     {
 bool rightVRTrigger = GameHandToVRController(false) ? s_leftTriggerPressed : s_rightTriggerPressed;
        
      if (rightVRTrigger)
       {
 _MESSAGE("VRInputHandler: RIGHT hand TRIGGER HELD - Re-equipping grabbed weapon");
      
  bool wasDualWieldingSame = EquipManager::GetSingleton()->WasDualWieldingSameWeapon(false);
       
    PlayerCharacter* player = *g_thePlayer;
 if (player)
          {
      if (wasDualWieldingSame)
     {
     DeleteWorldObject(droppedWeaponRight);
     }
    else
          {
        EquipManager::s_suppressPickupSound = true;
       SafeActivate(droppedWeaponRight, player, 0, 0, 1, false);
        EquipManager::s_suppressPickupSound = false;
       }

      EquipManager::GetSingleton()->ClearDroppedWeaponRef(false);
      EquipManager::GetSingleton()->ClearPendingReequip(false);
     
   EquipManager::s_suppressDrawSound = true;
   EquipManager::GetSingleton()->ForceReequipRightHand();
  EquipManager::s_suppressDrawSound = false;
        
      _MESSAGE("VRInputHandler: Weapon re-equipped to RIGHT hand (trigger held)");
    }
  }
        }
    }
   
        // ============================================
// TRIGGER RELEASED - Start delayed unequip timer for EACH hand
  // ============================================
  
        // LEFT hand trigger released
        bool leftVRTriggerWas = GameHandToVRController(true) ? s_leftTriggerWasPressed : s_rightTriggerWasPressed;
bool leftVRTriggerNow = GameHandToVRController(true) ? s_leftTriggerPressed : s_rightTriggerPressed;
 
     if (!leftVRTriggerNow && leftVRTriggerWas && !droppedWeaponLeft)
        {
    // Check if left hand has an equipped weapon
   PlayerCharacter* player = *g_thePlayer;
     if (player)
      {
     TESForm* leftEquipped = player->GetEquippedObject(true);
  if (leftEquipped && EquipManager::IsWeapon(leftEquipped))
     {
   s_pendingTriggerUnequip = true;
      s_triggerUnequipTimer = 0.0f;
     s_pendingUnequipHand = true;  // true = left hand
        _MESSAGE("VRInputHandler: LEFT hand TRIGGER RELEASED - Starting %.3f sec delay before unequip", triggerUnequipDelay);
       }
   }
        }
        
    // RIGHT hand trigger released
        bool rightVRTriggerWas = GameHandToVRController(false) ? s_leftTriggerWasPressed : s_rightTriggerWasPressed;
        bool rightVRTriggerNow = GameHandToVRController(false) ? s_leftTriggerPressed : s_rightTriggerPressed;
        
        if (!rightVRTriggerNow && rightVRTriggerWas && !droppedWeaponRight)
   {
     // Check if right hand has an equipped weapon
 PlayerCharacter* player = *g_thePlayer;
  if (player)
    {
   TESForm* rightEquipped = player->GetEquippedObject(false);
      if (rightEquipped && EquipManager::IsWeapon(rightEquipped))
 {
     s_pendingTriggerUnequip = true;
     s_triggerUnequipTimer = 0.0f;
    s_pendingUnequipHand = false;  // false = right hand
    _MESSAGE("VRInputHandler: RIGHT hand TRIGGER RELEASED - Starting %.3f sec delay before unequip", triggerUnequipDelay);
      }
       }
     }
        
 // ============================================
     // CANCEL pending unequip if trigger pressed again
     // ============================================
        bool pendingHandTrigger = s_pendingUnequipHand ? 
 (GameHandToVRController(true) ? s_leftTriggerPressed : s_rightTriggerPressed) :
   (GameHandToVRController(false) ? s_leftTriggerPressed : s_rightTriggerPressed);
        
if (pendingHandTrigger && s_pendingTriggerUnequip)
        {
     s_pendingTriggerUnequip = false;
    s_triggerUnequipTimer = 0.0f;
       _MESSAGE("VRInputHandler: Trigger pressed - cancelled pending unequip");
  }
      
// ============================================
        // PROCESS pending unequip after delay
        // ============================================
        if (s_pendingTriggerUnequip)
        {
   s_triggerUnequipTimer += 0.011f;  // ~90fps
       
  if (s_triggerUnequipTimer >= triggerUnequipDelay)
      {
     s_pendingTriggerUnequip = false;
    s_triggerUnequipTimer = 0.0f;
        
 bool handToUnequip = s_pendingUnequipHand;
        
     PlayerCharacter* player = *g_thePlayer;
     if (player)
    {
       TESForm* handEquipped = player->GetEquippedObject(handToUnequip);
    
       if (handEquipped && EquipManager::IsWeapon(handEquipped))
     {
    _MESSAGE("VRInputHandler: Delay elapsed - Unequipping %s hand weapon for HIGGS grab", 
        handToUnequip ? "LEFT" : "RIGHT");
     EquipManager::GetSingleton()->ForceUnequipAndGrab(handToUnequip);
    }
         }
      }
 }
    }

    bool VRInputHandler::IsLeftTriggerPressed() { return s_leftTriggerPressed; }
    bool VRInputHandler::IsRightTriggerPressed() { return s_rightTriggerPressed; }
    
    // Check if trigger is pressed for the VR controller corresponding to a game hand
    bool VRInputHandler::IsTriggerHeldForGameHand(bool isLeftGameHand)
    {
      bool isLeftVRController = GameHandToVRController(isLeftGameHand);
        return isLeftVRController ? s_leftTriggerPressed : s_rightTriggerPressed;
    }
    
    void VRInputHandler::RegisterTriggerCallback()
    {
  _MESSAGE("VRInputHandler: Trigger button tracking initialized (polled in OnPrePhysicsStep)");
    }
}

