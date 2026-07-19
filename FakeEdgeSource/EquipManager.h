#pragma once

#include "skse64/GameReferences.h"
#include "skse64/GameObjects.h"
#include "skse64/GameEvents.h"
#include "skse64/GameRTTI.h"
#include "skse64/PluginAPI.h"
#include "config.h"
#include <chrono>

namespace FalseEdgeVR
{
    // Weapon type classification
    enum class WeaponType
    {
        None = 0,
        Sword,
        Dagger,
        Mace,
        Axe,
        Shield,
        Staff,
        TwoHanded
        // Note: Bows and crossbows are NOT tracked - they return None
    };

    // Represents an equipped item in a hand slot
    struct EquippedWeapon
    {
        TESForm* form = nullptr;
        WeaponType type = WeaponType::None;
        bool isEquipped = false;
        
        void Clear()
        {
            form = nullptr;
            type = WeaponType::None;
            isEquipped = false;
        }
    };

    // Equipment state for tracking both hands
    struct PlayerEquipState
    {
        EquippedWeapon leftHand;
        EquippedWeapon rightHand;
  
        bool HasOneWeaponEquipped() const
        {
         return (leftHand.isEquipped && !rightHand.isEquipped) ||
              (!leftHand.isEquipped && rightHand.isEquipped);
        }
        
        bool HasBothWeaponsEquipped() const
  {
            return leftHand.isEquipped && rightHand.isEquipped;
    }
        
        bool HasNoWeaponsEquipped() const
        {
    return !leftHand.isEquipped && !rightHand.isEquipped;
     }
     
 int GetEquippedWeaponCount() const
        {
 int count = 0;
            if (leftHand.isEquipped) count++;
            if (rightHand.isEquipped) count++;
            return count;
        }
    };

 // Equipment event handler - listens for equip/unequip events
    class EquipEventHandler : public BSTEventSink<TESEquipEvent>
    {
    public:
  virtual EventResult ReceiveEvent(TESEquipEvent* evn, EventDispatcher<TESEquipEvent>* dispatcher) override;
        
        static EquipEventHandler* GetSingleton();
        
    private:
        EquipEventHandler() = default;
 ~EquipEventHandler() = default;
    EquipEventHandler(const EquipEventHandler&) = delete;
        EquipEventHandler& operator=(const EquipEventHandler&) = delete;
    };

    // Main equip manager class
    class EquipManager
    {
    public:
        static EquipManager* GetSingleton();
        
 // Initialize the equip manager and register event handlers
      void Initialize();
        
        // Flag to suppress weapon pickup sound during internal re-equip
        static bool s_suppressPickupSound;
        
     // Flag to suppress weapon draw sound during internal collision re-equip
        static bool s_suppressDrawSound;
        
     // Flag to suppress weapon sheath sound during internal collision unequip
 static bool s_suppressSheathSound;
   
     // Update equipment state from current player equipped items
        void UpdateEquipmentState();
        
        // Handle equip event
      void OnEquip(TESForm* item, Actor* actor, bool isLeftHand);
   
   // Handle unequip event
        void OnUnequip(TESForm* item, Actor* actor, bool isLeftHand);
        
   // Get current equipment state
        const PlayerEquipState& GetEquipState() const { return m_equipState; }
 
        // Check if player has only one weapon equipped
   bool HasSingleWeaponEquipped() const { return m_equipState.HasOneWeaponEquipped(); }
        
        // Get weapon type from a form
  static WeaponType GetWeaponType(TESForm* form);
  
     // Get weapon type name as string
 static const char* GetWeaponTypeName(WeaponType type);
        
        // Check if form is a weapon (sword, mace, axe, dagger, etc.)
        static bool IsWeapon(TESForm* form);
        
        // Check if form is a shield
        static bool IsShield(TESForm* form);
        
   // Check if form is a bow, crossbow, or untracked two-handed melee (when 2H tracking disabled)
        static bool IsTwoHandedWeapon(TESForm* form);
   
