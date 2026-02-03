#include "WeaponGeometry.h"
#include "Engine.h"
#include "EquipManager.h"
#include "VRInputHandler.h"
#include "config.h"
#include "skse64/GameRTTI.h"
#include "skse64/NiNodes.h"
#include <cmath>
#include <cfloat>

namespace FalseEdgeVR
{
    // Static constant for blade radius (thickness)
    const float WeaponGeometryTracker::BLADE_RADIUS = 2.0f;  // Approximate blade thickness in units

    // ============================================
    // WeaponGeometryTracker Implementation
    // ============================================

 WeaponGeometryTracker* WeaponGeometryTracker::GetSingleton()
    {
        static WeaponGeometryTracker instance;
        return &instance;
    }

    void WeaponGeometryTracker::Initialize()
    {
  if (m_initialized)
        return;

        LOG("WeaponGeometryTracker: Initializing...");
        
m_geometryState.leftHand.Clear();
        m_geometryState.rightHand.Clear();
        m_lastCollision.Clear();
      m_bladesInContact = false;
    m_wasInContact = false;
        m_collisionImminent = false;
    m_wasImminent = false;
    m_bladesGrinding = false;
      m_wasGrinding = false;
        m_grindStartTime = 0.0f;
        m_grindDuration = 0.0f;
        m_framesSinceEquipChange = 0;
        m_lastUpdateTime = 0.0f;
   
        // Load thresholds from config
        m_collisionThreshold = bladeCollisionThreshold;
      m_imminentThreshold = bladeImminentThreshold;
        
        _MESSAGE("WeaponGeometryTracker: Collision threshold: %.2f, Imminent threshold: %.2f", 
            m_collisionThreshold, m_imminentThreshold);
        _MESSAGE("WeaponGeometryTracker: Raycasting enabled with %d samples per blade, blade radius: %.2f",
      RAYCAST_SAMPLES, BLADE_RADIUS);
        
        m_initialized = true;
LOG("WeaponGeometryTracker: Initialized successfully");
    }

