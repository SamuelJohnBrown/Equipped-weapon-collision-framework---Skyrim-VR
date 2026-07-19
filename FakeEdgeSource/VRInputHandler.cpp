#include "VRInputHandler.h"
#include "Engine.h"
#include "WeaponGeometry.h"
#include "ActivateHook.h"
#include "skse64/GameReferences.h"
#include "skse64/GameMenus.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace FalseEdgeVR
{
    // Forward declaration for trigger polling
    void PollTriggerState();

    // ============================================
    // Per-frame menu safety check
    // Polling controller state while certain menus are open re-enters
    // SkyUI-VR's controller state hook / Scaleform and causes CTD
    // (e.g. blacksmith forge weapon creation menu).
    // The event-based PauseTracking() can be defeated by nested menus
    // closing while the crafting menu is still open, so we ALSO check
    // MenuManager directly every frame. This is authoritative.
    // ============================================
    bool IsAnyBlockingMenuOpen()
    {
        MenuManager* mm = MenuManager::GetSingleton();
        if (!mm)
            return false;

        static BSFixedString blockingMenus[] = {
            BSFixedString("Crafting Menu"),      // Smithing, Alchemy, Enchanting
            BSFixedString("RaceSex Menu"),       // Character creation
            BSFixedString("ContainerMenu"),      // Containers
            BSFixedString("BarterMenu"),         // Trading
            BSFixedString("GiftMenu"),           // Gift giving
            BSFixedString("Lockpicking Menu"),   // Lockpicking
            BSFixedString("Book Menu"),          // Reading books
            BSFixedString("Sleep/Wait Menu"),    // Sleep/Wait
            BSFixedString("Loading Menu"),       // Loading screen
            BSFixedString("Fader Menu"),         // Cell transition fade (often before Loading Menu)
            BSFixedString("Journal Menu"),       // Journal/Quest menu
            BSFixedString("MapMenu"),            // Map
            BSFixedString("InventoryMenu"),      // Inventory
            BSFixedString("MagicMenu"),          // Magic menu
            BSFixedString("FavoritesMenu"),      // Favorites
            BSFixedString("StatsMenu"),          // Stats/Perk menu
            BSFixedString("Training Menu"),      // Training menu
            BSFixedString("MessageBoxMenu"),     // Message boxes (item naming, confirmations)
            BSFixedString("Console"),            // Console
            BSFixedString("Dialogue Menu"),      // NPC dialogue
            BSFixedString("TweenMenu"),          // Tween (B-button) menu
            BSFixedString("MainMenu"),           // Main menu
        };

        for (BSFixedString& menuName : blockingMenus)
        {
            if (mm->IsMenuOpen(&menuName))
                return true;
        }

        return false;
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

    // Left VR trigger was engaged before right (order-sensitive dual-trigger path)
    static bool s_leftTriggerBeforeRight = false;
    static bool s_dualTriggerLeftRestoreIssued = false;

    // Trigger TOUCH state (finger on trigger but not pressing)
    static bool s_leftTriggerTouched = false;
    static bool s_rightTriggerTouched = false;
    static bool s_leftTriggerWasTouched = false;
    static bool s_rightTriggerWasTouched = false;

    // Grip state

    // Grip state
    static bool s_leftGripPressed = false;
    static bool s_rightGripPressed = false;
    static bool s_leftGripWasPressed = false;
    static bool s_rightGripWasPressed = false;

    // Temporary collision -> equipped transition driven by the support hand's
    // grip.  Fake Edge keeps the original game hand authoritative throughout;
    // the opposite controller is only the support hand and never becomes the
    // owner of the weapon.
    static constexpr float kOppositeGripMaxControllerDistance = 42.0f;
    struct OppositeGrip2HTransitionState
    {
        bool active = false;
        bool ownerGameHandIsLeft = false;
        bool supportVRControllerIsLeft = false;
        bool equipQueued = false;
        bool equippedObserved = false;
        bool gripObservedHeld = false;
        bool higgsGripSettingSuppressed = false;
        bool higgsGripRearmSucceeded = false;
        bool higgsGripRearmFailureLogged = false;
        bool higgsSupportHandDisabled = false;
        bool twoHandGrabValidated = false;
        bool ownerGripWasHeld = false;
        bool nativeWeaponSoundsMuted = false;
        int recoveryRetryCount = 0;
        int wrongHandCorrectionCount = 0;
        int higgsGripRearmAttemptCount = 0;
        UInt32 weaponFormID = 0;
        UInt32 collisionRefID = 0;
        float equipWaitTimer = 0.0f;
        float equippedSettleTimer = 0.0f;
        float higgsGripRearmTimer = 0.0f;
        float higgsGripLastAttemptTime = 0.0f;
        float armGraceTimer = 0.0f;
        float validationGraceTimer = 0.0f;
        float recoveryRetryTimer = 0.0f;
        float wrongHandCorrectionTimer = 0.0f;
        float supportLostTimer = 0.0f;
        double savedHiggsEnableGrip = 1.0;
        BGSSoundDescriptorForm* savedNativePickUpSound = nullptr;
        BGSSoundDescriptorForm* savedNativePutDownSound = nullptr;
    };

    static OppositeGrip2HTransitionState s_oppositeGrip2H;

    // HIGGS completes GrabObject on a later physics update.  When the support
    // grip is released, that delayed grab belongs to the newly recreated
    // collision object and can occur just after the transition state resets.
    // Keep only HIGGS physics audio muted through that short completion tail.
    static constexpr ULONGLONG kOppositeGripReleaseSoundTailMs = 1000;
    static std::atomic<ULONGLONG> s_oppositeGripReleaseSoundSuppressUntilMs{ 0 };

    static bool TryGetVRControllerSeparation(
        bool firstVRControllerIsLeft, bool secondVRControllerIsLeft,
        float& outDistance)
    {
        outDistance = 9999.0f;

        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return false;

        NiNode* rootNode = player->GetNiNode();
        if (!rootNode)
            rootNode = player->GetNiRootNode(0);
        if (!rootNode)
            rootNode = player->GetNiRootNode(1);
        if (!rootNode)
            return false;

        BSFixedString leftHandName("NPC L Hand [LHnd]");
        BSFixedString rightHandName("NPC R Hand [RHnd]");
        NiAVObject* leftHandNode = rootNode->GetObjectByName(&leftHandName.data);
        NiAVObject* rightHandNode = rootNode->GetObjectByName(&rightHandName.data);
        if (!leftHandNode || !rightHandNode)
            return false;

        NiAVObject* firstNode = firstVRControllerIsLeft ? leftHandNode : rightHandNode;
        NiAVObject* secondNode = secondVRControllerIsLeft ? leftHandNode : rightHandNode;
        const NiPoint3& firstPos = firstNode->m_worldTransform.pos;
        const NiPoint3& secondPos = secondNode->m_worldTransform.pos;
        const float dx = firstPos.x - secondPos.x;
        const float dy = firstPos.y - secondPos.y;
        const float dz = firstPos.z - secondPos.z;
        outDistance = sqrt(dx * dx + dy * dy + dz * dz);
        return true;
    }

    static bool BothHiggsHandsHoldTransitionReference(
        const OppositeGrip2HTransitionState& state)
    {
        if (!higgsInterface || state.collisionRefID == 0)
            return false;

        const bool ownerVRControllerIsLeft =
            GameHandToVRController(state.ownerGameHandIsLeft);
        TESObjectREFR* ownerHeld =
            higgsInterface->GetGrabbedObject(ownerVRControllerIsLeft);
        TESObjectREFR* supportHeld =
            higgsInterface->GetGrabbedObject(state.supportVRControllerIsLeft);

        if (!ownerHeld || !supportHeld)
            return false;

        return ownerHeld == supportHeld &&
            ownerHeld->formID == state.collisionRefID &&
            ownerHeld->baseForm &&
            ownerHeld->baseForm->formID == state.weaponFormID;
    }

    static void ResetOppositeGrip2HTransition()
    {
        // Never leave HIGGS grip handling disabled if the transition is
        // interrupted by a menu, load, death, or external equipment change.
        if (s_oppositeGrip2H.higgsGripSettingSuppressed && higgsInterface)
        {
            higgsInterface->SetSettingDouble(
                "EnableGrip", s_oppositeGrip2H.savedHiggsEnableGrip);
        }
        if (s_oppositeGrip2H.higgsSupportHandDisabled && higgsInterface)
        {
            higgsInterface->EnableHand(
                s_oppositeGrip2H.supportVRControllerIsLeft);
        }
        if (s_oppositeGrip2H.nativeWeaponSoundsMuted &&
            s_oppositeGrip2H.weaponFormID != 0)
        {
            TESForm* form = LookupFormByID(s_oppositeGrip2H.weaponFormID);
            TESObjectWEAP* weapon = form ?
                DYNAMIC_CAST(form, TESForm, TESObjectWEAP) : nullptr;
            if (weapon)
            {
                weapon->pickupSounds.pickUp =
                    s_oppositeGrip2H.savedNativePickUpSound;
                weapon->pickupSounds.putDown =
                    s_oppositeGrip2H.savedNativePutDownSound;
            }
        }
        s_oppositeGrip2H = OppositeGrip2HTransitionState();
    }

    // ============================================
    // Drop Protection Override (Grip Spam Detection)
    // Configurable via INI: [IntentionalDrop] section
    // This allows the player to intentionally drop a weapon
    // Works regardless of whether a weapon is currently held
    // ============================================

    // Grip spam detection - LEFT VR controller
    static int s_leftGripPressCount = 0;
    static float s_leftGripSpamWindowTimer = 0.0f;
    static bool s_leftDropProtectionDisabled = false;
    static float s_leftDropProtectionDisableTimer = 0.0f;

    // Grip spam detection - RIGHT VR controller
    static int s_rightGripPressCount = 0;
    static float s_rightGripSpamWindowTimer = 0.0f;
    static bool s_rightDropProtectionDisabled = false;
    static float s_rightDropProtectionDisableTimer = 0.0f;

    // Grip spam thresholds - now configurable via INI (config.h):
    // gripSpamThreshold, gripSpamWindow, dropProtectionDisableTime

    // ============================================
    // Weapon Lock (Trigger Spam Detection)
    // If trigger is pressed 4 times within 2 seconds while holding a weapon,
    // lock it to equipped state (won't unequip on trigger release)
    // Press trigger 4 times again to unlock and return to normal grab behavior
    // ============================================

    // Trigger spam detection - LEFT VR controller
    static int s_leftTriggerPressCount = 0;
    static float s_leftTriggerSpamWindowTimer = 0.0f;
    static bool s_leftWeaponLocked = false;  // true = weapon locked to equipped state

    // Trigger spam detection - RIGHT VR controller
    static int s_rightTriggerPressCount = 0;
    static float s_rightTriggerSpamWindowTimer = 0.0f;
    static bool s_rightWeaponLocked = false;  // true = weapon locked to equipped state

    // Time (seconds) each GAME hand has been continuously unequipped. Used to
    // reset the weapon lock after a true unequip lasting >= 0.3s.
    static float s_leftGameHandUnequipTimer = 0.0f;
    static float s_rightGameHandUnequipTimer = 0.0f;
    static const float WEAPON_LOCK_RESET_UNEQUIP_TIME = 0.3f;

    // Tap-then-hold equip gesture (while a weapon is grabbed in that hand):
    //   tap (press + release), then an immediate press-and-hold equips the
    //   weapon for the duration of the hold.
    // State: 0 = idle, 1 = tap pressed, 2 = tap released (awaiting hold press)
    static int s_grabEquipTapStateLeft = 0;
    static int s_grabEquipTapStateRight = 0;
    static float s_grabEquipTapTimerLeft = 0.0f;
    static float s_grabEquipTapTimerRight = 0.0f;
    static const float GRAB_EQUIP_TAP_WINDOW = 0.4f;  // max gap between tap release and hold press

    // Weapon lock thresholds - now configurable via INI [WeaponLock] section:
 // triggerSpamThreshold, triggerSpamWindow (defined in config.h/config.cpp)

    // ============================================
    // FormID auto-equip pickup settle (after SafeActivate before EquipItem)
    // ============================================

    static const float AUTO_EQUIP_PICKUP_SETTLE_TIME = 0.025f;

    // Pending unequip state (for delayed trigger release unequip).
    // Tracked PER HAND so both hands can have an independent pending unequip in
    // flight at once - a single shared slot let one hand's release clobber or
    // mis-cancel the other's when both triggers were used in quick succession.
    static bool s_pendingTriggerUnequipLeft = false;
    static bool s_pendingTriggerUnequipRight = false;
    static float s_triggerUnequipTimerLeft = 0.0f;
    static float s_triggerUnequipTimerRight = 0.0f;

    static constexpr float DOOR_TRANSITION_GUARD_SECONDS = 2.0f;
    static float s_doorTransitionGuardTimer = 0.0f;
    static bool s_pendingPostDoorGrabResolve = false;

    static bool IsDroppedWeaponRefReadable(TESObjectREFR* droppedWeapon)
    {
        if (!droppedWeapon)
            return false;

        __try
        {
            return droppedWeapon->baseForm != nullptr &&
                droppedWeapon->formType == kFormType_Reference;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool IsTwoHandedMeleeBaseForm(TESForm* form)
    {
        if (!form || form->formType != kFormType_Weapon)
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
            return true;
        default:
            return false;
        }
    }

    static bool IsGrabEquipBlockedByOppositeHand2HTriggerHold(bool isLeftGameHand)
    {
        if (!twoHandedTrackingEnabled)
            return false;

        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return false;

        const bool oppositeGameHand = !isLeftGameHand;
        TESForm* oppositeEquipped = player->GetEquippedObject(oppositeGameHand);
        if (!IsTwoHandedMeleeBaseForm(oppositeEquipped))
            return false;

        const bool oppositeVRControllerIsLeft = GameHandToVRController(oppositeGameHand);
        const bool oppositeTriggerHeld = oppositeVRControllerIsLeft ?
            s_leftTriggerPressed : s_rightTriggerPressed;

        return oppositeTriggerHeld;
    }

    // While the off (left) hand has a weapon using the Two-Handed skill record equipped,
    // the main (right) hand cannot grab-to-equip: holding trigger on the main-hand
    // grabbed weapon will not equip it; it stays grabbed.
    static bool IsMainHandGrabEquipBlockedByOffHand2HSkill(bool isLeftGameHand)
    {
        if (!twoHandedTrackingEnabled)
            return false;

        // Only the main (right) hand is blocked; the off hand is the left game hand.
        if (isLeftGameHand)
            return false;

        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return false;

        TESForm* offHandEquipped = player->GetEquippedObject(true);
        return EquipManager::UsesTwoHandedSkill(offHandEquipped);
    }

    // While the main (right) hand has a 2H-skill weapon equip-LOCKED, the off (left) hand
    // cannot trigger-equip ANY grabbed weapon (1H or 2H): holding trigger on the off-hand
    // grabbed weapon will not equip it; it stays grabbed until the main-hand lock is released.
    static bool IsOffHandGrabEquipBlockedByLockedMainHand2H(bool isLeftGameHand)
    {
        if (!twoHandedTrackingEnabled)
            return false;

        // Restriction applies to the off (left) game hand only.
        if (!isLeftGameHand)
            return false;

        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return false;

        // Main (right) hand must have a 2H-skill weapon equipped AND be equip-locked.
        TESForm* mainEquipped = player->GetEquippedObject(false);
        if (!EquipManager::UsesTwoHandedSkill(mainEquipped))
            return false;

        const bool mainVRControllerIsLeft = GameHandToVRController(false);
        return VRInputHandler::IsWeaponLocked(mainVRControllerIsLeft);
    }

    // Runs on the main game thread: resolve the form by ID and log it. GetName() is not
    // safe to call from the HIGGS physics/job thread, so the lock logging is deferred here.
    class LogWeaponLock2HSkillTask : public TaskDelegate
    {
    public:
        UInt32 m_formID;
        bool m_isLeftVRController;
        bool m_gameHandIsLeft;

        LogWeaponLock2HSkillTask(UInt32 formID, bool isLeftVRController, bool gameHandIsLeft)
            : m_formID(formID), m_isLeftVRController(isLeftVRController), m_gameHandIsLeft(gameHandIsLeft) {}

        virtual void Run() override
        {
            TESForm* form = LookupFormByID(m_formID);
            const char* weaponName = form ? form->GetName() : nullptr;
            _MESSAGE("[FalseEdgeVR] Weapon lock engaged on %s controller (%s game hand) for Two-Handed-skill weapon: %s (0x%08X)",
                m_isLeftVRController ? "LEFT" : "RIGHT",
                m_gameHandIsLeft ? "LEFT" : "RIGHT",
                weaponName ? weaponName : "(unnamed)",
                m_formID);
        }

        virtual void Dispose() override { delete this; }
    };

    // Log (when the 4x-trigger weapon lock engages) if the weapon equipped on the hand
    // mapped to this VR controller uses the Two-Handed skill record. The actual logging
    // (which needs GetName) is deferred to the main thread; this runs on the physics thread.
    static void LogWeaponLockIf2HSkill(bool isLeftVRController)
    {
        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return;

        // Map the VR controller back to its game hand.
        const bool gameHandIsLeft = (GameHandToVRController(true) == isLeftVRController);

        TESForm* equipped = player->GetEquippedObject(gameHandIsLeft);
        if (!EquipManager::UsesTwoHandedSkill(equipped))
            return;

        if (g_task)
            g_task->AddTask(new LogWeaponLock2HSkillTask(equipped->formID, isLeftVRController, gameHandIsLeft));
    }

    // Only one Two-Handed-skill weapon may be locked at a time. Returns true if engaging
    // a lock on this controller should be blocked because THIS hand holds a 2H-skill
    // weapon while the OPPOSITE hand already has a locked 2H-skill weapon.
    static bool IsTwoHandedSkillLockBlockedByOppositeHand(bool isLeftVRController)
    {
        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return false;

        const bool gameHandIsLeft = (GameHandToVRController(true) == isLeftVRController);

        // Restriction only applies when the weapon being locked uses the 2H skill.
        TESForm* thisEquipped = player->GetEquippedObject(gameHandIsLeft);
        if (!EquipManager::UsesTwoHandedSkill(thisEquipped))
            return false;

        // The opposite hand must also hold a 2H-skill weapon...
        TESForm* oppositeEquipped = player->GetEquippedObject(!gameHandIsLeft);
        if (!EquipManager::UsesTwoHandedSkill(oppositeEquipped))
            return false;

        // ...and that opposite controller's lock must currently be engaged.
        const bool oppositeLocked = isLeftVRController ? s_rightWeaponLocked : s_leftWeaponLocked;
        return oppositeLocked;
    }

    static TESForm* ResolveTwoHandedMeleeFormFromPlayer()
    {
        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return nullptr;

        TESForm* equippedRight = player->GetEquippedObject(false);
        if (IsTwoHandedMeleeBaseForm(equippedRight))
            return equippedRight;

        TESForm* equippedLeft = player->GetEquippedObject(true);
        if (IsTwoHandedMeleeBaseForm(equippedLeft))
            return equippedLeft;

        return nullptr;
    }

    static TESForm* ResolveDualHand2HWeaponForm(
        TESObjectREFR*& outLeftGrabbed,
        TESObjectREFR*& outRightGrabbed)
    {
        outLeftGrabbed = nullptr;
        outRightGrabbed = nullptr;

        if (!higgsInterface)
            return nullptr;

        outLeftGrabbed = higgsInterface->GetGrabbedObject(true);
        outRightGrabbed = higgsInterface->GetGrabbedObject(false);

        if (outLeftGrabbed && outLeftGrabbed->baseForm && IsTwoHandedMeleeBaseForm(outLeftGrabbed->baseForm))
            return outLeftGrabbed->baseForm;

        if (outRightGrabbed && outRightGrabbed->baseForm && IsTwoHandedMeleeBaseForm(outRightGrabbed->baseForm))
            return outRightGrabbed->baseForm;

        EquipManager* equipMgr = EquipManager::GetSingleton();
        for (int handIdx = 0; handIdx < 2; ++handIdx)
        {
            const bool isLeftGameHand = (handIdx == 0);
            TESObjectREFR* dropped = equipMgr->GetDroppedWeaponRef(isLeftGameHand);
            if (dropped && dropped->baseForm && IsTwoHandedMeleeBaseForm(dropped->baseForm))
            {
                if (isLeftGameHand)
                    outLeftGrabbed = outLeftGrabbed ? outLeftGrabbed : dropped;
                else
                    outRightGrabbed = outRightGrabbed ? outRightGrabbed : dropped;
                return dropped->baseForm;
            }
        }

        if (higgsInterface->IsTwoHanding())
            return ResolveTwoHandedMeleeFormFromPlayer();

        return nullptr;
    }

    static void TryLogDualHand2HWeaponGrab()
    {
        static bool s_wasDualHand2HGrab = false;

        if (!higgsInterface)
        {
            s_wasDualHand2HGrab = false;
            return;
        }

        const bool leftHolding = higgsInterface->IsHoldingObject(true);
        const bool rightHolding = higgsInterface->IsHoldingObject(false);
        if (!leftHolding || !rightHolding)
        {
            s_wasDualHand2HGrab = false;
            return;
        }

        TESObjectREFR* leftGrabbed = nullptr;
        TESObjectREFR* rightGrabbed = nullptr;
        TESForm* weaponForm = ResolveDualHand2HWeaponForm(leftGrabbed, rightGrabbed);
        if (!weaponForm)
        {
            s_wasDualHand2HGrab = false;
            return;
        }

        const bool twoHanding = higgsInterface->IsTwoHanding();
        const bool sameGrabbedRef = leftGrabbed && rightGrabbed &&
            (leftGrabbed == rightGrabbed ||
                (leftGrabbed->baseForm && leftGrabbed->baseForm == rightGrabbed->baseForm));

        const bool isDualHand2H = twoHanding || sameGrabbedRef ||
            (leftHolding && rightHolding && weaponForm != nullptr);

        if (isDualHand2H && !s_wasDualHand2HGrab)
        {
            // NOTE: runs on the HIGGS physics thread — do NOT call GetName() here (it can
            // crash inside the game's form code). Log the formID only.
            _MESSAGE("[FalseEdgeVR] 2H weapon grabbed in both hands: 0x%08X twoHanding=%d leftRef=0x%08X rightRef=0x%08X leftHold=%d rightHold=%d",
                weaponForm->formID,
                twoHanding ? 1 : 0,
                leftGrabbed ? leftGrabbed->formID : 0u,
                rightGrabbed ? rightGrabbed->formID : 0u,
                leftHolding ? 1 : 0,
                rightHolding ? 1 : 0);
        }

        s_wasDualHand2HGrab = isDualHand2H;
    }

    // Resolve the weapon ref currently grabbed in a game hand (HIGGS grab first, then
    // the tracked dropped ref as a fallback).
    static TESObjectREFR* ResolveGrabbedWeaponRefForGameHand(bool isLeftGameHand)
    {
        TESObjectREFR* ref = nullptr;
        if (higgsInterface)
        {
            const bool vrControllerIsLeft = GameHandToVRController(isLeftGameHand);
            ref = higgsInterface->GetGrabbedObject(vrControllerIsLeft);
        }
        if (!ref)
            ref = EquipManager::GetSingleton()->GetDroppedWeaponRef(isLeftGameHand);
        return ref;
    }

    // Find the nearest door in the player's cell that is within maxDist and that the
    // player is roughly facing (forward/door-direction dot >= minFacingDot).
    static TESObjectREFR* FindFacedDoorInFront(PlayerCharacter* player, float maxDist, float minFacingDot)
    {
        if (!player)
            return nullptr;

        TESObjectCELL* cell = player->parentCell;
        if (!cell || !cell->refData.refArray)
            return nullptr;

        const NiPoint3 ppos = player->pos;
        const float yaw = player->rot.z;
        const float fx = sinf(yaw);   // Skyrim forward from yaw: (sin, cos)
        const float fy = cosf(yaw);
        const float maxDistSq = maxDist * maxDist;

        TESObjectREFR* best = nullptr;
        float bestDistSq = maxDistSq;

        for (UInt32 i = 0; i < cell->refData.maxSize; i++)
        {
            if (!cell->refData.refArray[i].unk08 || !cell->refData.refArray[i].ref)
                continue;

            TESObjectREFR* ref = cell->refData.refArray[i].ref;
            if (!ref->baseForm || ref->baseForm->formType != kFormType_Door)
                continue;

            const float dx = ref->pos.x - ppos.x;
            const float dy = ref->pos.y - ppos.y;
            const float distSq = dx * dx + dy * dy;
            if (distSq > maxDistSq || distSq < 1.0f)
                continue;

            const float dist = sqrtf(distSq);
            const float dot = (fx * dx + fy * dy) / dist;
            if (dot < minFacingDot)
                continue;  // door is not in front / player not facing it

            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                best = ref;
            }
        }

        return best;
    }

    // Door-stow state for the dual-2H-grab-facing-door scenario.
    static bool s_doorStowActive = false;
    static UInt32 s_doorStowLeftFormID = 0;
    static UInt32 s_doorStowRightFormID = 0;

    // Facing thresholds. Entry is stricter than the "stay stowed" exit check to provide
    // hysteresis and avoid rapid stow/re-equip toggling at the boundary. The proximity
    // (distance) is tunable via the INI; the stay-distance adds a fixed hysteresis margin.
    static const float kDoorEnterDot = 0.64f;      // ~50 degrees
    static const float kDoorStayDot = 0.50f;       // ~60 degrees
    static const float kDoorStayDistMargin = 84.0f; // extra units kept stowed before re-equip

    // When the player is grabbing a distinct 2H melee weapon in EACH hand while facing a
    // door, safe-stow both weapons to inventory (no spawn, no duplicate) and cache their
    // base forms. While they keep facing the door they stay stowed. Once they turn away,
    // re-equip the cached weapons — the normal equip flow auto-grabs them again.
    static void UpdateDoorStowForDual2HGrab()
    {
        if (!twoHandedTrackingEnabled || !doorStowDual2HEnabled)
            return;

        PlayerCharacter* player = *g_thePlayer;
        if (!player || !higgsInterface)
            return;

        const float enterDist = doorStowDual2HProximity;
        const float stayDist = doorStowDual2HProximity + kDoorStayDistMargin;

        EquipManager* mgr = EquipManager::GetSingleton();

        if (!s_doorStowActive)
        {
            TESObjectREFR* left = ResolveGrabbedWeaponRefForGameHand(true);
            TESObjectREFR* right = ResolveGrabbedWeaponRefForGameHand(false);

            const bool dual2H = left && right && left != right &&
                left->baseForm && right->baseForm &&
                IsTwoHandedMeleeBaseForm(left->baseForm) &&
                IsTwoHandedMeleeBaseForm(right->baseForm);
            if (!dual2H)
                return;

            TESObjectREFR* door = FindFacedDoorInFront(player, enterDist, kDoorEnterDot);
            if (!door)
                return;

            // Cache base forms BEFORE stowing so we can re-equip later.
            s_doorStowLeftFormID = left->baseForm->formID;
            s_doorStowRightFormID = right->baseForm->formID;

            // NOTE: runs on the HIGGS physics thread — do NOT call GetName() here (it can
            // crash inside the game's form code). Log formIDs only.
            _MESSAGE("[FalseEdgeVR] Dual 2H grabbed (one per hand) while facing door: left=0x%08X right=0x%08X door=0x%08X — stowing to inventory",
                s_doorStowLeftFormID, s_doorStowRightFormID, door->formID);

            // Return both grabbed weapons to inventory (AddItem + delete world copy, no
            // spawn) and clear all grab tracking.
            mgr->PickUpGrabbedWeaponBeforeEquip(true, true);
            mgr->PickUpGrabbedWeaponBeforeEquip(false, true);

            s_doorStowActive = true;
            return;
        }

        // Stowed: keep stowed while still facing a nearby door (lenient hysteresis).
        if (FindFacedDoorInFront(player, stayDist, kDoorStayDot))
            return;

        // No longer facing the door — re-equip the cached weapons. The normal equip flow
        // converts them back to the grabbed state via our existing functionality.
        _MESSAGE("[FalseEdgeVR] No longer facing door — re-equipping stowed 2H weapons: left=0x%08X right=0x%08X",
            s_doorStowLeftFormID, s_doorStowRightFormID);

        if (s_doorStowRightFormID != 0)
        {
            TESForm* rightForm = LookupFormByID(s_doorStowRightFormID);
            if (rightForm)
                mgr->EquipWeaponToGameHand(player, rightForm, false);
        }
        if (s_doorStowLeftFormID != 0)
        {
            TESForm* leftForm = LookupFormByID(s_doorStowLeftFormID);
            if (leftForm)
                mgr->EquipWeaponToGameHand(player, leftForm, true);
        }

        s_doorStowLeftFormID = 0;
        s_doorStowRightFormID = 0;
        s_doorStowActive = false;
    }

    static bool EquipGrabbedWeaponForGameHand(bool isLeftGameHand, bool forceEquip = false)
    {
        if (!forceEquip && IsGrabEquipBlockedByOppositeHand2HTriggerHold(isLeftGameHand))
            return false;

        if (!forceEquip && IsMainHandGrabEquipBlockedByOffHand2HSkill(isLeftGameHand))
            return false;

        if (!forceEquip && IsOffHandGrabEquipBlockedByLockedMainHand2H(isLeftGameHand))
            return false;

        EquipManager* equipMgr = EquipManager::GetSingleton();
        TESObjectREFR* droppedWeapon = equipMgr->GetDroppedWeaponRef(isLeftGameHand);
        if (!IsDroppedWeaponRefReadable(droppedWeapon))
        {
            if (droppedWeapon)
            {
                equipMgr->ClearDroppedWeaponRef(isLeftGameHand);
                equipMgr->ClearPendingReequip(isLeftGameHand);
            }
            return false;
        }

        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return false;

        // Opposite-grip conversion must not SafeActivate the shared HIGGS
        // world reference.  SafeActivate attributes the pickup to whichever
        // physical hand most recently grabbed it, which can equip the 2H
        // weapon in the support hand and orphan the original-hand tracking.
        // Recover the world copy to inventory first, then explicitly equip it
        // on the game thread to the authoritative owner hand.
        if (forceEquip && s_oppositeGrip2H.active)
        {
            TESForm* weaponForm = droppedWeapon->baseForm;
            if (!weaponForm || weaponForm->formID != s_oppositeGrip2H.weaponFormID)
                return false;

            if (higgsInterface && !s_oppositeGrip2H.higgsSupportHandDisabled)
            {
                higgsInterface->DisableHand(
                    s_oppositeGrip2H.supportVRControllerIsLeft);
                s_oppositeGrip2H.higgsSupportHandDisabled = true;
            }

            if (!equipMgr->PickUpGrabbedWeaponBeforeEquip(isLeftGameHand, false))
                return false;

            equipMgr->ScheduleEquipWeaponToGameHand(
                weaponForm->formID, isLeftGameHand);
            return true;
        }

        const bool wasDualWieldingSame = equipMgr->WasDualWieldingSameWeapon(isLeftGameHand);
        if (wasDualWieldingSame)
        {
            DeleteWorldObject(droppedWeapon);
        }
        else
        {
            EquipManager::s_suppressPickupSound = true;
            SafeActivate(droppedWeapon, player, 0, 0, 1, false);
            EquipManager::s_suppressPickupSound = false;
        }

        equipMgr->ClearDroppedWeaponRef(isLeftGameHand);
        equipMgr->ClearPendingReequip(isLeftGameHand);
        equipMgr->ScheduleForceReequip(isLeftGameHand);
        return true;
    }

    static void UpdateOppositeGrip2HTransition(float deltaTime)
    {
        OppositeGrip2HTransitionState& state = s_oppositeGrip2H;
        if (!state.active)
            return;

        const bool rawSupportGripHeld = state.supportVRControllerIsLeft ?
            s_leftGripPressed : s_rightGripPressed;
        const bool higgsSupportHold = higgsInterface &&
            higgsInterface->IsHoldingObject(state.supportVRControllerIsLeft);
        const bool supportGripHeld = rawSupportGripHeld || higgsSupportHold;

        const bool ownerVRControllerIsLeft =
            GameHandToVRController(state.ownerGameHandIsLeft);
        const bool rawOwnerGripHeld = ownerVRControllerIsLeft ?
            s_leftGripPressed : s_rightGripPressed;

        // Once the support grip has produced an active weapon, a new press of
        // the original/owner grip is reserved for Skyrim's current interaction
        // prompt.  The support grip remains authoritative for combat mode.
        if (state.equippedObserved && rawOwnerGripHeld && !state.ownerGripWasHeld)
        {
            TryActivateTrackedPrompt("opposite-grip owner hand");
        }
        state.ownerGripWasHeld = rawOwnerGripHeld;

        if (supportGripHeld)
        {
            state.gripObservedHeld = true;
            state.supportLostTimer = 0.0f;
        }

        // HIGGS normally requires a fresh grip rising edge to grab an equipped
        // weapon.  Because the player is deliberately continuing the same
        // physical hold across this conversion, briefly suppress HIGGS's own
        // EnableGrip setting and then restore it.  That creates an internal
        // falling/rising edge without consuming or changing the game's raw
        // controller input.
        if (state.higgsGripSettingSuppressed)
        {
            state.higgsGripRearmTimer += deltaTime;
            if (state.higgsGripRearmTimer >= 0.10f)
            {
                if (higgsInterface)
                {
                    higgsInterface->SetSettingDouble(
                        "EnableGrip", state.savedHiggsEnableGrip);
                }
                state.higgsGripSettingSuppressed = false;
                _MESSAGE(
                    "[FalseEdgeVR] Re-armed HIGGS support grip after 2H equip: "
                    "form=0x%08X support-controller=%s attempt=%d/3",
                    state.weaponFormID,
                    state.supportVRControllerIsLeft ? "LEFT" : "RIGHT",
                    state.higgsGripRearmAttemptCount);
            }
        }

        // Wait until the next input poll after HIGGS reports the second grab.
        // This lets HIGGS establish its physical support grip before the world
        // reference is activated and converted back into an equipped weapon.
        if (!state.equipQueued)
        {
            // The grabbed callback can precede both OpenVR's cached button
            // update and HIGGS's holding-state update by a physics tick.  Do
            // not interpret that short gap as a release.
            if (!supportGripHeld)
            {
                state.armGraceTimer += deltaTime;
                if (state.armGraceTimer < 0.25f)
                    return;

                _MESSAGE(
                    "[FalseEdgeVR] Opposite-grip 2H transition cancelled before equip: "
                    "form=0x%08X owner-game-hand=%s raw-grip=%d higgs-hold=%d",
                    state.weaponFormID,
                    state.ownerGameHandIsLeft ? "LEFT" : "RIGHT",
                    rawSupportGripHeld ? 1 : 0,
                    higgsSupportHold ? 1 : 0);
                ResetOppositeGrip2HTransition();
                return;
            }

            state.armGraceTimer = 0.0f;

            // A HIGGS grab callback alone is not sufficient: stale callbacks
            // can be emitted when the support controller is nowhere near the
            // weapon.  Before any inventory/world mutation, require both HIGGS
            // hands to resolve to this exact live reference and require the two
            // physical controller nodes to be close together.
            if (!state.twoHandGrabValidated)
            {
                const bool ownerVRControllerIsLeft =
                    GameHandToVRController(state.ownerGameHandIsLeft);
                float controllerDistance = 9999.0f;
                const bool haveControllerDistance = TryGetVRControllerSeparation(
                    ownerVRControllerIsLeft,
                    state.supportVRControllerIsLeft,
                    controllerDistance);
                const bool bothHandsHoldSameRef =
                    BothHiggsHandsHoldTransitionReference(state);

                if (!haveControllerDistance ||
                    controllerDistance > kOppositeGripMaxControllerDistance ||
                    !bothHandsHoldSameRef)
                {
                    state.validationGraceTimer += deltaTime;
                    if (state.validationGraceTimer < 0.50f)
                        return;

                    _MESSAGE(
                        "[FalseEdgeVR] Rejected unsafe 2H conversion before mutation: "
                        "form=0x%08X controller-distance=%.2f distance-valid=%d "
                        "both-hands-same-ref=%d",
                        state.weaponFormID,
                        controllerDistance,
                        haveControllerDistance ? 1 : 0,
                        bothHandsHoldSameRef ? 1 : 0);
                    ResetOppositeGrip2HTransition();
                    return;
                }

                state.twoHandGrabValidated = true;
                state.validationGraceTimer = 0.0f;
                _MESSAGE(
                    "[FalseEdgeVR] Validated close two-hand collision grip: "
                    "form=0x%08X controller-distance=%.2f",
                    state.weaponFormID, controllerDistance);
            }

            if (state.recoveryRetryTimer > 0.0f)
            {
                state.recoveryRetryTimer -= deltaTime;
                return;
            }

            if (!EquipGrabbedWeaponForGameHand(state.ownerGameHandIsLeft, true))
            {
                state.recoveryRetryCount++;
                if (state.recoveryRetryCount <= 10)
                {
                    state.recoveryRetryTimer = 0.03f;
                    _MESSAGE(
                        "[FalseEdgeVR] Opposite-grip recovery deferred; retrying: "
                        "form=0x%08X owner-game-hand=%s attempt=%d/10",
                        state.weaponFormID,
                        state.ownerGameHandIsLeft ? "LEFT" : "RIGHT",
                        state.recoveryRetryCount);
                    return;
                }

                _MESSAGE(
                    "[FalseEdgeVR] Opposite-grip 2H equip failed: form=0x%08X "
                    "owner-game-hand=%s",
                    state.weaponFormID,
                    state.ownerGameHandIsLeft ? "LEFT" : "RIGHT");
                ResetOppositeGrip2HTransition();
                return;
            }

            state.equipQueued = true;
            state.equipWaitTimer = 0.0f;
            _MESSAGE(
                "[FalseEdgeVR] Opposite grip activating 2H weapon in original hand: "
                "form=0x%08X owner-game-hand=%s support-controller=%s "
                "raw-grip=%d higgs-hold=%d",
                state.weaponFormID,
                state.ownerGameHandIsLeft ? "LEFT" : "RIGHT",
                state.supportVRControllerIsLeft ? "LEFT" : "RIGHT",
                rawSupportGripHeld ? 1 : 0,
                higgsSupportHold ? 1 : 0);
            return;
        }

        PlayerCharacter* player = *g_thePlayer;
        TESForm* equipped = player ?
            player->GetEquippedObject(state.ownerGameHandIsLeft) : nullptr;
        bool matchingWeaponEquipped =
            equipped && equipped->formID == state.weaponFormID &&
            EquipManager::IsWeapon(equipped);

        // 2H Weapons Unlocked can expose the worn hand more reliably through
        // the inventory worn lists than through GetEquippedObject().
        TESForm* transitionWeapon = LookupFormByID(state.weaponFormID);
        bool wornLeft = false;
        bool wornRight = false;
        if (player && transitionWeapon)
        {
            EquipManager::Get2HWeaponWornGameHands(
                player, transitionWeapon, wornLeft, wornRight);
            matchingWeaponEquipped = matchingWeaponEquipped ||
                (state.ownerGameHandIsLeft ? wornLeft : wornRight);
        }

        const bool desiredHandWorn =
            state.ownerGameHandIsLeft ? wornLeft : wornRight;
        const bool wrongHandWorn =
            state.ownerGameHandIsLeft ? wornRight : wornLeft;

        // 2hWeaponsUnlocked can briefly honor the support hand's queued HIGGS
        // grab after our explicit original-hand equip request.  Keep the
        // transition weapon protected from Fake Edge's normal auto-holster,
        // remove only that incorrect worn slot, and retry the authoritative
        // original-hand equip while the HIGGS support hand remains disabled.
        if (!matchingWeaponEquipped && wrongHandWorn && !desiredHandWorn &&
            !state.equippedObserved)
        {
            state.wrongHandCorrectionTimer += deltaTime;
            if (state.wrongHandCorrectionTimer >= 0.0f &&
                state.wrongHandCorrectionCount < 3)
            {
                EquipManager* equipMgr = EquipManager::GetSingleton();
                const bool wrongHandIsLeft = !state.ownerGameHandIsLeft;
                const bool wrongHandUnequipped =
                    equipMgr->FullUnequipHand(wrongHandIsLeft);
                equipMgr->ScheduleEquipWeaponToGameHand(
                    state.weaponFormID, state.ownerGameHandIsLeft);
                state.wrongHandCorrectionCount++;
                state.wrongHandCorrectionTimer = 0.0f;
                state.equipWaitTimer = 0.0f;

                _MESSAGE(
                    "[FalseEdgeVR] Correcting 2H support-hand equip: "
                    "form=0x%08X wrong-hand=%s original-hand=%s "
                    "unequipped=%d attempt=%d/3",
                    state.weaponFormID,
                    wrongHandIsLeft ? "LEFT" : "RIGHT",
                    state.ownerGameHandIsLeft ? "LEFT" : "RIGHT",
                    wrongHandUnequipped ? 1 : 0,
                    state.wrongHandCorrectionCount);
                return;
            }
        }
        else
        {
            state.wrongHandCorrectionTimer = 0.0f;
        }

        if (matchingWeaponEquipped)
        {
            // The bundled 2H add-on no longer treats a HIGGS support grab as a
            // hand-transfer request, so support re-arming is now symmetric.
            if (state.higgsSupportHandDisabled && higgsInterface)
            {
                higgsInterface->EnableHand(state.supportVRControllerIsLeft);
                state.higgsSupportHandDisabled = false;
                _MESSAGE(
                    "[FalseEdgeVR] Re-enabled HIGGS support hand after original-hand 2H equip: "
                    "form=0x%08X support-controller=%s",
                    state.weaponFormID,
                    state.supportVRControllerIsLeft ? "LEFT" : "RIGHT");
            }
            if (!state.equippedObserved)
            {
                _MESSAGE(
                    "[FalseEdgeVR] Opposite-grip 2H weapon active: form=0x%08X "
                    "owner-game-hand=%s (held until support grip release)",
                    state.weaponFormID,
                    state.ownerGameHandIsLeft ? "LEFT" : "RIGHT");
            }
            state.equippedObserved = true;
            state.equipWaitTimer = 0.0f;
            state.equippedSettleTimer += deltaTime;

            // Do not assume the synthetic falling/rising edge worked.  HIGGS
            // exposes its actual two-hand state, so verify it and retry after
            // the weapon geometry has had more time to settle if necessary.
            if (higgsInterface && higgsInterface->IsTwoHanding())
            {
                if (!state.higgsGripRearmSucceeded)
                {
                    state.higgsGripRearmSucceeded = true;
                    _MESSAGE(
                        "[FalseEdgeVR] HIGGS support grip verified on active 2H weapon: "
                        "form=0x%08X owner-game-hand=%s support-controller=%s "
                        "attempt=%d",
                        state.weaponFormID,
                        state.ownerGameHandIsLeft ? "LEFT" : "RIGHT",
                        state.supportVRControllerIsLeft ? "LEFT" : "RIGHT",
                        state.higgsGripRearmAttemptCount);
                }
            }

            const bool firstRearmReady =
                state.higgsGripRearmAttemptCount == 0 &&
                state.equippedSettleTimer >= 0.12f;
            const bool retryRearmReady =
                state.higgsGripRearmAttemptCount > 0 &&
                state.equippedSettleTimer - state.higgsGripLastAttemptTime >= 0.35f;
            if (state.gripObservedHeld &&
                !state.higgsGripRearmSucceeded &&
                !state.higgsGripSettingSuppressed &&
                state.higgsGripRearmAttemptCount < 3 &&
                (firstRearmReady || retryRearmReady))
            {
                state.higgsGripRearmAttemptCount++;
                state.higgsGripLastAttemptTime = state.equippedSettleTimer;

                if (higgsInterface && !higgsInterface->IsTwoHanding())
                {
                    double enableGrip = 1.0;
                    if (higgsInterface->GetSettingDouble("EnableGrip", enableGrip) &&
                        enableGrip > 0.5 &&
                        higgsInterface->SetSettingDouble("EnableGrip", 0.0))
                    {
                        state.savedHiggsEnableGrip = enableGrip;
                        state.higgsGripSettingSuppressed = true;
                        state.higgsGripRearmTimer = 0.0f;
                        _MESSAGE(
                            "[FalseEdgeVR] Pulsing HIGGS support-grip detection: "
                            "form=0x%08X support-controller=%s attempt=%d/3 "
                            "(raw input unchanged)",
                            state.weaponFormID,
                            state.supportVRControllerIsLeft ? "LEFT" : "RIGHT",
                            state.higgsGripRearmAttemptCount);
                    }
                    else
                    {
                        _MESSAGE(
                            "[FalseEdgeVR] HIGGS support-grip re-arm unavailable: "
                            "form=0x%08X",
                            state.weaponFormID);
                    }
                }
            }

            if (!state.higgsGripRearmSucceeded &&
                !state.higgsGripRearmFailureLogged &&
                state.higgsGripRearmAttemptCount >= 3 &&
                !state.higgsGripSettingSuppressed &&
                state.equippedSettleTimer - state.higgsGripLastAttemptTime >= 0.35f)
            {
                state.higgsGripRearmFailureLogged = true;
                _MESSAGE(
                    "[FalseEdgeVR] HIGGS support grip could not be verified after 3 attempts: "
                    "form=0x%08X owner-game-hand=%s support-controller=%s "
                    "raw-grip=%d can-grab=%d",
                    state.weaponFormID,
                    state.ownerGameHandIsLeft ? "LEFT" : "RIGHT",
                    state.supportVRControllerIsLeft ? "LEFT" : "RIGHT",
                    rawSupportGripHeld ? 1 : 0,
                    higgsInterface &&
                        higgsInterface->CanGrabObject(state.supportVRControllerIsLeft) ? 1 : 0);
            }
        }
        else if (state.equippedObserved)
        {
            // An external equip/unequip won the race.  Do not later holster an
            // unrelated weapon merely because the old support grip changed.
            _MESSAGE(
                "[FalseEdgeVR] Opposite-grip 2H state ended after external equipment change: "
                "form=0x%08X owner-game-hand=%s",
                state.weaponFormID,
                state.ownerGameHandIsLeft ? "LEFT" : "RIGHT");
            ResetOppositeGrip2HTransition();
            return;
        }
        else
        {
            state.equipWaitTimer += deltaTime;
            if (state.equipWaitTimer >= 2.0f)
            {
                _MESSAGE(
                    "[FalseEdgeVR] Opposite-grip 2H equip timed out: form=0x%08X "
                    "owner-game-hand=%s",
                    state.weaponFormID,
                    state.ownerGameHandIsLeft ? "LEFT" : "RIGHT");
                ResetOppositeGrip2HTransition();
            }
            return;
        }

        // Recovering the collision reference briefly makes HIGGS report that
        // neither hand holds it.  Give the support hand time to attach to the
        // newly equipped weapon before treating loss of both raw grip and the
        // HIGGS hold as a genuine release.
        if (!supportGripHeld && state.gripObservedHeld)
        {
            state.supportLostTimer += deltaTime;
            if (state.supportLostTimer < 0.35f)
                return;

            EquipManager* equipMgr = EquipManager::GetSingleton();
            if (equipMgr->ForceUnequipAndGrab(state.ownerGameHandIsLeft))
            {
                s_oppositeGripReleaseSoundSuppressUntilMs.store(
                    GetTickCount64() + kOppositeGripReleaseSoundTailMs,
                    std::memory_order_release);
                _MESSAGE(
                    "[FalseEdgeVR] Opposite grip released; restored 2H collision weapon: "
                    "form=0x%08X owner-game-hand=%s "
                    "(HIGGS audio tail suppressed %llu ms)",
                    state.weaponFormID,
                    state.ownerGameHandIsLeft ? "LEFT" : "RIGHT",
                    kOppositeGripReleaseSoundTailMs);
                ResetOppositeGrip2HTransition();
            }
        }
    }

    static bool ControllerTriggerHoldsEquippedWeapon(bool isLeftVRController)
    {
        const bool triggerHeld = isLeftVRController ?
            s_leftTriggerPressed : s_rightTriggerPressed;
        if (!triggerHeld)
            return false;

        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return false;

        const bool isLeftGameHand = VRControllerToGameHand(isLeftVRController);
        TESForm* equipped = player->GetEquippedObject(isLeftGameHand);
        if (equipped && EquipManager::IsWeapon(equipped))
            return true;

        const PlayerEquipState& equipState =
            EquipManager::GetSingleton()->GetEquipState();
        return isLeftGameHand ?
            equipState.leftHand.isEquipped : equipState.rightHand.isEquipped;
    }

    static void UpdateTriggerHeldPromptActivation()
    {
        // Opposite-grip mode has its own owner-hand prompt edge.  This path is
        // specifically for the mod's ordinary "hold trigger to equip" mode.
        if (s_oppositeGrip2H.active)
            return;

        const bool leftGripJustPressed =
            s_leftGripPressed && !s_leftGripWasPressed;
        const bool rightGripJustPressed =
            s_rightGripPressed && !s_rightGripWasPressed;
        if (!leftGripJustPressed && !rightGripJustPressed)
            return;

        if (!ControllerTriggerHoldsEquippedWeapon(true) &&
            !ControllerTriggerHoldsEquippedWeapon(false))
            return;

        const char* sourceLabel = leftGripJustPressed && rightGripJustPressed ?
            "trigger-held mode, both grips" :
            (leftGripJustPressed ?
                "trigger-held mode, left grip" :
                "trigger-held mode, right grip");
        TryActivateTrackedPrompt(sourceLabel);
    }

    static void ForceEquipAllGrabbedWeapons()
    {
        EquipGrabbedWeaponForGameHand(true, true);
        EquipGrabbedWeaponForGameHand(false, true);
    }

    // When the main (right) hand has a 2H melee weapon equipped, the off (left) hand
    // can only legitimately hold a separate weapon via mods like 2hWeaponsUnlocked.
    // Returns true (and unequips) if such a distinct off-hand weapon is present.
    static bool UnequipOffHandWeaponWhenMainHandHas2H()
    {
        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return false;

        TESForm* mainEquipped = player->GetEquippedObject(false);
        if (!IsTwoHandedMeleeBaseForm(mainEquipped))
            return false;

        TESForm* offEquipped = player->GetEquippedObject(true);
        // A lone 2H spans both slots and reports as the same form on the left — skip it.
        if (!offEquipped || offEquipped == mainEquipped ||
            offEquipped->formType != kFormType_Weapon)
            return false;

        return EquipManager::GetSingleton()->FullUnequipHand(true);
    }

    static bool s_wasPlayerMounted = false;

    static void UpdateMountWeaponState()
    {
        const bool mounted = IsPlayerMounted();

        if (!mountWeaponHandlingEnabled)
        {
            s_wasPlayerMounted = mounted;
            return;
        }

        if (mounted && !s_wasPlayerMounted)
        {
            s_pendingTriggerUnequipLeft = false;
            s_pendingTriggerUnequipRight = false;
            s_triggerUnequipTimerLeft = 0.0f;
            s_triggerUnequipTimerRight = 0.0f;

            ForceEquipAllGrabbedWeapons();
            _MESSAGE("[FalseEdgeVR] Mounted — equipping grabbed weapons");
        }
        else if (!mounted && s_wasPlayerMounted)
        {
            EquipManager::GetSingleton()->HolsterAllEquippedWeaponHands();
            _MESSAGE("[FalseEdgeVR] Dismounted — holstering equipped weapons to grab");
        }

        s_wasPlayerMounted = mounted;
    }

    static void ResolveGrabStateAfterDoorTransition()
    {
        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return;

        EquipManager* equipMgr = EquipManager::GetSingleton();

        for (int handIdx = 0; handIdx < 2; handIdx++)
        {
            const bool isLeftGameHand = (handIdx == 0);
            const bool vrControllerIsLeft = GameHandToVRController(isLeftGameHand);
            const bool triggerHeld = vrControllerIsLeft ? s_leftTriggerPressed : s_rightTriggerPressed;

            if (triggerHeld || VRInputHandler::IsWeaponLocked(vrControllerIsLeft))
                continue;

            if (equipMgr->GetDroppedWeaponRef(isLeftGameHand))
                continue;

            TESForm* equipped = player->GetEquippedObject(isLeftGameHand);
            if (equipped && EquipManager::IsWeapon(equipped))
                equipMgr->ForceUnequipAndGrab(isLeftGameHand);
        }

        _MESSAGE("[FalseEdgeVR] Door transition guard ended — holstered per trigger state");
    }

    bool IsDoorTransitionGuardActive()
    {
        return s_doorTransitionGuardTimer > 0.0f;
    }

    bool IsWeaponGrabToHolsterBlocked()
    {
        if (VRInputHandler::GetSingleton()->IsPaused())
            return true;

        if (IsAnyBlockingMenuOpen())
            return true;

        if (mountWeaponHandlingEnabled && IsPlayerMounted())
            return true;

        return IsDoorTransitionGuardActive();
    }

    void NotifyDoorOrTransitionActivated()
    {
        s_doorTransitionGuardTimer = DOOR_TRANSITION_GUARD_SECONDS;
        s_pendingPostDoorGrabResolve = false;

        s_pendingTriggerUnequipLeft = false;
        s_pendingTriggerUnequipRight = false;
        s_triggerUnequipTimerLeft = 0.0f;
        s_triggerUnequipTimerRight = 0.0f;

        // When activating a door with a 2H melee weapon equipped in the main (right)
        // hand, any weapon equipped in the off (left) hand — 1H or 2H, e.g. via
        // 2hWeaponsUnlocked — is unequipped straight back to inventory. The main hand
        // keeps its weapon and follows the normal door-transition logic.
        if (twoHandedTrackingEnabled && UnequipOffHandWeaponWhenMainHandHas2H())
        {
            _MESSAGE("[FalseEdgeVR] Door/cell transition — main-hand 2H equipped; unequipped off-hand weapon to inventory");
        }

        ForceEquipAllGrabbedWeapons();

        _MESSAGE("[FalseEdgeVR] Door/cell transition — force-equipping grabbed weapons for %.0fs",
            DOOR_TRANSITION_GUARD_SECONDS);
    }

    void UpdateWeaponTransitionGuard(float deltaTime)
    {
        if (s_doorTransitionGuardTimer <= 0.0f)
            return;

        const float previous = s_doorTransitionGuardTimer;
        s_doorTransitionGuardTimer = (std::max)(0.0f, s_doorTransitionGuardTimer - deltaTime);

        if (previous > 0.0f && s_doorTransitionGuardTimer <= 0.0f)
            s_pendingPostDoorGrabResolve = true;
    }

    static const char* GetFirstOpenBlockingMenuName()
    {
        MenuManager* mm = MenuManager::GetSingleton();
        if (!mm)
            return nullptr;

        static BSFixedString blockingMenus[] = {
            BSFixedString("Crafting Menu"),
            BSFixedString("RaceSex Menu"),
            BSFixedString("ContainerMenu"),
            BSFixedString("BarterMenu"),
            BSFixedString("GiftMenu"),
            BSFixedString("Lockpicking Menu"),
            BSFixedString("Book Menu"),
            BSFixedString("Sleep/Wait Menu"),
            BSFixedString("Loading Menu"),
            BSFixedString("Fader Menu"),
            BSFixedString("Journal Menu"),
            BSFixedString("MapMenu"),
            BSFixedString("InventoryMenu"),
            BSFixedString("MagicMenu"),
            BSFixedString("FavoritesMenu"),
            BSFixedString("StatsMenu"),
            BSFixedString("Training Menu"),
            BSFixedString("MessageBoxMenu"),
            BSFixedString("Console"),
            BSFixedString("Dialogue Menu"),
            BSFixedString("TweenMenu"),
            BSFixedString("MainMenu"),
        };

        for (BSFixedString& menuName : blockingMenus)
        {
            if (mm->IsMenuOpen(&menuName))
                return menuName.data;
        }

        return nullptr;
    }

    static bool HasTrackableWeaponEquipped(bool isLeftGameHand)
    {
        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return false;

        TESForm* equipped = player->GetEquippedObject(isLeftGameHand);
        return equipped && EquipManager::IsWeapon(equipped);
    }

    static bool IsFalseEdgeBypassActive(VRInputHandler* handler)
    {
        if (handler->IsPaused())
            return true;

        if (IsAnyBlockingMenuOpen())
            return true;

        if (IsDoorTransitionGuardActive())
            return true;

        if (mountWeaponHandlingEnabled && IsPlayerMounted())
            return true;

        return false;
    }

    static bool BothHandsVanillaDefaultEquip(VRInputHandler* handler)
    {
        if (IsFalseEdgeBypassActive(handler))
            return false;

        if (!HasTrackableWeaponEquipped(true) || !HasTrackableWeaponEquipped(false))
            return false;

        EquipManager* equipMgr = EquipManager::GetSingleton();
        if (equipMgr->GetDroppedWeaponRef(true) || equipMgr->GetDroppedWeaponRef(false))
            return false;

        return true;
    }

        static bool BothEquippedHandTriggersHeld()
        {
            const bool leftHandTrigger = GameHandToVRController(true) ?
                s_leftTriggerPressed : s_rightTriggerPressed;
            const bool rightHandTrigger = GameHandToVRController(false) ?
                s_leftTriggerPressed : s_rightTriggerPressed;
            return leftHandTrigger && rightHandTrigger;
        }

        static float GetTriggerUnequipDelayForHand(bool isLeftGameHand)
        {
            PlayerCharacter* player = *g_thePlayer;
            if (!player)
                return triggerUnequipDelay;

            TESForm* equipped = player->GetEquippedObject(isLeftGameHand);
            if (equipped && staffTrackingEnabled &&
                EquipManager::GetWeaponType(equipped) == WeaponType::Staff)
            {
                return staffTriggerReleaseUnequipDelay;
            }

            return triggerUnequipDelay;
        }

        static void LogFalseEdgeDiagnosticContext(VRInputHandler* handler, bool pollTriggerRan, const char* eventLabel)
    {
        EquipManager* equipMgr = EquipManager::GetSingleton();
        PlayerCharacter* player = *g_thePlayer;

        TESForm* leftEquipped = player ? player->GetEquippedObject(true) : nullptr;
        TESForm* rightEquipped = player ? player->GetEquippedObject(false) : nullptr;

        const char* openMenu = GetFirstOpenBlockingMenuName();

        _MESSAGE(
            "[FalseEdgeVR] DIAG (%s): paused=%d menuOpen=%d menu=\"%s\" pollTrigger=%d "
            "doorGuard=%.2fs postDoorResolve=%d mounted=%d mountHandling=%d "
            "grabListen=%d holsterBlocked=%d higgs=%d "
            "leftEq=0x%08X rightEq=0x%08X leftDropRef=%d rightDropRef=%d "
            "leftPendingUnequip=%d rightPendingUnequip=%d leftPendingReequip=%d rightPendingReequip=%d "
            "leftWeaponLock=%d rightWeaponLock=%d leftTrigger=%d rightTrigger=%d",
            eventLabel,
            handler->IsPaused() ? 1 : 0,
            IsAnyBlockingMenuOpen() ? 1 : 0,
            openMenu ? openMenu : "",
            pollTriggerRan ? 1 : 0,
            s_doorTransitionGuardTimer,
            s_pendingPostDoorGrabResolve ? 1 : 0,
            IsPlayerMounted() ? 1 : 0,
            mountWeaponHandlingEnabled ? 1 : 0,
            handler->IsListening() ? 1 : 0,
            IsWeaponGrabToHolsterBlocked() ? 1 : 0,
            higgsInterface ? 1 : 0,
            leftEquipped ? leftEquipped->formID : 0u,
            rightEquipped ? rightEquipped->formID : 0u,
            equipMgr->GetDroppedWeaponRef(true) ? 1 : 0,
            equipMgr->GetDroppedWeaponRef(false) ? 1 : 0,
            s_pendingTriggerUnequipLeft ? 1 : 0,
            s_pendingTriggerUnequipRight ? 1 : 0,
            equipMgr->HasPendingReequip(true) ? 1 : 0,
            equipMgr->HasPendingReequip(false) ? 1 : 0,
            VRInputHandler::IsWeaponLocked(true) ? 1 : 0,
            VRInputHandler::IsWeaponLocked(false) ? 1 : 0,
            s_leftTriggerPressed ? 1 : 0,
            s_rightTriggerPressed ? 1 : 0);
    }

    static void ForceRecoverVanillaDefaultEquip(VRInputHandler* handler, bool pollTriggerRan)
    {
        _MESSAGE("[FalseEdgeVR] DIAG: Vanilla default equip persisted for %.1fs — forcing equipped weapons back to grab state.",
            vanillaDefaultRecoverySeconds);
        LogFalseEdgeDiagnosticContext(handler, pollTriggerRan, "vanilla-default-recovery");

        s_pendingTriggerUnequipLeft = false;
        s_pendingTriggerUnequipRight = false;
        s_triggerUnequipTimerLeft = 0.0f;
        s_triggerUnequipTimerRight = 0.0f;

        VRInputHandler::ClearWeaponLock(true);
        VRInputHandler::ClearWeaponLock(false);

        EquipManager::GetSingleton()->HolsterAllEquippedWeaponHands();
    }

    static void DiagnoseFalseEdgeTriggerHoldState(VRInputHandler* handler, bool pollTriggerRan, float deltaTime)
    {
        static bool s_prevBothHandsVanillaDefault = false;
        static bool s_loggedStuckPause = false;
        static int s_stuckPauseFrameCount = 0;
        static float s_vanillaDefaultTimer = 0.0f;
        static bool s_vanillaDefaultRecoveryTriggered = false;

        const bool bothVanilla = BothHandsVanillaDefaultEquip(handler);
        const bool bypass = IsFalseEdgeBypassActive(handler);

        if (bothVanilla && !s_prevBothHandsVanillaDefault)
        {
            _MESSAGE("[FalseEdgeVR] DIAG: Both hands on vanilla default equip — False Edge trigger-hold is NOT managing either hand.");
            LogFalseEdgeDiagnosticContext(handler, pollTriggerRan, "vanilla-default-enter");
            s_vanillaDefaultTimer = 0.0f;
            s_vanillaDefaultRecoveryTriggered = false;
        }
        else if (!bothVanilla && s_prevBothHandsVanillaDefault && !bypass)
        {
            _MESSAGE("[FalseEdgeVR] DIAG: False Edge trigger-hold active again (at least one hand holstered/equip-managed).");
            LogFalseEdgeDiagnosticContext(handler, pollTriggerRan, "mod-managed-enter");
            s_vanillaDefaultTimer = 0.0f;
            s_vanillaDefaultRecoveryTriggered = false;
        }

        s_prevBothHandsVanillaDefault = bothVanilla;

        const bool recoveryEligible = bothVanilla && !BothEquippedHandTriggersHeld();
        if (recoveryEligible && vanillaDefaultRecoverySeconds > 0.0f && !s_vanillaDefaultRecoveryTriggered)
        {
            s_vanillaDefaultTimer += deltaTime;
            if (s_vanillaDefaultTimer >= vanillaDefaultRecoverySeconds)
            {
                if (!IsWeaponGrabToHolsterBlocked())
                {
                    s_vanillaDefaultRecoveryTriggered = true;
                    ForceRecoverVanillaDefaultEquip(handler, pollTriggerRan);
                }
            }
        }
        else if (!bothVanilla || bypass)
        {
            s_vanillaDefaultTimer = 0.0f;
            s_vanillaDefaultRecoveryTriggered = false;
        }

        const bool stuckPause = handler->IsPaused() && !IsAnyBlockingMenuOpen();
        if (stuckPause)
        {
            s_stuckPauseFrameCount++;
            if (!s_loggedStuckPause && s_stuckPauseFrameCount >= 45)
            {
                s_loggedStuckPause = true;
                _MESSAGE("[FalseEdgeVR] DIAG: Tracking paused but no blocking menu is open (stuck pause — trigger-hold suspended).");
                LogFalseEdgeDiagnosticContext(handler, pollTriggerRan, "stuck-pause");
            }
        }
        else
        {
            s_stuckPauseFrameCount = 0;
            if (s_loggedStuckPause)
            {
                _MESSAGE("[FalseEdgeVR] DIAG: Stuck pause cleared — trigger polling resumed.");
                LogFalseEdgeDiagnosticContext(handler, pollTriggerRan, "stuck-pause-cleared");
            }
            s_loggedStuckPause = false;
        }
    }

    // Trigger button mask (SteamVR trigger button = button 33)
    static const uint64_t TRIGGER_BUTTON_MASK = (1ull << 33);

    // Grip button mask (k_EButton_Grip = 2)
    static const uint64_t GRIP_BUTTON_MASK = (1ull << 2);
    // Oculus/OpenComposite commonly exposes the analog hand trigger as Axis2
    // instead of setting the legacy digital Grip bit.
    static const uint64_t GRIP_AXIS2_BUTTON_MASK = (1ull << 34);
    static const float GRIP_ANALOG_PRESS_THRESHOLD = 0.50f;

    static bool IsRawControllerGripDownNow(bool isLeftVRController)
    {
        BSOpenVR* openVR = (*g_openVR);
        if (!openVR || !openVR->vrSystem)
            return false;

        vr_1_0_12::IVRSystem* vrSystem = openVR->vrSystem;
        const vr_1_0_12::ETrackedControllerRole role = isLeftVRController ?
            vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_LeftHand :
            vr_1_0_12::ETrackedControllerRole::TrackedControllerRole_RightHand;
        const vr_1_0_12::TrackedDeviceIndex_t controller =
            vrSystem->GetTrackedDeviceIndexForControllerRole(role);

        vr_1_0_12::VRControllerState_t state;
        if (!vrSystem->GetControllerState(controller, &state, sizeof(state)))
            return false;

        return (state.ulButtonPressed & GRIP_BUTTON_MASK) != 0 ||
            (state.ulButtonPressed & GRIP_AXIS2_BUTTON_MASK) != 0 ||
            state.rAxis[2].x > GRIP_ANALOG_PRESS_THRESHOLD;
    }

    // Track player draw/sheathe for weapons that stay equipped (SKSE action events often miss in VR)
    static bool s_prevPlayerWeaponDrawn = false;

    // Block sheathing while a mod-tracked weapon remains equipped (False Edge keeps weapons drawn).
    static void PreventWeaponSheath()
    {
        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return;

        TESForm* left = player->GetEquippedObject(true);
        TESForm* right = player->GetEquippedObject(false);
        bool hasEquippedWeapon =
            (left && EquipManager::IsWeapon(left)) ||
            (right && EquipManager::IsWeapon(right));

        if (!hasEquippedWeapon)
        {
            s_prevPlayerWeaponDrawn = player->actorState.IsWeaponDrawn();
            return;
        }

        bool drawn = player->actorState.IsWeaponDrawn();
        if (!drawn)
        {
            if (s_prevPlayerWeaponDrawn)
            {
                // NOTE: physics thread — no GetName() (can crash in game form code).
                if (left && EquipManager::IsWeapon(left))
                {
                    _MESSAGE("[FalseEdgeVR] Weapon redrawn in LEFT game hand: 0x%08X", left->formID);
                }
                if (right && EquipManager::IsWeapon(right))
                {
                    _MESSAGE("[FalseEdgeVR] Weapon redrawn in RIGHT game hand: 0x%08X", right->formID);
                }
            }

            EquipManager::s_suppressDrawSound = true;
            player->DrawSheatheWeapon(true);
            EquipManager::s_suppressDrawSound = false;
            drawn = player->actorState.IsWeaponDrawn();
        }

        s_prevPlayerWeaponDrawn = drawn;
    }

    // ============================================
    // Shoulder Zone Detection
    // Detects when controller with grabbed weapon is near shoulder
    // Used for holstering/sheathing detection
    // ============================================

    // Shoulder zone tracking state
    static bool s_leftControllerNearLeftShoulder = false;
    static bool s_leftControllerNearRightShoulder = false;
    static bool s_rightControllerNearLeftShoulder = false;
    static bool s_rightControllerNearRightShoulder = false;

    // Previous frame state for edge detection
    static bool s_prevLeftNearLeftShoulder = false;
    static bool s_prevLeftNearRightShoulder = false;
    static bool s_prevRightNearLeftShoulder = false;
    static bool s_prevRightNearRightShoulder = false;

    // Shoulder zone configuration (in Skyrim units, ~70 units = 1 meter)
    static constexpr float SHOULDER_ZONE_RADIUS = 25.0f;  // Detection radius around shoulder
    static constexpr float SHOULDER_OFFSET_X = 17.5f;     // Left/right offset from head (matches HIGGS RightShoulderHmdOffsetX)
    static constexpr float SHOULDER_OFFSET_Y = -5.0f;     // Forward/back offset from head (matches HIGGS RightShoulderHmdOffsetY)
    static constexpr float SHOULDER_OFFSET_Z = -6.85f;    // Up/down offset from head (matches HIGGS RightShoulderHmdOffsetZ)

    // Node names for VR tracking
    static const char* kHMDNodeName = "NPC Head [Head]";
    static const char* kLeftHandNodeName = "NPC L Hand [LHnd]";
    static const char* kRightHandNodeName = "NPC R Hand [RHnd]";

    // Helper function to check if a VR controller is in ANY shoulder zone
    bool IsControllerInShoulderZone(bool isLeftVRController)
    {
        if (isLeftVRController)
        {
            return s_leftControllerNearLeftShoulder || s_leftControllerNearRightShoulder;
        }
        else
        {
            return s_rightControllerNearLeftShoulder || s_rightControllerNearRightShoulder;
        }
    }
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


        // Register HIGGS callbacks if HIGGS is available
        RegisterHiggsCallbacks();

        // Trigger button tracking initialization
        RegisterTriggerCallback();

        m_initialized = true;
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
    }

    void VRInputHandler::UpdateGrabListening()
    {
        bool shouldListen = ShouldListenForGrabs();

    if (shouldListen && !m_isListening)
        {
            m_isListening = true;
     }
   else if (!shouldListen && m_isListening)
   {
     m_isListening = false;
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
        
        if (!hasWeaponOrShield)
        {
            PlayerCharacter* player = *g_thePlayer;
            if (player)
            {
                TESForm* leftEquipped = player->GetEquippedObject(true);
                TESForm* rightEquipped = player->GetEquippedObject(false);
                hasWeaponOrShield =
                    (leftEquipped && EquipManager::IsShield(leftEquipped)) ||
                    (rightEquipped && EquipManager::IsShield(rightEquipped));
            }
        }
        
        return hasWeaponOrShield;
    }

    bool VRInputHandler::IsTwoHanding() const
    {
        if (!higgsInterface)
            return false;

        return higgsInterface->IsTwoHanding();
    }

    bool VRInputHandler::IsOppositeGrip2HActiveForGameHand(bool isLeftGameHand, UInt32 weaponFormID)
    {
        return s_oppositeGrip2H.active &&
            s_oppositeGrip2H.ownerGameHandIsLeft == isLeftGameHand &&
            (weaponFormID == 0 || s_oppositeGrip2H.weaponFormID == weaponFormID);
    }

    bool VRInputHandler::IsOppositeGrip2HTransitionWeapon(UInt32 weaponFormID)
    {
        return s_oppositeGrip2H.active && weaponFormID != 0 &&
            s_oppositeGrip2H.weaponFormID == weaponFormID;
    }

    bool VRInputHandler::ShouldSuppressHiggsPhysicsSound()
    {
        // Once armed, every HIGGS grab/drop sound associated with the short
        // collision-reference/equipped-weapon handoff is transitional.
        if (s_oppositeGrip2H.active)
            return true;

        const ULONGLONG releaseSuppressUntil =
            s_oppositeGripReleaseSoundSuppressUntilMs.load(
                std::memory_order_acquire);
        if (releaseSuppressUntil != 0 &&
            GetTickCount64() < releaseSuppressUntil)
        {
            return true;
        }

        if (!twoHandedTrackingEnabled || !higgsInterface)
            return false;

        EquipManager* equipMgr = EquipManager::GetSingleton();
        if (!equipMgr)
            return false;

        // The first HIGGS grab sound is emitted immediately before its grabbed
        // callback, so the transition is not armed yet.  Recognize that exact
        // pre-callback state from the tracked collision reference, the owner
        // HIGGS hand, the live support grip, and controller proximity.
        for (int hand = 0; hand < 2; ++hand)
        {
            const bool ownerGameHandIsLeft = hand != 0;
            TESObjectREFR* collisionRef =
                equipMgr->GetDroppedWeaponRef(ownerGameHandIsLeft);
            if (!IsDroppedWeaponRefReadable(collisionRef) ||
                !collisionRef->baseForm ||
                !IsTwoHandedMeleeBaseForm(collisionRef->baseForm))
            {
                continue;
            }

            const bool ownerVRControllerIsLeft =
                GameHandToVRController(ownerGameHandIsLeft);
            const bool supportVRControllerIsLeft = !ownerVRControllerIsLeft;
            TESObjectREFR* ownerHeld =
                higgsInterface->GetGrabbedObject(ownerVRControllerIsLeft);
            if (ownerHeld != collisionRef ||
                !IsRawControllerGripDownNow(supportVRControllerIsLeft))
            {
                continue;
            }

            float controllerDistance = 9999.0f;
            if (TryGetVRControllerSeparation(
                    ownerVRControllerIsLeft,
                    supportVRControllerIsLeft,
                    controllerDistance) &&
                controllerDistance <= kOppositeGripMaxControllerDistance)
            {
                return true;
            }
        }

        return false;
    }

    // ============================================
    // HIGGS Callback Handlers
    // ============================================

    void VRInputHandler::OnPrePhysicsStep(void* world)
    {
      static int frameCount = 0;
        static bool loggedOnce = false;
   
        VRInputHandler* handler = GetSingleton();

        static auto lastTime = std::chrono::high_resolution_clock::now();
        auto currentTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
        lastTime = currentTime;
        if (deltaTime > 0.1f) deltaTime = 0.1f;
        if (deltaTime < 0.0001f) deltaTime = 0.0001f;

        UpdateWeaponTransitionGuard(deltaTime);
      
        // ============================================
 // SAFE TRACKING: Skip ALL processing when paused
        // This prevents CTD during smithing/crafting menus
        // Checks BOTH the event-driven pause flag AND MenuManager directly
        // (the flag alone can be cleared by nested menu close events while
        //  the crafting menu is still open - see forge CTD)
        // ============================================
        if (handler->m_paused || IsAnyBlockingMenuOpen())
      {
   // Only log occasionally to avoid spam
        if (frameCount % 500 == 0)
     {
   }
            frameCount++;
            DiagnoseFalseEdgeTriggerHoldState(handler, false, deltaTime);
          return;
        }

        if (s_pendingPostDoorGrabResolve && !IsWeaponGrabToHolsterBlocked())
        {
            s_pendingPostDoorGrabResolve = false;
            ResolveGrabStateAfterDoorTransition();
        }

        UpdateMountWeaponState();
  
        frameCount++;
  
      // Log once to confirm callback is working
    if (!loggedOnce)
        {
loggedOnce = true;
        }
    
  // Poll trigger button state each frame
        PollTriggerState();

        // Opposite-hand support grip is an independent temporary equip state.
        // Process it after polling so grip release is authoritative, and before
        // normal pending auto-unequip so trigger logic cannot fight the hold.
        UpdateOppositeGrip2HTransition(deltaTime);

        // Skyrim VR's native Activate binding is suppressed while a trigger is
        // held for Fake Edge's temporary weapon mode.  Re-emit only a fresh
        // grip press against the current interaction target.
        UpdateTriggerHeldPromptActivation();

        TryLogDualHand2HWeaponGrab();
        UpdateDoorStowForDual2HGrab();

        PreventWeaponSheath();
   
        // Check for pending auto-unequip (trigger-based weapon hold system)
        EquipManager::GetSingleton()->CheckPendingAutoUnequip();
    
     // Log every 500 frames to confirm still running
        if (frameCount % 500 == 0)
        {
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
     if (handler->m_rightHandCooldownTimer >= bladeReequipCooldown)
            {
        handler->m_rightHandOnCooldown = false;
  handler->m_rightHandCooldownTimer = 0.0f;
            }
        }

        handler->CheckAutoEquipGrabbedWeapon(deltaTime);
        handler->UpdateShieldBashTracking(deltaTime);
        UpdateWeaponGeometry(deltaTime);

        DiagnoseFalseEdgeTriggerHoldState(handler, true, deltaTime);

        // Reset the weapon lock for a game hand once it has been truly
        // unequipped (no weapon in that hand) for at least 0.3 seconds.
        {
            PlayerCharacter* lockPlayer = *g_thePlayer;
            if (lockPlayer)
            {
                for (int hand = 0; hand < 2; ++hand)
                {
                    bool isLeftGameHand = (hand == 0);
                    TESForm* equipped = lockPlayer->GetEquippedObject(isLeftGameHand);
                    bool hasWeapon = equipped && equipped->formType == kFormType_Weapon;

                    float& timer = isLeftGameHand ? s_leftGameHandUnequipTimer : s_rightGameHandUnequipTimer;

                    if (hasWeapon)
                    {
                        timer = 0.0f;
                    }
                    else
                    {
                        timer += deltaTime;
                        if (timer >= WEAPON_LOCK_RESET_UNEQUIP_TIME)
                        {
                            bool vrControllerIsLeft = GameHandToVRController(isLeftGameHand);
                            if (IsWeaponLocked(vrControllerIsLeft))
                                ClearWeaponLock(vrControllerIsLeft);
                        }
                    }
                }
            }
        }
    }
    

    void VRInputHandler::PauseTracking(bool pause)
    {
    m_paused = pause;
      
     if (pause)
        {
     
      // Clear any pending state that could cause issues when resuming
            m_autoEquipPendingLeft = false;
            m_autoEquipPendingRight = false;
          m_autoEquipTimerLeft = 0.0f;
     m_autoEquipTimerRight = 0.0f;
      m_autoEquipWeaponLeft = nullptr;
            m_autoEquipWeaponRight = nullptr;
            m_autoEquipFormIDLeft = 0;
     m_autoEquipFormIDRight = 0;

            s_pendingTriggerUnequipLeft = false;
            s_pendingTriggerUnequipRight = false;
            s_triggerUnequipTimerLeft = 0.0f;
            s_triggerUnequipTimerRight = 0.0f;
            
        }
        else
        {
            // Force equipment state refresh when menu closes
            EquipManager::GetSingleton()->UpdateEquipmentState();
            UpdateGrabListening();
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
  return;
        }
  
        // If this is the first bash, start the window timer
        if (m_shieldBashCount == 0)
     {
            m_shieldBashWindowTimer = 0.0f;
        }
  
        m_shieldBashCount++;
   
     // Check if threshold reached
        if (m_shieldBashCount >= shieldBashThreshold)
 {
     
            // Cast spell on player (Skyrim.esm 0x000AA026)
          const UInt32 SHIELD_BASH_SPELL_FORM_ID = 0x000AA026;
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
  }
            
  if (m_shieldBashLockoutTimer >= shieldBashLockoutDuration)
  {
   m_shieldBashLockoutActive = false;
        m_shieldBashLockoutTimer = 0.0f;
      lockoutLogTimer = 0.0f;
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
      m_shieldBashCount = 0;
     m_shieldBashWindowTimer = 0.0f;
    }
        }
    }

    void VRInputHandler::OnGrabbed(bool isLeftVRController, TESObjectREFR* grabbedRefr)
    {
        VRInputHandler* handler = GetSingleton();

        // Skip if tracking is paused (menu open)
        if (handler->m_paused || IsAnyBlockingMenuOpen())
            return;

        // Convert VR controller to game hand
        bool isLeftGameHand = VRControllerToGameHand(isLeftVRController);

     const char* vrControllerName = isLeftVRController ? "Left" : "Right";
        const char* gameHandName = isLeftGameHand ? "Left" : "Right";

        if (grabbedRefr)
        {

            // Get the base form to check what type of object was grabbed
          TESForm* baseForm = grabbedRefr->baseForm;
         if (baseForm)
         {
    
         // Check if grabbed object is a weapon
         if (baseForm->formType == kFormType_Weapon)
     {
    if (!isLeftGameHand && EquipManager::IsTwoHandedWeapon(baseForm))
    {
        EquipManager::GetSingleton()->TryLog2HLeftHandWithRightGameHandTrigger(nullptr);
    }

    // Check if this is a valid tracked weapon (bows/bound/excluded items are skipped above)
    if (!EquipManager::IsWeapon(baseForm))
    {
    return;
    }

    // Once an opposite-grip transition owns this form, every later HIGGS grab
    // callback for the same weapon is transitional.  In particular, after the
    // world copy is recovered the dropped-ref tracking is intentionally gone;
    // allowing a re-arm callback to fall through to the original auto-pickup
    // path can consume the newly equipped weapon or assign it to the other hand.
    if (s_oppositeGrip2H.active &&
        s_oppositeGrip2H.weaponFormID == baseForm->formID)
    {
        _MESSAGE(
            "[FalseEdgeVR] Ignored transitional 2H grab callback: form=0x%08X "
            "owner-game-hand=%s callback-controller=%s",
            baseForm->formID,
            s_oppositeGrip2H.ownerGameHandIsLeft ? "LEFT" : "RIGHT",
            isLeftVRController ? "LEFT" : "RIGHT");
        return;
    }

    // A second HIGGS hand grabbing the same physical 2H collision weapon is a
    // temporary combat-mode request, not a hand transfer.  Keep the original
    // hand authoritative, preserve the opposite controller as HIGGS's support
    // hand, and let the per-frame state machine equip/restore around that grip.
    EquipManager* equipMgr = EquipManager::GetSingleton();
    bool otherGameHandIsLeft = !isLeftGameHand;
    TESObjectREFR* otherHandDroppedWeapon =
        equipMgr->GetDroppedWeaponRef(otherGameHandIsLeft);
    bool isHandSwap =
        (otherHandDroppedWeapon == grabbedRefr) ||
        (equipMgr->GetDroppedWeaponRefID(otherGameHandIsLeft) == grabbedRefr->formID &&
         grabbedRefr->formID != 0);

    if (twoHandedTrackingEnabled &&
        IsTwoHandedMeleeBaseForm(baseForm) &&
        isHandSwap)
    {
        const bool ownerVRControllerIsLeft =
            GameHandToVRController(otherGameHandIsLeft);
        float controllerDistance = 9999.0f;
        const bool haveControllerDistance = TryGetVRControllerSeparation(
            ownerVRControllerIsLeft,
            isLeftVRController,
            controllerDistance);

        // Always block the transfer, but do not arm a collision->weapon
        // conversion when the support controller is distant or its position
        // cannot be verified.  This makes a distant grip a true no-op.
        if (!haveControllerDistance ||
            controllerDistance > kOppositeGripMaxControllerDistance)
        {
            _MESSAGE(
                "[FalseEdgeVR] Blocked distant 2H support grab without conversion: "
                "form=0x%08X controller-distance=%.2f distance-valid=%d "
                "max-distance=%.2f",
                baseForm->formID,
                controllerDistance,
                haveControllerDistance ? 1 : 0,
                kOppositeGripMaxControllerDistance);
            return;
        }

        if (!s_oppositeGrip2H.active)
        {
            s_oppositeGrip2H.active = true;
            s_oppositeGrip2H.ownerGameHandIsLeft = otherGameHandIsLeft;
            s_oppositeGrip2H.supportVRControllerIsLeft = isLeftVRController;
            s_oppositeGrip2H.equipQueued = false;
            s_oppositeGrip2H.equippedObserved = false;
            // OnGrabbed itself proves the support input was engaged even if
            // OpenVR's cached grip state is not updated until the next poll.
            s_oppositeGrip2H.gripObservedHeld = true;
            s_oppositeGrip2H.weaponFormID = baseForm->formID;
            s_oppositeGrip2H.collisionRefID = grabbedRefr->formID;
            s_oppositeGrip2H.equipWaitTimer = 0.0f;
            const bool ownerVRControllerIsLeft =
                GameHandToVRController(otherGameHandIsLeft);
            s_oppositeGrip2H.ownerGripWasHeld = ownerVRControllerIsLeft ?
                s_leftGripPressed : s_rightGripPressed;

            // Silence the base weapon's native pickup/putdown descriptors for
            // the duration of the inventory/world conversion.  Fake Edge's
            // own transition sounds are independently disabled in EquipManager.
            TESObjectWEAP* transitionWeapon =
                DYNAMIC_CAST(baseForm, TESForm, TESObjectWEAP);
            if (transitionWeapon)
            {
                s_oppositeGrip2H.savedNativePickUpSound =
                    transitionWeapon->pickupSounds.pickUp;
                s_oppositeGrip2H.savedNativePutDownSound =
                    transitionWeapon->pickupSounds.putDown;
                transitionWeapon->pickupSounds.pickUp = nullptr;
                transitionWeapon->pickupSounds.putDown = nullptr;
                s_oppositeGrip2H.nativeWeaponSoundsMuted = true;
            }

            // For a right-owned collision weapon, suspend LEFT support
            // immediately to avoid a transient ownership transfer.
            // For a left-owned collision weapon, RIGHT support must first be
            // allowed to establish a genuine HIGGS two-hand grip; otherwise the
            // callback is a false success that the player cannot actually feel.
            const bool waitForPhysicalSupportGrip = otherGameHandIsLeft;
            s_oppositeGrip2H.twoHandGrabValidated = !waitForPhysicalSupportGrip;
            if (!waitForPhysicalSupportGrip && higgsInterface)
            {
                higgsInterface->DisableHand(isLeftVRController);
                s_oppositeGrip2H.higgsSupportHandDisabled = true;
            }
            else if (waitForPhysicalSupportGrip)
            {
                _MESSAGE(
                    "[FalseEdgeVR] Waiting for physical RIGHT support grip on "
                    "left-owned collision weapon: form=0x%08X ref=0x%08X",
                    baseForm->formID,
                    grabbedRefr->formID);
            }

            // A trigger-release timer from the owner hand must not holster the
            // weapon while the independent support-grip state is taking over.
            if (otherGameHandIsLeft)
            {
                s_pendingTriggerUnequipLeft = false;
                s_triggerUnequipTimerLeft = 0.0f;
            }
            else
            {
                s_pendingTriggerUnequipRight = false;
                s_triggerUnequipTimerRight = 0.0f;
            }

            _MESSAGE(
                "[FalseEdgeVR] Opposite grip armed 2H transition: form=0x%08X "
                "owner-game-hand=%s support-controller=%s controller-distance=%.2f "
                "(hand transfer blocked; support-state=%s; native sounds muted)",
                baseForm->formID,
                otherGameHandIsLeft ? "LEFT" : "RIGHT",
                isLeftVRController ? "LEFT" : "RIGHT",
                controllerDistance,
                waitForPhysicalSupportGrip ? "awaiting-physical-grip" : "suspended-for-conversion");
        }
        else
        {
            _MESSAGE(
                "[FalseEdgeVR] Ignored duplicate 2H support grab while transition active: "
                "form=0x%08X support-controller=%s",
                baseForm->formID,
                isLeftVRController ? "LEFT" : "RIGHT");
        }

        _MESSAGE(
            "[FalseEdgeVR] Blocked 2H hand-transfer path: form=0x%08X "
            "owner-game-hand=%s support-game-hand=%s (grip input unchanged)",
            baseForm->formID,
            otherGameHandIsLeft ? "LEFT" : "RIGHT",
            isLeftGameHand ? "LEFT" : "RIGHT");
        return;
    }
  
   // Check if this grab is from our collision avoidance system
      // If so, skip auto-equip - our system will handle it
  bool isFromCollisionAvoidance = EquipManager::GetSingleton()->HasPendingReequip(isLeftGameHand);
    
    if (isFromCollisionAvoidance)
      {
  return;
    }
   
 // ============================================
 // CHECK IF PLAYER HAS 2H WEAPON EQUIPPED
   // If so, just pick up the weapon normally without auto-equip
   // The HIGGS grab will be converted to a normal inventory pickup
  // ============================================

if (EquipManager::PlayerHasTwoHandedEquipped())
            {
       
   PlayerCharacter* player = *g_thePlayer;
       if (player)
    {
             // Just activate (pick up) the weapon - don't auto-equip
            // This adds it to inventory and the world object is consumed
  EquipManager::s_suppressPickupSound = false;  // Allow normal pickup sound
       SafeActivate(grabbedRefr, player, 0, 0, 1, false);
           }
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
      return;
      }
                }
          }
   
         // ============================================
     // AUTO-EQUIP GRABBED 1H WEAPON
 // This will trigger OnEquip which will then HIGGS grab it
       // (unless trigger is held) - registering it with our trigger system
       // ============================================
     
 // ============================================
            // CHECK IF OTHER HAND WAS TRACKING THIS WEAPON
            // If the weapon was grabbed from the other hand, clear that hand's tracking
       // to prevent trigger press on old controller re-equipping it
        // ============================================

            if (otherHandDroppedWeapon == grabbedRefr || isHandSwap)
            {
                equipMgr->TransferFavoriteCacheForHandSwap(otherGameHandIsLeft, isLeftGameHand, baseForm->formID);
                equipMgr->ClearDroppedWeaponRef(otherGameHandIsLeft);
                equipMgr->ClearPendingReequip(otherGameHandIsLeft);
                equipMgr->ClearCachedWeaponFormID(otherGameHandIsLeft);
            }
      
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
        }

        TryLogDualHand2HWeaponGrab();
    }

    void VRInputHandler::OnDropped(bool isLeftVRController, TESObjectREFR* droppedRefr)
    {
        // Skip if tracking is paused (menu open)
 if (GetSingleton()->m_paused || IsAnyBlockingMenuOpen())
         return;

        if (!droppedRefr)
   return;

        // Convert VR controller to game hand
     bool isLeftGameHand = VRControllerToGameHand(isLeftVRController);

        // Activating a world object that HIGGS currently holds can emit drop
        // callbacks for either physical hand.  Swallow those callbacks for the
        // active transition: raw grip polling, not the disappearing reference,
        // decides when the support hand actually released.
        TESForm* droppedBaseForm = droppedRefr->baseForm;
        const bool isTransitionDrop =
            s_oppositeGrip2H.active && droppedBaseForm &&
            droppedBaseForm->formID == s_oppositeGrip2H.weaponFormID &&
            (droppedRefr->formID == s_oppositeGrip2H.collisionRefID ||
             s_oppositeGrip2H.equipQueued);

        if (isTransitionDrop && twoHandedTrackingEnabled &&
            IsTwoHandedMeleeBaseForm(droppedBaseForm) &&
            s_oppositeGrip2H.active)
        {
            _MESSAGE(
                "[FalseEdgeVR] Ignored transitional 2H drop callback: form=0x%08X "
                "owner-game-hand=%s callback-controller=%s (raw grip remains authoritative)",
                droppedBaseForm->formID,
                s_oppositeGrip2H.ownerGameHandIsLeft ? "LEFT" : "RIGHT",
                isLeftVRController ? "LEFT" : "RIGHT");
            return;
        }
   
        const char* vrControllerName = isLeftVRController ? "Left" : "Right";
        const char* gameHandName = isLeftGameHand ? "Left" : "Right";
        

        VRInputHandler* handler = GetSingleton();


        // ============================================
        // COUNT DROPS FOR GRIP SPAM DETECTION
        // Each drop event counts as a "grip release" for spam detection
        // ============================================
        if (isLeftVRController)
        {
            s_leftGripPressCount++;
            if (s_leftGripPressCount == 1)
            {
                s_leftGripSpamWindowTimer = 0.0f;
            }

            if (s_leftGripPressCount >= gripSpamThreshold && s_leftGripSpamWindowTimer <= gripSpamWindow)
            {
                s_leftDropProtectionDisabled = true;
                s_leftDropProtectionDisableTimer = dropProtectionDisableTime;
                s_leftGripPressCount = 0;
                s_leftGripSpamWindowTimer = 0.0f;
            }
        }
        else
        {
            s_rightGripPressCount++;
            if (s_rightGripPressCount == 1)
            {
                s_rightGripSpamWindowTimer = 0.0f;
            }

            if (s_rightGripPressCount >= gripSpamThreshold && s_rightGripSpamWindowTimer <= gripSpamWindow)
            {
                s_rightDropProtectionDisabled = true;
                s_rightDropProtectionDisableTimer = dropProtectionDisableTime;
                s_rightGripPressCount = 0;
                s_rightGripSpamWindowTimer = 0.0f;
            }
        }

        // ============================================
 // CHECK IF DROP PROTECTION IS DISABLED (INTENTIONAL DROP)
 // If player spammed grip, they want to drop - clear all tracking and don't re-grab
 // ============================================
        if (IsDropProtectionDisabled(isLeftVRController))
        {

           

            // Clear auto-equip tracking
  if (isLeftVRController)
   {
 handler->m_autoEquipPendingLeft = false;
 handler->m_autoEquipTimerLeft = 0.0f;
        handler->m_autoEquipWeaponLeft = nullptr;
        handler->m_autoEquipFormIDLeft = 0;
      }
     else
  {
  handler->m_autoEquipPendingRight = false;
    handler->m_autoEquipTimerRight = 0.0f;
 handler->m_autoEquipWeaponRight = nullptr;
 handler->m_autoEquipFormIDRight = 0;
        }
            
 // Clear grabbed weapon tracking (restores weapon scale)
     // REMOVED: ClearGrabbedWeapon(isLeftVRController);
     
    // Clear weapon lock state
       ClearWeaponLock(isLeftVRController);

            // DUPLICATION GUARD: if this is our spawned dual-wield-same duplicate,
            // delete the world copy. The inventory still holds the original
            // (removal was skipped at spawn), so leaving the copy on the ground
            // would permanently duplicate the weapon.
            for (int h = 0; h < 2; h++)
            {
                bool handIsLeft = (h == 0);
                if (EquipManager::GetSingleton()->GetDroppedWeaponRef(handIsLeft) == droppedRefr &&
                    EquipManager::GetSingleton()->WasDualWieldingSameWeapon(handIsLeft))
                {
                    DeleteWorldObject(droppedRefr);
                    EquipManager::GetSingleton()->ClearDroppedWeaponRef(handIsLeft);
                    EquipManager::GetSingleton()->ClearPendingReequip(handIsLeft);
                    EquipManager::GetSingleton()->ClearCachedWeaponFormID(handIsLeft);
                    break;
                }
            }

    // Clear EquipManager dropped weapon tracking
            UInt32 droppedBaseID = EquipManager::GetSingleton()->GetDroppedWeaponBaseID(isLeftGameHand);
            if (droppedBaseID == 0 && droppedRefr->baseForm)
                droppedBaseID = droppedRefr->baseForm->formID;
            if (droppedBaseID != 0)
                EquipManager::GetSingleton()->PreserveFavoriteForForm(droppedBaseID, isLeftGameHand);

  EquipManager::GetSingleton()->ClearDroppedWeaponRef(isLeftGameHand);
       EquipManager::GetSingleton()->ClearPendingReequip(isLeftGameHand);
        EquipManager::GetSingleton()->ClearCachedWeaponFormID(isLeftGameHand);
    
  return;  // Exit early - don't do any re-grab logic
        }
        
        // ============================================
        // Determine DROP REASON for logging/tracking
        // ============================================
        bool isAutoEquipWeapon = false;
        bool isCollisionAvoidanceWeapon = false;
        
        // Check if this was a weapon we were waiting to auto-equip
        if (isLeftVRController && handler->m_autoEquipWeaponLeft == droppedRefr)
        {
isAutoEquipWeapon = true;
            
   handler->m_autoEquipPendingLeft = false;
            handler->m_autoEquipTimerLeft = 0.0f;
    handler->m_autoEquipWeaponLeft = nullptr;
        }
        else if (!isLeftVRController && handler->m_autoEquipWeaponRight == droppedRefr)
        {
            isAutoEquipWeapon = true;
     
 handler->m_autoEquipPendingRight = false;
            handler->m_autoEquipTimerRight = 0.0f;
 handler->m_autoEquipWeaponRight = nullptr;
        }

        // Check if this is the weapon we were tracking for collision avoidance
    TESObjectREFR* trackedWeapon = EquipManager::GetSingleton()->GetDroppedWeaponRef(isLeftGameHand);
    
      if (trackedWeapon && droppedRefr == trackedWeapon)
        {
     isCollisionAvoidanceWeapon = true;
   
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
          
   }
    }
         }
      
    // Force HIGGS to grab it immediately
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

            // A 2H weapon normally leaves the off-hand empty, so otherHandHasWeapon is
            // false and this accidental-drop re-grab would be skipped. For 2H-skill
            // weapons, allow the re-grab regardless of the off-hand state. (Intentional
            // grip-spam drops already returned above, so reaching here means accidental.)
            const bool is2HSkillWeapon = twoHandedTrackingEnabled &&
                EquipManager::UsesTwoHandedSkill(droppedRefr->baseForm);

        if ((otherHandHasWeapon || is2HSkillWeapon) && higgsInterface)
            {
   
            // Check if the hand can grab an object right now
    if (higgsInterface->CanGrabObject(isLeftVRController))
       {
         // Use HIGGS to grab the weapon again
        higgsInterface->GrabObject(droppedRefr, isLeftVRController);
         
         return;
 }
        else
            {
          }
            }
        }

    // ============================================
    // Clear tracking state for collision avoidance weapon
        // ============================================
        if (isCollisionAvoidanceWeapon)
        {
  
     // Clear the dropped weapon reference (by game hand)
            EquipManager::GetSingleton()->ClearDroppedWeaponRef(isLeftGameHand);
            
         // Clear the pending re-equip (by game hand)
    EquipManager::GetSingleton()->ClearPendingReequip(isLeftGameHand);
   
      // Clear the cached FormID (by game hand)
    EquipManager::GetSingleton()->ClearCachedWeaponFormID(isLeftGameHand);
    
        }
        
    // Log if this was a completely untracked weapon drop (not from our systems)