        // Check if the player currently has a 2H weapon equipped
        static bool PlayerHasTwoHandedEquipped();

        static bool Get2HWeaponWornGameHands(Actor* actor, TESForm* weapon, bool& wornLeft, bool& wornRight);
        static TESForm* Get2HWeaponWornOnGameHand(Actor* actor, bool isLeftGameHand);

        // Determine which game hand an item was just equipped to (worn lists, then GetEquippedObject).
        static bool TryGetPlayerEquipHand(Actor* actor, TESForm* item, bool& isLeftHandOut);
        
        // ============================================
        // Forced Equip/Unequip Functions
        // ============================================
  
        // Unequip weapon and drop it for HIGGS to grab. Returns true if unequip+spawn succeeded.
        bool ForceUnequipAndGrab(bool isLeftHand);

        // Holster equipped weapons to HIGGS grab on both hands (e.g. after dismount).
        void HolsterAllEquippedWeaponHands();

        // Resolve the grabbed world weapon for a game hand (tracked ref, refID lookup, or HIGGS).
        TESObjectREFR* ResolveGrabbedWeaponRefForHand(bool isLeftGameHand) const;

        // True when this game hand has a HIGGS-grabbed weapon that is not the item being equipped.
        bool HasConflictingGrabbedWeaponInHand(bool isLeftGameHand, TESForm* itemBeingEquipped) const;

        // Recover a holstered/grabbed world weapon without re-entering EquipItem.
        // Prefer AddItem + delete; SafeActivate only when allowActivateFallback is true
        // (deferred game-thread task, never from inside EquipItem).
        bool PickUpGrabbedWeaponBeforeEquip(bool isLeftGameHand, bool allowActivateFallback = false);

        // Defer grabbed-weapon recovery until after the current equip finishes (SpellWheel, etc.).
        void SchedulePickUpGrabbedWeaponBeforeEquip(bool isLeftGameHand);

        void ScheduleDelayedLogPlayer2HWeaponEquip(UInt32 weaponFormID);
        void LogPlayer2HWeaponEquip(TESForm* weapon);

        // True if the weapon uses the Two-Handed skill record (gameData.skill), regardless
        // of its animation type (catches 1H-type weapons that use the 2H skill).
        static bool UsesTwoHandedSkill(TESForm* weapon);
        // Logs when a weapon using the Two-Handed skill record is equipped to the off hand.
        void LogOffHandTwoHandedSkillWeaponEquip(TESForm* weapon);
        void TryLog2HLeftHandWithRightGameHandTrigger(TESForm* weapon = nullptr);
        void CheckLeftHandUnequipAfterCombo(UInt32 leftWeaponFormID);

      // Unequip the weapon from the specified hand (stores for re-equip)
  void ForceUnequipHand(bool isLeftHand);
        
 // Unequip the left hand weapon
      void ForceUnequipLeftHand();
        
        // Unequip the right hand weapon
        void ForceUnequipRightHand();
        
  // Re-equip a previously unequipped weapon to specified hand
        void ForceReequipHand(bool isLeftHand);
        
        // Re-equip the left hand weapon
    void ForceReequipLeftHand();
        
    // Re-equip the right hand weapon
        void ForceReequipRightHand();

        // Schedule a force re-equip on the game thread that waits for the
        // activated item to arrive in the player's inventory first.
        // Use this instead of ForceReequipHand() right after SafeActivate()
        // to avoid the same-frame pickup/equip race (weapon "disappearing").
        void ScheduleForceReequip(bool isLeftHand);

        // Queue an explicit game-thread equip without relying on the cached
        // hand that SafeActivate/HIGGS may rewrite during a two-hand pickup.
        void ScheduleEquipWeaponToGameHand(UInt32 weaponFormID, bool isLeftGameHand);

        // Equip a weapon to a game hand, preserving tempering/enchant/favorite extra data.
        void EquipWeaponToGameHand(PlayerCharacter* player, TESForm* weaponForm, bool isLeftGameHand);

