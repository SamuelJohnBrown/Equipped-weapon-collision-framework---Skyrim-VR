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
  void UpdateCombatTracking();
   bool IsPlayerInCombat() const { return m_isInCombat; }
   float GetClosestTargetDistance() const { return m_closestTargetDistance; }
        bool IsInCloseCombatMode() const { return m_closeCombatMode; }
     void ForceEquipGrabbedWeapons();
        
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
 
        // Track HIGGS collision state for grabbed weapons (left hand - blade vs blade)
        void OnHiggsCollisionDetected(bool isLeft);
        bool IsHiggsCollisionActive() const { return m_higgsCollisionActive; }
        void CheckCollisionTimeout(float deltaTime);
   
   // Track shield collision state (right hand weapon vs shield)
        void OnShieldCollisionDetected();
        bool IsShieldCollisionActive() const { return m_shieldCollisionActive; }
        void CheckShieldCollisionTimeout(float deltaTime);
        
        // Get current blade distance (from geometry tracker)
        float GetCurrentBladeDistance() const;
        
// Get distance from HIGGS grabbed weapon to equipped weapon in other hand
      float GetGrabbedToEquippedDistance(bool isLeftVRController) const;
 
     // Get current weapon-shield distance
 float GetCurrentWeaponShieldDistance() const;
     
        // Check for pending re-equip after activation
   void CheckPendingReequip(float deltaTime);
      
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
     
        // Combat tracking state
  bool m_isInCombat = false;
    bool m_wasInCombat = false;
     float m_closestTargetDistance = 9999.0f;
UInt32 m_closestTargetHandle = 0;
    float m_combatLogTimer = 0.0f;
    
        // Close combat mode - disables collision avoidance when too close to enemy
    bool m_closeCombatMode = false;
    
        // Shield bash tracking
int m_shieldBashCount = 0;
        float m_shieldBashWindowTimer = 0.0f;      // Time since first bash in current window
      float m_shieldBashLockoutTimer = 0.0f;    // Lockout timer after 3 bashes
        bool m_shieldBashLockoutActive = false;
        static constexpr float kShieldBashWindow = 6.0f;   // 6 second window for 3 bashes
        static constexpr float kShieldBashLockout = 240.0f;   // 4 minute lockout (240 seconds)
        static constexpr int kShieldBashThreshold = 3;    // Number of bashes to trigger
        
        // Weapon swing tracking (game-registered swings)
        int m_leftSwingCount = 0;
      int m_rightSwingCount = 0;
   
        // HIGGS collision tracking (left hand weapon grabbed - blade vs blade)
        bool m_higgsCollisionActive = false;
      bool m_wasHiggsCollisionActive = false;
        float m_timeSinceLastCollision = 0.0f;
 
        // Shield collision tracking (right hand weapon grabbed - weapon vs shield)
        bool m_shieldCollisionActive = false;
        bool m_wasShieldCollisionActive = false;
    float m_timeSinceLastShieldCollision = 0.0f;
     
 // Pending re-equip tracking (for left hand - blade collision)
     bool m_pendingReequip = false;
        bool m_pendingReequipIsLeft = false;
  float m_pendingReequipTimer = 0.0f;
        
        // Pending re-equip tracking (for right hand - shield collision)
        bool m_pendingReequipRight = false;
        float m_pendingReequipRightTimer = 0.0f;
 
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

        UInt32 m_lastCombatTarget = 0;
        float m_combatStartTime = 0.0f;
        float m_combatDuration = 0.0f;
    };

    // Convenience function
    void InitializeVRInput();
}
