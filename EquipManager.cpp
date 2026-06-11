#include "EquipManager.h"
#include "config.h"
#include "VRInputHandler.h"
#include "Engine.h"
#include "SkyrimVRESLAPI.h"
#include "ActivateHook.h"
#include "skse64/GameData.h"
#include "skse64/GameForms.h"
#include "skse64/GameExtraData.h"
#include "skse64/GameReferences.h"
#include "skse64/PapyrusActor.h"
#include "skse64/PluginAPI.h"
#include <thread>
#include <chrono>
#include <unordered_map>
#include <mutex>

namespace TwinGripVR
{
    extern SKSETaskInterface* g_task;

  // Static member initialization
    bool EquipManager::s_suppressPickupSound = false;
    bool EquipManager::s_suppressDrawSound = false;
    bool EquipManager::s_suppressSheathSound = false;

    // Per-weapon draw cooldowns (prevent same weapon draw sound within cooldown after unequip)
    // Key: weapon FormID, Value: time when weapon was UNEQUIPPED
    static std::unordered_map<UInt32, std::chrono::steady_clock::time_point> s_lastUnequipTimes;
    
    // Per-weapon sheath cooldowns (prevent same weapon sheath sound within cooldown after equip)
    // Key: weapon FormID, Value: time when weapon was EQUIPPED
    static std::unordered_map<UInt32, std::chrono::steady_clock::time_point> s_lastEquipTimes;
    
    static std::mutex s_drawMutex;
    static std::mutex s_sheathMutex;
    static const int DRAW_SOUND_COOLDOWN_SECONDS = 5;
    static const int SHEATH_SOUND_COOLDOWN_SECONDS = 5;

    // ============================================
    // Delayed Equip Weapon Task (runs on game thread)
    // ============================================
    class DelayedEquipWeaponTask : public TaskDelegate
    {
    public:
        UInt32 m_weaponFormId;
        bool m_equipToLeftHand;

        DelayedEquipWeaponTask(UInt32 weaponFormId, bool equipToLeftHand) 
            : m_weaponFormId(weaponFormId), m_equipToLeftHand(equipToLeftHand) {}

        virtual void Run() override
        {
            Actor* player = (*g_thePlayer);
            if (!player)
            {
                _MESSAGE("[DelayedEquipWeapon] Player not available");
                return;
            }

            TESForm* weaponForm = LookupFormByID(m_weaponFormId);
            if (!weaponForm)
            {
                _MESSAGE("[DelayedEquipWeapon] Weapon form %08X not found", m_weaponFormId);
                return;
            }

            ::EquipManager* equipMan = ::EquipManager::GetSingleton();
            if (!equipMan)
            {
                _MESSAGE("[DelayedEquipWeapon] EquipManager not available");
                return;
            }

            // Get the appropriate slot for left or right hand
            BGSEquipSlot* slot = m_equipToLeftHand ? GetLeftHandSlot() : GetRightHandSlot();

            // Suppress draw sound during delayed equip
            EquipManager::s_suppressDrawSound = true;
        
 // EquipItem params: actor, item, extraData, count, slot, withEquipSound, preventUnequip, showMsg, unk
            CALL_MEMBER_FN(equipMan, EquipItem)(player, weaponForm, nullptr, 1, slot, false, true, false, nullptr);
          
  EquipManager::s_suppressDrawSound = false;
            _MESSAGE("[DelayedEquipWeapon] Equipped weapon %08X to %s hand (silent)", m_weaponFormId, m_equipToLeftHand ? "LEFT" : "RIGHT");
        }

        virtual void Dispose() override
        {
            delete this;
        }
    };

    // ============================================
    // Scheduled Force Re-equip Task (runs on game thread)
    // Used by the trigger-held re-equip path. SafeActivate() puts the
    // grabbed weapon into inventory, but the pickup is not always
    // processed by the time ForceReequipHand() runs on the same frame
    // (and PollTriggerState runs in HIGGS's pre-physics callback, not
    // the game thread). Equipping too early silently fails and the
    // weapon "disappears" into the inventory.
    // This task runs on the game thread and waits (retrying once per
    // frame) until the item is actually present in the player's
    // inventory before calling ForceReequipHand().
    // ============================================
    class ScheduledForceReequipTask : public TaskDelegate
    {
    public:
        bool m_isLeftHand;
        int m_retriesLeft;

        ScheduledForceReequipTask(bool isLeftHand, int retriesLeft)
            : m_isLeftHand(isLeftHand), m_retriesLeft(retriesLeft) {}

        virtual void Run() override
        {
            EquipManager* manager = EquipManager::GetSingleton();

            UInt32 cachedFormID = manager->GetCachedWeaponFormID(m_isLeftHand);
            if (cachedFormID == 0)
            {
                // Cache was cleared (death/load/ClearAllState) - nothing to equip
                _MESSAGE("[ScheduledForceReequip] No cached FormID for %s hand - aborting", m_isLeftHand ? "LEFT" : "RIGHT");
                return;
            }

            PlayerCharacter* player = *g_thePlayer;
            TESForm* weaponForm = LookupFormByID(cachedFormID);

            if (player && weaponForm)
            {
                // Check if the activated item has arrived in the player's inventory yet
                bool itemInInventory = false;
                ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
                    player->extraData.GetByType(kExtraData_ContainerChanges));
                if (containerChanges && containerChanges->data)
                {
                    InventoryEntryData* entryData = containerChanges->data->FindItemEntry(weaponForm);
                    if (entryData && entryData->countDelta > 0)
                        itemInInventory = true;
                }

                if (!itemInInventory && m_retriesLeft > 0)
                {
                    // Pickup not processed yet - try again next frame
                    if (g_task)
                    {
                        g_task->AddTask(new ScheduledForceReequipTask(m_isLeftHand, m_retriesLeft - 1));
                        return;
                    }
                }

                if (!itemInInventory)
                {
                    _MESSAGE("[ScheduledForceReequip] WARNING: Item %08X never appeared in inventory - attempting equip anyway", cachedFormID);
                }
            }

            // Item is in inventory (or retries exhausted) - do the real equip
            EquipManager::s_suppressDrawSound = true;
            manager->ForceReequipHand(m_isLeftHand);
            EquipManager::s_suppressDrawSound = false;
        }