    void WeaponGeometryTracker::Update(float deltaTime)
    {
   static int updateCount = 0;
   static bool loggedOnce = false;
        
        if (!m_initialized)
          return;

    updateCount++;
     
        // Track time for grinding duration
 m_lastUpdateTime += deltaTime;
 
        // Log once to confirm update is being called
  if (!loggedOnce)
    {
       _MESSAGE("WeaponGeometryTracker::Update - First update call!");
loggedOnce = true;
        }

     PlayerCharacter* player = *g_thePlayer;
        if (!player || !player->loadedState)
    return;

      const PlayerEquipState& equipState = EquipManager::GetSingleton()->GetEquipState();
      
        // DIRECT check for shields - more reliable than EquipManager state
   TESForm* leftEquipped = player->GetEquippedObject(true);
   TESForm* rightEquipped = player->GetEquippedObject(false);
        bool leftIsShield = leftEquipped && EquipManager::IsShield(leftEquipped);
 bool rightIsShield = rightEquipped && EquipManager::IsShield(rightEquipped);
        
        // Check for equipment changes - reset grace period if weapons changed
        // Note: Ignore shields - they are handled by ShieldCollisionTracker
   UInt32 currentLeftFormID = (leftEquipped && !leftIsShield) ? leftEquipped->formID : 0;
        UInt32 currentRightFormID = (rightEquipped && !rightIsShield) ? rightEquipped->formID : 0;
    
     if (currentLeftFormID != m_lastLeftWeaponFormID || currentRightFormID != m_lastRightWeaponFormID)
   {
        _MESSAGE("WeaponGeometryTracker: Equipment changed! Left: %08X->%08X, Right: %08X->%08X",
    m_lastLeftWeaponFormID, currentLeftFormID,
    m_lastRightWeaponFormID, currentRightFormID);
     _MESSAGE("WeaponGeometryTracker: Starting %d frame grace period before collision detection", equipGraceFrames);
            
            m_lastLeftWeaponFormID = currentLeftFormID;
   m_lastRightWeaponFormID = currentRightFormID;
m_framesSinceEquipChange = 0;
          
    // Clear geometry to force fresh calculation
         m_geometryState.leftHand.Clear();
     m_geometryState.rightHand.Clear();
            m_bladesInContact = false;
  m_wasInContact = false;
    m_collisionImminent = false;
            m_wasImminent = false;
 }
 
        // Increment frame counter
   m_framesSinceEquipChange++;
        
   // Check if off-hand has HIGGS-grabbed weapon (from our collision avoidance)
// Off-hand is determined by INI setting CollisionAvoidanceHand (0=left, 1=right)
   bool offHandIsLeft = GetCollisionAvoidanceHandIsLeft();
   bool offHandVRControllerIsLeft = GameHandToVRController(offHandIsLeft);
   bool offHandHiggsGrabbed = false;
   TESObjectREFR* higgsHeldOffHand = nullptr;

   // Debug: Log handedness mode periodically
   static int handednessLogCounter = 0;
   handednessLogCounter++;
   if (handednessLogCounter % 500 == 1)
 {
       _MESSAGE("WeaponGeometry: IsLeftHandedMode()=%s, offHandIsLeft=%s, offHandVRControllerIsLeft=%s",
     IsLeftHandedMode() ? "YES" : "NO",
  offHandIsLeft ? "YES" : "NO",
           offHandVRControllerIsLeft ? "YES" : "NO");
   }

   if (EquipManager::GetSingleton()->HasPendingReequip(offHandIsLeft))
   {
       // Get the dropped weapon reference we created
       higgsHeldOffHand = EquipManager::GetSingleton()->GetDroppedWeaponRef(offHandIsLeft);
       if (higgsHeldOffHand)
       {
           // Check if HIGGS is actually holding it
           if (higgsInterface)
           {
               TESObjectREFR* currentlyHeld = higgsInterface->GetGrabbedObject(offHandVRControllerIsLeft);
               if (currentlyHeld == higgsHeldOffHand)
               {
                   offHandHiggsGrabbed = true;
               }
           }
       }
   }

   // Debug logging for HIGGS state
   static bool loggedHiggsState = false;
   if (EquipManager::GetSingleton()->HasPendingReequip(offHandIsLeft) && !loggedHiggsState)
   {
       _MESSAGE("WeaponGeometry: Pending reequip - DroppedRef: %p, HIGGS holding: %s",
           higgsHeldOffHand, offHandHiggsGrabbed ? "YES" : "NO");
       loggedHiggsState = true;
   }
   
   // Update left hand geometry - skip if shield (ShieldCollisionTracker handles that)
   if (leftEquipped && !leftIsShield)
   {
       // Normal equipped weapon
       UpdateHandGeometry(true, deltaTime);
   }
   else if (offHandHiggsGrabbed && higgsHeldOffHand && offHandIsLeft)
   {
       // HIGGS-grabbed weapon in left hand - update geometry from the grabbed object
       UpdateHiggsGrabbedGeometry(true, higgsHeldOffHand, deltaTime);
   }
   else
   {
       m_geometryState.leftHand.Clear();
   }

   // Update right hand if weapon equipped - skip if shield
   if (rightEquipped && !rightIsShield)
   {
       UpdateHandGeometry(false, deltaTime);
   }
   else if (offHandHiggsGrabbed && higgsHeldOffHand && !offHandIsLeft)
   {
       // HIGGS-grabbed weapon in right hand - update geometry from the grabbed object
       UpdateHiggsGrabbedGeometry(false, higgsHeldOffHand, deltaTime);
   }
   else
   {
       m_geometryState.rightHand.Clear();
   }
        
        // Check for blade collision if both hands have valid weapons
    // Also verify geometry is actually reasonable (not at origin)
        bool leftGeomValid = m_geometryState.leftHand.isValid && 
      (m_geometryState.leftHand.bladeLength > 1.0f) &&
       (fabs(m_geometryState.leftHand.basePosition.x) > 0.1f ||
         fabs(m_geometryState.leftHand.basePosition.y) > 0.1f ||
   fabs(m_geometryState.leftHand.basePosition.z) > 0.1f);
     
  bool rightGeomValid = m_geometryState.rightHand.isValid && 
      (m_geometryState.rightHand.bladeLength > 1.0f) &&
    (fabs(m_geometryState.rightHand.basePosition.x) > 0.1f ||
  fabs(m_geometryState.rightHand.basePosition.y) > 0.1f ||
   fabs(m_geometryState.rightHand.basePosition.z) > 0.1f);

        if (leftGeomValid && rightGeomValid)
   {
            // Log once when both weapons are valid (including HIGGS grabbed)
       static bool loggedBothValid = false;
         static bool loggedHiggsTracking = false;
            
  if (!loggedBothValid && !offHandHiggsGrabbed)
{
      _MESSAGE("WeaponGeometryTracker: Both hands have valid geometry!");
     loggedBothValid = true;
   }
  
  if (offHandHiggsGrabbed && !loggedHiggsTracking)
   {
  _MESSAGE("WeaponGeometryTracker: Tracking HIGGS-grabbed weapon + equipped weapon");
      _MESSAGE("  Left (HIGGS):  Base(%.1f, %.1f, %.1f) Tip(%.1f, %.1f, %.1f)",
           m_geometryState.leftHand.basePosition.x,
       m_geometryState.leftHand.basePosition.y,
   m_geometryState.leftHand.basePosition.z,
               m_geometryState.leftHand.tipPosition.x,
    m_geometryState.leftHand.tipPosition.y,
       m_geometryState.leftHand.tipPosition.z);
  _MESSAGE("  Right (Equipped): Base(%.1f, %.1f, %.1f) Tip(%.1f, %.1f, %.1f)",
     m_geometryState.rightHand.basePosition.x,
   m_geometryState.rightHand.basePosition.y,
         m_geometryState.rightHand.basePosition.z,
   m_geometryState.rightHand.tipPosition.x,
          m_geometryState.rightHand.tipPosition.y,
         m_geometryState.rightHand.tipPosition.z);
           loggedHiggsTracking = true;
     }
     
m_wasInContact = m_bladesInContact;
            m_wasImminent = m_collisionImminent;
          m_wasGrinding = m_bladesGrinding;
          
            BladeCollisionResult collision;
   bool detected = CheckBladeCollision(collision);
    
         // Log distance periodically when HIGGS grabbed
    static int distanceLogCounter = 0;
  if (offHandHiggsGrabbed)
            {
          distanceLogCounter++;
             if (distanceLogCounter % 100 == 1)
    {
   _MESSAGE("HIGGS Blade Distance Check: %.2f (touch: %.2f, imminent: %.2f, rayHits: %d)",
  collision.closestDistance, m_collisionThreshold, m_imminentThreshold, collision.raycastHitCount);
 }
   }
            
    // Update collision state
m_bladesInContact = collision.isColliding;
       m_collisionImminent = collision.isImminent;
         m_bladesGrinding = collision.isGrinding;
            
            if (m_bladesInContact)
           {
   m_lastCollision = collision;

     // Fire collision callback if blades just came into contact
         if (!m_wasInContact && m_collisionCallback)
      {
  m_collisionCallback(collision);
              }
    
            // Log collision event (only on initial contact)
      if (!m_wasInContact)
               {
     _MESSAGE("WeaponGeometry: === BLADES TOUCHING === (HIGGS grabbed: %s, Grinding: %s)", 
    offHandHiggsGrabbed ? "YES" : "NO",
    collision.isGrinding ? "YES" : "NO");
           _MESSAGE("  Collision Point: (%.2f, %.2f, %.2f)",
             collision.collisionPoint.x,
       collision.collisionPoint.y,
    collision.collisionPoint.z);
_MESSAGE("  Distance: %.2f, Raycast Hits: %d", collision.closestDistance, collision.raycastHitCount);
     }
  
       // Log when grinding starts
                if (collision.isGrinding && !m_wasGrinding)
               {
           _MESSAGE("WeaponGeometry: *** GRINDING STARTED *** (duration: %.2fs, velocity: %.1f)",
         collision.grindDuration, collision.relativeVelocity);
    }

         // Check for X-POSE every frame while blades are touching
             CheckXPose(m_geometryState.leftHand, m_geometryState.rightHand);
    }
   else if (m_collisionImminent)
    {
   m_lastCollision = collision;
  
       // Fire imminent callback if collision just became imminent
     if (!m_wasImminent && !m_wasInContact && m_imminentCallback)
               {
     m_imminentCallback(collision);
       }
    
              // IMPORTANT: Don't trigger during grace period or if grinding was just happening
        bool offHandOnCooldown = VRInputHandler::GetSingleton()->IsHandOnCooldown(offHandIsLeft);
     bool withinBackupOnly = (collision.closestDistance <= bladeImminentThresholdBackup) && 
  (collision.closestDistance > m_imminentThreshold);
               bool inGracePeriod = (m_framesSinceEquipChange < equipGraceFrames);
            bool wasJustGrinding = m_wasGrinding;  // Don't trigger immediately after grinding stops
       
         if (inGracePeriod)
       {
static bool loggedGracePeriod = false;
      if (!loggedGracePeriod)
   {
   _MESSAGE("WeaponGeometryTracker: In grace period (%d/%d frames) - collision detection disabled",
    m_framesSinceEquipChange, equipGraceFrames);
   loggedGracePeriod = true;
       }
          }
else if (wasJustGrinding)
               {
          // Don't trigger unequip right after grinding - allow smooth transition
       static bool loggedGrindTransition = false;
        if (!loggedGrindTransition)
           {
       _MESSAGE("WeaponGeometry: Just stopped grinding - allowing smooth transition, not triggering unequip");
    loggedGrindTransition = true;
     }
      }
     else if (!m_wasImminent && !m_wasInContact && !offHandHiggsGrabbed && 
  (!offHandOnCooldown || withinBackupOnly))
                {
  static bool loggedGracePeriod = false;
           loggedGracePeriod = false;
             static bool loggedGrindTransition = false;
       loggedGrindTransition = false;
   
      if (VRInputHandler::GetSingleton()->IsInCloseCombatMode())
            {
          static bool loggedCloseCombatSkip = false;
            if (!loggedCloseCombatSkip)
       {
           _MESSAGE("WeaponGeometry: Collision imminent but CLOSE COMBAT MODE active - skipping unequip");
        loggedCloseCombatSkip = true;
           }
          }
        else if (VRInputHandler::IsLeftTriggerPressed() || VRInputHandler::IsRightTriggerPressed())
         {
      static bool loggedTriggerSkip = false;
          if (!loggedTriggerSkip)
       {
          _MESSAGE("WeaponGeometry: Collision imminent but TRIGGER HELD - skipping unequip");
   loggedTriggerSkip = true;
      }
      }
        else
  {
  static bool loggedCloseCombatSkip = false;
         loggedCloseCombatSkip = false;
           static bool loggedTriggerSkip = false;
     loggedTriggerSkip = false;
   
          _MESSAGE("WeaponGeometry: COLLISION IMMINENT!%s", withinBackupOnly ? " (BACKUP THRESHOLD)" : "");
      _MESSAGE("  Distance: %.2f, Time to collision: %.3f sec, Raycast Hits: %d",
         collision.closestDistance,
           collision.timeToCollision,
       collision.raycastHitCount);
     
    bool offHandIsLeft = GetCollisionAvoidanceHandIsLeft();
       _MESSAGE("WeaponGeometry: Triggering game %s hand unequip + HIGGS grab!", 
       offHandIsLeft ? "LEFT" : "RIGHT");
      EquipManager::GetSingleton()->ForceUnequipAndGrab(offHandIsLeft);
   }
                }
          else if (!offHandOnCooldown && !inGracePeriod && !wasJustGrinding)
     {
static bool loggedCooldownSkip = false;
      loggedCooldownSkip = false;
          }
            }
            else
            {
       // Blades no longer colliding or imminent
         if (m_wasInContact)
     {
        _MESSAGE("WeaponGeometry: === BLADES NO LONGER TOUCHING === (HIGGS grabbed: %s)",
     offHandHiggsGrabbed ? "YES" : "NO");
        _MESSAGE("  Current distance: %.2f (threshold: %.2f)", 
collision.closestDistance, m_collisionThreshold);
        
          if (m_wasGrinding)
     {
     _MESSAGE("WeaponGeometry: *** GRINDING ENDED *** (total duration: %.2fs)",
         m_grindDuration);
  }
     
     if (m_inXPose)
      {
          _MESSAGE("WeaponGeometry: *** X-POSE ENDED *** (blades separated)");
             m_inXPose = false;
       }

     if (IsBlocking())
    {
         _MESSAGE("WeaponGeometry: Stopping block (blades separated)");
        StopBlocking();
    }
    }
           
     m_lastCollision.Clear();
      }
        }
    }

