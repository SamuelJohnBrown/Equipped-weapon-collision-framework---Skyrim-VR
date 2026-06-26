#pragma once

#include "skse64/GameReferences.h"
#include "skse64/PapyrusEvents.h"
#include "higgsinterface001.h"
#include "EquipManager.h"
#include "config.h"

namespace FalseEdgeVR
{
    // VR Input handler for tracking controller/hand events
    class VRInputHandler
    {
    public:
        static VRInputHandler* GetSingleton();
        
        // Initialize VR input handling
      void Initialize();

        // Register HIGGS callbacks
        void RegisterHiggsCallbacks();
        
        // Update grab listening state based on equipment
      void UpdateGrabListening();
        
   // Check if we should be listening for grab events
        bool ShouldListenForGrabs() const;
    
        // Check if player is two-handing a weapon
    bool IsTwoHanding() const;
        
        // Check if currently listening
        bool IsListening() const { return m_isListening; }
        
        // ============================================
// Legacy/Compatibility Methods (stubbed out)
// ============================================
        
        // Check if a hand is on cooldown (recently re-equipped, can't trigger again yet)
        bool IsHandOnCooldown(bool isLeftGameHand) const 
        { 
            return isLeftGameHand ? m_leftHandOnCooldown : m_rightHandOnCooldown; 
    }
        void CheckAutoEquipGrabbedWeapon(float deltaTime);
    void PauseTracking(bool pause);
     bool IsPaused() const { return m_paused; }
        
        // Shield bash tracking
    void OnShieldBash();
        void UpdateShieldBashTracking(float deltaTime);
     bool IsShieldBashLockoutActive() const { return m_shieldBashLockoutActive; }
        int GetShieldBashCount() const { return m_shieldBashCount; }
        
        // Weapon swing tracking (game-registered swings, not VR controller input)
        void OnWeaponSwing(bool isLeftHand, TESForm* weapon);
   int GetWeaponSwingCount(bool isLeftHand) const { return isLeftHand ? m_leftSwingCount : m_rightSwingCount; }
     
        // Clear all tracking state (call on death, load, etc.)
  void ClearAllState();
 
    // ============================================
        // Trigger Button Tracking
      // ============================================
        
        // Check if trigger is currently pressed
        static bool IsLeftTriggerPressed();
  static bool IsRightTriggerPressed();
        
        // Check if trigger is held for the VR controller corresponding to a game hand
        static bool IsTriggerHeldForGameHand(bool isLeftGameHand);
  
        // Register the trigger callback with PapyrusVR
        static void RegisterTriggerCallback();
   
        // ============================================
        // Grip Button Tracking
   // ============================================
   
        // Check if grip is currently pressed
        static bool IsLeftGripPressed();
     static bool IsRightGripPressed();
        
        // Check if grip is held for the VR controller corresponding to a game hand
        static bool IsGripHeldForGameHand(bool isLeftGameHand);
      
        // ============================================
        // Drop Protection Override (Grip Spam Detection)
        // ============================================
        
        // Check if drop protection is currently disabled for a VR controller
        // (true = protection disabled, player can drop weapons freely)
     static bool IsDropProtectionDisabled(bool isLeftVRController);
    
  // Get remaining time for drop protection disable (for debugging)
  static float GetDropProtectionDisableTimeRemaining(bool isLeftVRController);
        
        // ============================================
        // Weapon Lock (Trigger Spam Detection)
        // ============================================
        
        // Check if weapon is locked to equipped state for a VR controller
        // (true = weapon stays equipped even when trigger is released)
        static bool IsWeaponLocked(bool isLeftVRController);
        
        // Clear weapon lock for a VR controller (call when weapon is dropped/unequipped)
      static void ClearWeaponLock(bool isLeftVRController);
 
     // Get the velocity of a grabbed weapon (from geometry tracker)
 float GetGrabbedWeaponVelocity(bool isLeftGameHand) const;
 
// Get distance from HIGGS grabbed weapon to equipped weapon in other hand
      float GetGrabbedToEquippedDistance(bool isLeftVRController) const;
      
    private:
        VRInputHandler() = default;
        ~VRInputHandler() = default;
     VRInputHandler(const VRInputHandler&) = delete;
        VRInputHandler& operator=(const VRInputHandler&) = delete;
    
   // HIGGS callback functions (static to match callback signature)
        static void OnGrabbed(bool isLeft, TESObjectREFR* grabbedRefr);
        static void OnDropped(bool isLeft, TESObjectREFR* droppedRefr);
        static void OnPulled(bool isLeft, TESObjectREFR* pulledRefr);
    static void OnCollision(bool isLeft, float mass, float separatingVelocity);
        static void OnStartTwoHanding();
     static void OnStopTwoHanding();
        static void OnPrePhysicsStep(void* world);
    
        bool m_initialized = false;
        bool m_callbacksRegistered = false;
        bool m_isListening = false;
        bool m_paused = false; // When true, per-frame tracking updates are suspended
     
        // Shield bash tracking
int m_shieldBashCount = 0;
        float m_shieldBashWindowTimer = 0.0f;      // Time since first bash in current window
      float m_shieldBashLockoutTimer = 0.0f;    // Lockout timer after 3 bashes
        bool m_shieldBashLockoutActive = false;
        // Weapon swing tracking (game-registered swings)
        int m_leftSwingCount = 0;
      int m_rightSwingCount = 0;
   
      // Cooldown tracking to prevent rapid unequip/re-equip cycles
float m_leftHandCooldownTimer = 0.0f;
        float m_rightHandCooldownTimer = 0.0f;
        bool m_leftHandOnCooldown = false;
        bool m_rightHandOnCooldown = false;
        
        // Auto-equip grabbed weapon tracking
        // When player grabs a weapon with HIGGS while having another weapon equipped,
     // auto-equip the grabbed weapon after a delay
      bool m_autoEquipPendingLeft = false;
        bool m_autoEquipPendingRight = false;
        float m_autoEquipTimerLeft = 0.0f;
     float m_autoEquipTimerRight = 0.0f;
        TESObjectREFR* m_autoEquipWeaponLeft = nullptr;
        TESObjectREFR* m_autoEquipWeaponRight = nullptr;
        UInt32 m_autoEquipFormIDLeft = 0;   // FormID for grabbed weapon auto-equip
        UInt32 m_autoEquipFormIDRight = 0;  // FormID for grabbed weapon auto-equip
    };

    // Convenience function
    void InitializeVRInput();

    // True while a door/cell transition guard window is active (force-equip grabbed weapons).
    bool IsDoorTransitionGuardActive();

    // Block holster-to-grab during door guard or blocking menus (not general input).
    bool IsWeaponGrabToHolsterBlocked();

    // Call when the player activates a door (or similar cell-loading ref).
    void NotifyDoorOrTransitionActivated();

    void UpdateWeaponTransitionGuard(float deltaTime);
}