        virtual void Dispose() override
        {
            delete this;
        }
    };

    void EquipManager::ScheduleForceReequip(bool isLeftHand)
    {
        if (g_task)
        {
            // ~12 retries at one per frame = up to ~130ms grace for the pickup to process
            g_task->AddTask(new ScheduledForceReequipTask(isLeftHand, 12));
            _MESSAGE("EquipManager: Scheduled force re-equip for %s hand (game thread, waits for inventory)", isLeftHand ? "LEFT" : "RIGHT");
        }
        else
        {
            // Fallback: previous behavior (immediate equip)
            _MESSAGE("EquipManager: g_task unavailable - falling back to immediate re-equip for %s hand", isLeftHand ? "LEFT" : "RIGHT");
            s_suppressDrawSound = true;
            ForceReequipHand(isLeftHand);
            s_suppressDrawSound = false;
        }
    }

    // ============================================
    // Thread function to delay then queue the equip task
    // ============================================
    static void DelayedEquipWeaponThread(UInt32 weaponFormId, bool equipToLeftHand, int delayMs)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
   
        if (g_task)
        {
            g_task->AddTask(new DelayedEquipWeaponTask(weaponFormId, equipToLeftHand));
            _MESSAGE("[EquipManager] Queued weapon equip task after %dms delay for weapon %08X to %s hand", 
        delayMs, weaponFormId, equipToLeftHand ? "LEFT" : "RIGHT");
        }
        else
        {
            _MESSAGE("[EquipManager] ERROR: g_task not available for delayed equip!");
        }
    }

    // ============================================
    // ContainerChangeEventHandler - Logs when weapons are added to player inventory
    // ============================================
    
    class ContainerChangeEventHandler : public BSTEventSink<TESContainerChangedEvent>
    {
    public:
   static ContainerChangeEventHandler* GetSingleton()
        {
     static ContainerChangeEventHandler instance;
            return &instance;
        }
        
        virtual EventResult ReceiveEvent(TESContainerChangedEvent* evn, EventDispatcher<TESContainerChangedEvent>* dispatcher) override;
      
    private:
        ContainerChangeEventHandler() = default;
      ~ContainerChangeEventHandler() = default;
        ContainerChangeEventHandler(const ContainerChangeEventHandler&) = delete;
 ContainerChangeEventHandler& operator=(const ContainerChangeEventHandler&) = delete;
    };

    // ============================================
    // EquipEventHandler Implementation
    // ============================================
    
    EquipEventHandler* EquipEventHandler::GetSingleton()
    {
   static EquipEventHandler instance;
        return &instance;
    }

    EventResult EquipEventHandler::ReceiveEvent(TESEquipEvent* evn, EventDispatcher<TESEquipEvent>* dispatcher)
{
        if (!evn)
     return kEvent_Continue;

    Actor* actor = DYNAMIC_CAST(evn->actor, TESObjectREFR, Actor);
        if (!actor)
     return kEvent_Continue;

        TESForm* item = LookupFormByID(evn->baseObject);
        if (!item)
 return kEvent_Continue;

        bool isEquipping = evn->equipped;

        // Check if this is a one-handed weapon we track
        if (!EquipManager::IsWeapon(item))
  {
    // For player, also check shields
    if (actor == *g_thePlayer && EquipManager::IsShield(item))
 {
    // Continue to player handling below
}
else
   {
    // ============================================
// CHECK FOR 2H WEAPON EQUIP - CLEAN UP GRABBED WEAPONS
   // When player equips a 2H weapon, any grabbed weapons need to be
// picked up to inventory and tracking cleared
     // ============================================
  if (actor == *g_thePlayer && isEquipping && EquipManager::IsTwoHandedWeapon(item))
  {
        _MESSAGE("EquipEventHandler: Player equipped 2H weapon - checking for grabbed weapons to clean up");
        
PlayerCharacter* player = *g_thePlayer;
        EquipManager* equipMgr = EquipManager::GetSingleton();
     
      // Check left hand for grabbed weapon
  TESObjectREFR* droppedLeft = equipMgr->GetDroppedWeaponRef(true);
 if (droppedLeft)
     {
            _MESSAGE("EquipEventHandler: Found grabbed weapon in LEFT hand - picking up to inventory");
        if (player)
    {
     EquipManager::s_suppressPickupSound = true;
SafeActivate(droppedLeft, player, 0, 0, 1, false);
        EquipManager::s_suppressPickupSound = false;
      }
        equipMgr->ClearDroppedWeaponRef(true);
            equipMgr->ClearPendingReequip(true);
      equipMgr->ClearCachedWeaponFormID(true);
   }
        
      // Check right hand for grabbed weapon
  TESObjectREFR* droppedRight = equipMgr->GetDroppedWeaponRef(false);
   if (droppedRight)
        {
     _MESSAGE("EquipEventHandler: Found grabbed weapon in RIGHT hand - picking up to inventory");
  if (player)
            {
         EquipManager::s_suppressPickupSound = true;
 SafeActivate(droppedRight, player, 0, 0, 1, false);
     EquipManager::s_suppressPickupSound = false;
    }
   equipMgr->ClearDroppedWeaponRef(false);
         equipMgr->ClearPendingReequip(false);
   equipMgr->ClearCachedWeaponFormID(false);
     }
    }
    
    return kEvent_Continue;
   }
    }

        // ============================================
        // NPC EQUIP TRACKING (within 1000 units of player)
      // ============================================
   if (actor != *g_thePlayer && isEquipping)
        {
     PlayerCharacter* player = *g_thePlayer;
  if (player)
    {
    // Calculate distance to player
       float dx = actor->pos.x - player->pos.x;
   float dy = actor->pos.y - player->pos.y;
    float dz = actor->pos.z - player->pos.z;
       float distance = sqrt(dx*dx + dy*dy + dz*dz);
          
    // Only log and play sound if within 1000 units
 if (distance <= 1000.0f)
      {
          WeaponType type = EquipManager::GetWeaponType(item);
    const char* npcName = CALL_MEMBER_FN(actor, GetReferenceName)();
   
    // Cache sound FormIDs from Fake Edge VR.esp (ESL-flagged)
           // Base FormIDs: Dagger=0x806, Sword=0x807, Axe=0x808, Mace=0x809
       static UInt32 cachedDaggerSound = 0;
static UInt32 cachedSwordSound = 0;
   static UInt32 cachedAxeSound = 0;
             static UInt32 cachedMaceSound = 0;
     static bool soundsCached = false;
                
     if (!soundsCached)
      {
  cachedDaggerSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x806);
        cachedSwordSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x807);
     cachedAxeSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x808);
          cachedMaceSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x809);
        soundsCached = true;
    _MESSAGE("EquipManager: Cached weapon draw sounds - Dagger:%08X, Sword:%08X, Axe:%08X, Mace:%08X",
       cachedDaggerSound, cachedSwordSound, cachedAxeSound, cachedMaceSound);
     }
      
  switch (type)
     {
    case WeaponType::Dagger:
    _MESSAGE(">>> NPC EQUIPPED: DAGGER - NPC: %s (RefID: %08X), Distance: %.1f units, WeaponID: %08X",
    npcName ? npcName : "Unknown", actor->formID, distance, item->formID);
   if (cachedDaggerSound != 0)
   PlaySoundAtActor(cachedDaggerSound, actor);
   break;
   case WeaponType::Sword:
    _MESSAGE(">>> NPC EQUIPPED: 1H SWORD - NPC: %s (RefID: %08X), Distance: %.1f units, WeaponID: %08X",
       npcName ? npcName : "Unknown", actor->formID, distance, item->formID);
        if (cachedSwordSound != 0)
       PlaySoundAtActor(cachedSwordSound, actor);
   break;
  case WeaponType::Mace:
  _MESSAGE(">>> NPC EQUIPPED: 1H MACE - NPC: %s (RefID: %08X), Distance: %.1f units, WeaponID: %08X",
  npcName ? npcName : "Unknown", actor->formID, distance, item->formID);
   if (cachedMaceSound != 0)
   PlaySoundAtActor(cachedMaceSound, actor);
   break;
case WeaponType::Axe:
 _MESSAGE(">>> NPC EQUIPPED: 1H AXE - NPC: %s (RefID: %08X), Distance: %.1f units, WeaponID: %08X",
  npcName ? npcName : "Unknown", actor->formID, distance, item->formID);
        if (cachedAxeSound != 0)
   PlaySoundAtActor(cachedAxeSound, actor);
      break;
 default:
      break;
 }
  }
 }
     return kEvent_Continue;
    }

        // ============================================
        // NPC UNEQUIP TRACKING (within 1000 units of player)
        // ============================================
  if (actor != *g_thePlayer && !isEquipping)
        {
        PlayerCharacter* player = *g_thePlayer;
      if (player)
            {
                // Calculate distance to player
  float dx = actor->pos.x - player->pos.x;
         float dy = actor->pos.y - player->pos.y;
                float dz = actor->pos.z - player->pos.z;
    float distance = sqrt(dx*dx + dy*dy + dz*dz);
            
 // Only play sound if within 1000 units
     if (distance <= 1000.0f)
 {
         WeaponType type = EquipManager::GetWeaponType(item);
                const char* npcName = CALL_MEMBER_FN(actor, GetReferenceName)();
     
                    // Cache sheath sound FormIDs from Fake Edge VR.esp (ESL-flagged)
  // Base FormIDs: Dagger=0x80A, Sword=0x80B, Axe=0x80C, Mace=0x80D
     static UInt32 cachedDaggerSheathSound = 0;
    static UInt32 cachedSwordSheathSound = 0;
                    static UInt32 cachedAxeSheathSound = 0;
         static UInt32 cachedMaceSheathSound = 0;
     static bool sheathSoundsCached = false;
    
                    if (!sheathSoundsCached)
  {
          cachedDaggerSheathSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x80A);
   cachedSwordSheathSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x80B);
        cachedAxeSheathSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x80C);
    cachedMaceSheathSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x80D);
         sheathSoundsCached = true;
            _MESSAGE("EquipManager: Cached weapon sheath sounds - Dagger:%08X, Sword:%08X, Axe:%08X, Mace:%08X",
      cachedDaggerSheathSound, cachedSwordSheathSound, cachedAxeSheathSound, cachedMaceSheathSound);
          }
       
           switch (type)
  {
            case WeaponType::Dagger:
         _MESSAGE(">>> NPC UNEQUIPPED: DAGGER - NPC: %s (RefID: %08X), Distance: %.1f units, WeaponID: %08X",
              npcName ? npcName : "Unknown", actor->formID, distance, item->formID);
   if (cachedDaggerSheathSound != 0)
    PlaySoundAtActor(cachedDaggerSheathSound, actor);
      break;
          case WeaponType::Sword:
           _MESSAGE(">>> NPC UNEQUIPPED: 1H SWORD - NPC: %s (RefID: %08X), Distance: %.1f units, WeaponID: %08X",
              npcName ? npcName : "Unknown", actor->formID, distance, item->formID);
  if (cachedSwordSheathSound != 0)
         PlaySoundAtActor(cachedSwordSheathSound, actor);
       break;
        case WeaponType::Mace:
  _MESSAGE(">>> NPC UNEQUIPPED: 1H MACE - NPC: %s (RefID: %08X), Distance: %.1f units, WeaponID: %08X",
       npcName ? npcName : "Unknown", actor->formID, distance, item->formID);
 if (cachedMaceSheathSound != 0)
          PlaySoundAtActor(cachedMaceSheathSound, actor);
                  break;
       case WeaponType::Axe:
        _MESSAGE(">>> NPC UNEQUIPPED: 1H AXE - NPC: %s (RefID: %08X), Distance: %.1f units, WeaponID: %08X",
                  npcName ? npcName : "Unknown", actor->formID, distance, item->formID);
          if (cachedAxeSheathSound != 0)
    PlaySoundAtActor(cachedAxeSheathSound, actor);
        break;
         default:
   break;
              }
                }
            }
    return kEvent_Continue;
        }

        // ============================================
        // PLAYER EQUIP TRACKING (existing logic)
     // ============================================
        if (actor != *g_thePlayer)
        return kEvent_Continue;

     _MESSAGE("EquipEventHandler: Received equip event for FormID %08X, equipped=%d", evn->baseObject, evn->equipped);

     // Determining which hand based on the equipped flag
     
        // Get the actual hand from the currently equipped objects
     bool isLeftHand = false;
        TESForm* leftEquipped = actor->GetEquippedObject(true);
        TESForm* rightEquipped = actor->GetEquippedObject(false);
        
      if (isEquipping)
{
     if (leftEquipped && leftEquipped->formID == item->formID)
   isLeftHand = true;
   else if (rightEquipped && rightEquipped->formID == item->formID)
         isLeftHand = false;
            
EquipManager::GetSingleton()->OnEquip(item, actor, isLeftHand);
   }
        else
        {
 const PlayerEquipState& state = EquipManager::GetSingleton()->GetEquipState();
            if (state.leftHand.form && state.leftHand.form->formID == item->formID)
      isLeftHand = true;
        else if (state.rightHand.form && state.rightHand.form->formID == item->formID)
   isLeftHand = false;
  
            EquipManager::GetSingleton()->OnUnequip(item, actor, isLeftHand);
   }

      return kEvent_Continue;
    }

    // ============================================
    // EquipManager Implementation
    // ============================================

    EquipManager* EquipManager::GetSingleton()
    {
        static EquipManager instance;
 return &instance;
    }

    void EquipManager::Initialize()
 {
        if (m_initialized)
          return;

        _MESSAGE("EquipManager: Initializing...");
        
        m_equipState.leftHand.Clear();
      m_equipState.rightHand.Clear();
        
        m_initialized = true;
     _MESSAGE("EquipManager: Initialized successfully");
    }

    void EquipManager::UpdateEquipmentState()
{
        PlayerCharacter* player = *g_thePlayer;
        if (!player)
   {
   _MESSAGE("EquipManager::UpdateEquipmentState - No player!");
      return;
      }

  TESForm* leftItem = player->GetEquippedObject(true);
        TESForm* rightItem = player->GetEquippedObject(false);

 _MESSAGE("EquipManager::UpdateEquipmentState - Left: %08X, Right: %08X", 
            leftItem ? leftItem->formID : 0, 
      rightItem ? rightItem->formID : 0);

    // Update left hand
  if (leftItem && (IsWeapon(leftItem) || IsShield(leftItem)))
        {
    m_equipState.leftHand.form = leftItem;
  m_equipState.leftHand.type = GetWeaponType(leftItem);
            m_equipState.leftHand.isEquipped = true;
        }
     else
 {
   m_equipState.leftHand.Clear();
        }

        // Update right hand
        if (rightItem && (IsWeapon(rightItem) || IsShield(rightItem)))
        {
         m_equipState.rightHand.form = rightItem;
  m_equipState.rightHand.type = GetWeaponType(rightItem);
        m_equipState.rightHand.isEquipped = true;
        }
  else
        {
     m_equipState.rightHand.Clear();
   }

     LogEquipmentState();
    }

    void EquipManager::OnEquip(TESForm* item, Actor* actor, bool isLeftHand)
    {
      if (!item)
       return;

  WeaponType type = GetWeaponType(item);
        const char* typeName = GetWeaponTypeName(type);
   const char* handName = isLeftHand ? "Left" : "Right";

  EquippedWeapon& hand = isLeftHand ? m_equipState.leftHand : m_equipState.rightHand;
   hand.form = item;
        hand.type = type;
 hand.isEquipped = true;

  _MESSAGE("EquipManager: EQUIPPED %s in %s hand (FormID: %08X)", typeName, handName, item->formID);
        
    // Record equip time for sheath sound cooldown
        // Only track weapons (not shields)
 if (type != WeaponType::Shield && type != WeaponType::None)
        {
    std::lock_guard<std::mutex> lock(s_sheathMutex);
      s_lastEquipTimes[item->formID] = std::chrono::steady_clock::now();
   _MESSAGE("EquipManager: Recorded equip time for weapon %08X (5s sheath sound cooldown started)", item->formID);
        }
     
        // ============================================
     // TRIGGER-BASED WEAPON HOLD: Auto-unequip weapon on equip
   // ANY 1H weapon will be HIGGS grabbed unless trigger is held
        // This applies to: single weapon, dual-wield, and shield+weapon
    // EXCLUDED: 2H weapons, bows, staffs, bound weapons (handled by IsWeapon check above)
   // ============================================
        if (type != WeaponType::Shield && type != WeaponType::None)
        {
      // Check if trigger is currently held for this hand - READ DIRECTLY FROM OPENVR
   bool triggerHeld = false;
   BSOpenVR* openVR = (*g_openVR);
        if (openVR && openVR->vrSystem)
       {
    vr_1_0_12::IVRSystem* vrSystem = openVR->vrSystem;
      bool vrControllerIsLeft = GameHandToVRController(isLeftHand);
             
     vr_1_0_12::TrackedDeviceIndex_t controller = vrSystem->GetTrackedDeviceIndexForControllerRole(
   vrControllerIsLeft ? 
        vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_LeftHand :
             vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_RightHand);
          
        vr_1_0_12::VRControllerState_t state;
           if (vrSystem->GetControllerState(controller, &state, sizeof(state)))
  {
        const uint64_t TRIGGER_BUTTON_MASK = (1ull << 33);
   bool digitalPressed = (state.ulButtonPressed & TRIGGER_BUTTON_MASK) != 0;
     bool analogPressed = (state.rAxis[1].x > 0.5f);
 triggerHeld = digitalPressed || analogPressed;
    }
            }
         
   _MESSAGE("EquipManager: WEAPON EQUIP - %s hand trigger state check: %s", 
         isLeftHand ? "LEFT" : "RIGHT", triggerHeld ? "HELD" : "NOT HELD");
    
          if (!triggerHeld)
        {
  _MESSAGE("EquipManager: WEAPON EQUIPPED - auto-unequipping for HIGGS grab");
  _MESSAGE("EquipManager: Trigger not held, weapon will be grabbed by HIGGS");
 
 // Delay the unequip slightly to let the equip complete
      m_pendingAutoUnequipLeft = isLeftHand;
  m_pendingAutoUnequipRight = !isLeftHand;
     m_pendingAutoUnequipForm = item;
 }
  else
            {
        _MESSAGE("EquipManager: WEAPON EQUIPPED - trigger held, keeping equipped");
   }
   }
    
        // Cache sound FormIDs from Fake Edge VR.esp (ESL-flagged)
        // Base FormIDs: Dagger=0x806, Sword=0x807, Axe=0x808, Mace=0x809
        static UInt32 cachedDaggerSound = 0;
   static UInt32 cachedSwordSound = 0;
     static UInt32 cachedAxeSound = 0;
        static UInt32 cachedMaceSound = 0;
     static bool soundsCached = false;
  
        if (!soundsCached)
        {
          cachedDaggerSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x806);
        cachedSwordSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x807);
 cachedAxeSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x808);
    cachedMaceSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x809);
     soundsCached = true;
   }
        
        // Log specific weapon types and play draw sounds (unless suppressed by collision logic or excluded weapons)
    bool shouldExclude = IsExcludedWeaponFormID(item->formID);
        
      // Check draw sound cooldown (5 seconds from last unequip of same weapon)
     bool onDrawCooldown = false;
        {
    std::lock_guard<std::mutex> lock(s_drawMutex);
       auto it = s_lastUnequipTimes.find(item->formID);
    if (it != s_lastUnequipTimes.end())
     {
     auto now = std::chrono::steady_clock::now();
          auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
       if (elapsed < DRAW_SOUND_COOLDOWN_SECONDS)
       {
            onDrawCooldown = true;
    _MESSAGE("EquipManager: Draw sound on cooldown for %08X (%lld/%d seconds since unequip)", 
     item->formID, elapsed, DRAW_SOUND_COOLDOWN_SECONDS);
         }
}
        }
      
        switch (type)
        {
            case WeaponType::Dagger:
       _MESSAGE(">>> PLAYER EQUIPPED: DAGGER (FormID: %08X) in %s hand", item->formID, handName);
    if (!s_suppressDrawSound && !shouldExclude && !onDrawCooldown && cachedDaggerSound != 0)
  PlaySoundAtPlayer(cachedDaggerSound);
          break;
        case WeaponType::Sword:
            _MESSAGE(">>> PLAYER EQUIPPED: 1H SWORD (FormID: %08X) in %s hand", item->formID, handName);
                if (!s_suppressDrawSound && !shouldExclude && !onDrawCooldown && cachedSwordSound != 0)
       PlaySoundAtPlayer(cachedSwordSound);
       break;
            case WeaponType::Mace:
         _MESSAGE(">>> PLAYER EQUIPPED: 1H MACE (FormID: %08X) in %s hand", item->formID, handName);
      if (!s_suppressDrawSound && !shouldExclude && !onDrawCooldown && cachedMaceSound != 0)
           PlaySoundAtPlayer(cachedMaceSound);
   break;
   case WeaponType::Axe:
      _MESSAGE(">>> PLAYER EQUIPPED: 1H AXE (FormID: %08X) in %s hand", item->formID, handName);
     if (!s_suppressDrawSound && !shouldExclude && !onDrawCooldown && cachedAxeSound != 0)
      PlaySoundAtPlayer(cachedAxeSound);
  break;
   case WeaponType::Shield:
  _MESSAGE(">>> PLAYER EQUIPPED: SHIELD (FormID: %08X) in %s hand", item->formID, handName);
   break;
            default:
          break;
        }
 
        if (s_suppressDrawSound)
        {
            _MESSAGE("EquipManager: Skipping draw sound (internal collision re-equip)");
        }
 
        LogEquipmentState();
   
        // Update VR input handler grab listening
        VRInputHandler::GetSingleton()->UpdateGrabListening();
    }

    void EquipManager::OnUnequip(TESForm* item, Actor* actor, bool isLeftHand)
    {
      if (!item)
   return;

        WeaponType type = GetWeaponType(item);
  const char* typeName = GetWeaponTypeName(type);
        const char* handName = isLeftHand ? "Left" : "Right";

   EquippedWeapon& hand = isLeftHand ? m_equipState.leftHand : m_equipState.rightHand;
    hand.Clear();

   _MESSAGE("EquipManager: UNEQUIPPED %s from %s hand (FormID: %08X)", typeName, handName, item->formID);
     
        // Record unequip time for draw sound cooldown
      // Only track weapons (not shields)
   if (type != WeaponType::Shield && type != WeaponType::None)
        {
    std::lock_guard<std::mutex> lock(s_drawMutex);
      s_lastUnequipTimes[item->formID] = std::chrono::steady_clock::now();
   _MESSAGE("EquipManager: Recorded unequip time for weapon %08X (5s draw sound cooldown started)", item->formID);
        }
        
        // ============================================
        // PLAYER SHEATH SOUNDS
 // ============================================
     // Cache sheath sound FormIDs from Fake Edge VR.esp (ESL-flagged)
        // Base FormIDs: Dagger=0x80A, Sword=0x80B, Axe=0x80C, Mace=0x80D
     static UInt32 cachedDaggerSheathSound = 0;
        static UInt32 cachedSwordSheathSound = 0;
        static UInt32 cachedAxeSheathSound = 0;
        static UInt32 cachedMaceSheathSound = 0;
        static bool sheathSoundsCached = false;
        
        if (!sheathSoundsCached)
     {
    cachedDaggerSheathSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x80A);
       cachedSwordSheathSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x80B);
            cachedAxeSheathSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x80C);
            cachedMaceSheathSound = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x80D);
            sheathSoundsCached = true;
   _MESSAGE("EquipManager: Cached player sheath sounds - Dagger:%08X, Sword:%08X, Axe:%08X, Mace:%08X",
          cachedDaggerSheathSound, cachedSwordSheathSound, cachedAxeSheathSound, cachedMaceSheathSound);
     }
        
        // Check if this weapon should be excluded from sounds
        bool shouldExclude = IsExcludedWeaponFormID(item->formID);
        
  // Check sheath sound cooldown (5 seconds from last equip of same weapon)
 bool onSheathCooldown = false;
  {
std::lock_guard<std::mutex> lock(s_sheathMutex);
        auto it = s_lastEquipTimes.find(item->formID);
   if (it != s_lastEquipTimes.end())
        {
    auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
                if (elapsed < SHEATH_SOUND_COOLDOWN_SECONDS)
      {
      onSheathCooldown = true;
          _MESSAGE("EquipManager: Sheath sound on cooldown for %08X (%lld/%d seconds since equip)", 
            item->formID, elapsed, SHEATH_SOUND_COOLDOWN_SECONDS);
                }
            }
        }
        
        switch (type)
     {
     case WeaponType::Dagger:
           _MESSAGE(">>> PLAYER UNEQUIPPED: DAGGER (FormID: %08X) from %s hand", item->formID, handName);
                if (!s_suppressSheathSound && !shouldExclude && !onSheathCooldown && cachedDaggerSheathSound != 0)
      PlaySoundAtPlayer(cachedDaggerSheathSound);
     break;
            case WeaponType::Sword:
          _MESSAGE(">>> PLAYER UNEQUIPPED: 1H SWORD (FormID: %08X) from %s hand", item->formID, handName);
                if (!s_suppressSheathSound && !shouldExclude && !onSheathCooldown && cachedSwordSheathSound != 0)
   PlaySoundAtPlayer(cachedSwordSheathSound);
                break;
case WeaponType::Mace:
      _MESSAGE(">>> PLAYER UNEQUIPPED: 1H MACE (FormID: %08X) from %s hand", item->formID, handName);
                if (!s_suppressSheathSound && !shouldExclude && !onSheathCooldown && cachedMaceSheathSound != 0)
          PlaySoundAtPlayer(cachedMaceSheathSound);
      break;
            case WeaponType::Axe:
    _MESSAGE(">>> PLAYER UNEQUIPPED: 1H AXE (FormID: %08X) from %s hand", item->formID, handName);
       if (!s_suppressSheathSound && !shouldExclude && !onSheathCooldown && cachedAxeSheathSound != 0)
         PlaySoundAtPlayer(cachedAxeSheathSound);
   break;
            case WeaponType::Shield:
           _MESSAGE(">>> PLAYER UNEQUIPPED: SHIELD (FormID: %08X) from %s hand", item->formID, handName);
     break;
            default:
      break;
   }
    
        if (s_suppressSheathSound)
        {
     _MESSAGE("EquipManager: Skipping sheath sound (internal collision unequip)");
        }
        
        if (m_equipState.HasOneWeaponEquipped())
 {
            const char* remainingHand = m_equipState.leftHand.isEquipped ? "Left" : "Right";
            WeaponType remainingType = m_equipState.leftHand.isEquipped 
  ? m_equipState.leftHand.type 
     : m_equipState.rightHand.type;

            _MESSAGE("EquipManager: Player now has SINGLE weapon equipped - %s in %s hand", 
    GetWeaponTypeName(remainingType), remainingHand);
   }
   
  LogEquipmentState();
  
        // Update VR input handler grab listening
        VRInputHandler::GetSingleton()->UpdateGrabListening();
    }

    void EquipManager::LogEquipmentState()
    {
        _MESSAGE("EquipManager: === Equipment State ===");
        _MESSAGE("  Left Hand:  %s (%s)", 
            m_equipState.leftHand.isEquipped ? GetWeaponTypeName(m_equipState.leftHand.type) : "Empty",
       m_equipState.leftHand.form ? std::to_string(m_equipState.leftHand.form->formID).c_str() : "None");
        _MESSAGE("  Right Hand: %s (%s)", 
      m_equipState.rightHand.isEquipped ? GetWeaponTypeName(m_equipState.rightHand.type) : "Empty",
            m_equipState.rightHand.form ? std::to_string(m_equipState.rightHand.form->formID).c_str() : "None");
    _MESSAGE("  Weapon Count: %d", m_equipState.GetEquippedWeaponCount());
        _MESSAGE("  Single Weapon: %s", m_equipState.HasOneWeaponEquipped() ? "YES" : "NO");
        _MESSAGE("==============================");
    }

    WeaponType EquipManager::GetWeaponType(TESForm* form)
    {
        if (!form)
  return WeaponType::None;

   if (IsShield(form))
     return WeaponType::Shield;

      TESObjectWEAP* weapon = DYNAMIC_CAST(form, TESForm, TESObjectWEAP);
        if (!weapon)
     return WeaponType::None;

     switch (weapon->gameData.type)
        {
 case TESObjectWEAP::GameData::kType_OneHandSword:
        case TESObjectWEAP::GameData::kType_1HS:
                return WeaponType::Sword;
     
   case TESObjectWEAP::GameData::kType_OneHandDagger:
      case TESObjectWEAP::GameData::kType_1HD:
       return WeaponType::Dagger;
  
    case TESObjectWEAP::GameData::kType_OneHandMace:
  case TESObjectWEAP::GameData::kType_1HM:
             return WeaponType::Mace;
            
 case TESObjectWEAP::GameData::kType_OneHandAxe:
            case TESObjectWEAP::GameData::kType_1HA:
      return WeaponType::Axe;

            // Two-handed weapons, bows, staffs - EXCLUDED from our tracking
            case TESObjectWEAP::GameData::kType_TwoHandSword:
            case TESObjectWEAP::GameData::kType_2HS:
            case TESObjectWEAP::GameData::kType_TwoHandAxe:
   case TESObjectWEAP::GameData::kType_2HA:
     case TESObjectWEAP::GameData::kType_Bow:
     case TESObjectWEAP::GameData::kType_Staff:
            case TESObjectWEAP::GameData::kType_CrossBow:
       return WeaponType::None;  // Treat as not a weapon for our purposes
            

            default:
   return WeaponType::None;
        }
    }

    const char* EquipManager::GetWeaponTypeName(WeaponType type)
    {
   switch (type)
        {
 case WeaponType::Sword:      return "Sword";
   case WeaponType::Dagger:   return "Dagger";
            case WeaponType::Mace:       return "Mace";
        case WeaponType::Axe:     return "Axe";
       case WeaponType::Shield:     return "Shield";
     case WeaponType::None:
       default: return "None";
        }
    }

    bool EquipManager::IsWeapon(TESForm* form)
    {
        if (!form)
  return false;

        // Exclude items listed in TwinGripVR.ini [WeaponExclusions]
     if (IsExcludedWeaponFormID(form->formID))
         return false;

        if (form->formType != kFormType_Weapon)
       return false;
        
        // Check if it's a weapon type we actually track (one-handed only)
    // Exclude bows, staffs, crossbows, two-handed weapons, and bound weapons
  TESObjectWEAP* weapon = DYNAMIC_CAST(form, TESForm, TESObjectWEAP);
        if (!weapon)
   return false;
     
        // Check for bound weapon keyword - exclude bound weapons
        BGSKeywordForm* keywordForm = DYNAMIC_CAST(form, TESForm, BGSKeywordForm);
        if (keywordForm)
        {
            // WeapTypeBoundWeapon keyword FormID is 0x0010D501 in Skyrim.esm
        static const UInt32 kWeapTypeBoundWeapon = 0x0010D501;
            BGSKeyword* boundKeyword = DYNAMIC_CAST(LookupFormByID(kWeapTypeBoundWeapon), TESForm, BGSKeyword);
    if (boundKeyword && keywordForm->HasKeyword(boundKeyword))
        {
        return false;  // Bound weapon - don't track
        }
        }
 
        switch (weapon->gameData.type)
        {
   case TESObjectWEAP::GameData::kType_OneHandSword:
            case TESObjectWEAP::GameData::kType_1HS:
      case TESObjectWEAP::GameData::kType_OneHandDagger:
case TESObjectWEAP::GameData::kType_1HD:
    case TESObjectWEAP::GameData::kType_OneHandMace:
          case TESObjectWEAP::GameData::kType_1HM:
   case TESObjectWEAP::GameData::kType_OneHandAxe:
         case TESObjectWEAP::GameData::kType_1HA:
      return true;  // One-handed weapons - we track these
            
            default:
 return false;  // Bows, staffs, crossbows, two-handed - don't track
   }
    }

    bool EquipManager::IsShield(TESForm* form)
    {
      if (!form)
     return false;

 if (form->formType != kFormType_Armor)
      return false;

TESObjectARMO* armor = DYNAMIC_CAST(form, TESForm, TESObjectARMO);
        if (!armor)
   return false;

 return (armor->bipedObject.GetSlotMask() & BGSBipedObjectForm::kPart_Shield) != 0;
    }

    bool EquipManager::IsTwoHandedWeapon(TESForm* form)
    {
     if (!form)
    return false;
        
  if (form->formType != kFormType_Weapon)
      return false;
        
 TESObjectWEAP* weapon = DYNAMIC_CAST(form, TESForm, TESObjectWEAP);
        if (!weapon)
  return false;
        
 switch (weapon->gameData.type)
   {
      case TESObjectWEAP::GameData::kType_TwoHandSword:
          case TESObjectWEAP::GameData::kType_2HS:
     case TESObjectWEAP::GameData::kType_TwoHandAxe:
        case TESObjectWEAP::GameData::kType_2HA:
      case TESObjectWEAP::GameData::kType_Bow:
     case TESObjectWEAP::GameData::kType_Staff:
         case TESObjectWEAP::GameData::kType_CrossBow:
return true;
   default:
    return false;
    }
    }
    
  bool EquipManager::PlayerHasTwoHandedEquipped()
    {
      PlayerCharacter* player = *g_thePlayer;
      if (!player)
       return false;
      
      // Check both hands - 2H weapons typically show in right hand
 TESForm* rightEquipped = player->GetEquippedObject(false);
        TESForm* leftEquipped = player->GetEquippedObject(true);
        
    if (rightEquipped && IsTwoHandedWeapon(rightEquipped))
         return true;
        
    if (leftEquipped && IsTwoHandedWeapon(leftEquipped))
     return true;
        
        return false;
    }

  // ============================================
    // Forced Unequip Functions
    // ============================================

    void EquipManager::ForceUnequipHand(bool isLeftHand)
    {
   PlayerCharacter* player = *g_thePlayer;
        if (!player)
      {
      _MESSAGE("EquipManager::ForceUnequipHand - No player!");
     return;
        }

 EquippedWeapon& hand = isLeftHand ? m_equipState.leftHand : m_equipState.rightHand;
     if (!hand.isEquipped || !hand.form)
        {
 _MESSAGE("EquipManager::ForceUnequipHand - %s hand has no weapon to unequip", 
   isLeftHand ? "Left" : "Right");
       return;
        }

   TESForm* item = hand.form;
 
        // Store the weapon for later re-equip
        if (isLeftHand)
     {
     m_pendingReequipLeft = item;
        }
        else
{
         m_pendingReequipRight = item;
        }
    
        _MESSAGE("EquipManager: FORCE UNEQUIPPING %s from %s hand (FormID: %08X) - stored for re-equip", 
      GetWeaponTypeName(hand.type), 
        isLeftHand ? "Left" : "Right", 
     item->formID);

 // Get the EquipManager singleton from the game
    ::EquipManager* equipManager = ::EquipManager::GetSingleton();
        if (!equipManager)
        {
   _MESSAGE("EquipManager::ForceUnequipHand - Failed to get game EquipManager!");
  return;
        }

        // Get container changes to find the equipped item's extra data
        ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
   player->extraData.GetByType(kExtraData_ContainerChanges));
      
        if (!containerChanges || !containerChanges->data)
    {
      _MESSAGE("EquipManager::ForceUnequipHand - No container changes data!");
   return;
   }

     // Find the inventory entry for this item
 InventoryEntryData* entryData = containerChanges->data->FindItemEntry(item);
     if (!entryData)
   {
        _MESSAGE("EquipManager::ForceUnequipHand - Item not found in inventory!");
 return;
        }

      // Get the extra data lists for worn items
        BaseExtraList* rightEquipList = NULL;
      BaseExtraList* leftEquipList = NULL;
        entryData->GetExtraWornBaseLists(&rightEquipList, &leftEquipList);

    // Get the correct equip list and slot based on hand
        BaseExtraList* equipList = isLeftHand ? leftEquipList : rightEquipList;
     BGSEquipSlot* equipSlot = isLeftHand ? GetLeftHandSlot() : GetRightHandSlot();

        if (!equipList)
        {
 _MESSAGE("EquipManager::ForceUnequipHand - No equip list found for %s hand!", 
     isLeftHand ? "Left" : "Right");
            return;
        }

 // Remove CannotWear flag if present
        BSExtraData* xCannotWear = equipList->GetByType(kExtraData_CannotWear);
  if (xCannotWear)
        {
    equipList->Remove(kExtraData_CannotWear, xCannotWear);
        }

      // Unequip the item (silent - no sound, no message)
        s_suppressSheathSound = true;
  CALL_MEMBER_FN(equipManager, UnequipItem)(player, item, equipList, 1, equipSlot, false, true, true, false, NULL);
        s_suppressSheathSound = false;

    _MESSAGE("EquipManager: Force unequip command sent for %s hand (silent)", isLeftHand ? "Left" : "Right");
}

    void EquipManager::ForceUnequipLeftHand()
    {
        ForceUnequipHand(true);
    }

    void EquipManager::ForceUnequipRightHand()
    {
        ForceUnequipHand(false);
    }

    void EquipManager::ForceReequipHand(bool isLeftHand)
    {
      PlayerCharacter* player = *g_thePlayer;
     if (!player)
        {
     _MESSAGE("EquipManager::ForceReequipHand - No player!");
            return;
        }

     // Use correct cached FormID for each hand
     UInt32 cachedFormID = isLeftHand ? m_cachedWeaponFormIDLeft : m_cachedWeaponFormIDRight;

     if (cachedFormID == 0)
     {
         _MESSAGE("EquipManager::ForceReequipHand - No cached weapon FormID for %s hand!",
             isLeftHand ? "Left" : "Right");
         return;
     }

     TESForm* weaponForm = LookupFormByID(cachedFormID);
     if (!weaponForm)
     {
         _MESSAGE("EquipManager::ForceReequipHand - Weapon form %08X not found!", cachedFormID);
         return;
     }

     ::EquipManager* equipMan = ::EquipManager::GetSingleton();
     if (!equipMan)
     {
         _MESSAGE("EquipManager::ForceReequipHand - EquipManager not available!");
         return;
     }
    
      // Get the appropriate slot for left or right hand
        BGSEquipSlot* slot = isLeftHand ? GetLeftHandSlot() : GetRightHandSlot();
 
    // ============================================
// FIND THE CORRECT INVENTORY ITEM WITH TEMPERING/ENCHANTMENT
      // We need to find the item with matching health value (tempering) AND enchantment
   // ============================================
  BaseExtraList* extraDataToUse = nullptr;
  bool hasCachedHealth = isLeftHand ? m_hasCachedHealthLeft : m_hasCachedHealthRight;
    float cachedHealth = isLeftHand ? m_cachedWeaponHealthLeft : m_cachedWeaponHealthRight;
        bool hasCachedEnchant = isLeftHand ? m_hasCachedEnchantmentLeft : m_hasCachedEnchantmentRight;
        UInt32 cachedEnchantID = isLeftHand ? m_cachedEnchantmentFormIDLeft : m_cachedEnchantmentFormIDRight;
        
        // Only search for specific item if we have either tempering or enchantment data
        if ((hasCachedHealth && cachedHealth > 1.0f) || hasCachedEnchant)
        {
   _MESSAGE("EquipManager::ForceReequipHand - Looking for weapon with health: %.2f, enchant: %08X", 
     cachedHealth, cachedEnchantID);
 
            // Get container changes to find the item with matching extra data
       ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
        player->extraData.GetByType(kExtraData_ContainerChanges));
      
     if (containerChanges && containerChanges->data)
            {
                InventoryEntryData* entryData = containerChanges->data->FindItemEntry(weaponForm);
            if (entryData && entryData->extendDataList)
    {
     // Search through all extra data lists for this item to find one with matching attributes
     for (ExtendDataList::Iterator it = entryData->extendDataList->Begin(); !it.End(); ++it)
      {
       BaseExtraList* extraList = it.Get();
            if (extraList)
      {
    // Check health value (tempering)
      bool healthMatches = true;
                   if (hasCachedHealth && cachedHealth > 1.0f)
        {
           ExtraHealth* xHealth = static_cast<ExtraHealth*>(extraList->GetByType(kExtraData_Health));
  if (xHealth)
          {
          float diff = xHealth->health - cachedHealth;
    healthMatches = (diff < 0.01f && diff > -0.01f);
     }
                    else
         {
              healthMatches = false;  // We need tempered, but this isn't
             }
  }
     
        // Check enchantment
 bool enchantMatches = true;
      if (hasCachedEnchant)
    {
     ExtraEnchantment* xEnchant = static_cast<ExtraEnchantment*>(extraList->GetByType(kExtraData_Enchantment));
if (xEnchant && xEnchant->enchant)
    {
      enchantMatches = (xEnchant->enchant->formID == cachedEnchantID);
                }
   else
     {
     enchantMatches = false;  // We need enchanted, but this isn't
    }
               }
 else
             {
  // We don't want an enchanted item - make sure this one isn't enchanted
     ExtraEnchantment* xEnchant = static_cast<ExtraEnchantment*>(extraList->GetByType(kExtraData_Enchantment));
       if (xEnchant && xEnchant->enchant)
     {
        enchantMatches = false;  // This has enchantment but we don't want one
 }
   }
         
                if (healthMatches && enchantMatches)
      {
          // Check that this item isn't already equipped
      bool isWorn = extraList->HasType(kExtraData_Worn);
           bool isWornLeft = extraList->HasType(kExtraData_WornLeft);
          
           if (!isWorn && !isWornLeft)
{
         extraDataToUse = extraList;
         _MESSAGE("EquipManager::ForceReequipHand - Found matching item (health: %.2f, enchant: %s)",
   cachedHealth, hasCachedEnchant ? "YES" : "NO");
            break;
             }
               else
                {
   _MESSAGE("EquipManager::ForceReequipHand - Found matching item but already equipped");
          }
           }
        }
             }
 
        if (!extraDataToUse)
   {
            _MESSAGE("EquipManager::ForceReequipHand - WARNING: Could not find item with matching attributes! Will equip base weapon.");
               }
                }
        }
      }