    // Update geometry for a HIGGS-grabbed weapon
    void WeaponGeometryTracker::UpdateHiggsGrabbedGeometry(bool isLeftHand, TESObjectREFR* grabbedRef, float deltaTime)
    {
        BladeGeometry& geometry = isLeftHand ? m_geometryState.leftHand : m_geometryState.rightHand;

        // Store previous positions for velocity calculation
        geometry.prevTipPosition = geometry.tipPosition;
        geometry.prevBasePosition = geometry.basePosition;
 
        if (!grabbedRef)
        {
   static bool loggedNoRef = false;
      if (!loggedNoRef)
          {
        _MESSAGE("UpdateHiggsGrabbedGeometry: No grabbed ref!");
            loggedNoRef = true;
        }
        geometry.isValid = false;
     return;
        }

        // Get the 3D node of the grabbed object
        NiNode* objectNode = grabbedRef->GetNiNode();
     if (!objectNode)
        {
    static bool loggedNoNode = false;
       if (!loggedNoNode)
 {
        _MESSAGE("UpdateHiggsGrabbedGeometry: No NiNode for grabbed object!");
           loggedNoNode = true;
          }
    geometry.isValid = false;
        return;
        }

        // Get weapon info from the base form
        TESForm* baseForm = grabbedRef->baseForm;
        TESObjectWEAP* weapon = DYNAMIC_CAST(baseForm, TESForm, TESObjectWEAP);
     
        if (!weapon)
        {
     static bool loggedNoWeapon = false;
            if (!loggedNoWeapon)
            {
                _MESSAGE("UpdateHiggsGrabbedGeometry: Base form is not a weapon!");
        loggedNoWeapon = true;
  }
            geometry.isValid = false;
       return;
        }

  // Calculate blade positions from the grabbed object's transform
        float reach = weapon->gameData.reach;
   float bladeLength = reach * 70.0f;
   
        // Base position is the object's world position
        geometry.basePosition = objectNode->m_worldTransform.pos;
 
        // Get blade direction from object's rotation
        NiMatrix33& rot = objectNode->m_worldTransform.rot;
        NiPoint3 bladeDirection(
            rot.data[0][1],
  rot.data[1][1],
     rot.data[2][1]
        );
     
      float dirLength = sqrt(bladeDirection.x * bladeDirection.x +
            bladeDirection.y * bladeDirection.y +
            bladeDirection.z * bladeDirection.z);
 if (dirLength > 0.0001f)
        {
   bladeDirection.x /= dirLength;
    bladeDirection.y /= dirLength;
         bladeDirection.z /= dirLength;
        }

        // Tip position
        geometry.tipPosition.x = geometry.basePosition.x + bladeDirection.x * bladeLength;
        geometry.tipPosition.y = geometry.basePosition.y + bladeDirection.y * bladeLength;
        geometry.tipPosition.z = geometry.basePosition.z + bladeDirection.z * bladeLength;
    
   // Calculate blade length
        NiPoint3 bladeVector;
        bladeVector.x = geometry.tipPosition.x - geometry.basePosition.x;
        bladeVector.y = geometry.tipPosition.y - geometry.basePosition.y;
        bladeVector.z = geometry.tipPosition.z - geometry.basePosition.z;
        geometry.bladeLength = sqrt(bladeVector.x * bladeVector.x + 
bladeVector.y * bladeVector.y + 
          bladeVector.z * bladeVector.z);
  
        // Calculate velocities (units per second)
 if (deltaTime > 0.0f && (geometry.prevTipPosition.x != 0.0f || 
            geometry.prevTipPosition.y != 0.0f || 
            geometry.prevTipPosition.z != 0.0f))
        {
          geometry.tipVelocity.x = (geometry.tipPosition.x - geometry.prevTipPosition.x) / deltaTime;
    geometry.tipVelocity.y = (geometry.tipPosition.y - geometry.prevTipPosition.y) / deltaTime;
        geometry.tipVelocity.z = (geometry.tipPosition.z - geometry.prevTipPosition.z) / deltaTime;
        
            geometry.baseVelocity.x = (geometry.basePosition.x - geometry.prevBasePosition.x) / deltaTime;
   geometry.baseVelocity.y = (geometry.basePosition.y - geometry.prevBasePosition.y) / deltaTime;
         geometry.baseVelocity.z = (geometry.basePosition.z - geometry.prevBasePosition.z) / deltaTime;
    }
      
        geometry.isValid = true;
        
        // Log periodically to confirm tracking is working
        static int logCounter = 0;
logCounter++;
        if (logCounter % 400 == 1)  // Log every 500 frames
        {
            _MESSAGE("HIGGS Grabbed Geometry Update - Base(%.1f, %.1f, %.1f) Tip(%.1f, %.1f, %.1f) Length: %.1f",
       geometry.basePosition.x, geometry.basePosition.y, geometry.basePosition.z,
     geometry.tipPosition.x, geometry.tipPosition.y, geometry.tipPosition.z,
     geometry.bladeLength);
        }
    }