        // Move favorite cache when a grabbed weapon transfers to the other game hand.
        void TransferFavoriteCacheForHandSwap(bool fromLeftGameHand, bool toLeftGameHand, UInt32 weaponFormID);

        // Remember a favorited weapon across intentional drops (inventory copy is removed on grab spawn).
        void PreserveFavoriteForForm(UInt32 weaponFormID, bool isLeftGameHand);

        // Reset the cached re-equip data (FormID/health/enchant/favorite) for a hand.
        // ScheduledForceReequipTask calls this after the equip completes.
        void ClearReequipCache(bool isLeftHand);

        // Get the cached weapon FormID for a hand (0 = none)
        UInt32 GetCachedWeaponFormID(bool isLeftHand) const
        {
            return isLeftHand ? m_cachedWeaponFormIDLeft : m_cachedWeaponFormIDRight;
        }
        
        // Check if there's a pending re-equip for a hand
        bool HasPendingReequip(bool isLeftHand) const;
 
        // Clear pending re-equip for a hand
     void ClearPendingReequip(bool isLeftHand);
        
  // Check if HIGGS is currently holding the dropped weapon
        bool IsHiggsHoldingDroppedWeapon(bool isLeftHand) const;
   
        // Get the dropped weapon reference (for HIGGS grab)
        TESObjectREFR* GetDroppedWeaponRef(bool isLeftHand) const;

        UInt32 GetDroppedWeaponBaseID(bool isLeftHand) const
        {
            return isLeftHand ? m_droppedWeaponBaseIDLeft : m_droppedWeaponBaseIDRight;
        }

        UInt32 GetDroppedWeaponRefID(bool isLeftHand) const
        {
            return isLeftHand ? m_droppedWeaponRefIDLeft : m_droppedWeaponRefIDRight;
        }
        
  // Clear dropped weapon reference
        void ClearDroppedWeaponRef(bool isLeftHand);
        
        // Clear cached weapon FormID
        void ClearCachedWeaponFormID(bool isLeftHand);
        
        // Track if we're in dual-wield same weapon mode (for cleanup after re-equip)
        bool WasDualWieldingSameWeapon(bool isLeftHand) const;

        // Delete orphaned dual-wield duplicate world copies (call on death/load).
        // In the dual-wield same-weapon case the spawned world copy coexists with
        // the inventory original (+1 weapon) until the trigger re-equip deletes it.
        // If tracking is about to be wiped, the copy must be deleted or it becomes
        // a permanent duplicate. Uses RefID lookup, safe across save/load.
        void CleanupOrphanedDuplicates();

        // Return mod-grabbed world weapons to inventory after save/load (or delete duplicates).
        void RecoverGrabbedWeaponsOnLoad();

        // On load, fully unequip anything in either hand slot (no spawn/grab).
        void UnequipEquippedWeaponsOnLoad();

        // Directly unequip the weapon currently in a hand slot (robust, no spawn/grab).
        bool FullUnequipHand(bool isLeftGameHand);

        // Merge live dropped-weapon tracking into load-recovery state before it is cleared.
        void CaptureDroppedWeaponsForLoadRecovery();

        bool WasDroppedWeaponFavorited(bool isLeftGameHand) const;

        static void RestoreFavoriteInInventory(PlayerCharacter* player, TESForm* weaponForm);

        static void RegisterSerialization(SKSESerializationInterface* intfc);
        
   // Check and process pending auto-unequip (for trigger-based weapon hold)
        void CheckPendingAutoUnequip();
        
   // Check if there's a pending auto-unequip
     bool HasPendingAutoUnequip() const { return m_pendingAutoUnequipLeftForm != nullptr || m_pendingAutoUnequipRightForm != nullptr; }

    private:
        void ProcessPendingAutoUnequip(bool isLeftHand);

 EquipManager() = default;
        ~EquipManager() = default;
 EquipManager(const EquipManager&) = delete;
  EquipManager& operator=(const EquipManager&) = delete;
        
        void LogEquipmentState();