else
  {
  _MESSAGE("EquipManager::ForceReequipHand - Weapon is base (not tempered/enchanted), using standard equip");
        }
      
        // Equip with the found extra data (or nullptr for base weapon)
   CALL_MEMBER_FN(equipMan, EquipItem)(player, weaponForm, extraDataToUse, 1, slot, false, true, false, nullptr);
     
        _MESSAGE("EquipManager: FORCE RE-EQUIPPED to %s hand (FormID: %08X, ExtraData: %p) - direct call", 
            isLeftHand ? "Left" : "Right", cachedFormID, extraDataToUse);
  
     // ============================================
        // RESTORE FAVORITE STATE IF NEEDED
        // ============================================
    bool wasFavorited = isLeftHand ? m_wasFavoritedLeft : m_wasFavoritedRight;
    if (wasFavorited)
    {
        _MESSAGE("EquipManager: Restoring favorite state for %s hand weapon", isLeftHand ? "Left" : "Right");
   
        // Get the equipped item's extra data list
        ExtraContainerChanges* containerChanges2 = static_cast<ExtraContainerChanges*>(
            player->extraData.GetByType(kExtraData_ContainerChanges));
        
     if (containerChanges2 && containerChanges2->data)
        {
     InventoryEntryData* entryData2 = containerChanges2->data->FindItemEntry(weaponForm);
          if (entryData2 && entryData2->extendDataList)
       {
    // Find the equipped item's extra data list (has Worn or WornLeft)
   for (ExtendDataList::Iterator it = entryData2->extendDataList->Begin(); !it.End(); ++it)
          {
                  BaseExtraList* extraList2 = it.Get();
         if (extraList2)
  {
            bool isWorn2 = extraList2->HasType(kExtraData_Worn);
       bool isWornLeft2 = extraList2->HasType(kExtraData_WornLeft);
    
      // Check if this is the item we just equipped (correct hand)
      bool isCorrectHand = isLeftHand ? isWornLeft2 : isWorn2;
        
     if (isCorrectHand)
             {
         // Check if it doesn't already have a hotkey
             if (!extraList2->HasType(kExtraData_Hotkey))
     {
    // Create and add ExtraHotkey with -1 (favorited but no hotkey assigned)
     ExtraHotkey* xHotkey = ExtraHotkey::Create();
xHotkey->hotkey = -1;
  extraList2->Add(kExtraData_Hotkey, xHotkey);
        _MESSAGE("EquipManager: Added ExtraHotkey to %s hand weapon (restored favorite)", 
   isLeftHand ? "Left" : "Right");
         }
        else
           {
     _MESSAGE("EquipManager: %s hand weapon already has ExtraHotkey", 
        isLeftHand ? "Left" : "Right");
            }
     break;
     }
 }
        }
      }
        }
  }

    // Clear the pending re-equip and cached data for this hand
    ClearPendingReequip(isLeftHand);
  if (isLeftHand)
  {
        m_cachedWeaponFormIDLeft = 0;
        m_hasCachedHealthLeft = false;
   m_cachedWeaponHealthLeft = 1.0f;
        m_hasCachedEnchantmentLeft = false;
m_cachedEnchantmentFormIDLeft = 0;
        m_wasFavoritedLeft = false;
    }
    else
    {
        m_cachedWeaponFormIDRight = 0;
m_hasCachedHealthRight = false;
    m_cachedWeaponHealthRight = 1.0f;
      m_hasCachedEnchantmentRight = false;
  m_cachedEnchantmentFormIDRight = 0;
        m_wasFavoritedRight = false;
    }
}

    void EquipManager::ForceReequipLeftHand()
    {
        ForceReequipHand(true);
 }

    void EquipManager::ForceReequipRightHand()
    {
        ForceReequipHand(false);
    }

    bool EquipManager::HasPendingReequip(bool isLeftHand) const
    {
        return isLeftHand ? (m_pendingReequipLeft != nullptr) : (m_pendingReequipRight != nullptr);
    }

    void EquipManager::ClearPendingReequip(bool isLeftHand)
    {
    if (isLeftHand)
    {
      m_pendingReequipLeft = nullptr;
     }
        else
      {
         m_pendingReequipRight = nullptr;
        }
    }

    void EquipManager::ForceUnequipAndGrab(bool isLeftGameHand)
    {
    PlayerCharacter* player = *g_thePlayer;
        if (!player)
        {
            _MESSAGE("EquipManager::ForceUnequipAndGrab - No player!");
            return;
        }

    // ALWAYS use direct player check for what's equipped - our state might be stale
        TESForm* leftEquipped = player->GetEquippedObject(true);
   TESForm* rightEquipped = player->GetEquippedObject(false);
     
      TESForm* item = isLeftGameHand ? leftEquipped : rightEquipped;
        if (!item)
    {
  _MESSAGE("EquipManager::ForceUnequipAndGrab - %s GAME hand has no weapon (direct check)", 
        isLeftGameHand ? "Left" : "Right");
            return;
        }
   
        // Check if this is a weapon we should handle
   if (!IsWeapon(item))
  {
 _MESSAGE("EquipManager::ForceUnequipAndGrab - %s GAME hand item is not a weapon (FormID: %08X)", 
        isLeftGameHand ? "Left" : "Right", item->formID);
    return;
        }

        // Check if both hands have the SAME weapon (same FormID) - use DIRECT check
   bool bothHandsSameWeapon = leftEquipped && rightEquipped && 
  (leftEquipped->formID == rightEquipped->formID);
        
        if (bothHandsSameWeapon)
        {
   _MESSAGE("EquipManager::ForceUnequipAndGrab - SAME WEAPON in both hands (FormID: %08X)", item->formID);
        _MESSAGE("EquipManager::ForceUnequipAndGrab - Using special handling for duplicate weapons");
  }
        
        // Track if we were dual-wielding same weapon (for cleanup after re-equip)
    if (isLeftGameHand)
  {
     m_wasDualWieldingSameWeaponLeft = bothHandsSameWeapon;
  }
        else
   {
   m_wasDualWieldingSameWeaponRight = bothHandsSameWeapon;
  }
        
        // Cache the FormID for later re-equip (use correct cache for each GAME hand)
        if (isLeftGameHand)
 {
        m_cachedWeaponFormIDLeft = item->formID;
      _MESSAGE("EquipManager: Cached LEFT GAME hand weapon FormID: %08X for re-equip", m_cachedWeaponFormIDLeft);
   }
 else
        {
   m_cachedWeaponFormIDRight = item->formID;
            _MESSAGE("EquipManager: Cached RIGHT GAME hand weapon FormID: %08X for re-equip", m_cachedWeaponFormIDRight);
  }
        
// Store for potential re-equip later
        if (isLeftGameHand)
        {
    m_pendingReequipLeft = item;
        }
        else
        {
  m_pendingReequipRight = item;
  }

        _MESSAGE("EquipManager: FORCE UNEQUIP AND GRAB - %s from %s GAME hand (FormID: %08X)", 
            GetWeaponTypeName(GetWeaponType(item)), 
  isLeftGameHand ? "Left" : "Right", 
  item->formID);

   // Step 1: Unequip the item first (uses GAME HAND)
        ::EquipManager* equipManager = ::EquipManager::GetSingleton();
        if (!equipManager)
        {
         _MESSAGE("EquipManager::ForceUnequipAndGrab - Failed to get game EquipManager!");
          return;
}

  ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
    player->extraData.GetByType(kExtraData_ContainerChanges));
     
   if (!containerChanges || !containerChanges->data)
        {
 _MESSAGE("EquipManager::ForceUnequipAndGrab - No container changes data!");
          return;
}

        InventoryEntryData* entryData = containerChanges->data->FindItemEntry(item);
        if (!entryData)
        {
      _MESSAGE("EquipManager::ForceUnequipAndGrab - Item not found in inventory!");
    return;
        }

 BaseExtraList* rightEquipList = NULL;
      BaseExtraList* leftEquipList = NULL;
        entryData->GetExtraWornBaseLists(&rightEquipList, &leftEquipList);

        // Debug: Log what we got from GetExtraWornBaseLists
        _MESSAGE("EquipManager::ForceUnequipAndGrab - GetExtraWornBaseLists results:");
        _MESSAGE("  leftEquipList: %p, rightEquipList: %p", leftEquipList, rightEquipList);
        _MESSAGE("  Requested hand: %s GAME hand", isLeftGameHand ? "Left" : "Right");
        
        if (bothHandsSameWeapon)
   {
     _MESSAGE("  NOTE: Both hands have SAME weapon - entryData count: %d", entryData->countDelta);
    }

    // Note: These are GAME hand equip lists
        BaseExtraList* equipList = isLeftGameHand ? leftEquipList : rightEquipList;
 BGSEquipSlot* equipSlot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();

        if (!equipList)
        {
       _MESSAGE("EquipManager::ForceUnequipAndGrab - No equip list found for %s hand!", 
isLeftGameHand ? "Left" : "Right");
    _MESSAGE("EquipManager::ForceUnequipAndGrab - leftEquipList: %p, rightEquipList: %p", 
      leftEquipList, rightEquipList);
  
// If we couldn't get the equip list for the requested hand, we cannot safely unequip
 // Using the other hand's equip list would unequip the WRONG weapon!
     // This can happen with same weapon in both hands - just abort
    _MESSAGE("EquipManager::ForceUnequipAndGrab - Cannot get correct equip list, aborting to prevent wrong weapon unequip");
  return;
        }

   // ============================================
        // CACHE TEMPERING DATA (ExtraHealth) BEFORE UNEQUIPPING
        // This preserves upgraded weapon stats for re-equip
        // ============================================
        if (equipList)
        {
            // Cache tempering (ExtraHealth)
  ExtraHealth* xHealth = static_cast<ExtraHealth*>(equipList->GetByType(kExtraData_Health));
            if (xHealth)
  {
   if (isLeftGameHand)
     {
    m_cachedWeaponHealthLeft = xHealth->health;
    m_hasCachedHealthLeft = true;
           _MESSAGE("EquipManager: Cached LEFT hand weapon health: %.2f (tempered)", xHealth->health);
        }
      else
    {
          m_cachedWeaponHealthRight = xHealth->health;
      m_hasCachedHealthRight = true;
          _MESSAGE("EquipManager: Cached RIGHT hand weapon health: %.2f (tempered)", xHealth->health);
   }
            }
            else
            {
      // No extra health = base weapon (not tempered)
      if (isLeftGameHand)
     {
   m_cachedWeaponHealthLeft = 1.0f;
       m_hasCachedHealthLeft = false;
              _MESSAGE("EquipManager: LEFT hand weapon has no tempering (base weapon)");
         }
       else
   {
       m_cachedWeaponHealthRight = 1.0f;
 m_hasCachedHealthRight = false;
                    _MESSAGE("EquipManager: RIGHT hand weapon has no tempering (base weapon)");
          }
            }
  
       // Cache player-applied enchantment (ExtraEnchantment)
   // This is separate from base weapon enchantments - these are enchantments added by the player
 ExtraEnchantment* xEnchant = static_cast<ExtraEnchantment*>(equipList->GetByType(kExtraData_Enchantment));
  if (xEnchant && xEnchant->enchant)
        {
     if (isLeftGameHand)
      {
    m_cachedEnchantmentFormIDLeft = xEnchant->enchant->formID;
               m_hasCachedEnchantmentLeft = true;
        _MESSAGE("EquipManager: Cached LEFT hand weapon enchantment: %08X (player-enchanted)", xEnchant->enchant->formID);
        }
     else
        {
      m_cachedEnchantmentFormIDRight = xEnchant->enchant->formID;
  m_hasCachedEnchantmentRight = true;
      _MESSAGE("EquipManager: Cached RIGHT hand weapon enchantment: %08X (player-enchanted)", xEnchant->enchant->formID);
      }
      }
      else
            {
  if (isLeftGameHand)
    {
        m_cachedEnchantmentFormIDLeft = 0;
        m_hasCachedEnchantmentLeft = false;
 }
       else
     {
       m_cachedEnchantmentFormIDRight = 0;
   m_hasCachedEnchantmentRight = false;
}
     }
   
   // Cache favorite state (ExtraHotkey indicates item is favorited)
      ExtraHotkey* xHotkey = static_cast<ExtraHotkey*>(equipList->GetByType(kExtraData_Hotkey));
            if (xHotkey)
          {
 if (isLeftGameHand)
       {
  m_wasFavoritedLeft = true;
             _MESSAGE("EquipManager: Cached LEFT hand weapon was FAVORITED");
     }
     else
    {
        m_wasFavoritedRight = true;
          _MESSAGE("EquipManager: Cached RIGHT hand weapon was FAVORITED");
                }
          }
          else
         {
    if (isLeftGameHand)
  {
 m_wasFavoritedLeft = false;
   }
      else
           {
           m_wasFavoritedRight = false;
    }
            }
        }