    void WeaponGeometryTracker::UpdateHandGeometry(bool isLeftHand, float deltaTime)
    {
        BladeGeometry& geometry = isLeftHand ? m_geometryState.leftHand : m_geometryState.rightHand;

        // Store previous positions for velocity calculation
        geometry.prevTipPosition = geometry.tipPosition;
        geometry.prevBasePosition = geometry.basePosition;
        
        // Get the weapon node
        NiAVObject* weaponNode = GetWeaponNode(isLeftHand);
        if (!weaponNode)
        {
    static bool loggedLeftFail = false;
      static bool loggedRightFail = false;
if (isLeftHand && !loggedLeftFail)
            {
    _MESSAGE("WeaponGeometryTracker: Failed to get LEFT weapon node!");
         loggedLeftFail = true;
         }
          else if (!isLeftHand && !loggedRightFail)
      {
            _MESSAGE("WeaponGeometryTracker: Failed to get RIGHT weapon node!");
              loggedRightFail = true;
 }
  geometry.isValid = false;
            return;
        }
        
        // Get the equipped weapon form
        PlayerCharacter* player = *g_thePlayer;
 TESForm* equippedForm = player->GetEquippedObject(isLeftHand);
      TESObjectWEAP* weapon = DYNAMIC_CAST(equippedForm, TESForm, TESObjectWEAP);
      
        if (!weapon)
    {
      static bool loggedLeftNoWeap = false;
            static bool loggedRightNoWeap = false;
   if (isLeftHand && !loggedLeftNoWeap)
     {
 _MESSAGE("WeaponGeometryTracker: LEFT hand - no weapon form (FormID: %08X, Type: %d)", 
        equippedForm ? equippedForm->formID : 0,
        equippedForm ? equippedForm->formType : -1);
  loggedLeftNoWeap = true;
          }
      else if (!isLeftHand && !loggedRightNoWeap)
     {
                _MESSAGE("WeaponGeometryTracker: RIGHT hand - no weapon form (FormID: %08X, Type: %d)", 
    equippedForm ? equippedForm->formID : 0,
         equippedForm ? equippedForm->formType : -1);
     loggedRightNoWeap = true;
     }
            geometry.isValid = false;
  return;
        }
 
        // Log success once per hand
        static bool loggedLeftSuccess = false;
        static bool loggedRightSuccess = false;
        if (isLeftHand && !loggedLeftSuccess)
        {
_MESSAGE("WeaponGeometryTracker: LEFT hand - Got weapon node and form! Reach: %.2f", weapon->gameData.reach);
 loggedLeftSuccess = true;
        }
      else if (!isLeftHand && !loggedRightSuccess)
    {
            _MESSAGE("WeaponGeometryTracker: RIGHT hand - Got weapon node and form! Reach: %.2f", weapon->gameData.reach);
  loggedRightSuccess = true;
  }
        
        // Calculate blade positions
        geometry.basePosition = CalculateBladeBase(weaponNode, isLeftHand);
        geometry.tipPosition = CalculateBladeTip(weaponNode, weapon, isLeftHand);
      
        // Calculate blade length
        NiPoint3 bladeVector;
 bladeVector.x = geometry.tipPosition.x - geometry.basePosition.x;
        bladeVector.y = geometry.tipPosition.y - geometry.basePosition.y;
      bladeVector.z = geometry.tipPosition.z - geometry.basePosition.z;
     geometry.bladeLength = sqrt(bladeVector.x * bladeVector.x + 
   bladeVector.y * bladeVector.y + 
         bladeVector.z * bladeVector.z);
  
        // Calculate velocities (units per second)
        if (deltaTime > 0.0f && (geometry.prevTipPosition.x != 0.0f || 
  geometry.prevTipPosition.y != 0.0f || 
     geometry.prevTipPosition.z != 0.0f))
  {
            geometry.tipVelocity.x = (geometry.tipPosition.x - geometry.prevTipPosition.x) / deltaTime;
            geometry.tipVelocity.y = (geometry.tipPosition.y - geometry.prevTipPosition.y) / deltaTime;
            geometry.tipVelocity.z = (geometry.tipPosition.z - geometry.prevTipPosition.z) / deltaTime;
        
            geometry.baseVelocity.x = (geometry.basePosition.x - geometry.prevBasePosition.x) / deltaTime;
  geometry.baseVelocity.y = (geometry.basePosition.y - geometry.prevBasePosition.y) / deltaTime;
         geometry.baseVelocity.z = (geometry.basePosition.z - geometry.prevBasePosition.z) / deltaTime;
        }
      
        geometry.isValid = true;
    }

    NiAVObject* WeaponGeometryTracker::GetWeaponNode(bool isLeftHand)
    {
        PlayerCharacter* player = *g_thePlayer;
        if (!player || !player->loadedState)
        {
        static bool loggedNoPlayer = false;
      if (!loggedNoPlayer)
            {
      _MESSAGE("WeaponGeometryTracker::GetWeaponNode - No player or loadedState!");
         loggedNoPlayer = true;
 }
        return nullptr;
        }

        NiNode* rootNode = player->GetNiRootNode(0); // First person root
        if (!rootNode)
{
        rootNode = player->GetNiRootNode(1); // Third person root
     }
        
    if (!rootNode)
   {
            static bool loggedNoRoot = false;
      if (!loggedNoRoot)
        {
 _MESSAGE("WeaponGeometryTracker::GetWeaponNode - No root node found!");
                loggedNoRoot = true;
            }
        return nullptr;
        }

        // Log available nodes once for debugging
        static bool loggedNodes = false;
  if (!loggedNodes)
        {
            _MESSAGE("WeaponGeometryTracker: Root node found: %s", rootNode->m_name ? rootNode->m_name : "unnamed");
     _MESSAGE("WeaponGeometryTracker: Searching for weapon offset nodes...");
  loggedNodes = true;
      }

        const char* nodeName = GetWeaponOffsetNodeName(isLeftHand);
      
        BSFixedString nodeNameStr(nodeName);
        NiAVObject* weaponNode = rootNode->GetObjectByName(&nodeNameStr.data);
        
        if (!weaponNode)
        {
        static bool loggedLeftNotFound = false;
         static bool loggedRightNotFound = false;
   if (isLeftHand && !loggedLeftNotFound)
            {
     _MESSAGE("WeaponGeometryTracker: Node '%s' NOT FOUND in skeleton!", nodeName);
    loggedLeftNotFound = true;
 }
            else if (!isLeftHand && !loggedRightNotFound)
      {
       _MESSAGE("WeaponGeometryTracker: Node '%s' NOT FOUND in skeleton!", nodeName);
   loggedRightNotFound = true;
            }
        }
        
        return weaponNode;
    }

    const char* WeaponGeometryTracker::GetWeaponOffsetNodeName(bool isLeftHand)
    {
        // In Skyrim VR:
        // Left hand weapon node is called "SHIELD" (even for weapons, not just shields!)
        // Right hand weapon node is called "WEAPON"
        if (isLeftHand)
        {
return "SHIELD";
        }
        else
        {
            return "WEAPON";
     }
    }

    NiPoint3 WeaponGeometryTracker::CalculateBladeBase(NiAVObject* weaponNode, bool isLeftHand)
    {
        if (!weaponNode)
        return NiPoint3(0, 0, 0);

        return NiPoint3(
     weaponNode->m_worldTransform.pos.x,
 weaponNode->m_worldTransform.pos.y,
         weaponNode->m_worldTransform.pos.z
 );
    }

