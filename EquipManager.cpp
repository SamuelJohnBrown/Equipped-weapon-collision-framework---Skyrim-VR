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
#include "skse64/NiNodes.h"
#include <thread>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <unordered_set>

namespace
{
    void CopyHotkeyExtraDataToWorldRef(BaseExtraList* source, TESObjectREFR* destRef)
    {
        if (!source || !destRef || destRef->extraData.GetByType(kExtraData_Hotkey))
            return;

        ExtraHotkey* srcHotkey = static_cast<ExtraHotkey*>(source->GetByType(kExtraData_Hotkey));
        if (!srcHotkey)
            return;

        ExtraHotkey* dstHotkey = ExtraHotkey::Create();
        dstHotkey->hotkey = srcHotkey->hotkey;
        destRef->extraData.Add(kExtraData_Hotkey, dstHotkey);
    }

    bool WorldRefHasHotkey(TESObjectREFR* ref)
    {
        return ref && ref->extraData.GetByType(kExtraData_Hotkey) != nullptr;
    }

    bool IsReadableWeaponWorldRef(TESObjectREFR* ref)
    {
        if (!ref)
            return false;

        __try
        {
            if (ref->formType != kFormType_Reference)
                return false;

            TESForm* baseForm = ref->baseForm;
            return baseForm && baseForm->formType == kFormType_Weapon;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    TESObjectREFR* ResolveLiveWeaponWorldRef(TESObjectREFR* ref)
    {
        if (!ref)
            return nullptr;

        const UInt32 refID = ref->formID;
        if (refID != 0)
        {
            TESForm* form = LookupFormByID(refID);
            TESObjectREFR* liveRef = form ? DYNAMIC_CAST(form, TESForm, TESObjectREFR) : nullptr;
            if (liveRef && IsReadableWeaponWorldRef(liveRef))
                return liveRef;
        }

        if (IsReadableWeaponWorldRef(ref))
            return ref;

        return nullptr;
    }

    SInt32 GetPlayerWeaponCount(PlayerCharacter* player, TESForm* weaponForm)
    {
        if (!player || !weaponForm)
            return 0;

        ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
            player->extraData.GetByType(kExtraData_ContainerChanges));
        if (!containerChanges || !containerChanges->data)
            return 0;

        InventoryEntryData* entryData = containerChanges->data->FindItemEntry(weaponForm);
        return entryData ? entryData->countDelta : 0;
    }
}

namespace FalseEdgeVR
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
            PlayerCharacter* player = *g_thePlayer;
            if (!player)
            {
                _MESSAGE("[DelayedEquipWeapon] Player not available");
                return;
            }

            TESForm* weaponForm = LookupFormByID(m_weaponFormId);
            if (!weaponForm)
            {
                return;
            }