BSExtraData* xCannotWear = equipList->GetByType(kExtraData_CannotWear);
   if (xCannotWear)
        {
 equipList->Remove(kExtraData_CannotWear, xCannotWear);
    }

   // Unequip the item (silent - no sound, no message)
   s_suppressSheathSound = true;
   CALL_MEMBER_FN(equipManager, UnequipItem)(player, item, equipList, 1, equipSlot, false, true, true, false, NULL);
   s_suppressSheathSound = false;

   _MESSAGE("EquipManager: Item unequipped (silent), now creating world object for HIGGS grab...");

   // Clear weapon lock for the VR controller corresponding to this game hand
   // This prevents the controller from getting stuck in lock mode when weapon is unequipped
   bool isLeftVRController = GameHandToVRController(isLeftGameHand);
   VRInputHandler::ClearWeaponLock(isLeftVRController);
   _MESSAGE("EquipManager: Cleared weapon lock for %s VR controller", isLeftVRController ? "LEFT" : "RIGHT");

   // Step 2: Determine spawn position based on mount state

        // Step 2: Determine spawn position based on mount state
      // - If player is MOUNTED: spawn at hand position (to avoid horse collision)
   // - If player is NOT mounted: spawn BEHIND player (so they can't see the weapon appear)
      NiNode* rootNode = player->GetNiRootNode(0);
        if (!rootNode)
        {
            rootNode = player->GetNiRootNode(1);
        }
        
        NiPoint3 spawnPos = player->pos;
        
        // Check if player is mounted
      NiPointer<Actor> mountActor;
      bool isMounted = CALL_MEMBER_FN(player, GetMount)(mountActor) && mountActor;
        
      if (isMounted)
      {
          // MOUNTED: Spawn at player position with configurable mounted offsets
          _MESSAGE("EquipManager: Player is MOUNTED - spawning weapon with mounted offsets from player");

          // Simple offset from player world position (mounted settings)
          spawnPos.x = player->pos.x + spawnOffsetMountedX;
          spawnPos.y = player->pos.y + spawnOffsetMountedY;
          spawnPos.z = player->pos.z + spawnOffsetMountedZ;

          _MESSAGE("EquipManager: Spawning weapon at player + mounted offset (%.2f, %.2f, %.2f)",
              spawnPos.x, spawnPos.y, spawnPos.z);
      }
    else
  {
          // NOT MOUNTED: Spawn at player position with configurable offsets
          _MESSAGE("EquipManager: Player is NOT mounted - spawning weapon with offsets from player");

          // Simple offset from player world position
          spawnPos.x = player->pos.x + spawnOffsetX;
          spawnPos.y = player->pos.y + spawnOffsetY;
          spawnPos.z = player->pos.z + spawnOffsetZ;

          _MESSAGE("EquipManager: Spawning weapon at player + offset (%.2f, %.2f, %.2f)",
              spawnPos.x, spawnPos.y, spawnPos.z);
        }

      // Step 3: Create a world object using PlaceAtMe
      TESObjectREFR* droppedWeapon = PlaceAtMe_Native(nullptr, 0, player, item, 1, false, false);

      if (droppedWeapon)
      {
          _MESSAGE("EquipManager: Created world weapon reference (RefID: %08X)", droppedWeapon->formID);

          // Step 3.1: Move weapon to calculated spawn position
          droppedWeapon->pos = spawnPos;
          _MESSAGE("EquipManager: Moved weapon to spawn position (%.2f, %.2f, %.2f)", spawnPos.x, spawnPos.y, spawnPos.z);

          // Step 3.25: Set ownership to player to prevent "stolen" flag when picking up
          SetOwnerToPlayer(droppedWeapon);

  // Step 3.5: Remove the item from inventory to prevent duplication
  // PlaceAtMe creates a COPY, so we need to remove the original from inventory
        // EXCEPTION: If both hands have the same weapon, don't remove - we need it for the other hand!
        if (!bothHandsSameWeapon)
        {
    RemoveItemFromInventory(player, item, 1, true);
  _MESSAGE("EquipManager: Removed 1x item from inventory to prevent duplication");
        }
     else
   {
            _MESSAGE("EquipManager: SKIPPING inventory removal - same weapon in both hands, need it for other hand");
        }

 // Store the reference (by GAME hand)
 // Also store RefID + base FormID so the world copy can be safely
 // cleaned up later even if the pointer goes stale (save/load)
 if (isLeftGameHand)
   {
      m_droppedWeaponLeft = droppedWeapon;
      m_droppedWeaponRefIDLeft = droppedWeapon->formID;
      m_droppedWeaponBaseIDLeft = item->formID;
 }
     else
          {
   m_droppedWeaponRight = droppedWeapon;
   m_droppedWeaponRefIDRight = droppedWeapon->formID;
   m_droppedWeaponBaseIDRight = item->formID;
       }

            // Step 4: Use HIGGS to grab the object
            // IMPORTANT: HIGGS uses VR CONTROLLER, not game hand!
   // We need to convert game hand to VR controller
    bool isLeftVRController = GameHandToVRController(isLeftGameHand);
        
     if (higgsInterface)
     {
 // Check if VR controller can grab
            if (higgsInterface->CanGrabObject(isLeftVRController))
    {
        _MESSAGE("EquipManager: HIGGS grabbing weapon with %s VR controller (game %s hand)!", 
      isLeftVRController ? "Left" : "Right",
   isLeftGameHand ? "Left" : "Right");
     higgsInterface->GrabObject(droppedWeapon, isLeftVRController);
     
 // REMOVED: Track the grabbed weapon for scaling
     // TrackGrabbedWeapon(droppedWeapon, isLeftVRController);
   }
  else
       {
  _MESSAGE("EquipManager: HIGGS cannot grab with %s VR controller right now", 
       isLeftVRController ? "Left" : "Right");
    }
    }
  else
         {
         _MESSAGE("EquipManager: HIGGS interface not available!");
       }
     }
        else
  {
          _MESSAGE("EquipManager: Failed to create world weapon reference!");
 }
    }

    bool EquipManager::IsHiggsHoldingDroppedWeapon(bool isLeftHand) const
    {
        if (!higgsInterface)
            return false;
            
        TESObjectREFR* droppedRef = isLeftHand ? m_droppedWeaponLeft : m_droppedWeaponRight;
        if (!droppedRef)
      return false;
      
        TESObjectREFR* heldObject = higgsInterface->GetGrabbedObject(isLeftHand);
        return (heldObject == droppedRef);
    }

    TESObjectREFR* EquipManager::GetDroppedWeaponRef(bool isLeftHand) const
    {
      return isLeftHand ? m_droppedWeaponLeft : m_droppedWeaponRight;
    }

    void EquipManager::ClearDroppedWeaponRef(bool isLeftHand)
    {
        if (isLeftHand)
    {
          m_droppedWeaponLeft = nullptr;
          m_droppedWeaponRefIDLeft = 0;
          m_droppedWeaponBaseIDLeft = 0;
     }
        else
   {
 m_droppedWeaponRight = nullptr;
 m_droppedWeaponRefIDRight = 0;
 m_droppedWeaponBaseIDRight = 0;
    }
    }

    void EquipManager::CleanupOrphanedDuplicates()
    {
        for (int h = 0; h < 2; h++)
        {
            bool isLeftHand = (h == 0);
            UInt32 refID = isLeftHand ? m_droppedWeaponRefIDLeft : m_droppedWeaponRefIDRight;
            UInt32 baseID = isLeftHand ? m_droppedWeaponBaseIDLeft : m_droppedWeaponBaseIDRight;
            bool wasDualSame = isLeftHand ? m_wasDualWieldingSameWeaponLeft : m_wasDualWieldingSameWeaponRight;

            // Only the dual-wield same-weapon case leaves a +1 duplicate behind
            // (normal case already removed the inventory original at spawn time)
            if (refID == 0 || !wasDualSame)
                continue;

            // Look the reference up fresh by RefID - never trust the stale pointer.
            // Returns null if the object no longer exists (e.g. not in the loaded save).
            TESForm* form = LookupFormByID(refID);
            TESObjectREFR* ref = form ? DYNAMIC_CAST(form, TESForm, TESObjectREFR) : nullptr;

            // Verify it's still OUR spawned weapon copy before deleting
            if (ref && ref->baseForm && ref->baseForm->formID == baseID)
            {
                _MESSAGE("EquipManager: Deleting orphaned dual-wield duplicate (RefID: %08X, Base: %08X, %s hand)",
                    refID, baseID, isLeftHand ? "LEFT" : "RIGHT");
                DeleteWorldObject(ref);
            }

            if (isLeftHand)
            {
                m_droppedWeaponRefIDLeft = 0;
                m_droppedWeaponBaseIDLeft = 0;
                m_wasDualWieldingSameWeaponLeft = false;
            }
            else
            {
                m_droppedWeaponRefIDRight = 0;
                m_droppedWeaponBaseIDRight = 0;
                m_wasDualWieldingSameWeaponRight = false;
            }
        }
    }

    void EquipManager::ClearCachedWeaponFormID(bool isLeftHand)
    {
        if (isLeftHand)
        {
    m_cachedWeaponFormIDLeft = 0;
   _MESSAGE("EquipManager: Cleared cached weapon FormID for LEFT hand");
        }
  else
        {
        m_cachedWeaponFormIDRight = 0;
   _MESSAGE("EquipManager: Cleared cached weapon FormID for RIGHT hand");
     }
    }

  bool EquipManager::WasDualWieldingSameWeapon(bool isLeftHand) const
    {
        return isLeftHand ? m_wasDualWieldingSameWeaponLeft : m_wasDualWieldingSameWeaponRight;
    }

    void EquipManager::CheckPendingAutoUnequip()
    {
        if (!m_pendingAutoUnequipForm)
    return;
            
    bool isLeftHand = m_pendingAutoUnequipLeft;
      TESForm* weaponForm = m_pendingAutoUnequipForm;
        
   // Clear the pending flags immediately
        m_pendingAutoUnequipLeft = false;
        m_pendingAutoUnequipRight = false;
        m_pendingAutoUnequipForm = nullptr;

        _MESSAGE("EquipManager::CheckPendingAutoUnequip - Processing auto-unequip for %s hand", 
            isLeftHand ? "LEFT" : "RIGHT");
 
        // Double-check conditions are still valid
        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return;
      
        // Check if the weapon is still equipped
        TESForm* currentlyEquipped = player->GetEquippedObject(isLeftHand);
        if (!currentlyEquipped || currentlyEquipped->formID != weaponForm->formID)
        {
            _MESSAGE("EquipManager::CheckPendingAutoUnequip - Weapon no longer equipped, skipping");
            return;
        }
        
  // Check if trigger is still not held
  bool offHandIsLeft = GetCollisionAvoidanceHandIsLeft();
 bool offHandVRControllerIsLeft = GameHandToVRController(offHandIsLeft);
 bool triggerHeld = offHandVRControllerIsLeft ? 
         VRInputHandler::IsLeftTriggerPressed() : VRInputHandler::IsRightTriggerPressed();
      
   if (triggerHeld)
        {
     _MESSAGE("EquipManager::CheckPendingAutoUnequip - Trigger now held, keeping equipped");
    return;
        }
        
        // Check if weapon is locked (trigger spam = 4x to lock)
        bool weaponLocked = VRInputHandler::IsWeaponLocked(offHandVRControllerIsLeft);
        if (weaponLocked)
   {
    _MESSAGE("EquipManager::CheckPendingAutoUnequip - Weapon is LOCKED, keeping equipped");
     return;
        }
   
 // Perform the unequip and HIGGS grab
        _MESSAGE("EquipManager::CheckPendingAutoUnequip - Unequipping and HIGGS grabbing weapon");
  ForceUnequipAndGrab(isLeftHand);
    }

    // ============================================
    // ContainerChangeEventHandler Implementation
    // ============================================
    
    EventResult ContainerChangeEventHandler::ReceiveEvent(TESContainerChangedEvent* evn, EventDispatcher<TESContainerChangedEvent>* dispatcher)
  {
        if (!evn)
  return kEvent_Continue;
            
     // Get the player's FormID
   PlayerCharacter* player = *g_thePlayer;
     if (!player)
       return kEvent_Continue;
        
        UInt32 playerFormID = player->formID;
        
        // Check if item is being added TO the player (player is the destination)
        if (evn->toFormId != playerFormID)
           return kEvent_Continue;
 
        // Look up the item form
      TESForm* itemForm = LookupFormByID(evn->itemFormId);
        if (!itemForm)
     return kEvent_Continue;
            
   // Check if this is a valid one-handed weapon we track
        if (!EquipManager::IsWeapon(itemForm))
           return kEvent_Continue;
        
      // Get weapon name for logging
        const char* weaponName = nullptr;
     TESFullName* fullName = DYNAMIC_CAST(itemForm, TESForm, TESFullName);
        if (fullName)
  {
 weaponName = fullName->GetName();
        }
        
    // Get weapon type
        WeaponType weaponType = EquipManager::GetWeaponType(itemForm);
        const char* typeName = EquipManager::GetWeaponTypeName(weaponType);
        
        // Log the weapon being added
 _MESSAGE("=== WEAPON ADDED TO PLAYER INVENTORY ===");
        _MESSAGE("  Name: %s", weaponName ? weaponName : "Unknown");
        _MESSAGE("  Type: %s", typeName);
  _MESSAGE("  FormID: %08X", evn->itemFormId);
        _MESSAGE("  Count: %d", evn->count);
     _MESSAGE("  From: %08X", evn->fromFormId);
    _MESSAGE("=========================================");
        
     // Play the weapon pickup sound from Fake Edge VR.esp (ESL-flagged)
     // BUT skip if this is from our internal re-equip logic (SafeActivate)
    if (EquipManager::s_suppressPickupSound)
     {
   _MESSAGE("EquipManager: Skipping pickup sound (internal re-equip)");
          return kEvent_Continue;
     }
        
     // Skip pickup sound for excluded items (see TwinGripVR.ini [WeaponExclusions])
   if (IsExcludedWeaponFormID(evn->itemFormId))
        {
         _MESSAGE("EquipManager: Skipping pickup sound (excluded item)");
   return kEvent_Continue;
 }
        
  // Base FormID is 0x800, plugin name is "Fake Edge VR.esp"
   static UInt32 cachedSoundFormId = 0;
   if (cachedSoundFormId == 0)
   {
          cachedSoundFormId = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x800);
 if (cachedSoundFormId != 0)
  {
  _MESSAGE("EquipManager: Cached weapon pickup sound FormID: %08X", cachedSoundFormId);
         }
     else
   {
    _MESSAGE("EquipManager: WARNING - Could not find weapon pickup sound in Fake Edge VR.esp");
   }
}

        PlaySoundAtPlayer(cachedSoundFormId);
        
 return kEvent_Continue;
    }

    // ============================================
    // Convenience Functions
    // ============================================

    void RegisterEquipEventHandler()
    {
        _MESSAGE("EquipManager: Registering event handlers...");
        
        auto* eventDispatcher = GetEventDispatcherList();
        if (eventDispatcher)
        {
            // Register equip event handler
          eventDispatcher->unk4D0.AddEventSink(EquipEventHandler::GetSingleton());
    _MESSAGE("EquipManager: Equip event handler registered successfully");
            
  // Register container change event handler
   eventDispatcher->unk370.AddEventSink(ContainerChangeEventHandler::GetSingleton());
            _MESSAGE("EquipManager: Container change event handler registered successfully");
        }
      else
        {
 _MESSAGE("EquipManager: ERROR - Failed to get event dispatcher list!");
        }
    }

    void UnregisterEquipEventHandler()
    {
    auto* eventDispatcher = GetEventDispatcherList();
     if (eventDispatcher)
        {
     eventDispatcher->unk4D0.RemoveEventSink(EquipEventHandler::GetSingleton());
            eventDispatcher->unk370.RemoveEventSink(ContainerChangeEventHandler::GetSingleton());
      _MESSAGE("EquipManager: Event handlers unregistered");
 }
    }
}