    NiPoint3 WeaponGeometryTracker::CalculateBladeTip(NiAVObject* weaponNode, TESObjectWEAP* weapon, bool isLeftHand)
    {
        if (!weaponNode || !weapon)
         return NiPoint3(0, 0, 0);

        float reach = weapon->gameData.reach;
        float bladeLength = reach * 70.0f;
        
        NiMatrix33& rot = weaponNode->m_worldTransform.rot;
        
        NiPoint3 bladeDirection(
            rot.data[0][1],
         rot.data[1][1],
            rot.data[2][1]
);
     
        float dirLength = sqrt(bladeDirection.x * bladeDirection.x +
  bladeDirection.y * bladeDirection.y +
         bladeDirection.z * bladeDirection.z);
        if (dirLength > 0.0001f)
        {
  bladeDirection.x /= dirLength;
     bladeDirection.y /= dirLength;
        bladeDirection.z /= dirLength;
    }
        
        NiPoint3 basePos = CalculateBladeBase(weaponNode, isLeftHand);

        return NiPoint3(
            basePos.x + bladeDirection.x * bladeLength,
            basePos.y + bladeDirection.y * bladeLength,
          basePos.z + bladeDirection.z * bladeLength
  );
    }

    const BladeGeometry& WeaponGeometryTracker::GetBladeGeometry(bool isLeftHand) const
    {
        return isLeftHand ? m_geometryState.leftHand : m_geometryState.rightHand;
    }

    void WeaponGeometryTracker::LogGeometryState()
    {
        if (m_geometryState.leftHand.isValid)
        {
    LOG("WeaponGeometry: Left Hand - Base(%.2f, %.2f, %.2f) Tip(%.2f, %.2f, %.2f) Length: %.2f",
       m_geometryState.leftHand.basePosition.x,
    m_geometryState.leftHand.basePosition.y,
             m_geometryState.leftHand.basePosition.z,
      m_geometryState.leftHand.tipPosition.x,
      m_geometryState.leftHand.tipPosition.y,
  m_geometryState.leftHand.tipPosition.z,
         m_geometryState.leftHand.bladeLength);
        }
        
        if (m_geometryState.rightHand.isValid)
        {
  LOG("WeaponGeometry: Right Hand - Base(%.2f, %.2f, %.2f) Tip(%.2f, %.2f, %.2f) Length: %.2f",
    m_geometryState.rightHand.basePosition.x,
          m_geometryState.rightHand.basePosition.y,
    m_geometryState.rightHand.basePosition.z,
         m_geometryState.rightHand.tipPosition.x,
  m_geometryState.rightHand.tipPosition.y,
           m_geometryState.rightHand.tipPosition.z,
           m_geometryState.rightHand.bladeLength);
        }
    }

    // ============================================
    // Blade Collision Detection
    // ============================================