            EquipManager::s_suppressDrawSound = true;
            EquipManager::GetSingleton()->EquipWeaponToGameHand(player, weaponForm, m_equipToLeftHand);
            EquipManager::s_suppressDrawSound = false;
        }

        virtual void Dispose() override
        {
            delete this;
        }
    };

    static bool s_scheduledGrabbedPickupLeft = false;
    static bool s_scheduledGrabbedPickupRight = false;

    // Pick up a holstered/grabbed world weapon after an external equip (SpellWheel, etc.)
    // finishes. Must not run SafeActivate inside EquipItem — that re-enters the engine and
    // can CTD on stale holstered refs (invalid BSExtraDataList).
    class DelayedPickUpGrabbedWeaponBeforeEquipTask : public TaskDelegate
    {
    public:
        bool m_isLeftGameHand;

        explicit DelayedPickUpGrabbedWeaponBeforeEquipTask(bool isLeftGameHand)
            : m_isLeftGameHand(isLeftGameHand) {}

        virtual void Run() override
        {
            bool& scheduled = m_isLeftGameHand ? s_scheduledGrabbedPickupLeft : s_scheduledGrabbedPickupRight;
            scheduled = false;
            EquipManager::GetSingleton()->PickUpGrabbedWeaponBeforeEquip(m_isLeftGameHand, true);
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
    // Scheduled force re-equip: wait for inventory, settle, then equip once.
    // (A post-equip 3D node verify was removed — it false-negative'd in VR even
    // when the weapon was visible, causing spurious errors and retry damage.)
    class ScheduledForceReequipTask : public TaskDelegate
    {
    public:
        enum Phase { kWaitInventory, kSettle, kEquip };

        static const int kInventoryRetries = 12;
        static const int kSettleFrames     = 3;

        bool  m_isLeftHand;
        Phase m_phase;
        int   m_countdown;

        ScheduledForceReequipTask(bool isLeftHand, Phase phase, int countdown)
            : m_isLeftHand(isLeftHand), m_phase(phase), m_countdown(countdown) {}

        void Queue(Phase phase, int countdown)
        {
            if (g_task)
                g_task->AddTask(new ScheduledForceReequipTask(m_isLeftHand, phase, countdown));
        }

        static bool ItemInInventory(PlayerCharacter* player, TESForm* weaponForm)
        {
            ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
                player->extraData.GetByType(kExtraData_ContainerChanges));
            if (containerChanges && containerChanges->data)
            {
                InventoryEntryData* entryData = containerChanges->data->FindItemEntry(weaponForm);
                if (entryData && entryData->countDelta > 0)
                    return true;
            }
            return false;
        }

        virtual void Run() override
        {
            EquipManager* manager = EquipManager::GetSingleton();
            PlayerCharacter* player = *g_thePlayer;

            UInt32 cachedFormID = manager->GetCachedWeaponFormID(m_isLeftHand);
            TESForm* weaponForm = (cachedFormID != 0) ? LookupFormByID(cachedFormID) : nullptr;

            if (!player || !weaponForm)
            {
                _MESSAGE("[ScheduledForceReequip] %s hand: player/weapon unavailable (cache %08X) - aborting",
                    m_isLeftHand ? "LEFT" : "RIGHT", cachedFormID);
                manager->ClearReequipCache(m_isLeftHand);
                return;
            }

            switch (m_phase)
            {
            case kWaitInventory:
            {
                if (ItemInInventory(player, weaponForm))
                {
                    Queue(kSettle, kSettleFrames);
                    return;
                }
                if (m_countdown > 0)
                {
                    Queue(kWaitInventory, m_countdown - 1);
                    return;
                }
                _MESSAGE("[ScheduledForceReequip] WARNING: Item %08X never appeared in inventory - equipping anyway", cachedFormID);
                Queue(kSettle, kSettleFrames);
                return;
            }

            case kSettle:
            {
                if (m_countdown > 0)
                {
                    Queue(kSettle, m_countdown - 1);
                    return;
                }
                Queue(kEquip, 0);
                return;
            }

            case kEquip:
            {
                EquipManager::s_suppressDrawSound = true;
                manager->ForceReequipHand(m_isLeftHand);
                EquipManager::s_suppressDrawSound = false;
                manager->ClearReequipCache(m_isLeftHand);
                return;
            }
            }
        }

        virtual void Dispose() override
        {
            delete this;
        }
    };

    class DelayedLog2HWeaponEquipTask : public TaskDelegate
    {
    public:
        UInt32 m_weaponFormID;

        DelayedLog2HWeaponEquipTask(UInt32 weaponFormID) : m_weaponFormID(weaponFormID) {}

        virtual void Run() override
        {
            if (m_weaponFormID == 0)
                return;

            TESForm* weapon = LookupFormByID(m_weaponFormID);
            if (weapon)
                EquipManager::GetSingleton()->LogPlayer2HWeaponEquip(weapon);
        }

        virtual void Dispose() override
        {
            delete this;
        }
    };

    // Runs on the game thread after the 0.1s delay to check whether the
    // left game hand weapon (captured at combo time) is still equipped.
    class DelayedComboLeftUnequipCheckTask : public TaskDelegate
    {
    public:
        UInt32 m_leftWeaponFormID;

        DelayedComboLeftUnequipCheckTask(UInt32 leftWeaponFormID) : m_leftWeaponFormID(leftWeaponFormID) {}

        virtual void Run() override
        {
            EquipManager::GetSingleton()->CheckLeftHandUnequipAfterCombo(m_leftWeaponFormID);
        }

        virtual void Dispose() override
        {
            delete this;
        }
    };

    // Sleeps the requested delay off the game thread, then queues the
    // game-thread check task.
    static void ComboLeftUnequipCheckThread(UInt32 leftWeaponFormID, int delayMs)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        if (g_task)
            g_task->AddTask(new DelayedComboLeftUnequipCheckTask(leftWeaponFormID));
    }

    void EquipManager::ScheduleForceReequip(bool isLeftHand)
    {
        if (g_task)
        {
            g_task->AddTask(new ScheduledForceReequipTask(
                isLeftHand, ScheduledForceReequipTask::kWaitInventory,
                ScheduledForceReequipTask::kInventoryRetries));
        }
        else
        {
            s_suppressDrawSound = true;
            ForceReequipHand(isLeftHand);
            s_suppressDrawSound = false;
            ClearReequipCache(isLeftHand);
        }
    }

    void EquipManager::SchedulePickUpGrabbedWeaponBeforeEquip(bool isLeftGameHand)
    {
        bool& scheduled = isLeftGameHand ? s_scheduledGrabbedPickupLeft : s_scheduledGrabbedPickupRight;
        if (scheduled)
            return;

        if (g_task)
        {
            scheduled = true;
            g_task->AddTask(new DelayedPickUpGrabbedWeaponBeforeEquipTask(isLeftGameHand));
            return;
        }

        PickUpGrabbedWeaponBeforeEquip(isLeftGameHand, true);
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

        if (actor == *g_thePlayer && isEquipping && EquipManager::IsTwoHandedWeapon(item))
        {
            EquipManager::GetSingleton()->ScheduleDelayedLogPlayer2HWeaponEquip(evn->baseObject);
        }

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
        
PlayerCharacter* player = *g_thePlayer;
        EquipManager* equipMgr = EquipManager::GetSingleton();
     
      // Check left hand for grabbed weapon
  TESObjectREFR* droppedLeft = equipMgr->GetDroppedWeaponRef(true);
 if (droppedLeft)
     {
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
     }
      
  switch (type)
     {
    case WeaponType::Dagger:
   if (cachedDaggerSound != 0)
   PlaySoundAtActor(cachedDaggerSound, actor);
   break;
   case WeaponType::Sword:
        if (cachedSwordSound != 0)
       PlaySoundAtActor(cachedSwordSound, actor);
   break;
  case WeaponType::Mace:
   if (cachedMaceSound != 0)
   PlaySoundAtActor(cachedMaceSound, actor);
   break;
case WeaponType::Axe:
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
          }
       
           switch (type)
  {
            case WeaponType::Dagger:
   if (cachedDaggerSheathSound != 0)
    PlaySoundAtActor(cachedDaggerSheathSound, actor);
      break;
          case WeaponType::Sword:
  if (cachedSwordSheathSound != 0)
         PlaySoundAtActor(cachedSwordSheathSound, actor);
       break;
        case WeaponType::Mace:
 if (cachedMaceSheathSound != 0)
          PlaySoundAtActor(cachedMaceSheathSound, actor);
                  break;
       case WeaponType::Axe:
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


     // Determining which hand based on the equipped flag
     
      if (isEquipping)
{
            bool isLeftHand = false;
            bool handKnown = EquipManager::TryGetPlayerEquipHand(actor, item, isLeftHand);

            EquipManager* equipMgr = EquipManager::GetSingleton();

            // If this hand already has a different weapon HIGGS-grabbed, pick it up first
            // so the new equip does not spawn a duplicate world copy on top of the grab.
            if (EquipManager::IsWeapon(item))
            {
                if (handKnown)
                {
                    if (equipMgr->HasConflictingGrabbedWeaponInHand(isLeftHand, item))
                        equipMgr->SchedulePickUpGrabbedWeaponBeforeEquip(isLeftHand);
                }
                else
                {
                    // Fallback: check both hands when worn-slot detection fails.
                    for (int h = 0; h < 2; h++)
                    {
                        bool handIsLeft = (h == 0);
                        if (equipMgr->HasConflictingGrabbedWeaponInHand(handIsLeft, item))
                            equipMgr->SchedulePickUpGrabbedWeaponBeforeEquip(handIsLeft);
                    }
                }
            }
            
            equipMgr->OnEquip(item, actor, isLeftHand);
            if (isLeftHand && EquipManager::IsWeapon(item))
                equipMgr->TryLog2HLeftHandWithRightGameHandTrigger(item);
   }
        else
        {
            bool isLeftHand = false;
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

        
        m_equipState.leftHand.Clear();
      m_equipState.rightHand.Clear();
        
        m_initialized = true;
    }

    void EquipManager::UpdateEquipmentState()
{
        PlayerCharacter* player = *g_thePlayer;
        if (!player)
   {
      return;
      }

  TESForm* leftItem = player->GetEquippedObject(true);
        TESForm* rightItem = player->GetEquippedObject(false);


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

        // Belt-and-suspenders: pick up a conflicting grabbed weapon before auto-unequip logic runs.
        if (actor == *g_thePlayer && IsWeapon(item) &&
            HasConflictingGrabbedWeaponInHand(isLeftHand, item))
        {
            SchedulePickUpGrabbedWeaponBeforeEquip(isLeftHand);
        }

  WeaponType type = GetWeaponType(item);
  EquippedWeapon& hand = isLeftHand ? m_equipState.leftHand : m_equipState.rightHand;
   hand.form = item;
        hand.type = type;
 hand.isEquipped = true;
        
    // Record equip time for sheath sound cooldown
        // Only track weapons (not shields)
 if (type != WeaponType::Shield && type != WeaponType::None)
        {
    std::lock_guard<std::mutex> lock(s_sheathMutex);
      s_lastEquipTimes[item->formID] = std::chrono::steady_clock::now();
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
         
          if (!triggerHeld)
        {
 // Delay the unequip slightly to let the equip complete.
      // Tracked per hand so the other hand's pending unequip is never clobbered.
      if (isLeftHand)
          m_pendingAutoUnequipLeftForm = item;
      else
          m_pendingAutoUnequipRightForm = item;
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
         }
}
        }
      
        switch (type)
        {
            case WeaponType::Dagger:
    if (!s_suppressDrawSound && !shouldExclude && !onDrawCooldown && cachedDaggerSound != 0)
  PlaySoundAtPlayer(cachedDaggerSound);
          break;
        case WeaponType::Sword:
                if (!s_suppressDrawSound && !shouldExclude && !onDrawCooldown && cachedSwordSound != 0)
       PlaySoundAtPlayer(cachedSwordSound);
       break;
            case WeaponType::Mace:
      if (!s_suppressDrawSound && !shouldExclude && !onDrawCooldown && cachedMaceSound != 0)
           PlaySoundAtPlayer(cachedMaceSound);
   break;
   case WeaponType::Axe:
     if (!s_suppressDrawSound && !shouldExclude && !onDrawCooldown && cachedAxeSound != 0)
      PlaySoundAtPlayer(cachedAxeSound);
  break;
   case WeaponType::Shield:
   break;
            default:
          break;
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
   EquippedWeapon& hand = isLeftHand ? m_equipState.leftHand : m_equipState.rightHand;
    hand.Clear();
     
        // Record unequip time for draw sound cooldown
      // Only track weapons (not shields)
   if (type != WeaponType::Shield && type != WeaponType::None)
        {
    std::lock_guard<std::mutex> lock(s_drawMutex);
      s_lastUnequipTimes[item->formID] = std::chrono::steady_clock::now();
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
                }
            }
        }
        
        switch (type)
     {
     case WeaponType::Dagger:
                if (!s_suppressSheathSound && !shouldExclude && !onSheathCooldown && cachedDaggerSheathSound != 0)
      PlaySoundAtPlayer(cachedDaggerSheathSound);
     break;
            case WeaponType::Sword:
                if (!s_suppressSheathSound && !shouldExclude && !onSheathCooldown && cachedSwordSheathSound != 0)
   PlaySoundAtPlayer(cachedSwordSheathSound);
                break;
case WeaponType::Mace:
                if (!s_suppressSheathSound && !shouldExclude && !onSheathCooldown && cachedMaceSheathSound != 0)
          PlaySoundAtPlayer(cachedMaceSheathSound);
      break;
            case WeaponType::Axe:
       if (!s_suppressSheathSound && !shouldExclude && !onSheathCooldown && cachedAxeSheathSound != 0)
         PlaySoundAtPlayer(cachedAxeSheathSound);
   break;
            case WeaponType::Shield:
     break;
            default:
      break;
   }
    
  LogEquipmentState();
  
        // Update VR input handler grab listening
        VRInputHandler::GetSingleton()->UpdateGrabListening();
    }

    void EquipManager::LogEquipmentState()
    {
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

            // Two-handed melee weapons - always excluded
            case TESObjectWEAP::GameData::kType_TwoHandSword:
            case TESObjectWEAP::GameData::kType_2HS:
            case TESObjectWEAP::GameData::kType_TwoHandAxe:
            case TESObjectWEAP::GameData::kType_2HA:
                return WeaponType::None;

            // Bows, staffs, crossbows - always excluded
            case TESObjectWEAP::GameData::kType_Bow:
            case TESObjectWEAP::GameData::kType_Staff:
            case TESObjectWEAP::GameData::kType_CrossBow:
       return WeaponType::None;
            

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

        // Exclude items listed in FalseEdgeVR.ini [WeaponExclusions]
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
      return true;

            case TESObjectWEAP::GameData::kType_TwoHandSword:
            case TESObjectWEAP::GameData::kType_2HS:
            case TESObjectWEAP::GameData::kType_TwoHandAxe:
            case TESObjectWEAP::GameData::kType_2HA:
                return false;
            
            default:
 return false;  // Bows, staffs, crossbows - don't track
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

    bool EquipManager::Get2HWeaponWornGameHands(Actor* actor, TESForm* weapon, bool& wornLeft, bool& wornRight)
    {
        wornLeft = false;
        wornRight = false;

        if (!actor || !weapon)
            return false;

        ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
            actor->extraData.GetByType(kExtraData_ContainerChanges));

        if (!containerChanges || !containerChanges->data)
            return false;

        InventoryEntryData* entryData = containerChanges->data->FindItemEntry(weapon);
        if (!entryData)
            return false;

        BaseExtraList* rightEquipList = NULL;
        BaseExtraList* leftEquipList = NULL;
        entryData->GetExtraWornBaseLists(&rightEquipList, &leftEquipList);

        wornRight = (rightEquipList != NULL);
        wornLeft = (leftEquipList != NULL);
        return wornLeft || wornRight;
    }

    TESForm* EquipManager::Get2HWeaponWornOnGameHand(Actor* actor, bool isLeftGameHand)
    {
        if (!actor)
            return nullptr;

        ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
            actor->extraData.GetByType(kExtraData_ContainerChanges));

        if (!containerChanges || !containerChanges->data || !containerChanges->data->objList)
            return nullptr;

        for (EntryDataList::Iterator it = containerChanges->data->objList->Begin(); !it.End(); ++it)
        {
            InventoryEntryData* entry = it.Get();
            if (!entry || !entry->type || !IsTwoHandedWeapon(entry->type))
                continue;

            bool wornLeft = false;
            bool wornRight = false;
            if (!Get2HWeaponWornGameHands(actor, entry->type, wornLeft, wornRight))
                continue;

            if (isLeftGameHand && wornLeft)
                return entry->type;
            if (!isLeftGameHand && wornRight)
                return entry->type;
        }

        return nullptr;
    }

    void EquipManager::TryLog2HLeftHandWithRightGameHandTrigger(TESForm* weapon)
    {
        if (!VRInputHandler::IsTriggerHeldForGameHand(false))
            return;

        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return;

        TESForm* leftEquipped = weapon ? weapon : player->GetEquippedObject(true);
        if (!leftEquipped || !IsWeapon(leftEquipped))
            return;

        TESForm* grabbed2HForm = nullptr;
        if (higgsInterface)
        {
            bool rightGameHandVRIsLeft = GameHandToVRController(false);
            TESObjectREFR* grabbed = higgsInterface->GetGrabbedObject(rightGameHandVRIsLeft);
            if (grabbed && grabbed->baseForm && IsTwoHandedWeapon(grabbed->baseForm))
                grabbed2HForm = grabbed->baseForm;
        }

        if (!grabbed2HForm)
            return;

        const char* leftWeaponName = leftEquipped->GetName();
        const char* grabbedWeaponName = grabbed2HForm->GetName();
        _MESSAGE("[FalseEdgeVR] LEFT game hand weapon equipped + RIGHT game hand trigger pressed + RIGHT game hand 2H grabbed: left=%s (0x%08X) grabbed=%s (0x%08X)",
            leftWeaponName ? leftWeaponName : "(unnamed)", leftEquipped->formID,
            grabbedWeaponName ? grabbedWeaponName : "(unnamed)", grabbed2HForm->formID);

        // After a short delay, re-check whether the combo caused the left game
        // hand weapon to unequip. Guard against stacking duplicate checks.
        if (!m_comboLeftUnequipCheckPending)
        {
            m_comboLeftUnequipCheckPending = true;
            std::thread(ComboLeftUnequipCheckThread, leftEquipped->formID, 100).detach();
        }
    }

    void EquipManager::CheckLeftHandUnequipAfterCombo(UInt32 leftWeaponFormID)
    {
        m_comboLeftUnequipCheckPending = false;

        if (leftWeaponFormID == 0)
            return;

        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return;

        TESForm* weapon = LookupFormByID(leftWeaponFormID);
        if (!weapon)
            return;

        bool wornLeft = false;
        bool wornRight = false;
        Get2HWeaponWornGameHands(player, weapon, wornLeft, wornRight);

        if (!wornLeft)
        {
            const char* weaponName = weapon->GetName();
            _MESSAGE("[FalseEdgeVR] right hand 2h weapon grabbed and trigger combo caused the left 2h weapon to unequip: %s (0x%08X)",
                weaponName ? weaponName : "(unnamed)", leftWeaponFormID);

            // TEMPORARILY DISABLED: auto re-equip of the left-hand 2H weapon after the
            // grab+trigger combo. Detection logging above is kept. Restore the block
            // below to re-enable the re-equip-to-left behavior.
            /*
            // Only re-equip if this same weapon had been equipped on the left
            // game hand for at least 0.2 seconds before it was unequipped.
            if (m_leftHand2HFormID != leftWeaponFormID)
            {
                _MESSAGE("[FalseEdgeVR] skipping re-equip - no equip-time record for this weapon");
                return;
            }

            float equippedSeconds = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - m_leftHand2HEquipTime).count();
            if (equippedSeconds < 0.2f)
            {
                _MESSAGE("[FalseEdgeVR] skipping re-equip - left 2h was equipped only %.2fs (need >= 0.2s)",
                    equippedSeconds);
                return;
            }

            // Re-equip the unequipped weapon back to the LEFT hand from inventory.
            // Mirrors DelayedEquipWeaponTask so no other equip/grab path is touched.
            ::EquipManager* equipMan = ::EquipManager::GetSingleton();
            if (equipMan)
            {
                BGSEquipSlot* slot = GetLeftHandSlot();
                s_suppressDrawSound = true;
                CALL_MEMBER_FN(equipMan, EquipItem)(player, weapon, nullptr, 1, slot, false, true, false, nullptr);
                s_suppressDrawSound = false;
                _MESSAGE("[FalseEdgeVR] re-equipped left 2h weapon to LEFT hand from inventory: %s (0x%08X)",
                    weaponName ? weaponName : "(unnamed)", leftWeaponFormID);
            }
            */
        }
    }

    void EquipManager::ScheduleDelayedLogPlayer2HWeaponEquip(UInt32 weaponFormID)
    {
        if (weaponFormID == 0 || !g_task)
            return;

        g_task->AddTask(new DelayedLog2HWeaponEquipTask(weaponFormID));
    }

    void EquipManager::LogPlayer2HWeaponEquip(TESForm* weapon)
    {
        if (!weapon || !IsTwoHandedWeapon(weapon))
            return;

        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return;

        bool wornLeft = false;
        bool wornRight = false;
        if (!Get2HWeaponWornGameHands(player, weapon, wornLeft, wornRight))
            return;

        const char* weaponName = weapon->GetName();

        if (wornLeft)
        {
            _MESSAGE("[FalseEdgeVR] 2H weapon equipped to LEFT game hand: %s (0x%08X)",
                weaponName ? weaponName : "(unnamed)", weapon->formID);

            // Record when this 2H became worn on the left game hand so the
            // combo re-equip can require a minimum equipped duration.
            m_leftHand2HFormID = weapon->formID;
            m_leftHand2HEquipTime = std::chrono::steady_clock::now();
        }

        if (wornRight)
        {
            _MESSAGE("[FalseEdgeVR] 2H weapon equipped to RIGHT game hand: %s (0x%08X)",
                weaponName ? weaponName : "(unnamed)", weapon->formID);
        }
    }

  // ============================================
    // Forced Unequip Functions
    // ============================================

    void EquipManager::ForceUnequipHand(bool isLeftHand)
    {
   PlayerCharacter* player = *g_thePlayer;
        if (!player)
      {
     return;
        }

 EquippedWeapon& hand = isLeftHand ? m_equipState.leftHand : m_equipState.rightHand;
     if (!hand.isEquipped || !hand.form)
        {
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
   return;
   }

     // Find the inventory entry for this item
 InventoryEntryData* entryData = containerChanges->data->FindItemEntry(item);
     if (!entryData)
   {
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

}

    void EquipManager::ForceUnequipLeftHand()
    {
        ForceUnequipHand(true);
    }

    void EquipManager::ForceUnequipRightHand()
    {
        ForceUnequipHand(false);
    }

    bool EquipManager::IsFormFavoritedInInventory(PlayerCharacter* player, TESForm* weaponForm)
    {
        if (!player || !weaponForm)
            return false;

        ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
            player->extraData.GetByType(kExtraData_ContainerChanges));
        if (!containerChanges || !containerChanges->data)
            return false;

        InventoryEntryData* entryData = containerChanges->data->FindItemEntry(weaponForm);
        if (!entryData || !entryData->extendDataList)
            return false;

        for (ExtendDataList::Iterator it = entryData->extendDataList->Begin(); !it.End(); ++it)
        {
            BaseExtraList* extraList = it.Get();
            if (extraList && extraList->HasType(kExtraData_Hotkey))
                return true;
        }

        return false;
    }

    void EquipManager::RestoreFavoriteInInventory(PlayerCharacter* player, TESForm* weaponForm)
    {
        if (!player || !weaponForm || IsFormFavoritedInInventory(player, weaponForm))
            return;

        ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
            player->extraData.GetByType(kExtraData_ContainerChanges));
        if (!containerChanges || !containerChanges->data)
            return;

        InventoryEntryData* entryData = containerChanges->data->FindItemEntry(weaponForm);
        if (!entryData || !entryData->extendDataList)
            return;

        for (ExtendDataList::Iterator it = entryData->extendDataList->Begin(); !it.End(); ++it)
        {
            BaseExtraList* extraList = it.Get();
            if (!extraList)
                continue;

            if (extraList->HasType(kExtraData_Worn) || extraList->HasType(kExtraData_WornLeft))
                continue;

            if (extraList->HasType(kExtraData_Hotkey))
                return;

            ExtraHotkey* xHotkey = ExtraHotkey::Create();
            xHotkey->hotkey = -1;
            extraList->Add(kExtraData_Hotkey, xHotkey);
            return;
        }
    }

    bool EquipManager::ShouldPreserveFavorite(TESForm* weaponForm) const
    {
        if (!weaponForm)
            return false;

        UInt32 formID = weaponForm->formID;
        if (m_preservedFavoriteFormIDLeft == formID || m_preservedFavoriteFormIDRight == formID)
            return true;
        if (m_cachedWeaponFormIDLeft == formID && m_wasFavoritedLeft)
            return true;
        if (m_cachedWeaponFormIDRight == formID && m_wasFavoritedRight)
            return true;

        PlayerCharacter* player = *g_thePlayer;
        return player && IsFormFavoritedInInventory(player, weaponForm);
    }

    BaseExtraList* EquipManager::FindInventoryExtraDataForEquip(PlayerCharacter* player, TESForm* weaponForm, bool isLeftGameHand)
    {
        if (!player || !weaponForm)
            return nullptr;

        bool hasCachedHealth = isLeftGameHand ? m_hasCachedHealthLeft : m_hasCachedHealthRight;
        float cachedHealth = isLeftGameHand ? m_cachedWeaponHealthLeft : m_cachedWeaponHealthRight;
        bool hasCachedEnchant = isLeftGameHand ? m_hasCachedEnchantmentLeft : m_hasCachedEnchantmentRight;
        UInt32 cachedEnchantID = isLeftGameHand ? m_cachedEnchantmentFormIDLeft : m_cachedEnchantmentFormIDRight;
        bool preserveFavorite = ShouldPreserveFavorite(weaponForm);

        if (!preserveFavorite &&
            !((hasCachedHealth && cachedHealth > 1.0f) || hasCachedEnchant))
        {
            return nullptr;
        }

        ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
            player->extraData.GetByType(kExtraData_ContainerChanges));
        if (!containerChanges || !containerChanges->data)
            return nullptr;

        InventoryEntryData* entryData = containerChanges->data->FindItemEntry(weaponForm);
        if (!entryData || !entryData->extendDataList)
            return nullptr;

        BaseExtraList* favoriteMatch = nullptr;
        BaseExtraList* attributeMatch = nullptr;

        for (ExtendDataList::Iterator it = entryData->extendDataList->Begin(); !it.End(); ++it)
        {
            BaseExtraList* extraList = it.Get();
            if (!extraList)
                continue;

            bool isWorn = extraList->HasType(kExtraData_Worn);
            bool isWornLeft = extraList->HasType(kExtraData_WornLeft);
            if (isWorn || isWornLeft)
                continue;

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
                    healthMatches = false;
                }
            }

            bool enchantMatches = true;
            if (hasCachedEnchant)
            {
                ExtraEnchantment* xEnchant = static_cast<ExtraEnchantment*>(extraList->GetByType(kExtraData_Enchantment));
                if (xEnchant && xEnchant->enchant)
                    enchantMatches = (xEnchant->enchant->formID == cachedEnchantID);
                else
                    enchantMatches = false;
            }
            else
            {
                ExtraEnchantment* xEnchant = static_cast<ExtraEnchantment*>(extraList->GetByType(kExtraData_Enchantment));
                if (xEnchant && xEnchant->enchant)
                    enchantMatches = false;
            }

            if (extraList->HasType(kExtraData_Hotkey))
            {
                if (!favoriteMatch || (healthMatches && enchantMatches))
                    favoriteMatch = extraList;
            }

            if (healthMatches && enchantMatches && !attributeMatch)
                attributeMatch = extraList;
        }

        if (preserveFavorite && favoriteMatch)
            return favoriteMatch;
        return attributeMatch;
    }

    void EquipManager::RestoreFavoriteOnEquippedHand(PlayerCharacter* player, TESForm* weaponForm, bool isLeftGameHand)
    {
        if (!player || !weaponForm || !ShouldPreserveFavorite(weaponForm))
            return;

        ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
            player->extraData.GetByType(kExtraData_ContainerChanges));
        if (!containerChanges || !containerChanges->data)
            return;

        InventoryEntryData* entryData = containerChanges->data->FindItemEntry(weaponForm);
        if (!entryData || !entryData->extendDataList)
            return;

        for (ExtendDataList::Iterator it = entryData->extendDataList->Begin(); !it.End(); ++it)
        {
            BaseExtraList* extraList = it.Get();
            if (!extraList)
                continue;

            bool isWorn = extraList->HasType(kExtraData_Worn);
            bool isWornLeft = extraList->HasType(kExtraData_WornLeft);
            bool isCorrectHand = isLeftGameHand ? isWornLeft : isWorn;
            if (!isCorrectHand)
                continue;

            if (!extraList->HasType(kExtraData_Hotkey))
            {
                ExtraHotkey* xHotkey = ExtraHotkey::Create();
                xHotkey->hotkey = -1;
                extraList->Add(kExtraData_Hotkey, xHotkey);
            }

            if (isLeftGameHand)
                m_wasFavoritedLeft = true;
            else
                m_wasFavoritedRight = true;

            if (isLeftGameHand)
                m_preservedFavoriteFormIDLeft = weaponForm->formID;
            else
                m_preservedFavoriteFormIDRight = weaponForm->formID;
            break;
        }
    }

    void EquipManager::PreserveFavoriteForForm(UInt32 weaponFormID, bool isLeftGameHand)
    {
        if (weaponFormID == 0)
            return;

        TESForm* weaponForm = LookupFormByID(weaponFormID);
        bool favorited = isLeftGameHand ? m_wasFavoritedLeft : m_wasFavoritedRight;
        if (!favorited && weaponForm)
        {
            favorited = (m_preservedFavoriteFormIDLeft == weaponFormID) ||
                (m_preservedFavoriteFormIDRight == weaponFormID);
        }
        if (!favorited && weaponForm)
        {
            PlayerCharacter* player = *g_thePlayer;
            favorited = player && IsFormFavoritedInInventory(player, weaponForm);
        }
        if (!favorited)
            return;

        if (isLeftGameHand)
        {
            m_preservedFavoriteFormIDLeft = weaponFormID;
            m_wasFavoritedLeft = true;
        }
        else
        {
            m_preservedFavoriteFormIDRight = weaponFormID;
            m_wasFavoritedRight = true;
        }
    }

    void EquipManager::EquipWeaponToGameHand(PlayerCharacter* player, TESForm* weaponForm, bool isLeftGameHand)
    {
        if (!player || !weaponForm)
            return;

        ::EquipManager* equipMan = ::EquipManager::GetSingleton();
        if (!equipMan)
            return;

        BaseExtraList* extraDataToUse = FindInventoryExtraDataForEquip(player, weaponForm, isLeftGameHand);
        if (extraDataToUse)
            SetPlayerOwnership(extraDataToUse);

        BGSEquipSlot* slot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();
        CALL_MEMBER_FN(equipMan, EquipItem)(player, weaponForm, extraDataToUse, 1, slot, false, true, false, nullptr);
        EnsurePlayerOwnsWeaponInInventory(player, weaponForm);
        RestoreFavoriteOnEquippedHand(player, weaponForm, isLeftGameHand);
    }

    void EquipManager::TransferFavoriteCacheForHandSwap(bool fromLeftGameHand, bool toLeftGameHand, UInt32 weaponFormID)
    {
        if (weaponFormID == 0)
            return;

        TESForm* weaponForm = LookupFormByID(weaponFormID);
        bool sourceFavorited = fromLeftGameHand ? m_wasFavoritedLeft : m_wasFavoritedRight;
        PlayerCharacter* player = *g_thePlayer;
        bool keepFavorited = sourceFavorited ||
            (weaponForm && IsFormFavoritedInInventory(player, weaponForm));
        UInt32 sourcePreserved = fromLeftGameHand ? m_preservedFavoriteFormIDLeft : m_preservedFavoriteFormIDRight;
        if (!keepFavorited && sourcePreserved == weaponFormID)
            keepFavorited = true;

        if (toLeftGameHand)
        {
            m_cachedWeaponFormIDLeft = weaponFormID;
            m_wasFavoritedLeft = keepFavorited;
            if (keepFavorited)
                m_preservedFavoriteFormIDLeft = weaponFormID;
        }
        else
        {
            m_cachedWeaponFormIDRight = weaponFormID;
            m_wasFavoritedRight = keepFavorited;
            if (keepFavorited)
                m_preservedFavoriteFormIDRight = weaponFormID;
        }

        if (fromLeftGameHand)
        {
            m_wasFavoritedLeft = false;
            m_preservedFavoriteFormIDLeft = 0;
        }
        else
        {
            m_wasFavoritedRight = false;
            m_preservedFavoriteFormIDRight = 0;
        }
    }

    void EquipManager::ForceReequipHand(bool isLeftHand)
    {
      PlayerCharacter* player = *g_thePlayer;
     if (!player)
        {
            return;
        }

     UInt32 cachedFormID = isLeftHand ? m_cachedWeaponFormIDLeft : m_cachedWeaponFormIDRight;
     if (cachedFormID == 0)
         return;

     TESForm* weaponForm = LookupFormByID(cachedFormID);
     if (!weaponForm)
         return;

     EquipWeaponToGameHand(player, weaponForm, isLeftHand);
     ClearPendingReequip(isLeftHand);
}

    void EquipManager::ClearReequipCache(bool isLeftHand)
    {
        if (isLeftHand)
        {
            m_cachedWeaponFormIDLeft = 0;
            m_hasCachedHealthLeft = false;
            m_cachedWeaponHealthLeft = 1.0f;
            m_hasCachedEnchantmentLeft = false;
            m_cachedEnchantmentFormIDLeft = 0;
            m_wasFavoritedLeft = false;
            m_preservedFavoriteFormIDLeft = 0;
        }
        else
        {
            m_cachedWeaponFormIDRight = 0;
            m_hasCachedHealthRight = false;
            m_cachedWeaponHealthRight = 1.0f;
            m_hasCachedEnchantmentRight = false;
            m_cachedEnchantmentFormIDRight = 0;
            m_wasFavoritedRight = false;
            m_preservedFavoriteFormIDRight = 0;
        }
    }

    void EquipManager::ForceReequipLeftHand()
    {
        ForceReequipHand(true);
        ClearReequipCache(true);
 }

    void EquipManager::ForceReequipRightHand()
    {
        ForceReequipHand(false);
        ClearReequipCache(false);
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

    bool EquipManager::TryGetPlayerEquipHand(Actor* actor, TESForm* item, bool& isLeftHandOut)
    {
        if (!actor || !item)
            return false;

        ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
            actor->extraData.GetByType(kExtraData_ContainerChanges));
        if (containerChanges && containerChanges->data)
        {
            InventoryEntryData* entryData = containerChanges->data->FindItemEntry(item);
            if (entryData)
            {
                BaseExtraList* rightEquipList = NULL;
                BaseExtraList* leftEquipList = NULL;
                entryData->GetExtraWornBaseLists(&rightEquipList, &leftEquipList);

                if (leftEquipList && !rightEquipList)
                {
                    isLeftHandOut = true;
                    return true;
                }
                if (rightEquipList && !leftEquipList)
                {
                    isLeftHandOut = false;
                    return true;
                }
            }
        }

        TESForm* leftEquipped = actor->GetEquippedObject(true);
        TESForm* rightEquipped = actor->GetEquippedObject(false);
        if (leftEquipped && leftEquipped->formID == item->formID)
        {
            isLeftHandOut = true;
            return true;
        }
        if (rightEquipped && rightEquipped->formID == item->formID)
        {
            isLeftHandOut = false;
            return true;
        }

        return false;
    }

    TESObjectREFR* EquipManager::ResolveGrabbedWeaponRefForHand(bool isLeftGameHand) const
    {
        TESObjectREFR* droppedRef = GetDroppedWeaponRef(isLeftGameHand);
        if (droppedRef)
            return droppedRef;

        UInt32 refID = isLeftGameHand ? m_droppedWeaponRefIDLeft : m_droppedWeaponRefIDRight;
        if (refID != 0)
        {
            TESForm* form = LookupFormByID(refID);
            TESObjectREFR* ref = form ? DYNAMIC_CAST(form, TESForm, TESObjectREFR) : nullptr;
            if (ref)
                return ref;
        }

        if (higgsInterface)
        {
            bool vrControllerIsLeft = GameHandToVRController(isLeftGameHand);
            return higgsInterface->GetGrabbedObject(vrControllerIsLeft);
        }

        return nullptr;
    }

    bool EquipManager::HasConflictingGrabbedWeaponInHand(bool isLeftGameHand, TESForm* itemBeingEquipped) const
    {
        if (!itemBeingEquipped)
            return false;

        UInt32 equipFormID = itemBeingEquipped->formID;

        UInt32 trackedBaseID = GetDroppedWeaponBaseID(isLeftGameHand);
        if (trackedBaseID != 0 && trackedBaseID != equipFormID)
        {
            if (GetDroppedWeaponRef(isLeftGameHand) ||
                (isLeftGameHand ? m_droppedWeaponRefIDLeft : m_droppedWeaponRefIDRight) != 0 ||
                HasPendingReequip(isLeftGameHand))
            {
                return true;
            }
        }

        if (HasPendingReequip(isLeftGameHand))
        {
            UInt32 cachedID = GetCachedWeaponFormID(isLeftGameHand);
            if (cachedID != 0 && cachedID != equipFormID)
                return true;
        }

        TESObjectREFR* grabbedRef = ResolveGrabbedWeaponRefForHand(isLeftGameHand);
        if (grabbedRef && grabbedRef->baseForm && grabbedRef->baseForm->formType == kFormType_Weapon)
        {
            if (grabbedRef->baseForm->formID != equipFormID)
                return true;
        }

        return false;
    }

    bool EquipManager::PickUpGrabbedWeaponBeforeEquip(bool isLeftGameHand, bool allowActivateFallback)
    {
        TESObjectREFR* weaponRef = ResolveGrabbedWeaponRefForHand(isLeftGameHand);
        if (!weaponRef)
            return false;

        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return false;

        TESObjectREFR* liveRef = ResolveLiveWeaponWorldRef(weaponRef);
        UInt32 grabbedBaseID = GetDroppedWeaponBaseID(isLeftGameHand);
        bool recovered = false;
        bool wasDualWieldingSame = WasDualWieldingSameWeapon(isLeftGameHand);

        if (!liveRef)
        {
            _MESSAGE("[FalseEdgeVR] Grabbed weapon ref invalid for %s game hand — clearing tracking only",
                isLeftGameHand ? "LEFT" : "RIGHT");
        }
        else
        {
            TESForm* grabbedForm = liveRef->baseForm;
            grabbedBaseID = grabbedForm ? grabbedForm->formID : grabbedBaseID;
            const bool worldRefHadHotkey = WorldRefHasHotkey(liveRef);

            if (wasDualWieldingSame)
            {
                DeleteWorldObject(liveRef);
                recovered = true;
            }
            else if (grabbedForm && GetPlayerWeaponCount(player, grabbedForm) > 0)
            {
                DeleteWorldObject(liveRef);
                if (worldRefHadHotkey)
                    RestoreFavoriteInInventory(player, grabbedForm);
                recovered = true;
            }
            else if (grabbedForm)
            {
                SetOwnerToPlayer(liveRef);
                const SInt32 countBefore = GetPlayerWeaponCount(player, grabbedForm);
                AddItem_Native(nullptr, 0, player, grabbedForm, 1, true);

                if (GetPlayerWeaponCount(player, grabbedForm) > countBefore)
                {
                    EnsurePlayerOwnsWeaponInInventory(player, grabbedForm);
                    if (worldRefHadHotkey)
                        RestoreFavoriteInInventory(player, grabbedForm);
                    DeleteWorldObject(liveRef);
                    recovered = true;
                }
                else if (allowActivateFallback)
                {
                    s_suppressPickupSound = true;
                    const bool pickedUp = SafeActivate(liveRef, player, 0, 0, 1, false);
                    s_suppressPickupSound = false;

                    if (pickedUp)
                    {
                        EnsurePlayerOwnsWeaponInInventory(player, grabbedForm);
                        if (worldRefHadHotkey)
                            RestoreFavoriteInInventory(player, grabbedForm);
                        recovered = true;
                    }
                    else
                    {
                        _MESSAGE("[FalseEdgeVR] Failed to recover grabbed weapon 0x%08X for %s hand",
                            grabbedBaseID, isLeftGameHand ? "LEFT" : "RIGHT");
                    }
                }
                else
                {
                    _MESSAGE("[FalseEdgeVR] Deferred AddItem recovery failed for 0x%08X — will retry next frame",
                        grabbedBaseID);
                    return false;
                }
            }
        }

        ClearDroppedWeaponRef(isLeftGameHand);
        ClearPendingReequip(isLeftGameHand);
        ClearCachedWeaponFormID(isLeftGameHand);
        ClearReequipCache(isLeftGameHand);

        if (isLeftGameHand)
            m_wasDualWieldingSameWeaponLeft = false;
        else
            m_wasDualWieldingSameWeaponRight = false;

        const bool vrControllerIsLeft = GameHandToVRController(isLeftGameHand);
        VRInputHandler::ClearWeaponLock(vrControllerIsLeft);

        if (recovered)
        {
            _MESSAGE("[FalseEdgeVR] Recovered grabbed weapon (0x%08X) from %s game hand before equipping different weapon",
                grabbedBaseID, isLeftGameHand ? "LEFT" : "RIGHT");
        }

        return recovered;
    }

    bool EquipManager::ForceUnequipAndGrab(bool isLeftGameHand)
    {
    if (IsWeaponGrabToHolsterBlocked())
        return false;

    PlayerCharacter* player = *g_thePlayer;
        if (!player)
        {
            return false;
        }

    // ALWAYS use direct player check for what's equipped - our state might be stale
        TESForm* leftEquipped = player->GetEquippedObject(true);
   TESForm* rightEquipped = player->GetEquippedObject(false);
     
      TESForm* item = isLeftGameHand ? leftEquipped : rightEquipped;
        if (!item)
    {
            return false;
        }
   
        if (!IsWeapon(item))
  {
    return false;
        }

        // Check if both hands have the SAME weapon (same FormID) - use DIRECT check
   bool bothHandsSameWeapon = leftEquipped && rightEquipped && 
  (leftEquipped->formID == rightEquipped->formID);
        
        if (bothHandsSameWeapon)
        {
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
   }
 else
        {
   m_cachedWeaponFormIDRight = item->formID;
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


   // Step 1: Unequip the item first (uses GAME HAND)
        ::EquipManager* equipManager = ::EquipManager::GetSingleton();
        if (!equipManager)
        {
         _MESSAGE("EquipManager::ForceUnequipAndGrab - Failed to get game EquipManager!");
          return false;
}

  ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
    player->extraData.GetByType(kExtraData_ContainerChanges));
     
   if (!containerChanges || !containerChanges->data)
        {
          return false;
}

        InventoryEntryData* entryData = containerChanges->data->FindItemEntry(item);
        if (!entryData)
        {
    return false;
        }

 BaseExtraList* rightEquipList = NULL;
      BaseExtraList* leftEquipList = NULL;
        entryData->GetExtraWornBaseLists(&rightEquipList, &leftEquipList);

        // Debug: Log what we got from GetExtraWornBaseLists
        
        if (bothHandsSameWeapon)
   {
    }

    // Note: These are GAME hand equip lists
        BaseExtraList* equipList = isLeftGameHand ? leftEquipList : rightEquipList;
 BGSEquipSlot* equipSlot = isLeftGameHand ? GetLeftHandSlot() : GetRightHandSlot();

        if (!equipList)
        {
  
// If we couldn't get the equip list for the requested hand, we cannot safely unequip
 // Using the other hand's equip list would unequip the WRONG weapon!
     // This can happen with same weapon in both hands - just abort
    _MESSAGE("EquipManager::ForceUnequipAndGrab - Cannot get correct equip list, aborting to prevent wrong weapon unequip");
  return false;
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
        }
      else
    {
          m_cachedWeaponHealthRight = xHealth->health;
      m_hasCachedHealthRight = true;
   }
            }
            else
            {
      // No extra health = base weapon (not tempered)
      if (isLeftGameHand)
     {
   m_cachedWeaponHealthLeft = 1.0f;
       m_hasCachedHealthLeft = false;
         }
       else
   {
       m_cachedWeaponHealthRight = 1.0f;
 m_hasCachedHealthRight = false;
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
        }
     else
        {
      m_cachedEnchantmentFormIDRight = xEnchant->enchant->formID;
  m_hasCachedEnchantmentRight = true;
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
  m_preservedFavoriteFormIDLeft = item->formID;
     }
     else
    {
        m_wasFavoritedRight = true;
        m_preservedFavoriteFormIDRight = item->formID;
                }
          }
          else
         {
    if (isLeftGameHand)
  {
 m_wasFavoritedLeft = false;
 m_preservedFavoriteFormIDLeft = 0;
   }
      else
           {
           m_wasFavoritedRight = false;
           m_preservedFavoriteFormIDRight = 0;
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


   // Clear weapon lock for the VR controller corresponding to this game hand
   // This prevents the controller from getting stuck in lock mode when weapon is unequipped
   bool isLeftVRController = GameHandToVRController(isLeftGameHand);
   VRInputHandler::ClearWeaponLock(isLeftVRController);

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

          // Simple offset from player world position (mounted settings)
          spawnPos.x = player->pos.x + spawnOffsetMountedX;
          spawnPos.y = player->pos.y + spawnOffsetMountedY;
          spawnPos.z = player->pos.z + spawnOffsetMountedZ;

      }
    else
  {
          // NOT MOUNTED: Spawn at player position with configurable offsets

          // Simple offset from player world position
          spawnPos.x = player->pos.x + spawnOffsetX;
          spawnPos.y = player->pos.y + spawnOffsetY;
          spawnPos.z = player->pos.z + spawnOffsetZ;

        }

      // Step 3: Create a world object using PlaceAtMe
      TESObjectREFR* droppedWeapon = PlaceAtMe_Native(nullptr, 0, player, item, 1, false, false);

      if (droppedWeapon)
      {

          // Step 3.1: Move weapon to calculated spawn position
          droppedWeapon->pos = spawnPos;

          // Step 3.25: Set ownership to player to prevent "stolen" flag when picking up
          SetOwnerToPlayer(droppedWeapon);

          // Copy favorite tag onto the world copy before inventory removal consumes the source stack.
          CopyHotkeyExtraDataToWorldRef(equipList, droppedWeapon);

  // Step 3.5: Remove the item from inventory to prevent duplication
  // PlaceAtMe creates a COPY, so we need to remove the original from inventory
        // EXCEPTION: If both hands have the same weapon, don't remove - we need it for the other hand!
        if (!bothHandsSameWeapon)
        {
    RemoveItemFromInventory(player, item, 1, true);
        }
     else
   {
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
     higgsInterface->GrabObject(droppedWeapon, isLeftVRController);
     
 // REMOVED: Track the grabbed weapon for scaling
     // TrackGrabbedWeapon(droppedWeapon, isLeftVRController);
   }
  else
       {
    }
    }
  else
         {
         _MESSAGE("EquipManager: HIGGS interface not available!");
       }
            return true;
     }
        else
  {
          _MESSAGE("EquipManager: Failed to create world weapon reference!");
          return false;
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

    namespace
    {
        constexpr UInt32 kDroppedWeaponsSaveRecord = 'FEDW';
        constexpr UInt32 kDroppedWeaponsSaveVersion = 3;

        struct DroppedWeaponsSaveDataV1
        {
            UInt32 refIDLeft = 0;
            UInt32 baseIDLeft = 0;
            UInt32 refIDRight = 0;
            UInt32 baseIDRight = 0;
        };

        struct DroppedWeaponsSaveDataV2
        {
            UInt32 refIDLeft = 0;
            UInt32 baseIDLeft = 0;
            UInt32 refIDRight = 0;
            UInt32 baseIDRight = 0;
            UInt8 wasFavoritedLeft = 0;
            UInt8 wasFavoritedRight = 0;
            UInt8 pad0 = 0;
            UInt8 pad1 = 0;
        };

        struct DroppedWeaponsSaveData
        {
            UInt32 refIDLeft = 0;
            UInt32 baseIDLeft = 0;
            UInt32 refIDRight = 0;
            UInt32 baseIDRight = 0;
            UInt32 favoritedBaseIDLeft = 0;
            UInt32 favoritedBaseIDRight = 0;
        };

        DroppedWeaponsSaveData s_savedDroppedWeapons;
        DroppedWeaponsSaveData s_capturedDroppedWeapons;

        static bool WasFavoritedForBaseForm(UInt32 baseFormID)
        {
            if (baseFormID == 0)
                return false;

            if (s_savedDroppedWeapons.favoritedBaseIDLeft == baseFormID ||
                s_savedDroppedWeapons.favoritedBaseIDRight == baseFormID ||
                s_capturedDroppedWeapons.favoritedBaseIDLeft == baseFormID ||
                s_capturedDroppedWeapons.favoritedBaseIDRight == baseFormID)
            {
                return true;
            }

            return false;
        }

        static void RestoreFavoriteIfNeeded(PlayerCharacter* player, TESForm* baseForm, bool worldRefHadHotkey = false)
        {
            if (!player || !baseForm)
                return;

            if (worldRefHadHotkey || WasFavoritedForBaseForm(baseForm->formID))
                EquipManager::RestoreFavoriteInInventory(player, baseForm);
        }

        static void RestoreAllPendingFavorites(PlayerCharacter* player)
        {
            if (!player)
                return;

            auto restoreForm = [&](UInt32 baseFormID) {
                if (baseFormID == 0 || !WasFavoritedForBaseForm(baseFormID))
                    return;

                TESForm* form = LookupFormByID(baseFormID);
                if (form)
                    EquipManager::RestoreFavoriteInInventory(player, form);
            };

            restoreForm(s_savedDroppedWeapons.favoritedBaseIDLeft);
            restoreForm(s_savedDroppedWeapons.favoritedBaseIDRight);
            restoreForm(s_capturedDroppedWeapons.favoritedBaseIDLeft);
            restoreForm(s_capturedDroppedWeapons.favoritedBaseIDRight);
        }

        static bool IsSavedGrabbedDropRef(const TESObjectREFR* ref)
        {
            if (!ref)
                return false;

            const UInt32 refID = ref->formID;
            const UInt32 baseID = ref->baseForm ? ref->baseForm->formID : 0;

            auto matches = [&](const DroppedWeaponsSaveData& data) {
                if (refID != 0 && (refID == data.refIDLeft || refID == data.refIDRight))
                    return true;

                const bool hadSavedDrop = data.refIDLeft != 0 || data.refIDRight != 0;
                if (hadSavedDrop && baseID != 0 &&
                    (baseID == data.baseIDLeft || baseID == data.baseIDRight))
                {
                    return true;
                }

                return false;
            };

            return matches(s_savedDroppedWeapons) || matches(s_capturedDroppedWeapons);
        }

        static bool ShouldRecoverWeaponRefOnLoad(TESObjectREFR* ref)
        {
            if (!ref || !ref->baseForm || !EquipManager::IsWeapon(ref->baseForm))
                return false;

            // Only favorited drops: hotkey copied onto world ref at unequip, or co-save favorite IDs.
            if (WorldRefHasHotkey(ref))
                return true;

            return WasFavoritedForBaseForm(ref->baseForm->formID);
        }

        static bool HasNoOwnership(TESObjectREFR* ref)
        {
            if (!ref)
                return false;

            ExtraOwnership* xOwnership = static_cast<ExtraOwnership*>(
                ref->extraData.GetByType(kExtraData_Ownership));
            return !xOwnership || !xOwnership->owner;
        }

        static float DistanceSquared(const NiPoint3& a, const NiPoint3& b)
        {
            float dx = a.x - b.x;
            float dy = a.y - b.y;
            float dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        }

        static SInt32 GetInventoryItemCount(PlayerCharacter* player, TESForm* itemForm)
        {
            if (!player || !itemForm)
                return 0;

            ExtraContainerChanges* containerChanges = static_cast<ExtraContainerChanges*>(
                player->extraData.GetByType(kExtraData_ContainerChanges));
            if (!containerChanges || !containerChanges->data)
                return 0;

            InventoryEntryData* entryData = containerChanges->data->FindItemEntry(itemForm);
            if (!entryData)
                return 0;

            return entryData->countDelta;
        }

        static TESObjectREFR* ResolveWeaponRef(UInt32 refID)
        {
            if (refID == 0)
                return nullptr;

            TESForm* form = LookupFormByID(refID);
            return form ? DYNAMIC_CAST(form, TESForm, TESObjectREFR) : nullptr;
        }

        static bool RecoverSingleWeaponWorldRef(PlayerCharacter* player, TESObjectREFR* weaponRef)
        {
            if (!player || !weaponRef)
                return false;

            TESObjectREFR* liveRef = ResolveWeaponRef(weaponRef->formID);
            if (!liveRef)
                liveRef = weaponRef;

            TESForm* baseForm = liveRef->baseForm;
            if (!baseForm || !EquipManager::IsWeapon(baseForm))
                return false;

            const bool worldRefHadHotkey = WorldRefHasHotkey(liveRef);

            if (GetInventoryItemCount(player, baseForm) > 0)
            {
                DeleteWorldObject(liveRef);
                RestoreFavoriteIfNeeded(player, baseForm, worldRefHadHotkey);
                return true;
            }

            SInt32 countBefore = GetInventoryItemCount(player, baseForm);

            SetOwnerToPlayer(liveRef);
            EquipManager::s_suppressPickupSound = true;
            bool pickedUp = SafeActivate(liveRef, player, 0, 0, 1, false);
            EquipManager::s_suppressPickupSound = false;

            if (!pickedUp)
            {
                AddItem_Native(nullptr, 0, player, baseForm, 1, true);
                pickedUp = GetInventoryItemCount(player, baseForm) > countBefore;
                if (pickedUp)
                    DeleteWorldObject(liveRef);
            }

            if (pickedUp)
            {
                EnsurePlayerOwnsWeaponInInventory(player, baseForm);
                RestoreFavoriteIfNeeded(player, baseForm, worldRefHadHotkey);
            }

            return pickedUp;
        }

        static void AddRecoveryCandidate(
            std::unordered_set<UInt32>& processedRefIDs,
            UInt32& recoveredCount,
            PlayerCharacter* player,
            UInt32 refID,
            UInt32 expectedBaseID)
        {
            if (refID == 0 || processedRefIDs.count(refID) != 0)
                return;

            TESObjectREFR* weaponRef = ResolveWeaponRef(refID);
            if (!weaponRef || !weaponRef->baseForm)
                return;

            if (expectedBaseID != 0 && weaponRef->baseForm->formID != expectedBaseID)
                return;

            processedRefIDs.insert(refID);
            if (RecoverSingleWeaponWorldRef(player, weaponRef))
            {
                recoveredCount++;
                return;
            }

            processedRefIDs.erase(refID);
        }

        static void TryRecoverDropRef(
            std::unordered_set<UInt32>& processedRefIDs,
            UInt32& recoveredCount,
            PlayerCharacter* player,
            TESObjectREFR* weaponRef)
        {
            if (!weaponRef || !weaponRef->baseForm || !EquipManager::IsWeapon(weaponRef->baseForm))
                return;

            const UInt32 refID = weaponRef->formID;
            if (refID == 0 || processedRefIDs.count(refID) != 0)
                return;

            processedRefIDs.insert(refID);
            if (RecoverSingleWeaponWorldRef(player, weaponRef))
            {
                recoveredCount++;
                return;
            }

            processedRefIDs.erase(refID);
        }

        static bool ShouldRecoverNearPlayerOnLoad(
            TESObjectREFR* ref,
            const NiPoint3& playerPos,
            float maxDistSq)
        {
            if (!ref || ref == *g_thePlayer || !ref->baseForm || ref->baseForm->formType != kFormType_Weapon)
                return false;

            if (!EquipManager::IsWeapon(ref->baseForm))
                return false;

            if (DistanceSquared(ref->pos, playerPos) > maxDistSq)
                return false;

            if (IsSavedGrabbedDropRef(ref))
                return true;

            return HasNoOwnership(ref) && ShouldRecoverWeaponRefOnLoad(ref);
        }

        void RecoverGrabbedWeaponsAfterLoad()
        {
            PlayerCharacter* player = *g_thePlayer;
            if (!player || !player->loadedState)
                return;

            std::unordered_set<UInt32> processedRefIDs;
            UInt32 recoveredCount = 0;

            AddRecoveryCandidate(processedRefIDs, recoveredCount, player,
                s_savedDroppedWeapons.refIDLeft, s_savedDroppedWeapons.baseIDLeft);
            AddRecoveryCandidate(processedRefIDs, recoveredCount, player,
                s_savedDroppedWeapons.refIDRight, s_savedDroppedWeapons.baseIDRight);
            AddRecoveryCandidate(processedRefIDs, recoveredCount, player,
                s_capturedDroppedWeapons.refIDLeft, s_capturedDroppedWeapons.baseIDLeft);
            AddRecoveryCandidate(processedRefIDs, recoveredCount, player,
                s_capturedDroppedWeapons.refIDRight, s_capturedDroppedWeapons.baseIDRight);

            if (higgsInterface)
            {
                TESObjectREFR* leftHeld = higgsInterface->GetGrabbedObject(true);
                TESObjectREFR* rightHeld = higgsInterface->GetGrabbedObject(false);
                if (IsSavedGrabbedDropRef(leftHeld))
                    TryRecoverDropRef(processedRefIDs, recoveredCount, player, leftHeld);
                else if (leftHeld && ShouldRecoverWeaponRefOnLoad(leftHeld))
                    TryRecoverDropRef(processedRefIDs, recoveredCount, player, leftHeld);

                if (IsSavedGrabbedDropRef(rightHeld))
                    TryRecoverDropRef(processedRefIDs, recoveredCount, player, rightHeld);
                else if (rightHeld && ShouldRecoverWeaponRefOnLoad(rightHeld))
                    TryRecoverDropRef(processedRefIDs, recoveredCount, player, rightHeld);
            }

            // Fallback at the player's feet: co-save drops (any ownership) or favorited/no-ownership.
            TESObjectCELL* cell = player->parentCell;
            if (cell && cell->refData.refArray)
            {
                NiPoint3 playerPos = player->pos;
                constexpr float kRecoveryRadiusUnits = 1.5f * 70.0f;
                const float maxDistSq = kRecoveryRadiusUnits * kRecoveryRadiusUnits;

                for (UInt32 i = 0; i < cell->refData.maxSize; i++)
                {
                    if (!cell->refData.refArray[i].unk08 || !cell->refData.refArray[i].ref)
                        continue;

                    TESObjectREFR* ref = cell->refData.refArray[i].ref;
                    if (!ShouldRecoverNearPlayerOnLoad(ref, playerPos, maxDistSq))
                        continue;

                    TryRecoverDropRef(processedRefIDs, recoveredCount, player, ref);
                }
            }

            if (recoveredCount > 0)
            {
                _MESSAGE("[FalseEdgeVR] Recovered %u grabbed weapon(s) to inventory after load", recoveredCount);
            }

            RestoreAllPendingFavorites(player);
        }

        class RecoverGrabbedWeaponsOnLoadTask : public TaskDelegate
        {
        public:
            RecoverGrabbedWeaponsOnLoadTask(int framesLeft, bool isFinalPass)
                : m_framesLeft(framesLeft), m_isFinalPass(isFinalPass)
            {
            }

            virtual void Run() override
            {
                if (m_framesLeft > 0 && g_task)
                {
                    g_task->AddTask(new RecoverGrabbedWeaponsOnLoadTask(m_framesLeft - 1, m_isFinalPass));
                    return;
                }

                RecoverGrabbedWeaponsAfterLoad();

                if (m_isFinalPass)
                {
                    s_capturedDroppedWeapons = {};
                    s_savedDroppedWeapons = {};
                }
            }

            virtual void Dispose() override
            {
                delete this;
            }

        private:
            int m_framesLeft;
            bool m_isFinalPass;
        };

        void SaveDroppedWeaponsCallback(SKSESerializationInterface* intfc)
        {
            if (!intfc)
                return;

            EquipManager* mgr = EquipManager::GetSingleton();
            DroppedWeaponsSaveData data;
            data.refIDLeft = mgr->GetDroppedWeaponRefID(true);
            data.baseIDLeft = mgr->GetDroppedWeaponBaseID(true);
            data.refIDRight = mgr->GetDroppedWeaponRefID(false);
            data.baseIDRight = mgr->GetDroppedWeaponBaseID(false);
            data.favoritedBaseIDLeft = mgr->WasDroppedWeaponFavorited(true) ? data.baseIDLeft : 0;
            data.favoritedBaseIDRight = mgr->WasDroppedWeaponFavorited(false) ? data.baseIDRight : 0;

            if (data.favoritedBaseIDLeft == 0 && data.refIDLeft != 0)
            {
                TESForm* form = LookupFormByID(data.refIDLeft);
                TESObjectREFR* ref = form ? DYNAMIC_CAST(form, TESForm, TESObjectREFR) : nullptr;
                if (WorldRefHasHotkey(ref))
                    data.favoritedBaseIDLeft = data.baseIDLeft;
            }

            if (data.favoritedBaseIDRight == 0 && data.refIDRight != 0)
            {
                TESForm* form = LookupFormByID(data.refIDRight);
                TESObjectREFR* ref = form ? DYNAMIC_CAST(form, TESForm, TESObjectREFR) : nullptr;
                if (WorldRefHasHotkey(ref))
                    data.favoritedBaseIDRight = data.baseIDRight;
            }

            intfc->WriteRecord(kDroppedWeaponsSaveRecord, kDroppedWeaponsSaveVersion, &data, sizeof(data));
        }

        static DroppedWeaponsSaveData NormalizeLoadedSaveData(const DroppedWeaponsSaveDataV1& legacyData)
        {
            DroppedWeaponsSaveData data;
            data.refIDLeft = legacyData.refIDLeft;
            data.baseIDLeft = legacyData.baseIDLeft;
            data.refIDRight = legacyData.refIDRight;
            data.baseIDRight = legacyData.baseIDRight;
            return data;
        }

        static DroppedWeaponsSaveData NormalizeLoadedSaveData(const DroppedWeaponsSaveDataV2& legacyData)
        {
            DroppedWeaponsSaveData data;
            data.refIDLeft = legacyData.refIDLeft;
            data.baseIDLeft = legacyData.baseIDLeft;
            data.refIDRight = legacyData.refIDRight;
            data.baseIDRight = legacyData.baseIDRight;
            if (legacyData.wasFavoritedLeft)
                data.favoritedBaseIDLeft = legacyData.baseIDLeft;
            if (legacyData.wasFavoritedRight)
                data.favoritedBaseIDRight = legacyData.baseIDRight;
            return data;
        }

        void LoadDroppedWeaponsCallback(SKSESerializationInterface* intfc)
        {
            if (!intfc)
                return;

            UInt32 type = 0;
            UInt32 version = 0;
            UInt32 length = 0;

            while (intfc->GetNextRecordInfo(&type, &version, &length))
            {
                if (type != kDroppedWeaponsSaveRecord)
                {
                    intfc->ReadRecordData(nullptr, length);
                    continue;
                }

                DroppedWeaponsSaveData data;
                if (length == sizeof(DroppedWeaponsSaveDataV1))
                {
                    DroppedWeaponsSaveDataV1 legacyData;
                    if (intfc->ReadRecordData(&legacyData, sizeof(legacyData)) != sizeof(legacyData))
                        continue;

                    data = NormalizeLoadedSaveData(legacyData);
                }
                else if (length == sizeof(DroppedWeaponsSaveDataV2))
                {
                    DroppedWeaponsSaveDataV2 legacyData;
                    if (intfc->ReadRecordData(&legacyData, sizeof(legacyData)) != sizeof(legacyData))
                        continue;

                    data = NormalizeLoadedSaveData(legacyData);
                }
                else if (length == sizeof(data))
                {
                    if (intfc->ReadRecordData(&data, sizeof(data)) != sizeof(data))
                        continue;
                }
                else
                {
                    intfc->ReadRecordData(nullptr, length);
                    continue;
                }

                intfc->ResolveFormId(data.baseIDLeft, &data.baseIDLeft);
                intfc->ResolveFormId(data.baseIDRight, &data.baseIDRight);
                intfc->ResolveFormId(data.favoritedBaseIDLeft, &data.favoritedBaseIDLeft);
                intfc->ResolveFormId(data.favoritedBaseIDRight, &data.favoritedBaseIDRight);

                s_savedDroppedWeapons = data;
            }
        }

        void RevertDroppedWeaponsCallback(SKSESerializationInterface*)
        {
            s_savedDroppedWeapons = {};
            s_capturedDroppedWeapons = {};
        }
    }

    void EquipManager::CaptureDroppedWeaponsForLoadRecovery()
    {
        s_capturedDroppedWeapons.refIDLeft = GetDroppedWeaponRefID(true);
        s_capturedDroppedWeapons.baseIDLeft = GetDroppedWeaponBaseID(true);
        s_capturedDroppedWeapons.refIDRight = GetDroppedWeaponRefID(false);
        s_capturedDroppedWeapons.baseIDRight = GetDroppedWeaponBaseID(false);
        s_capturedDroppedWeapons.favoritedBaseIDLeft =
            WasDroppedWeaponFavorited(true) ? s_capturedDroppedWeapons.baseIDLeft : 0;
        s_capturedDroppedWeapons.favoritedBaseIDRight =
            WasDroppedWeaponFavorited(false) ? s_capturedDroppedWeapons.baseIDRight : 0;
    }

    bool EquipManager::WasDroppedWeaponFavorited(bool isLeftGameHand) const
    {
        UInt32 baseID = GetDroppedWeaponBaseID(isLeftGameHand);
        if (baseID == 0)
            return false;

        if (isLeftGameHand)
            return m_wasFavoritedLeft || m_preservedFavoriteFormIDLeft == baseID;

        return m_wasFavoritedRight || m_preservedFavoriteFormIDRight == baseID;
    }

    void EquipManager::RecoverGrabbedWeaponsOnLoad()
    {
        if (!g_task)
            return;

        static const int kRecoveryDelays[] = { 90, 300, 600, 900 };
        for (size_t i = 0; i < sizeof(kRecoveryDelays) / sizeof(kRecoveryDelays[0]); ++i)
        {
            bool isFinalPass = (i + 1) == (sizeof(kRecoveryDelays) / sizeof(kRecoveryDelays[0]));
            g_task->AddTask(new RecoverGrabbedWeaponsOnLoadTask(kRecoveryDelays[i], isFinalPass));
        }
    }

    void EquipManager::RegisterSerialization(SKSESerializationInterface* intfc)
    {
        if (!intfc)
            return;

        intfc->SetUniqueID(g_pluginHandle, 'FEVR');
        intfc->SetRevertCallback(g_pluginHandle, RevertDroppedWeaponsCallback);
        intfc->SetSaveCallback(g_pluginHandle, SaveDroppedWeaponsCallback);
        intfc->SetLoadCallback(g_pluginHandle, LoadDroppedWeaponsCallback);
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
        }
  else
        {
        m_cachedWeaponFormIDRight = 0;
     }
    }

  bool EquipManager::WasDualWieldingSameWeapon(bool isLeftHand) const
    {
        return isLeftHand ? m_wasDualWieldingSameWeaponLeft : m_wasDualWieldingSameWeaponRight;
    }

    void EquipManager::CheckPendingAutoUnequip()
    {
        // Process both hands independently so a pending unequip on one hand can
        // never be clobbered or mis-gated by the other hand's input.
        ProcessPendingAutoUnequip(true);
        ProcessPendingAutoUnequip(false);
    }

    void EquipManager::ProcessPendingAutoUnequip(bool isLeftHand)
    {
        TESForm*& pendingForm = isLeftHand ? m_pendingAutoUnequipLeftForm : m_pendingAutoUnequipRightForm;
        if (!pendingForm)
            return;

        TESForm* weaponForm = pendingForm;

        // Clear the pending slot immediately
        pendingForm = nullptr;


        // Double-check conditions are still valid
        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return;

        // Check if the weapon is still equipped
        TESForm* currentlyEquipped = player->GetEquippedObject(isLeftHand);
        if (!currentlyEquipped || currentlyEquipped->formID != weaponForm->formID)
        {
            return;
        }

        // Gate on the trigger/lock of the hand actually being unequipped - NOT the
        // collision-avoidance off-hand. Using the off-hand here caused the left
        // weapon to drop while the right was wrongly protected (and vice-versa for
        // weapon lock) when both triggers were used in quick succession.
        bool vrControllerIsLeft = GameHandToVRController(isLeftHand);
        bool triggerHeld = vrControllerIsLeft ?
            VRInputHandler::IsLeftTriggerPressed() : VRInputHandler::IsRightTriggerPressed();

        if (triggerHeld)
        {
            return;
        }

        // Check if weapon is locked (trigger spam = 4x to lock)
        bool weaponLocked = VRInputHandler::IsWeaponLocked(vrControllerIsLeft);
        if (weaponLocked)
        {
            return;
        }

        // Perform the unequip and HIGGS grab
        if (IsWeaponGrabToHolsterBlocked())
            return;

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
        
     // Play the weapon pickup sound from Fake Edge VR.esp (ESL-flagged)
     // BUT skip if this is from our internal re-equip logic (SafeActivate)
    if (EquipManager::s_suppressPickupSound)
     {
          return kEvent_Continue;
     }
        
     // Skip pickup sound for excluded items (see FalseEdgeVR.ini [WeaponExclusions])
   if (IsExcludedWeaponFormID(evn->itemFormId))
        {
   return kEvent_Continue;
 }
        
  // Base FormID is 0x800, plugin name is "Fake Edge VR.esp"
   static UInt32 cachedSoundFormId = 0;
   if (cachedSoundFormId == 0)
   {
          cachedSoundFormId = GetFullFormIdFromEspAndFormId("Fake Edge VR.esp", 0x800);
 if (cachedSoundFormId != 0)
  {
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
        
        auto* eventDispatcher = GetEventDispatcherList();
        if (eventDispatcher)
        {
            // Register equip event handler
          eventDispatcher->unk4D0.AddEventSink(EquipEventHandler::GetSingleton());
            
  // Register container change event handler
   eventDispatcher->unk370.AddEventSink(ContainerChangeEventHandler::GetSingleton());
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
 }
    }
}