if (!isAutoEquipWeapon && !isCollisionAvoidanceWeapon && 
 droppedRefr->baseForm && droppedRefr->baseForm->formType == kFormType_Weapon)
{
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

         if (m_autoEquipTimerLeft >= AUTO_EQUIP_PICKUP_SETTLE_TIME)
            {
  
     bool isLeftGameHand = VRControllerToGameHand(true);
      PlayerCharacter* player = *g_thePlayer;
      TESForm* weaponForm = LookupFormByID(m_autoEquipFormIDLeft);
            
    if (player && weaponForm)
    {
   EquipManager* equipMan = EquipManager::GetSingleton();
      if (equipMan)
    {
   EquipManager::s_suppressDrawSound = true;
       equipMan->EquipWeaponToGameHand(player, weaponForm, isLeftGameHand);
  EquipManager::s_suppressDrawSound = false;
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

      if (m_autoEquipTimerRight >= AUTO_EQUIP_PICKUP_SETTLE_TIME)
     {
    
          bool isLeftGameHand = VRControllerToGameHand(false);
      PlayerCharacter* player = *g_thePlayer;
 TESForm* weaponForm = LookupFormByID(m_autoEquipFormIDRight);
 
      if (player && weaponForm)
              {
        EquipManager* equipMan = EquipManager::GetSingleton();
          if (equipMan)
          {
            EquipManager::s_suppressDrawSound = true;
          equipMan->EquipWeaponToGameHand(player, weaponForm, isLeftGameHand);
      EquipManager::s_suppressDrawSound = false;
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
     m_autoEquipPendingLeft = false;
       m_autoEquipTimerLeft = 0.0f;
           m_autoEquipWeaponLeft = nullptr;
   }
            else if (!higgsInterface || higgsInterface->GetGrabbedObject(true) != m_autoEquipWeaponLeft)
  {
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
 
      if (activated)
      {
    EquipManager* equipMan = EquipManager::GetSingleton();
  if (equipMan)
   {
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

      equipMan->EquipWeaponToGameHand(player, weaponForm, isLeftGameHand);

      // Restore enchantment immediately
      if (weapEnch && cachedEnchantAuto)
      {
          weapEnch->enchantable.enchantment = cachedEnchantAuto;
      }
 EquipManager::s_suppressDrawSound = false;
          
        // Start cooldown to prevent immediate collision detection re-triggering
   if (isLeftGameHand)
     {
         m_leftHandOnCooldown = true;
    m_leftHandCooldownTimer = 0.0f;
       }
        else
  {
  m_rightHandOnCooldown = true;
         m_rightHandCooldownTimer = 0.0f;
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
      m_autoEquipPendingRight = false;
      m_autoEquipTimerRight = 0.0f;
           m_autoEquipWeaponRight = nullptr;
 }
     else if (!higgsInterface || higgsInterface->GetGrabbedObject(false) != m_autoEquipWeaponRight)
       {
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
     
  if (activated)
   {
        EquipManager* equipMan = EquipManager::GetSingleton();
      if (equipMan)
        {
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

   equipMan->EquipWeaponToGameHand(player, weaponForm, isLeftGameHand);

   // Restore enchantment immediately
   if (weapEnch2 && cachedEnchantAuto2)
   {
       weapEnch2->enchantable.enchantment = cachedEnchantAuto2;
   }
  EquipManager::s_suppressDrawSound = false;
     
       // Start cooldown to prevent immediate collision detection re-triggering
         if (isLeftGameHand)
  {
            m_leftHandOnCooldown = true;
           m_leftHandCooldownTimer = 0.0f;
        }
       else
  {
           m_rightHandOnCooldown = true;
    m_rightHandCooldownTimer = 0.0f;
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
        TryLogDualHand2HWeaponGrab();
    }

    void VRInputHandler::OnStopTwoHanding()
    {
    }

    void VRInputHandler::ClearAllState()
    {

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
        m_autoEquipFormIDLeft = 0;
        m_autoEquipFormIDRight = 0;
        
        // Clear weapon lock state
     ClearWeaponLock(true);   // Left VR controller
     ClearWeaponLock(false);  // Right VR controller
        
  // Clear drop protection override state
        s_leftDropProtectionDisabled = false;
        s_leftDropProtectionDisableTimer = 0.0f;
   s_rightDropProtectionDisabled = false;
        s_rightDropProtectionDisableTimer = 0.0f;
      
   // Clear pending trigger unequip (both hands)
  s_pendingTriggerUnequipLeft = false;
      s_triggerUnequipTimerLeft = 0.0f;
  s_pendingTriggerUnequipRight = false;
      s_triggerUnequipTimerRight = 0.0f;

        s_prevPlayerWeaponDrawn = false;

        s_leftTriggerBeforeRight = false;
        s_dualTriggerLeftRestoreIssued = false;

        s_oppositeGripReleaseSoundSuppressUntilMs.store(
            0, std::memory_order_release);
        ResetOppositeGrip2HTransition();

        s_wasPlayerMounted = IsPlayerMounted();

        // Clear shield bash tracking completely on death/load
        m_shieldBashCount = 0;
  m_shieldBashWindowTimer = 0.0f;
        m_shieldBashLockoutActive = false;
    m_shieldBashLockoutTimer = 0.0f;
    
      // Clear grabbed weapon scaling (restores scale to 1.0 if still valid)
        // REMOVED: ClearGrabbedWeapon(true);
 // REMOVED: ClearGrabbedWeapon(false);

        // DUPLICATION GUARD: delete any orphaned dual-wield duplicate world copies
        // before wiping the tracking that points at them (death/load would otherwise
        // leave the copy in the world while the inventory keeps the original)
        EquipManager::GetSingleton()->CleanupOrphanedDuplicates();

    EquipManager::GetSingleton()->ClearDroppedWeaponRef(true);
 EquipManager::GetSingleton()->ClearDroppedWeaponRef(false);
    EquipManager::GetSingleton()->ClearPendingReequip(true);
     EquipManager::GetSingleton()->ClearPendingReequip(false);
    EquipManager::GetSingleton()->ClearCachedWeaponFormID(true);
EquipManager::GetSingleton()->ClearCachedWeaponFormID(false);
        
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
// Shoulder Zone Detection
// ============================================

    void CheckShoulderZones()
    {
        PlayerCharacter* player = *g_thePlayer;
        if (!player)
            return;

        NiNode* rootNode = player->GetNiNode();
        if (!rootNode)
            rootNode = player->GetNiRootNode(1);
        if (!rootNode)
            return;

        // Get HMD (head) node
        BSFixedString hmdNodeStr(kHMDNodeName);
        NiAVObject* hmdNode = rootNode->GetObjectByName(&hmdNodeStr.data);
        if (!hmdNode)
            return;

        NiPoint3 hmdPos = hmdNode->m_worldTransform.pos;
        NiMatrix33& hmdRot = hmdNode->m_worldTransform.rot;

        // Calculate shoulder positions in world space using HMD rotation
        // Left shoulder: negative X offset (left of head)
        NiPoint3 leftShoulderLocal(-SHOULDER_OFFSET_X, SHOULDER_OFFSET_Y, SHOULDER_OFFSET_Z);
        NiPoint3 leftShoulderWorld;
        leftShoulderWorld.x = hmdPos.x + hmdRot.data[0][0] * leftShoulderLocal.x + hmdRot.data[0][1] * leftShoulderLocal.y + hmdRot.data[0][2] * leftShoulderLocal.z;
        leftShoulderWorld.y = hmdPos.y + hmdRot.data[1][0] * leftShoulderLocal.x + hmdRot.data[1][1] * leftShoulderLocal.y + hmdRot.data[1][2] * leftShoulderLocal.z;
        leftShoulderWorld.z = hmdPos.z + hmdRot.data[2][0] * leftShoulderLocal.x + hmdRot.data[2][1] * leftShoulderLocal.y + hmdRot.data[2][2] * leftShoulderLocal.z;

        // Right shoulder: positive X offset (right of head)
        NiPoint3 rightShoulderLocal(SHOULDER_OFFSET_X, SHOULDER_OFFSET_Y, SHOULDER_OFFSET_Z);
        NiPoint3 rightShoulderWorld;
        rightShoulderWorld.x = hmdPos.x + hmdRot.data[0][0] * rightShoulderLocal.x + hmdRot.data[0][1] * rightShoulderLocal.y + hmdRot.data[0][2] * rightShoulderLocal.z;
        rightShoulderWorld.y = hmdPos.y + hmdRot.data[1][0] * rightShoulderLocal.x + hmdRot.data[1][1] * rightShoulderLocal.y + hmdRot.data[1][2] * rightShoulderLocal.z;
        rightShoulderWorld.z = hmdPos.z + hmdRot.data[2][0] * rightShoulderLocal.x + hmdRot.data[2][1] * rightShoulderLocal.y + hmdRot.data[2][2] * rightShoulderLocal.z;

        // Get hand nodes
        BSFixedString leftHandStr(kLeftHandNodeName);
        BSFixedString rightHandStr(kRightHandNodeName);
        NiAVObject* leftHandNode = rootNode->GetObjectByName(&leftHandStr.data);
        NiAVObject* rightHandNode = rootNode->GetObjectByName(&rightHandStr.data);

        // Store previous state
        s_prevLeftNearLeftShoulder = s_leftControllerNearLeftShoulder;
        s_prevLeftNearRightShoulder = s_leftControllerNearRightShoulder;
        s_prevRightNearLeftShoulder = s_rightControllerNearLeftShoulder;
        s_prevRightNearRightShoulder = s_rightControllerNearRightShoulder;

        // Check left controller distances
        if (leftHandNode)
        {
            NiPoint3 leftHandPos = leftHandNode->m_worldTransform.pos;

            float distToLeftShoulder = sqrt(
                (leftHandPos.x - leftShoulderWorld.x) * (leftHandPos.x - leftShoulderWorld.x) +
                (leftHandPos.y - leftShoulderWorld.y) * (leftHandPos.y - leftShoulderWorld.y) +
                (leftHandPos.z - leftShoulderWorld.z) * (leftHandPos.z - leftShoulderWorld.z));

            float distToRightShoulder = sqrt(
                (leftHandPos.x - rightShoulderWorld.x) * (leftHandPos.x - rightShoulderWorld.x) +
                (leftHandPos.y - rightShoulderWorld.y) * (leftHandPos.y - rightShoulderWorld.y) +
                (leftHandPos.z - rightShoulderWorld.z) * (leftHandPos.z - rightShoulderWorld.z));

            s_leftControllerNearLeftShoulder = (distToLeftShoulder <= SHOULDER_ZONE_RADIUS);
            s_leftControllerNearRightShoulder = (distToRightShoulder <= SHOULDER_ZONE_RADIUS);

            // Log enter/exit events for left controller
            if (s_leftControllerNearLeftShoulder && !s_prevLeftNearLeftShoulder)
            {
            }
            else if (!s_leftControllerNearLeftShoulder && s_prevLeftNearLeftShoulder)
            {
            }

            if (s_leftControllerNearRightShoulder && !s_prevLeftNearRightShoulder)
            {
            }
            else if (!s_leftControllerNearRightShoulder && s_prevLeftNearRightShoulder)
            {
            }
        }

        // Check right controller distances
        if (rightHandNode)
        {
            NiPoint3 rightHandPos = rightHandNode->m_worldTransform.pos;

            float distToLeftShoulder = sqrt(
                (rightHandPos.x - leftShoulderWorld.x) * (rightHandPos.x - leftShoulderWorld.x) +
                (rightHandPos.y - leftShoulderWorld.y) * (rightHandPos.y - leftShoulderWorld.y) +
                (rightHandPos.z - leftShoulderWorld.z) * (rightHandPos.z - leftShoulderWorld.z));

            float distToRightShoulder = sqrt(
                (rightHandPos.x - rightShoulderWorld.x) * (rightHandPos.x - rightShoulderWorld.x) +
                (rightHandPos.y - rightShoulderWorld.y) * (rightHandPos.y - rightShoulderWorld.y) +
                (rightHandPos.z - rightShoulderWorld.z) * (rightHandPos.z - rightShoulderWorld.z));

            s_rightControllerNearLeftShoulder = (distToLeftShoulder <= SHOULDER_ZONE_RADIUS);
            s_rightControllerNearRightShoulder = (distToRightShoulder <= SHOULDER_ZONE_RADIUS);

            // Log enter/exit events for right controller
            if (s_rightControllerNearLeftShoulder && !s_prevRightNearLeftShoulder)
            {
            }
            else if (!s_rightControllerNearLeftShoulder && s_prevRightNearLeftShoulder)
            {
            }

            if (s_rightControllerNearRightShoulder && !s_prevRightNearRightShoulder)
            {
         }
    else if (!s_rightControllerNearRightShoulder && s_prevRightNearRightShoulder)
       {
   }
        }
 
        // ============================================
  // CHECK GRIP + SHOULDER + GRABBED WEAPON
        // Log when grip is pressed while controller with grabbed weapon is in shoulder zone
   // ============================================

        
        // Check LEFT controller: in shoulder zone + has grabbed weapon + grip pressed
        if (IsControllerInShoulderZone(true) && s_leftGripPressed && higgsInterface)
  {
   TESObjectREFR* leftGrabbed = higgsInterface->GetGrabbedObject(true);
if (leftGrabbed && leftGrabbed->baseForm && leftGrabbed->baseForm->formType == kFormType_Weapon)
          {
        // Only log on grip press (edge detection)
                if (!s_leftGripWasPressed)
    {
         const char* shoulderSide = s_leftControllerNearLeftShoulder ? "LEFT" : "RIGHT";
           }
   }
        }
        
        // Check RIGHT controller: in shoulder zone + has grabbed weapon + grip pressed
        if (IsControllerInShoulderZone(false) && s_rightGripPressed && higgsInterface)
        {
            TESObjectREFR* rightGrabbed = higgsInterface->GetGrabbedObject(false);
         if (rightGrabbed && rightGrabbed->baseForm && rightGrabbed->baseForm->formType == kFormType_Weapon)
        {
    // Only log on grip press (edge detection)
   if (!s_rightGripWasPressed)
{
           const char* shoulderSide = s_rightControllerNearLeftShoulder ? "LEFT" : "RIGHT";
      }
            }
    }

        // ============================================
  // CHECK TRIGGER TOUCH + SHOULDER + GRABBED WEAPON
// When trigger is TOUCHED while controller with grabbed weapon is in shoulder zone,
// set HIGGS shoulder radius to 0 to allow holstering
// ============================================

    // Track if we've modified HIGGS settings (to restore them when leaving zone)
        static bool s_leftShoulderRadiusModified = false;
        static bool s_rightShoulderRadiusModified = false;
        static const double SHOULDER_RADIUS_DEFAULT = 11.0;
        static const double SHOULDER_RADIUS_HOLSTER = 0.0;

        // Check LEFT controller: in shoulder zone + has grabbed weapon + trigger touched
        bool leftInShoulderWithWeapon = false;
        if (IsControllerInShoulderZone(true) && s_leftTriggerTouched && higgsInterface)
        {
            TESObjectREFR* leftGrabbed = higgsInterface->GetGrabbedObject(true);
            if (leftGrabbed && leftGrabbed->baseForm && leftGrabbed->baseForm->formType == kFormType_Weapon)
            {
                leftInShoulderWithWeapon = true;

                // Set shoulder radius to 0 if not already done
                if (!s_leftShoulderRadiusModified)
                {
                    higgsInterface->SetSettingDouble("LeftShoulderRadius", SHOULDER_RADIUS_HOLSTER);
                    higgsInterface->SetSettingDouble("RightShoulderRadius", SHOULDER_RADIUS_HOLSTER);
                    s_leftShoulderRadiusModified = true;

                    const char* shoulderSide = s_leftControllerNearLeftShoulder ? "LEFT" : "RIGHT";
                }
            }
        }

        // Restore LEFT controller shoulder radius when conditions no longer met
        if (s_leftShoulderRadiusModified && !leftInShoulderWithWeapon)
        {
            higgsInterface->SetSettingDouble("LeftShoulderRadius", SHOULDER_RADIUS_DEFAULT);
            higgsInterface->SetSettingDouble("RightShoulderRadius", SHOULDER_RADIUS_DEFAULT);
            s_leftShoulderRadiusModified = false;
        }

        // Check RIGHT controller: in shoulder zone + has grabbed weapon + trigger touched
        bool rightInShoulderWithWeapon = false;
        if (IsControllerInShoulderZone(false) && s_rightTriggerTouched && higgsInterface)
        {
            TESObjectREFR* rightGrabbed = higgsInterface->GetGrabbedObject(false);
         if (rightGrabbed && rightGrabbed->baseForm && rightGrabbed->baseForm->formType == kFormType_Weapon)
        {
                rightInShoulderWithWeapon = true;

                // Set shoulder radius to 0 if not already done
                if (!s_rightShoulderRadiusModified)
                {
                    higgsInterface->SetSettingDouble("LeftShoulderRadius", SHOULDER_RADIUS_HOLSTER);
                    higgsInterface->SetSettingDouble("RightShoulderRadius", SHOULDER_RADIUS_HOLSTER);
                    s_rightShoulderRadiusModified = true;

                    const char* shoulderSide = s_rightControllerNearLeftShoulder ? "LEFT" : "RIGHT";
                }
            }
        }

        // Restore RIGHT controller shoulder radius when conditions no longer met
        if (s_rightShoulderRadiusModified && !rightInShoulderWithWeapon)
        {
            higgsInterface->SetSettingDouble("LeftShoulderRadius", SHOULDER_RADIUS_DEFAULT);
            higgsInterface->SetSettingDouble("RightShoulderRadius", SHOULDER_RADIUS_DEFAULT);
            s_rightShoulderRadiusModified = false;
        }

        // ============================================
        // CHECK GRIP PRESSED + SHOULDER + GRABBED WEAPON
        // Log when grip is pressed while controller with grabbed weapon is in shoulder zone
        // This indicates weapon is being added to inventory (holstered)
        // ============================================

        // Check LEFT controller: in shoulder zone + has grabbed weapon + grip pressed
        if (IsControllerInShoulderZone(true) && s_leftGripPressed && higgsInterface)
        {
            TESObjectREFR* leftGrabbed = higgsInterface->GetGrabbedObject(true);
            if (leftGrabbed && leftGrabbed->baseForm && leftGrabbed->baseForm->formType == kFormType_Weapon)
            {
                // Only log on grip press (edge detection)
                if (!s_leftGripWasPressed)
                {
                    const char* shoulderSide = s_leftControllerNearLeftShoulder ? "LEFT" : "RIGHT";
                    UInt32 weaponFormID = leftGrabbed->baseForm->formID;

                    bool isLeftGameHand = VRControllerToGameHand(true);
                    PlayerCharacter* player = *g_thePlayer;
                    TESForm* equipped = player ? player->GetEquippedObject(isLeftGameHand) : nullptr;
                    bool stillEquipped = equipped && equipped->formID == weaponFormID;
                    // NOTE: physics thread — no GetName() (can crash in game form code).
                    if (stillEquipped)
                    {
                        _MESSAGE("[FalseEdgeVR] Weapon sheathed (still equipped) via %s shoulder holster in %s game hand: 0x%08X",
                            shoulderSide,
                            isLeftGameHand ? "LEFT" : "RIGHT",
                            weaponFormID);
                    }
                    else
                    {
                        _MESSAGE("[FalseEdgeVR] Weapon holstered to %s shoulder in %s game hand (not equipped): 0x%08X",
                            shoulderSide,
                            isLeftGameHand ? "LEFT" : "RIGHT",
                            weaponFormID);
                    }

                    // Clear EquipManager tracking to prevent re-equip on trigger pull
                    EquipManager::GetSingleton()->ClearDroppedWeaponRef(isLeftGameHand);
                    EquipManager::GetSingleton()->ClearPendingReequip(isLeftGameHand);
                    EquipManager::GetSingleton()->ClearCachedWeaponFormID(isLeftGameHand);

                    // Remove duplicate from inventory (HIGGS adds it on holster)
                    DelayedRemoveItemFromInventory(weaponFormID, 200);
                }
            }
        }

        // Check RIGHT controller: in shoulder zone + has grabbed weapon + grip pressed
        if (IsControllerInShoulderZone(false) && s_rightGripPressed && higgsInterface)
        {
            TESObjectREFR* rightGrabbed = higgsInterface->GetGrabbedObject(false);
         if (rightGrabbed && rightGrabbed->baseForm && rightGrabbed->baseForm->formType == kFormType_Weapon)
        {
                // Only log on grip press (edge detection)
                if (!s_rightGripWasPressed)
                {
                    const char* shoulderSide = s_rightControllerNearLeftShoulder ? "LEFT" : "RIGHT";
                    UInt32 weaponFormID = rightGrabbed->baseForm->formID;

                    bool isRightGameHand = VRControllerToGameHand(false);
                    PlayerCharacter* player = *g_thePlayer;
                    TESForm* equipped = player ? player->GetEquippedObject(isRightGameHand) : nullptr;
                    bool stillEquipped = equipped && equipped->formID == weaponFormID;
                    // NOTE: physics thread — no GetName() (can crash in game form code).
                    if (stillEquipped)
                    {
                        _MESSAGE("[FalseEdgeVR] Weapon sheathed (still equipped) via %s shoulder holster in %s game hand: 0x%08X",
                            shoulderSide,
                            isRightGameHand ? "LEFT" : "RIGHT",
                            weaponFormID);
                    }
                    else
                    {
                        _MESSAGE("[FalseEdgeVR] Weapon holstered to %s shoulder in %s game hand (not equipped): 0x%08X",
                            shoulderSide,
                            isRightGameHand ? "LEFT" : "RIGHT",
                            weaponFormID);
                    }

                    // Clear EquipManager tracking to prevent re-equip on trigger pull
                    EquipManager::GetSingleton()->ClearDroppedWeaponRef(isRightGameHand);
                    EquipManager::GetSingleton()->ClearPendingReequip(isRightGameHand);
                    EquipManager::GetSingleton()->ClearCachedWeaponFormID(isRightGameHand);

                    // Remove duplicate from inventory (HIGGS adds it on holster)
                    DelayedRemoveItemFromInventory(weaponFormID, 200);
                }
            }
        }
    }
    
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
            // === TRIGGER ===
            s_leftTriggerWasPressed = s_leftTriggerPressed;
            // Check both ulButtonPressed (digital) and rAxis[1] (analog trigger)
            // Button 33 is k_EButton_SteamVR_Trigger
            bool digitalPressed = (leftState.ulButtonPressed & TRIGGER_BUTTON_MASK) != 0;
            bool analogPressed = (leftState.rAxis[1].x > 0.5f);  // Trigger axis
            s_leftTriggerPressed = digitalPressed || analogPressed;

            // Track trigger TOUCH (ulButtonTouched or light analog input)
            s_leftTriggerWasTouched = s_leftTriggerTouched;
            bool digitalTouched = (leftState.ulButtonTouched & TRIGGER_BUTTON_MASK) != 0;
            bool analogTouched = (leftState.rAxis[1].x > 0.1f);  // Light touch threshold
            s_leftTriggerTouched = digitalTouched || analogTouched;

            // === LEFT TRIGGER SPAM DETECTION (Weapon Lock) ===
            if (s_leftTriggerPressed && !s_leftTriggerWasPressed)
            {
                // Order tracking: left must be pressed while right is still up
                s_leftTriggerBeforeRight = !s_rightTriggerPressed;

                // Trigger just pressed - count it for spam detection
                s_leftTriggerPressCount++;

                if (s_leftTriggerPressCount == 1)
                {
                    // First press - start the window timer
                    s_leftTriggerSpamWindowTimer = 0.0f;
                }


                // Check if we've hit the threshold within the time window
                if (s_leftTriggerPressCount >= triggerSpamThreshold && s_leftTriggerSpamWindowTimer <= triggerSpamWindow)
                {
                    s_leftTriggerPressCount = 0;
                    s_leftTriggerSpamWindowTimer = 0.0f;

                    // Block locking a 2H-skill weapon while the opposite hand's 2H-skill
                    // weapon is already locked (only one may be locked at a time).
                    const bool wouldEngage = !s_leftWeaponLocked;
                    if (wouldEngage && IsTwoHandedSkillLockBlockedByOppositeHand(true))
                    {
                        _MESSAGE("[FalseEdgeVR] 2H weapon lock blocked on LEFT controller — opposite hand 2H weapon is already locked");
                    }
                    else
                    {
                        // Toggle weapon lock state
                        s_leftWeaponLocked = !s_leftWeaponLocked;

                        if (s_leftWeaponLocked)
                        {
                            LogWeaponLockIf2HSkill(true);
                        }
                    }
                }
            }

            if (!s_leftTriggerPressed && s_leftTriggerWasPressed)
            {
                s_leftTriggerBeforeRight = false;
                s_dualTriggerLeftRestoreIssued = false;
            }

            // === GRIP ===
            s_leftGripWasPressed = s_leftGripPressed;
            const bool leftDigitalGrip =
                (leftState.ulButtonPressed & GRIP_BUTTON_MASK) != 0;
            const bool leftAxis2Pressed =
                (leftState.ulButtonPressed & GRIP_AXIS2_BUTTON_MASK) != 0;
            const bool leftAnalogGrip =
                leftState.rAxis[2].x > GRIP_ANALOG_PRESS_THRESHOLD;
            s_leftGripPressed =
                leftDigitalGrip || leftAxis2Pressed || leftAnalogGrip;

            // Log grip press/release and handle grip spam detection for LEFT controller
            if (s_leftGripPressed && !s_leftGripWasPressed)
            {
            }
            else if (!s_leftGripPressed && s_leftGripWasPressed)
            {

                // Grip spam detection for LEFT VR controller (count RELEASES, not presses)
                s_leftGripPressCount++;

                if (s_leftGripPressCount == 1)
                {
                    // First release - start the window timer
                    s_leftGripSpamWindowTimer = 0.0f;
                }


                if (s_leftGripPressCount >= gripSpamThreshold && s_leftGripSpamWindowTimer <= gripSpamWindow)
                {
                    // Disable drop protection for LEFT controller only
                    s_leftDropProtectionDisabled = true;
                    s_leftDropProtectionDisableTimer = dropProtectionDisableTime;
                    s_leftGripPressCount = 0;
                    s_leftGripSpamWindowTimer = 0.0f;
                }
            }
        }

        // Get controller state for right hand
        vr_1_0_12::VRControllerState_t rightState;
        if (vrSystem->GetControllerState(rightController, &rightState, sizeof(rightState)))
        {
            // === TRIGGER ===
            s_rightTriggerWasPressed = s_rightTriggerPressed;
            bool digitalPressed = (rightState.ulButtonPressed & TRIGGER_BUTTON_MASK) != 0;
            bool analogPressed = (rightState.rAxis[1].x > 0.5f);  // Trigger axis
            s_rightTriggerPressed = digitalPressed || analogPressed;

            // Track trigger TOUCH (ulButtonTouched or light analog input)
            s_rightTriggerWasTouched = s_rightTriggerTouched;
            bool digitalTouched = (rightState.ulButtonTouched & TRIGGER_BUTTON_MASK) != 0;
            bool analogTouched = (rightState.rAxis[1].x > 0.1f);  // Light touch threshold
            s_rightTriggerTouched = digitalTouched || analogTouched;

            // === RIGHT TRIGGER SPAM DETECTION (Weapon Lock) ===
            if (s_rightTriggerPressed && !s_rightTriggerWasPressed)
            {
                // Trigger just pressed - count it for spam detection
                s_rightTriggerPressCount++;

                if (s_rightTriggerPressCount == 1)
                {
                    // First press - start the window timer
                    s_rightTriggerSpamWindowTimer = 0.0f;
                }


                // Check if we've hit the threshold within the time window
                if (s_rightTriggerPressCount >= triggerSpamThreshold && s_rightTriggerSpamWindowTimer <= triggerSpamWindow)
                {
                    s_rightTriggerPressCount = 0;
                    s_rightTriggerSpamWindowTimer = 0.0f;

                    // Block locking a 2H-skill weapon while the opposite hand's 2H-skill
                    // weapon is already locked (only one may be locked at a time).
                    const bool wouldEngage = !s_rightWeaponLocked;
                    if (wouldEngage && IsTwoHandedSkillLockBlockedByOppositeHand(false))
                    {
                        _MESSAGE("[FalseEdgeVR] 2H weapon lock blocked on RIGHT controller — opposite hand 2H weapon is already locked");
                    }
                    else
                    {
                        // Toggle weapon lock state
                        s_rightWeaponLocked = !s_rightWeaponLocked;

                        if (s_rightWeaponLocked)
                        {
                            LogWeaponLockIf2HSkill(false);
                        }
                    }
                }
            }

            if (!s_rightTriggerPressed && s_rightTriggerWasPressed)
            {
                s_dualTriggerLeftRestoreIssued = false;
            }

            // === GRIP ===
            s_rightGripWasPressed = s_rightGripPressed;
            const bool rightDigitalGrip =
                (rightState.ulButtonPressed & GRIP_BUTTON_MASK) != 0;
            const bool rightAxis2Pressed =
                (rightState.ulButtonPressed & GRIP_AXIS2_BUTTON_MASK) != 0;
            const bool rightAnalogGrip =
                rightState.rAxis[2].x > GRIP_ANALOG_PRESS_THRESHOLD;
            s_rightGripPressed =
                rightDigitalGrip || rightAxis2Pressed || rightAnalogGrip;

            // Log grip press/release and handle grip spam detection for RIGHT controller
            if (s_rightGripPressed && !s_rightGripWasPressed)
            {
            }
            else if (!s_rightGripPressed && s_rightGripWasPressed)
            {

                // Grip spam detection for RIGHT VR controller (count RELEASES, not presses)
                s_rightGripPressCount++;

                if (s_rightGripPressCount == 1)
                {
                    // First release - start the window timer
                    s_rightGripSpamWindowTimer = 0.0f;
                }


                if (s_rightGripPressCount >= gripSpamThreshold && s_rightGripSpamWindowTimer <= gripSpamWindow)
                {
                    // Disable drop protection for RIGHT controller only
                    s_rightDropProtectionDisabled = true;
                    s_rightDropProtectionDisableTimer = dropProtectionDisableTime;
                    s_rightGripPressCount = 0;
                    s_rightGripSpamWindowTimer = 0.0f;
                }
            }

            // ============================================
                // UPDATE DROP PROTECTION TIMERS
                // ============================================
            const float frameTime = 0.011f;

            // Update LEFT drop protection disable timer
            if (s_leftDropProtectionDisabled)
            {
                s_leftDropProtectionDisableTimer -= frameTime;
                if (s_leftDropProtectionDisableTimer <= 0.0f)
                {
                    s_leftDropProtectionDisabled = false;
                    s_leftDropProtectionDisableTimer = 0.0f;
                }
            }

            // Update RIGHT drop protection disable timer
            if (s_rightDropProtectionDisabled)
            {
                s_rightDropProtectionDisableTimer -= frameTime;
                if (s_rightDropProtectionDisableTimer <= 0.0f)
                {
                    s_rightDropProtectionDisabled = false;
                    s_rightDropProtectionDisableTimer = 0.0f;
                }
            }

            // ============================================
        // UPDATE GRIP SPAM TIMERS
        // ============================================

        // Update LEFT controller grip spam window timer
            if (s_leftGripPressCount > 0)
            {
                s_leftGripSpamWindowTimer += frameTime;
                // Reset if window expired
                if (s_leftGripSpamWindowTimer > gripSpamWindow)
                {
                    if (s_leftGripPressCount > 0)
                    {
                    }
                    s_leftGripPressCount = 0;
                    s_leftGripSpamWindowTimer = 0.0f;
                }
            }

            // Update RIGHT controller grip spam window timer
            if (s_rightGripPressCount > 0)
            {
                s_rightGripSpamWindowTimer += frameTime;
                // Reset if window expired
                if (s_rightGripSpamWindowTimer > gripSpamWindow)
                {
                    if (s_rightGripPressCount > 0)
                    {
                    }
                    s_rightGripPressCount = 0;
                    s_rightGripSpamWindowTimer = 0.0f;
                }
            }


            // ============================================
                // UPDATE TRIGGER SPAM TIMERS (Weapon Lock)
            // ============================================

                // Update LEFT controller trigger spam window timer
            if (s_leftTriggerPressCount > 0)
            {
                s_leftTriggerSpamWindowTimer += frameTime;
                // Reset if window expired
                if (s_leftTriggerSpamWindowTimer > triggerSpamWindow)
                {
                    if (s_leftTriggerPressCount > 0)
                    {
                    }
                    s_leftTriggerPressCount = 0;
                    s_leftTriggerSpamWindowTimer = 0.0f;
                }
            }

            // Update RIGHT controller trigger spam window timer
            if (s_rightTriggerPressCount > 0)
            {
                s_rightTriggerSpamWindowTimer += frameTime;
                // Reset if window expired
                if (s_rightTriggerSpamWindowTimer > triggerSpamWindow)
                {
                    if (s_rightTriggerPressCount > 0)
                    {
                    }
                    s_rightTriggerPressCount = 0;
                    s_rightTriggerSpamWindowTimer = 0.0f;
                }
            }
            // Check shoulder zone proximity
            CheckShoulderZones();

            // Debug logging removed - use temporary _MESSAGE when investigating input

            // ============================================
            // Dual-trigger + main-hand 2H HIGGS grab: restore left game hand
            // Only when LEFT VR trigger was held first, then RIGHT VR trigger
            // also held, main-hand controller is grabbing a 2H weapon, and the
            // left game hand weapon was wrongly left unequipped (cached for re-equip).
            // ============================================
            if (s_leftTriggerPressed && s_rightTriggerPressed &&
                s_leftTriggerBeforeRight && !s_dualTriggerLeftRestoreIssued && higgsInterface)
            {
                bool mainHandIsLeft = IsLeftHandedMode();
                bool mainHandVRControllerIsLeft = GameHandToVRController(mainHandIsLeft);

                bool mainHandGrabbing2H = false;
                TESObjectREFR* mainGrabbed = higgsInterface->GetGrabbedObject(mainHandVRControllerIsLeft);
                if (mainGrabbed && mainGrabbed->baseForm &&
                    EquipManager::IsTwoHandedWeapon(mainGrabbed->baseForm))
                {
                    mainHandGrabbing2H = true;
                }

                if (mainHandGrabbing2H)
                {
                    PlayerCharacter* player = *g_thePlayer;
                    EquipManager* equipMgr = EquipManager::GetSingleton();
                    UInt32 cachedLeftFormID = equipMgr->GetCachedWeaponFormID(true);

                    if (player && cachedLeftFormID != 0)
                    {
                        TESForm* leftEquipped = player->GetEquippedObject(true);
                        bool leftWeaponMissing = !leftEquipped ||
                            !EquipManager::IsWeapon(leftEquipped);

                        if (leftWeaponMissing)
                        {
                            s_pendingTriggerUnequipLeft = false;
                            s_triggerUnequipTimerLeft = 0.0f;
                            equipMgr->ScheduleForceReequip(true);
                            s_dualTriggerLeftRestoreIssued = true;
                        }
                    }
                }
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
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    _MESSAGE("VRInputHandler: LEFT hand dropped weapon reference is INVALID (exception) - clearing");
                    // REMOVED: ClearGrabbedWeapon(GameHandToVRController(true));
                    EquipManager::GetSingleton()->ClearDroppedWeaponRef(true);
                    EquipManager::GetSingleton()->ClearPendingReequip(true);
                    droppedWeaponLeft = nullptr;
                }

                if (droppedWeaponLeft && isValid)
                {
                    bool triggerNow = GameHandToVRController(true) ? s_leftTriggerPressed : s_rightTriggerPressed;
                    bool triggerWas = GameHandToVRController(true) ? s_leftTriggerWasPressed : s_rightTriggerWasPressed;
                    bool pressEdge = triggerNow && !triggerWas;
                    bool releaseEdge = !triggerNow && triggerWas;

                    bool doEquip = false;
                    if (!tapThenHoldGrabEquip)
                    {
                        // Default: simple trigger hold equips the grabbed weapon.
                        doEquip = triggerNow;
                    }
                    else
                    {
                        // Tap-then-hold gesture: tap (press+release), then press-and-hold to equip.
                        switch (s_grabEquipTapStateLeft)
                        {
                        case 0:  // idle - waiting for tap press
                            if (pressEdge)
                                s_grabEquipTapStateLeft = 1;
                            break;
                        case 1:  // tap pressed - waiting for tap release
                            if (releaseEdge)
                            {
                                s_grabEquipTapStateLeft = 2;
                                s_grabEquipTapTimerLeft = 0.0f;
                            }
                            break;
                        case 2:  // tap released - waiting for the hold press
                            if (pressEdge)
                            {
                                doEquip = true;
                                s_grabEquipTapStateLeft = 0;
                            }
                            else
                            {
                                s_grabEquipTapTimerLeft += 0.011f;
                                if (s_grabEquipTapTimerLeft > GRAB_EQUIP_TAP_WINDOW)
                                    s_grabEquipTapStateLeft = 0;  // window expired - reset
                            }
                            break;
                        }

                        if (doEquip)
                            _MESSAGE("[FalseEdgeVR] LEFT hand tap-then-hold equip gesture detected - equipping grabbed weapon");
                    }

                    if (doEquip)
                    {
                        EquipGrabbedWeaponForGameHand(true);
                    }
                }
            }
            else
            {
                // No weapon grabbed in left hand - reset the tap gesture state
                s_grabEquipTapStateLeft = 0;
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
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    _MESSAGE("VRInputHandler: RIGHT hand dropped weapon reference is INVALID (exception) - clearing");
                    // REMOVED: ClearGrabbedWeapon(GameHandToVRController(false));
                    EquipManager::GetSingleton()->ClearDroppedWeaponRef(false);
                    EquipManager::GetSingleton()->ClearPendingReequip(false);
                    droppedWeaponRight = nullptr;
                }

                if (droppedWeaponRight && isValid)
                {
                    bool triggerNow = GameHandToVRController(false) ? s_leftTriggerPressed : s_rightTriggerPressed;
                    bool triggerWas = GameHandToVRController(false) ? s_leftTriggerWasPressed : s_rightTriggerWasPressed;
                    bool pressEdge = triggerNow && !triggerWas;
                    bool releaseEdge = !triggerNow && triggerWas;

                    bool doEquip = false;
                    if (!tapThenHoldGrabEquip)
                    {
                        // Default: simple trigger hold equips the grabbed weapon.
                        doEquip = triggerNow;
                    }
                    else
                    {
                        // Tap-then-hold gesture: tap (press+release), then press-and-hold to equip.
                        switch (s_grabEquipTapStateRight)
                        {
                        case 0:  // idle - waiting for tap press
                            if (pressEdge)
                                s_grabEquipTapStateRight = 1;
                            break;
                        case 1:  // tap pressed - waiting for tap release
                            if (releaseEdge)
                            {
                                s_grabEquipTapStateRight = 2;
                                s_grabEquipTapTimerRight = 0.0f;
                            }
                            break;
                        case 2:  // tap released - waiting for the hold press
                            if (pressEdge)
                            {
                                doEquip = true;
                                s_grabEquipTapStateRight = 0;
                            }
                            else
                            {
                                s_grabEquipTapTimerRight += 0.011f;
                                if (s_grabEquipTapTimerRight > GRAB_EQUIP_TAP_WINDOW)
                                    s_grabEquipTapStateRight = 0;  // window expired - reset
                            }
                            break;
                        }

                        if (doEquip)
                            _MESSAGE("[FalseEdgeVR] RIGHT hand tap-then-hold equip gesture detected - equipping grabbed weapon");
                    }

                    if (doEquip)
                    {
                        EquipGrabbedWeaponForGameHand(false);
                    }
                }
            }
            else
            {
                // No weapon grabbed in right hand - reset the tap gesture state
                s_grabEquipTapStateRight = 0;
            }

            // During door/cell transition or while mounted, keep grabbed weapons equipped
            // and do not start holster timers.
            if (IsDoorTransitionGuardActive())
            {
                ForceEquipAllGrabbedWeapons();
            }

            if (IsDoorTransitionGuardActive() || (mountWeaponHandlingEnabled && IsPlayerMounted()))
                return;

            // ============================================
    // TRIGGER RELEASED - Start delayed unequip timer for EACH hand
      // ============================================
            bool leftVRTriggerWas = GameHandToVRController(true) ? s_leftTriggerWasPressed : s_rightTriggerWasPressed;
            bool leftVRTriggerNow = GameHandToVRController(true) ? s_leftTriggerPressed : s_rightTriggerPressed;

            // Get which VR controller corresponds to left game hand (for weapon lock check)
            bool leftGameHandVRController = GameHandToVRController(true);
            bool leftWeaponIsLocked = leftGameHandVRController ? s_leftWeaponLocked : s_rightWeaponLocked;

            if (!leftVRTriggerNow && leftVRTriggerWas && !droppedWeaponLeft &&
                !VRInputHandler::IsOppositeGrip2HActiveForGameHand(true))
            {
                // Check if left hand has an equipped weapon
                PlayerCharacter* player = *g_thePlayer;
                if (player)
                {
                    TESForm* leftEquipped = player->GetEquippedObject(true);
                    if (leftEquipped && EquipManager::IsWeapon(leftEquipped))
                    {
                        // Check if weapon is locked - if so, don't start unequip timer
                        if (leftWeaponIsLocked)
                        {
                            // NOTE: physics thread — no GetName() (can crash in game form code).
                            _MESSAGE("[FalseEdgeVR] Trigger released with weapon lock (still equipped) in LEFT game hand: 0x%08X",
                                leftEquipped->formID);
                        }
                        else
                        {
                            s_pendingTriggerUnequipLeft = true;
                            s_triggerUnequipTimerLeft = 0.0f;
                        }
                    }
                }
            }

            // RIGHT hand trigger released
            bool rightVRTriggerWas = GameHandToVRController(false) ? s_leftTriggerWasPressed : s_rightTriggerWasPressed;
            bool rightVRTriggerNow = GameHandToVRController(false) ? s_leftTriggerPressed : s_rightTriggerPressed;

            // Get which VR controller corresponds to right game hand (for weapon lock check)
            bool rightGameHandVRController = GameHandToVRController(false);
            bool rightWeaponIsLocked = rightGameHandVRController ? s_leftWeaponLocked : s_rightWeaponLocked;

            if (!rightVRTriggerNow && rightVRTriggerWas && !droppedWeaponRight &&
                !VRInputHandler::IsOppositeGrip2HActiveForGameHand(false))
            {
                // Check if right hand has an equipped weapon
                PlayerCharacter* player = *g_thePlayer;
                if (player)
                {
                    TESForm* rightEquipped = player->GetEquippedObject(false);
                    if (rightEquipped && EquipManager::IsWeapon(rightEquipped))
                    {
                        // Check if weapon is locked - if so, don't start unequip timer
                        if (rightWeaponIsLocked)
                        {
                            // NOTE: physics thread — no GetName() (can crash in game form code).
                            _MESSAGE("[FalseEdgeVR] Trigger released with weapon lock (still equipped) in RIGHT game hand: 0x%08X",
                                rightEquipped->formID);
                        }
                        else
                        {
                            s_pendingTriggerUnequipRight = true;
                            s_triggerUnequipTimerRight = 0.0f;
                        }
                    }
                }
            }

            // ============================================
                // CANCEL + PROCESS pending unequip - handled independently per hand
                // ============================================
            for (int handIdx = 0; handIdx < 2; handIdx++)
            {
                bool isLeftHand = (handIdx == 0);
                bool& pending = isLeftHand ? s_pendingTriggerUnequipLeft : s_pendingTriggerUnequipRight;
                if (!pending)
                    continue;

                float& timer = isLeftHand ? s_triggerUnequipTimerLeft : s_triggerUnequipTimerRight;

                // Opposite grip owns this temporary equipped state.  Cancel any
                // stale trigger-release timer instead of allowing it to holster
                // the weapon out from under the support hand.
                if (VRInputHandler::IsOppositeGrip2HActiveForGameHand(isLeftHand))
                {
                    pending = false;
                    timer = 0.0f;
                    continue;
                }

                // CANCEL if this hand's own trigger is pressed again
                bool handVRController = GameHandToVRController(isLeftHand);
                bool handTrigger = handVRController ? s_leftTriggerPressed : s_rightTriggerPressed;
                if (handTrigger)
                {
                    pending = false;
                    timer = 0.0f;
                    continue;
                }

                // PROCESS after delay (staffs use a longer fire-and-forget window)
                timer += 0.011f;  // ~90fps
                if (timer < GetTriggerUnequipDelayForHand(isLeftHand))
                    continue;

                if (IsDoorTransitionGuardActive())
                    continue;

                pending = false;
                timer = 0.0f;

                // Final check: is weapon now locked? (player may have spammed trigger during delay)
                bool weaponNowLocked = handVRController ? s_leftWeaponLocked : s_rightWeaponLocked;
                if (weaponNowLocked)
                {
                    continue;
                }

                PlayerCharacter* player = *g_thePlayer;
                if (player)
                {
                    TESForm* handEquipped = player->GetEquippedObject(isLeftHand);
                    if (handEquipped && EquipManager::IsWeapon(handEquipped))
                    {
                        EquipManager::GetSingleton()->ForceUnequipAndGrab(isLeftHand);
                    }
                }
            }
        }

        // Log left-hand weapon + right trigger + right-hand 2H HIGGS grab combo
        {
            static bool s_prevRightGameHandTrigger = false;
            bool rightGameHandTrigger = GameHandToVRController(false)
                ? s_leftTriggerPressed : s_rightTriggerPressed;
            if (rightGameHandTrigger && !s_prevRightGameHandTrigger)
            {
                EquipManager::GetSingleton()->TryLog2HLeftHandWithRightGameHandTrigger(nullptr);
            }
            s_prevRightGameHandTrigger = rightGameHandTrigger;
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
    
    // Grip button accessors
    bool VRInputHandler::IsLeftGripPressed() { return s_leftGripPressed; }
    bool VRInputHandler::IsRightGripPressed() { return s_rightGripPressed; }
    
    // Check if grip is pressed for the VR controller corresponding to a game hand
    bool VRInputHandler::IsGripHeldForGameHand(bool isLeftGameHand)
    {
        bool isLeftVRController = GameHandToVRController(isLeftGameHand);
  return isLeftVRController ? s_leftGripPressed : s_rightGripPressed;
    }
    
    // ============================================
    // Drop Protection Override Accessors
    // ============================================

    bool VRInputHandler::IsDropProtectionDisabled(bool isLeftVRController)
    {
      return isLeftVRController ? s_leftDropProtectionDisabled : s_rightDropProtectionDisabled;
    }
    
    float VRInputHandler::GetDropProtectionDisableTimeRemaining(bool isLeftVRController)
    {
   return isLeftVRController ? s_leftDropProtectionDisableTimer : s_rightDropProtectionDisableTimer;
    }
    
    // ============================================
    // Weapon Lock Accessors
    // ============================================

    bool VRInputHandler::IsWeaponLocked(bool isLeftVRController)
    {
return isLeftVRController ? s_leftWeaponLocked : s_rightWeaponLocked;
    }
    
  void VRInputHandler::ClearWeaponLock(bool isLeftVRController)
    {
       if (isLeftVRController)
     {
       if (s_leftWeaponLocked)
   {
    }
  s_leftWeaponLocked = false;
       s_leftTriggerPressCount = 0;
       s_leftTriggerSpamWindowTimer = 0.0f;
        }
     else
        {
      if (s_rightWeaponLocked)
   {
    }
  s_rightWeaponLocked = false;
   s_rightTriggerPressCount = 0;
 s_rightTriggerSpamWindowTimer = 0.0f;
        }
    }
    
    void VRInputHandler::RegisterTriggerCallback()
    {
    }
}