    bool WeaponGeometryTracker::CheckBladeCollision(BladeCollisionResult& outResult)
    {
      outResult.Clear();
        
        if (!m_geometryState.leftHand.isValid || !m_geometryState.rightHand.isValid)
            return false;
        
   const BladeGeometry& leftBlade = m_geometryState.leftHand;
const BladeGeometry& rightBlade = m_geometryState.rightHand;
        
    const NiPoint3& leftBase = leftBlade.basePosition;
        const NiPoint3& leftTip = leftBlade.tipPosition;
   const NiPoint3& rightBase = rightBlade.basePosition;
const NiPoint3& rightTip = rightBlade.tipPosition;
    
   // ============================================
        // RAYCASTING COLLISION DETECTION
        // Cast rays along both blades to detect intersection
        // ============================================
      
        BladeRaycastHit leftHits[RAYCAST_SAMPLES];
        BladeRaycastHit rightHits[RAYCAST_SAMPLES];
   
        // Calculate blade radii based on blade length (daggers are thinner)
  float leftRadius = BLADE_RADIUS * (leftBlade.bladeLength / 70.0f);
        float rightRadius = BLADE_RADIUS * (rightBlade.bladeLength / 70.0f);
  float avgRadius = (leftRadius + rightRadius) * 0.5f;
        if (avgRadius < 1.0f) avgRadius = 1.0f;  // Minimum radius
     if (avgRadius > 3.0f) avgRadius = 3.0f;  // Maximum radius
    
        // Cast rays from left blade toward right blade
        int leftHitCount = RaycastBladeIntersection(leftBlade, rightBlade, avgRadius, leftHits, RAYCAST_SAMPLES);
     
        // Cast rays from right blade toward left blade
        int rightHitCount = RaycastBladeIntersection(rightBlade, leftBlade, avgRadius, rightHits, RAYCAST_SAMPLES);
        
        int totalHitCount = leftHitCount + rightHitCount;
        outResult.raycastHitCount = totalHitCount;
        
      // ============================================
        // ALSO CALCULATE SEGMENT DISTANCE (as backup/comparison)
        // ============================================
     float leftParam, rightParam;
   NiPoint3 closestLeft, closestRight;
      
        float segmentDistance = ClosestDistanceBetweenSegments(
      leftBase, leftTip,
   rightBase, rightTip,
  leftParam, rightParam,
            closestLeft, closestRight
        );

        outResult.closestDistance = segmentDistance;
        outResult.leftBladeParameter = leftParam;
        outResult.rightBladeParameter = rightParam;
        outResult.leftBladeContactPoint = closestLeft;
        outResult.rightBladeContactPoint = closestRight;
        
   outResult.collisionPoint.x = (closestLeft.x + closestRight.x) * 0.5f;
        outResult.collisionPoint.y = (closestLeft.y + closestRight.y) * 0.5f;
        outResult.collisionPoint.z = (closestLeft.z + closestRight.z) * 0.5f;
        
 // If raycast detected hits, use the closest hit point
        if (totalHitCount > 0)
      {
  // Find the closest hit among all rays
   float closestHitDist = FLT_MAX;
            NiPoint3 closestHitPoint;
      
      for (int i = 0; i < leftHitCount; i++)
  {
      if (leftHits[i].hit && leftHits[i].hitDistance < closestHitDist)
    {
        closestHitDist = leftHits[i].hitDistance;
       closestHitPoint = leftHits[i].hitPoint;
    }
    }
        for (int i = 0; i < rightHitCount; i++)
   {
if (rightHits[i].hit && rightHits[i].hitDistance < closestHitDist)
      {
   closestHitDist = rightHits[i].hitDistance;
           closestHitPoint = rightHits[i].hitPoint;
        }
   }
  
            if (closestHitDist < FLT_MAX)
         {
      outResult.collisionPoint = closestHitPoint;
                outResult.closestDistance = closestHitDist;
       }
        }
     
        // ============================================
        // DYNAMIC THRESHOLD SCALING based on blade length
        // ============================================
        const float DAGGER_MAX_BLADE_LENGTH = 55.0f;
        
 float leftBladeLen = leftBlade.bladeLength;
        float rightBladeLen = rightBlade.bladeLength;
        
        bool leftIsDagger = (leftBladeLen > 0.1f && leftBladeLen <= DAGGER_MAX_BLADE_LENGTH);
        bool rightIsDagger = (rightBladeLen > 0.1f && rightBladeLen <= DAGGER_MAX_BLADE_LENGTH);
        bool bothDaggers = leftIsDagger && rightIsDagger;
        bool eitherDagger = leftIsDagger || rightIsDagger;
     
        float scaleFactor = 1.0f;
        if (bothDaggers)
        {
            scaleFactor = 0.25f;
        }
        else if (eitherDagger)
        {
       scaleFactor = 0.5f;
    }
        
        float scaledCollisionThreshold = m_collisionThreshold * scaleFactor;
        float scaledImminentThreshold = m_imminentThreshold * scaleFactor;
  float scaledBackupThreshold = bladeImminentThresholdBackup * scaleFactor;
        
        // ============================================
        // VELOCITY CALCULATIONS
        // ============================================
NiPoint3 leftVel, rightVel;
        
 leftVel.x = leftBlade.baseVelocity.x + 
        leftParam * (leftBlade.tipVelocity.x - leftBlade.baseVelocity.x);
    leftVel.y = leftBlade.baseVelocity.y + 
            leftParam * (leftBlade.tipVelocity.y - leftBlade.baseVelocity.y);
        leftVel.z = leftBlade.baseVelocity.z + 
            leftParam * (leftBlade.tipVelocity.z - leftBlade.baseVelocity.z);
        
        rightVel.x = rightBlade.baseVelocity.x + 
            rightParam * (rightBlade.tipVelocity.x - rightBlade.baseVelocity.x);
     rightVel.y = rightBlade.baseVelocity.y + 
            rightParam * (rightBlade.tipVelocity.y - rightBlade.baseVelocity.y);
      rightVel.z = rightBlade.baseVelocity.z + 
  rightParam * (rightBlade.tipVelocity.z - rightBlade.baseVelocity.z);
        
    NiPoint3 relVel;
        relVel.x = leftVel.x - rightVel.x;
        relVel.y = leftVel.y - rightVel.y;
        relVel.z = leftVel.z - rightVel.z;
        
        outResult.relativeVelocity = Length(relVel);
     
    // Calculate closing velocity
    NiPoint3 separationDir;
   separationDir.x = closestRight.x - closestLeft.x;
        separationDir.y = closestRight.y - closestLeft.y;
        separationDir.z = closestRight.z - closestLeft.z;
      
        float sepLength = Length(separationDir);
        if (sepLength > 0.0001f)
        {
     separationDir = Normalize(separationDir);
        }
   
    float closingVelocity = Dot(relVel, separationDir);
        
      outResult.timeToCollision = EstimateTimeToCollisionScaled(segmentDistance, closingVelocity, scaledCollisionThreshold);
      
        // ============================================
   // COLLISION STATE DETERMINATION
        // Use raycast hits as PRIMARY detection, segment distance as BACKUP
        // ============================================
        
    // Raycast collision: at least 2 rays hit (confirms actual blade intersection)
        bool raycastCollision = (totalHitCount >= 2);
        
  // Segment distance collision (backup)
      bool segmentCollision = (segmentDistance <= scaledCollisionThreshold);
        
        // Combined: either raycast hit OR very close segment distance
        outResult.isColliding = raycastCollision || segmentCollision;
        
        // ============================================
        // GRINDING DETECTION
        // Blades are grinding if they've been in contact for sustained period
        // with low relative velocity (not a fast impact)
        // ============================================
  const float GRIND_VELOCITY_THRESHOLD = 100.0f;  // Low velocity = grinding
     const float GRIND_MIN_DURATION = 0.15f;         // Must be in contact for 150ms to count as grinding
 
        if (outResult.isColliding)
        {
       // Update grind duration
            if (!m_wasInContact)
          {
    // Just started contact
   m_grindStartTime = m_lastUpdateTime;
              m_grindDuration = 0.0f;
            }
            else
  {
        m_grindDuration = m_lastUpdateTime - m_grindStartTime;
            }
   
         // Check if this is grinding vs impact
        bool lowVelocity = (outResult.relativeVelocity < GRIND_VELOCITY_THRESHOLD);
     bool sustainedContact = (m_grindDuration >= GRIND_MIN_DURATION);
  
         outResult.isGrinding = lowVelocity && sustainedContact;
 outResult.grindDuration = m_grindDuration;
 m_bladesGrinding = outResult.isGrinding;
        }
        else
        {
            m_grindDuration = 0.0f;
            m_bladesGrinding = false;
            outResult.isGrinding = false;
        }
        
        // ============================================
      // IMMINENT COLLISION DETECTION
        // Only trigger imminent if NOT already grinding
 // ============================================
     const float MIN_CLOSING_VELOCITY = 50.0f;
        
      bool withinPrimaryThreshold = (segmentDistance <= scaledImminentThreshold) && (closingVelocity >= MIN_CLOSING_VELOCITY);
        bool withinBackupThreshold = (segmentDistance <= scaledBackupThreshold) && (closingVelocity >= MIN_CLOSING_VELOCITY);
        
        // Fast approach only for longer weapons
        bool fastApproaching = false;
   if (!bothDaggers)
        {
     fastApproaching = (outResult.timeToCollision > 0.0f) && 
             (outResult.timeToCollision < bladeTimeToCollisionThreshold) &&
                (segmentDistance <= scaledBackupThreshold);
        }
      
        // IMPORTANT: Don't mark as imminent if we're already grinding
        // This allows sword grinding without constant unequip/re-equip
        outResult.isImminent = !outResult.isColliding && !m_bladesGrinding && 
   (withinPrimaryThreshold || withinBackupThreshold || fastApproaching);
        
        // Debug logging
        static int logCounter = 0;
        logCounter++;
        if (logCounter % 300 == 1)
        {
_MESSAGE("WeaponGeometry: Raycast hits: L=%d, R=%d, Total=%d, SegDist=%.2f",
   leftHitCount, rightHitCount, totalHitCount, segmentDistance);
            if (m_bladesGrinding)
            {
     _MESSAGE("WeaponGeometry: GRINDING detected (duration: %.2fs, velocity: %.1f)",
        m_grindDuration, outResult.relativeVelocity);
            }
        }
        
        if (outResult.isImminent)
        {
  _MESSAGE("WeaponGeometry: IMMINENT - dist=%.2f, rayHits=%d, closing=%.1f, grinding=%s",
      segmentDistance, totalHitCount, closingVelocity, m_bladesGrinding ? "YES" : "NO");
      }
  
        return outResult.isColliding || outResult.isImminent;
    }

    // ============================================
    // RAYCASTING METHODS
    // ============================================
    
    int WeaponGeometryTracker::RaycastBladeIntersection(
        const BladeGeometry& sourceBlade,
        const BladeGeometry& targetBlade,
        float bladeRadius,
  BladeRaycastHit* outHits,
        int maxHits)
    {
  int hitCount = 0;
        
        // Cast rays at multiple points along the source blade
      for (int i = 0; i < RAYCAST_SAMPLES && hitCount < maxHits; i++)
    {
            // Parameter along the blade (0 = base, 1 = tip)
    float t = (float)i / (float)(RAYCAST_SAMPLES - 1);
  
            // Calculate ray origin point on source blade
            NiPoint3 rayOrigin = PointAlongSegment(sourceBlade.basePosition, sourceBlade.tipPosition, t);
     
        // Calculate direction toward the closest point on target blade
            // First find the closest point on target blade to this ray origin
            NiPoint3 targetPoint = PointAlongSegment(targetBlade.basePosition, targetBlade.tipPosition, t);
       
            NiPoint3 rayDir;
          rayDir.x = targetPoint.x - rayOrigin.x;
 rayDir.y = targetPoint.y - rayOrigin.y;
            rayDir.z = targetPoint.z - rayOrigin.z;
  
      float rayLength = Length(rayDir);
            if (rayLength < 0.001f)
            {
     // Points are essentially the same - definite hit
     outHits[hitCount].hit = true;
  outHits[hitCount].hitPoint = rayOrigin;
          outHits[hitCount].hitDistance = 0.0f;
    outHits[hitCount].rayParameter = t;
         outHits[hitCount].bladeParameter = t;
 hitCount++;
                continue;
            }
            
    rayDir = Normalize(rayDir);
            
            // Cast ray toward target blade
      BladeRaycastHit hit;
            if (RaycastTowardBlade(rayOrigin, rayDir, targetBlade, rayLength + bladeRadius * 2.0f, hit))
            {
     hit.rayParameter = t;
        outHits[hitCount] = hit;
      hitCount++;
            }
        }
    
        return hitCount;
    }
    