        bool ShouldPreserveFavorite(TESForm* weaponForm) const;
        static bool IsFormFavoritedInInventory(PlayerCharacter* player, TESForm* weaponForm);
        BaseExtraList* FindInventoryExtraDataForEquip(PlayerCharacter* player, TESForm* weaponForm, bool isLeftGameHand);
        void RestoreFavoriteOnEquippedHand(PlayerCharacter* player, TESForm* weaponForm, bool isLeftGameHand);

  PlayerEquipState m_equipState;
        
   // Pending re-equip tracking - store FormID instead of pointer
        TESForm* m_pendingReequipLeft = nullptr;
        TESForm* m_pendingReequipRight = nullptr;
        UInt32 m_cachedWeaponFormIDLeft = 0;   // Cache the weapon FormID for left hand re-equip
    UInt32 m_cachedWeaponFormIDRight = 0;  // Cache the weapon FormID for right hand re-equip
        
        // Cache the extra data (tempering, enchantments) for re-equip
        // We store a copy of the health value since that's what determines tempering level
        float m_cachedWeaponHealthLeft = 1.0f;   // 1.0 = base, >1.0 = tempered
        float m_cachedWeaponHealthRight = 1.0f;
        bool m_hasCachedHealthLeft = false;
  bool m_hasCachedHealthRight = false;
     
        // Cache enchantment FormID for player-enchanted weapons (separate from base enchantment)
        UInt32 m_cachedEnchantmentFormIDLeft = 0;
      UInt32 m_cachedEnchantmentFormIDRight = 0;
  bool m_hasCachedEnchantmentLeft = false;
        bool m_hasCachedEnchantmentRight = false;
        
    // Cache favorite state for re-equip
   bool m_wasFavoritedLeft = false;
        bool m_wasFavoritedRight = false;

        // Survives intentional drops / tracking clears (world pickup has no ExtraHotkey copy)
        UInt32 m_preservedFavoriteFormIDLeft = 0;
        UInt32 m_preservedFavoriteFormIDRight = 0;
        
        // Dropped weapon world references
      TESObjectREFR* m_droppedWeaponLeft = nullptr;
        TESObjectREFR* m_droppedWeaponRight = nullptr;
        
        // RefID + base FormID of the spawned world copies (for safe cleanup after
        // the pointers go stale, e.g. across save/load)
        UInt32 m_droppedWeaponRefIDLeft = 0;
        UInt32 m_droppedWeaponRefIDRight = 0;
        UInt32 m_droppedWeaponBaseIDLeft = 0;
        UInt32 m_droppedWeaponBaseIDRight = 0;
        
        // Track if we were dual-wielding same weapon when collision was triggered
        // This is needed to know if we should clean up the duplicate after re-equip
        bool m_wasDualWieldingSameWeaponLeft = false;
   bool m_wasDualWieldingSameWeaponRight = false;
        
   // Pending auto-unequip tracking (for trigger-based weapon hold system)
   // When a weapon is equipped and its trigger is not held, flag it for unequip
   // on the next frame. Tracked PER HAND so equipping both weapons in quick
   // succession does not let one hand clobber the other's pending state.
        TESForm* m_pendingAutoUnequipLeftForm = nullptr;
        TESForm* m_pendingAutoUnequipRightForm = nullptr;
        
  // Track which hand we're currently force-unequipping (for same-weapon detection)
        // -1 = none, 0 = right hand, 1 = left hand
    int m_forceUnequipHand = -1;
        
      bool m_initialized = false;

        // Guards against stacking duplicate delayed left-hand unequip checks
        bool m_comboLeftUnequipCheckPending = false;

        // Tracks the left game hand 2H weapon and when it became equipped,
        // so the combo re-equip can require a minimum equipped duration.
        UInt32 m_leftHand2HFormID = 0;
        std::chrono::steady_clock::time_point m_leftHand2HEquipTime;
    };

    // Convenience functions
    void RegisterEquipEventHandler();
    void UnregisterEquipEventHandler();
}