    bool WeaponGeometryTracker::RaycastTowardBlade(
    const NiPoint3& rayOrigin,
        const NiPoint3& rayDirection,
        const BladeGeometry& targetBlade,
        float maxDistance,
 BladeRaycastHit& outHit)
 {
        outHit.Clear();
      
        // Approximate the blade as a cylinder and check for intersection
        float hitDistance;
      NiPoint3 hitPoint;
      
        // Use a minimum blade radius for hit detection
        float bladeRadius = BLADE_RADIUS;
   
        if (RayIntersectsCylinder(
  rayOrigin, rayDirection,
 targetBlade.basePosition, targetBlade.tipPosition,
            bladeRadius,
            hitDistance, hitPoint))
      {
    if (hitDistance <= maxDistance)
  {
            outHit.hit = true;
                outHit.hitPoint = hitPoint;
   outHit.hitDistance = hitDistance;
   
        // Calculate blade parameter (where on the blade did we hit?)
   NiPoint3 bladeDir;
   bladeDir.x = targetBlade.tipPosition.x - targetBlade.basePosition.x;
        bladeDir.y = targetBlade.tipPosition.y - targetBlade.basePosition.y;
              bladeDir.z = targetBlade.tipPosition.z - targetBlade.basePosition.z;
              
       NiPoint3 toHit;
                toHit.x = hitPoint.x - targetBlade.basePosition.x;
      toHit.y = hitPoint.y - targetBlade.basePosition.y;
      toHit.z = hitPoint.z - targetBlade.basePosition.z;

        float bladeLen = Length(bladeDir);
  if (bladeLen > 0.001f)
 {
         outHit.bladeParameter = Clamp(Dot(toHit, bladeDir) / (bladeLen * bladeLen), 0.0f, 1.0f);
                }
          
             return true;
       }
     }
    
        return false;
    }
    
    bool WeaponGeometryTracker::RayIntersectsCylinder(
const NiPoint3& rayOrigin,
        const NiPoint3& rayDirection,
        const NiPoint3& cylinderBase,
        const NiPoint3& cylinderTip,
        float cylinderRadius,
        float& outDistance,
        NiPoint3& outHitPoint)
    {
        // Cylinder axis
   NiPoint3 axis;
        axis.x = cylinderTip.x - cylinderBase.x;
        axis.y = cylinderTip.y - cylinderBase.y;
axis.z = cylinderTip.z - cylinderBase.z;
 
        float axisLenSq = Dot(axis, axis);
        if (axisLenSq < 0.0001f)
      return false;
        
    float axisLen = sqrt(axisLenSq);
        NiPoint3 axisNorm = Normalize(axis);
      
        // Vector from cylinder base to ray origin
        NiPoint3 oc;
        oc.x = rayOrigin.x - cylinderBase.x;
    oc.y = rayOrigin.y - cylinderBase.y;
        oc.z = rayOrigin.z - cylinderBase.z;
  
        // Project ray direction and oc onto plane perpendicular to cylinder axis
        float rayDotAxis = Dot(rayDirection, axisNorm);
        float ocDotAxis = Dot(oc, axisNorm);
        
        NiPoint3 rayPerp;
        rayPerp.x = rayDirection.x - rayDotAxis * axisNorm.x;
        rayPerp.y = rayDirection.y - rayDotAxis * axisNorm.y;
        rayPerp.z = rayDirection.z - rayDotAxis * axisNorm.z;
        
        NiPoint3 ocPerp;
        ocPerp.x = oc.x - ocDotAxis * axisNorm.x;
        ocPerp.y = oc.y - ocDotAxis * axisNorm.y;
        ocPerp.z = oc.z - ocDotAxis * axisNorm.z;
 
        // Quadratic equation coefficients for cylinder intersection
   float a = Dot(rayPerp, rayPerp);
     float b = 2.0f * Dot(rayPerp, ocPerp);
      float c = Dot(ocPerp, ocPerp) - cylinderRadius * cylinderRadius;
        
        float discriminant = b * b - 4.0f * a * c;
        
        if (discriminant < 0.0f)
    return false;  // No intersection with infinite cylinder
   
        if (a < 0.0001f)
        {
            // Ray is parallel to cylinder axis
            // Check if ray is inside cylinder
            if (c <= 0.0f)
      {
        outDistance = 0.0f;
            outHitPoint = rayOrigin;
       return true;
       }
 return false;
        }
        
        float sqrtDisc = sqrt(discriminant);
   float t1 = (-b - sqrtDisc) / (2.0f * a);
  float t2 = (-b + sqrtDisc) / (2.0f * a);
        
        // Check both intersection points
   for (int i = 0; i < 2; i++)
        {
       float t = (i == 0) ? t1 : t2;
            
     if (t < 0.0f)
 continue;  // Behind ray origin
            
// Calculate hit point
  NiPoint3 hitPoint;
        hitPoint.x = rayOrigin.x + t * rayDirection.x;
     hitPoint.y = rayOrigin.y + t * rayDirection.y;
  hitPoint.z = rayOrigin.z + t * rayDirection.z;
        
        // Check if hit point is within cylinder height (between base and tip)
      NiPoint3 toHit;
          toHit.x = hitPoint.x - cylinderBase.x;
   toHit.y = hitPoint.y - cylinderBase.y;
toHit.z = hitPoint.z - cylinderBase.z;
      
          float heightParam = Dot(toHit, axisNorm);
 
            if (heightParam >= 0.0f && heightParam <= axisLen)
            {
      outDistance = t;
          outHitPoint = hitPoint;
                return true;
            }
        }
        
  return false;
    }
    
    // ============================================
    // Additional Helper Methods
    // ============================================
    
    NiPoint3 WeaponGeometryTracker::Cross(const NiPoint3& a, const NiPoint3& b)
    {
  NiPoint3 result;
        result.x = a.y * b.z - a.z * b.y;
        result.y = a.z * b.x - a.x * b.z;
        result.z = a.x * b.y - a.y * b.x;
    return result;
    }
    
    float WeaponGeometryTracker::Length(const NiPoint3& v)
    {
    return sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }
    
    NiPoint3 WeaponGeometryTracker::Normalize(const NiPoint3& v)
    {
   float len = Length(v);
    if (len < 0.0001f)
          return NiPoint3(0, 0, 0);
        
      NiPoint3 result;
        result.x = v.x / len;
    result.y = v.y / len;
        result.z = v.z / len;
        return result;
    }

    float WeaponGeometryTracker::EstimateTimeToCollisionScaled(float distance, float closingVelocity, float scaledCollisionThreshold)
    {
        if (closingVelocity <= 0.0f || distance <= scaledCollisionThreshold)
            return -1.0f;
        
  float timeToCollision = (distance - scaledCollisionThreshold) / closingVelocity;
        
        if (timeToCollision > 2.0f)
         return -1.0f;
      
     return timeToCollision;
    }

    float WeaponGeometryTracker::EstimateTimeToCollision(float distance, float closingVelocity)
    {
     if (closingVelocity <= 0.0f || distance <= m_collisionThreshold)
    return -1.0f;
        
        float timeToCollision = (distance - m_collisionThreshold) / closingVelocity;
 
        if (timeToCollision > 2.0f)
return -1.0f;
        
        return timeToCollision;
 }

    float WeaponGeometryTracker::ClosestDistanceBetweenSegments(
        const NiPoint3& p1, const NiPoint3& q1,
const NiPoint3& p2, const NiPoint3& q2,
        float& outParam1, float& outParam2,
   NiPoint3& outClosestPoint1, NiPoint3& outClosestPoint2)
    {
        NiPoint3 d1, d2, r;
        d1.x = q1.x - p1.x;
        d1.y = q1.y - p1.y;
        d1.z = q1.z - p1.z;
   
        d2.x = q2.x - p2.x;
        d2.y = q2.y - p2.y;
     d2.z = q2.z - p2.z;
        
   r.x = p1.x - p2.x;
    r.y = p1.y - p2.y;
r.z = p1.z - p2.z;
        
        float a = Dot(d1, d1);
        float e = Dot(d2, d2);
     float f = Dot(d2, r);
        
        float s, t;

        if (a <= 0.0001f && e <= 0.0001f)
        {
  s = t = 0.0f;
     outClosestPoint1 = p1;
            outClosestPoint2 = p2;
        }
        else if (a <= 0.0001f)
   {
   s = 0.0f;
 t = Clamp(f / e, 0.0f, 1.0f);
   }
        else
     {
 float c = Dot(d1, r);
            if (e <= 0.0001f)
     {
        t = 0.0f;
          s = Clamp(-c / a, 0.0f, 1.0f);
       }
            else
 {
                float b = Dot(d1, d2);
    float denom = a * e - b * b;
         
   if (denom != 0.0f)
                {
  s = Clamp((b * f - c * e) / denom, 0.0f, 1.0f);
 }
     else
                {
   s = 0.0f;
   }
           
         t = (b * s + f) / e;
     
          if (t < 0.0f)
     {
          t = 0.0f;
    s = Clamp(-c / a, 0.0f, 1.0f);
          }
    else if (t > 1.0f)
 {
            t = 1.0f;
        s = Clamp((b - c) / a, 0.0f, 1.0f);
     }
 }
        }
   
        outParam1 = s;
        outParam2 = t;
     
        outClosestPoint1 = PointAlongSegment(p1, q1, s);
        outClosestPoint2 = PointAlongSegment(p2, q2, t);
        
        NiPoint3 diff;
        diff.x = outClosestPoint1.x - outClosestPoint2.x;
        diff.y = outClosestPoint1.y - outClosestPoint2.y;
        diff.z = outClosestPoint1.z - outClosestPoint2.z;

        return sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
    }

    float WeaponGeometryTracker::Dot(const NiPoint3& a, const NiPoint3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
  }

    float WeaponGeometryTracker::Clamp(float value, float min, float max)
    {
        if (value < min) return min;
   if (value > max) return max;
      return value;
    }

    NiPoint3 WeaponGeometryTracker::PointAlongSegment(const NiPoint3& start, const NiPoint3& end, float t)
    {
        NiPoint3 result;
        result.x = start.x + t * (end.x - start.x);
    result.y = start.y + t * (end.y - start.y);
      result.z = start.z + t * (end.z - start.z);
        return result;
    }

    void WeaponGeometryTracker::CheckXPose(const BladeGeometry& leftBlade, const BladeGeometry& rightBlade)
    {
        m_wasInXPose = m_inXPose;
        
        if (!leftBlade.isValid || !rightBlade.isValid)
   {
            m_inXPose = false;
          if (m_wasInXPose)
            {
  _MESSAGE("WeaponGeometry: *** X-POSE ENDED *** (blade geometry invalid)");
 }
  return;
  }

        NiPoint3 leftDir;
        leftDir.x = leftBlade.tipPosition.x - leftBlade.basePosition.x;
    leftDir.y = leftBlade.tipPosition.y - leftBlade.basePosition.y;
        leftDir.z = leftBlade.tipPosition.z - leftBlade.basePosition.z;

   NiPoint3 rightDir;
        rightDir.x = rightBlade.tipPosition.x - rightBlade.basePosition.x;
        rightDir.y = rightBlade.tipPosition.y - rightBlade.basePosition.y;
 rightDir.z = rightBlade.tipPosition.z - rightBlade.basePosition.z;

        float leftLen = sqrt(leftDir.x * leftDir.x + leftDir.y * leftDir.y + leftDir.z * leftDir.z);
        float rightLen = sqrt(rightDir.x * rightDir.x + rightDir.y * rightDir.y + rightDir.z * rightDir.z);

      if (leftLen < 0.001f || rightLen < 0.001f)
{
            m_inXPose = false;
       if (m_wasInXPose)
 {
     _MESSAGE("WeaponGeometry: *** X-POSE ENDED *** (blade length too short)");
            }
            return;
        }

        leftDir.x /= leftLen;
        leftDir.y /= leftLen;
     leftDir.z /= leftLen;

        rightDir.x /= rightLen;
     rightDir.y /= rightLen;
        rightDir.z /= rightLen;

        PlayerCharacter* player = *g_thePlayer;
        if (!player)
        {
            m_inXPose = false;
        if (m_wasInXPose)
       {
   _MESSAGE("WeaponGeometry: *** X-POSE ENDED *** (no player)");
       }
      return;
        }

        NiPoint3 playerForward;
   float heading = player->rot.z;
   playerForward.x = sin(heading);
        playerForward.y = cos(heading);
      playerForward.z = 0.0f;

        float bladeDot = leftDir.x * rightDir.x + leftDir.y * rightDir.y + leftDir.z * rightDir.z;
        float bladeAngle = acos(Clamp(bladeDot, -1.0f, 1.0f)) * (180.0f / 3.14159f);
 float leftForwardDot = leftDir.x * playerForward.x + leftDir.y * playerForward.y;
        float rightForwardDot = rightDir.x * playerForward.x + rightDir.y * playerForward.y;

        bool leftPointingUp = leftDir.z > 0.3f;
        bool rightPointingUp = rightDir.z > 0.3f;

        bool isCrossing = (bladeAngle > 30.0f && bladeAngle < 150.0f);
        bool bothPointingUp = leftPointingUp && rightPointingUp;
        bool facingForward = (leftForwardDot > -0.5f) && (rightForwardDot > -0.5f);

        m_inXPose = isCrossing && bothPointingUp && facingForward;

      if (m_inXPose && !m_wasInXPose)
        {
      _MESSAGE("WeaponGeometry: *** X-POSE DETECTED! *** Blades crossed facing forward!");
            StartBlocking();
 }
    else if (!m_inXPose && m_wasInXPose)
        {
   _MESSAGE("WeaponGeometry: *** X-POSE ENDED ***");
            StopBlocking();
        }
    }

    // ============================================
 // Convenience Functions
    // ============================================

    void InitializeWeaponGeometryTracker()
    {
        WeaponGeometryTracker::GetSingleton()->Initialize();
    }

    void UpdateWeaponGeometry(float deltaTime)
  {
        WeaponGeometryTracker::GetSingleton()->Update(deltaTime);
    }
}

