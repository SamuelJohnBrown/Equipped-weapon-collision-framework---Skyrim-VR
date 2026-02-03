#include "ShieldCollision.h"
#include "Engine.h"
#include "VRInputHandler.h"
#include "skse64/GameRTTI.h"
#include "skse64/NiNodes.h"
#include <cmath>
#include <cfloat>

namespace FalseEdgeVR
{
  // ============================================
  // ShieldCollisionTracker Implementation
    // ============================================

    ShieldCollisionTracker* ShieldCollisionTracker::GetSingleton()
    {
        static ShieldCollisionTracker instance;
return &instance;
    }

    void ShieldCollisionTracker::Initialize()
    {
        if (m_initialized)
   return;

        _MESSAGE("ShieldCollisionTracker: Initializing...");
        
        m_leftHandShield.Clear();
 m_rightHandShield.Clear();
   m_lastCollision.Clear();
        m_weaponContactingShield = false;
        m_wasContacting = false;
        m_hasShield = false;
        
        // Load thresholds from shield-specific config
        m_collisionThreshold = shieldCollisionThreshold;
        m_imminentThreshold = shieldImminentThreshold;

 _MESSAGE("ShieldCollisionTracker: Collision threshold: %.2f, Imminent threshold: %.2f",
   m_collisionThreshold, m_imminentThreshold);
        
   m_initialized = true;
  _MESSAGE("ShieldCollisionTracker: Initialized successfully");
    }

    void ShieldCollisionTracker::Update(float deltaTime)
    {
        if (!m_initialized)
      return;

        PlayerCharacter* player = *g_thePlayer;
        if (!player || !player->loadedState)
            return;

 // Log first update call to confirm tracker is running
        static bool loggedFirstUpdate = false;
        if (!loggedFirstUpdate)
        {
  _MESSAGE("ShieldCollisionTracker::Update - First update call!");
            loggedFirstUpdate = true;
        }

  const PlayerEquipState& equipState = EquipManager::GetSingleton()->GetEquipState();
        
        // Check directly what the player has equipped - this is always accurate
        TESForm* leftEquipped = player->GetEquippedObject(true);
TESForm* rightEquipped = player->GetEquippedObject(false);
   bool directLeftIsShield = leftEquipped && EquipManager::IsShield(leftEquipped);
 bool directRightIsShield = rightEquipped && EquipManager::IsShield(rightEquipped);
      
        // If we detect a mismatch between direct check and EquipManager, force update
        bool equipManagerKnowsShield = (equipState.leftHand.type == WeaponType::Shield) || 
     (equipState.rightHand.type == WeaponType::Shield);
 if ((directLeftIsShield || directRightIsShield) && !equipManagerKnowsShield)
{
    _MESSAGE("ShieldCollisionTracker: EquipManager mismatch - forcing UpdateEquipmentState");
  EquipManager::GetSingleton()->UpdateEquipmentState();
        }

        // ============================================
        // SHIELD DETECTION - DO THIS FIRST before anything else
     // ============================================
        // Re-fetch equipState after potential update
     const PlayerEquipState& currentEquipState = EquipManager::GetSingleton()->GetEquipState();
        
        // Set m_hasShield based on DIRECT player check (most reliable)
      m_hasShield = directLeftIsShield || directRightIsShield;
        m_shieldInLeftHand = directLeftIsShield;
        
        // Update shield geometry if we have a shield
        if (m_hasShield)
        {
            if (m_shieldInLeftHand)
            {
  UpdateShieldGeometry(true, deltaTime);
                m_rightHandShield.Clear();
            }
            else
       {
       UpdateShieldGeometry(false, deltaTime);
         m_leftHandShield.Clear();
    }
        }
        else
        {
     m_leftHandShield.Clear();
            m_rightHandShield.Clear();
        }
        
      // Debug logging - every 500 frames
        static int debugLogCounter = 0;
        debugLogCounter++;
        if (debugLogCounter % 500 == 1)
        {
            _MESSAGE("ShieldCollisionTracker: Debug - Left hand: type=%d isEquipped=%s, Right hand: type=%d isEquipped=%s, m_hasShield=%s, directShield=%s",
        (int)currentEquipState.leftHand.type, currentEquipState.leftHand.isEquipped ? "YES" : "NO",
      (int)currentEquipState.rightHand.type, currentEquipState.rightHand.isEquipped ? "YES" : "NO",
 m_hasShield ? "YES" : "NO",
   (directLeftIsShield || directRightIsShield) ? "YES" : "NO");
        }
        
        // Check weapon hand has HIGGS-grabbed weapon (from our shield collision avoidance)
       // Weapon hand is OPPOSITE of shield hand
        bool weaponHandIsLeft = !m_shieldInLeftHand;
        bool weaponVRControllerIsLeft = GameHandToVRController(weaponHandIsLeft);
   bool weaponHandHiggsGrabbed = false;
        TESObjectREFR* higgsHeldWeapon = nullptr;
 
        if (EquipManager::GetSingleton()->HasPendingReequip(weaponHandIsLeft))
        {
         // Get the dropped weapon reference we created
         higgsHeldWeapon = EquipManager::GetSingleton()->GetDroppedWeaponRef(weaponHandIsLeft);
        if (higgsHeldWeapon)
{
        // Check if HIGGS is actually holding it
         if (higgsInterface)
          {
           TESObjectREFR* currentlyHeld = higgsInterface->GetGrabbedObject(weaponVRControllerIsLeft);
                if (currentlyHeld == higgsHeldWeapon)
      {
   weaponHandHiggsGrabbed = true;
 }
    }
      }
        }
        
// Check for weapon-shield collision if we have a shield
      // and either a weapon equipped in the other hand OR a HIGGS-grabbed weapon
        if (m_hasShield)
      {
  bool hasWeaponToCheck = false;
            
     // Use DIRECT player check for weapon in other hand (more reliable than EquipManager state)
         if (m_shieldInLeftHand)
        {
                // Shield in left hand - check right hand for weapon (using direct check)
                bool directRightIsWeapon = rightEquipped && EquipManager::IsWeapon(rightEquipped);
  hasWeaponToCheck = directRightIsWeapon;
 }
       else
  {
      // Shield in right hand - check left hand for weapon (using direct check)
           bool directLeftIsWeapon = leftEquipped && EquipManager::IsWeapon(leftEquipped);
           hasWeaponToCheck = directLeftIsWeapon;
        }
    
    // Also check if we have a HIGGS-grabbed weapon
       if (hasWeaponToCheck || weaponHandHiggsGrabbed)
    {
     m_wasContacting = m_weaponContactingShield;
        m_wasImminent = m_collisionImminent;
    
   ShieldCollisionResult collision;
        bool detected = CheckWeaponShieldCollision(collision, weaponHandHiggsGrabbed);
       
  m_weaponContactingShield = collision.isColliding;
       m_collisionImminent = collision.isImminent;
  
    if (m_weaponContactingShield)
      {
m_lastCollision = collision;

   // Fire callback if weapon just made contact
     if (!m_wasContacting && m_collisionCallback)
          {
            m_collisionCallback(collision);
           }
       
     // Log collision event and notify VRInputHandler (only on initial contact)
   if (!m_wasContacting)
 {
         _MESSAGE("ShieldCollision: === WEAPON TOUCHING SHIELD === (HIGGS grabbed: %s)",
  weaponHandHiggsGrabbed ? "YES" : "NO");
_MESSAGE("  Collision Point: (%.2f, %.2f, %.2f)",
     collision.collisionPoint.x,
            collision.collisionPoint.y,
   collision.collisionPoint.z);
      _MESSAGE("  Distance: %.2f", collision.closestDistance);
   
     // Notify VRInputHandler for collision tracking
  if (weaponHandHiggsGrabbed)
      {
    VRInputHandler::GetSingleton()->OnShieldCollisionDetected();
   }
        }
 }
         else if (m_collisionImminent)
           {
  m_lastCollision = collision;
     
          // OLD BEHAVIOR: Auto-unequip on imminent collision
          // NEW BEHAVIOR: Trigger-based system handles equip/unequip
       // Just log the collision detection for debugging
     if (!m_wasImminent && !m_wasContacting && !weaponHandHiggsGrabbed)
   {
  _MESSAGE("ShieldCollision: WEAPON-SHIELD COLLISION IMMINENT (trigger system handles equip/unequip)");
     _MESSAGE("  Distance: %.2f, Time to collision: %.3f sec",
     collision.closestDistance,
 collision.timeToCollision);
       }
       }
        else
      {
     // Weapon no longer contacting or imminent
        if (m_wasContacting)
            {
       _MESSAGE("ShieldCollision: === WEAPON NO LONGER TOUCHING SHIELD === (HIGGS grabbed: %s)",
      weaponHandHiggsGrabbed ? "YES" : "NO");
 _MESSAGE("  Current distance: %.2f (threshold: %.2f)", 
   collision.closestDistance, m_collisionThreshold);
     }
             
        m_lastCollision.Clear();
   }
}
    else
            {
     m_weaponContactingShield = false;
        m_collisionImminent = false;
     m_wasContacting = false;
           m_wasImminent = false;
    }
        }
        else
        {
      m_weaponContactingShield = false;
   m_collisionImminent = false;
            m_wasContacting = false;
       m_wasImminent = false;
   }
    }

    void ShieldCollisionTracker::UpdateShieldGeometry(bool isLeftHand, float deltaTime)
    {
     ShieldGeometry& geometry = isLeftHand ? m_leftHandShield : m_rightHandShield;
        
     // Store previous position for velocity calculation
        geometry.prevCenterPosition = geometry.centerPosition;
        
        // Get the shield node
        NiAVObject* shieldNode = GetShieldNode(isLeftHand);
        if (!shieldNode)
        {
  geometry.isValid = false;
          return;
   }
 
        // Get shield center from world transform
        geometry.centerPosition = NiPoint3(
   shieldNode->m_worldTransform.pos.x,
       shieldNode->m_worldTransform.pos.y,
    shieldNode->m_worldTransform.pos.z
        );
        
        // Get shield facing direction (normal)
        // The shield's local Z axis typically points outward (facing direction)
        // NOTE: We negate this because in Skyrim VR the shield's Z axis points AWAY from the player
    // (toward the back of the shield), so we need to flip it to get the front face direction
      NiMatrix33& rot = shieldNode->m_worldTransform.rot;
        geometry.normal = NiPoint3(
         -rot.data[0][2],  // Z column X component (negated)
            -rot.data[1][2],  // Z column Y component (negated)
     -rot.data[2][2]   // Z column Z component (negated)
        );
        geometry.normal = Normalize(geometry.normal);
        
        // Set shield radius from config (focuses on shield face, not edges)
        geometry.radius = shieldRadius;
     
        // Calculate velocity
        if (deltaTime > 0.0f && (geometry.prevCenterPosition.x != 0.0f || 
      geometry.prevCenterPosition.y != 0.0f || 
            geometry.prevCenterPosition.z != 0.0f))
        {
       geometry.velocity.x = (geometry.centerPosition.x - geometry.prevCenterPosition.x) / deltaTime;
        geometry.velocity.y = (geometry.centerPosition.y - geometry.prevCenterPosition.y) / deltaTime;
geometry.velocity.z = (geometry.centerPosition.z - geometry.prevCenterPosition.z) / deltaTime;
 }
        
        geometry.isValid = true;
    }

    NiAVObject* ShieldCollisionTracker::GetShieldNode(bool isLeftHand)
    {
        PlayerCharacter* player = *g_thePlayer;
        if (!player || !player->loadedState)
       return nullptr;

   NiNode* rootNode = player->GetNiRootNode(0); // First person root
   if (!rootNode)
        {
            rootNode = player->GetNiRootNode(1); // Third person root
      }
   
        if (!rootNode)
     return nullptr;

 const char* nodeName = GetShieldOffsetNodeName(isLeftHand);
  
        BSFixedString nodeNameStr(nodeName);
        NiAVObject* shieldNode = rootNode->GetObjectByName(&nodeNameStr.data);
  
        return shieldNode;
    }

    const char* ShieldCollisionTracker::GetShieldOffsetNodeName(bool isLeftHand)
    {
        // In Skyrim VR:
        // Left hand (shield/weapon) node is called "SHIELD"
        // Right hand node is called "WEAPON"
        if (isLeftHand)
        {
   return "SHIELD";
        }
  else
        {
    return "WEAPON";
        }
    }

    bool ShieldCollisionTracker::HasShieldEquipped() const
    {
        return m_hasShield;
    }

    const ShieldGeometry& ShieldCollisionTracker::GetShieldGeometry(bool isLeftHand) const
    {
        return isLeftHand ? m_leftHandShield : m_rightHandShield;
    }

    // ============================================
    // Shield Collision Detection
    // ============================================

  bool ShieldCollisionTracker::CheckWeaponShieldCollision(ShieldCollisionResult& outResult, bool weaponHandHiggsGrabbed)
    {
        outResult.Clear();
        
if (!m_hasShield)
return false;
        
        // Get shield geometry
        const ShieldGeometry& shield = m_shieldInLeftHand ? m_leftHandShield : m_rightHandShield;
   if (!shield.isValid)
            return false;
      
      // Get weapon geometry from WeaponGeometryTracker (right hand weapon vs left hand shield)
 bool weaponIsLeftHand = !m_shieldInLeftHand;  // Weapon is in opposite hand from shield
    const BladeGeometry& weapon = WeaponGeometryTracker::GetSingleton()->GetBladeGeometry(weaponIsLeftHand);
      
        // For HIGGS-grabbed weapon, use the HIGGS grabbed geometry if available
        if (weaponHandHiggsGrabbed && m_higgsGrabbedWeapon.isValid)
        {
       // Use the HIGGS grabbed weapon geometry instead
    // (This would be updated separately)
        }
  
      if (!weapon.isValid)
       return false;
        
        outResult.isLeftHandWeapon = weaponIsLeftHand;
 outResult.isLeftHandShield = m_shieldInLeftHand;
        
        // Calculate closest distance from weapon blade to shield disc
        float bladeParam;
  NiPoint3 bladePoint, shieldPoint;
        
      float distance = ClosestDistanceBladeToShield(
            weapon.basePosition, weapon.tipPosition,
            shield.centerPosition, shield.normal, shield.radius,
            bladeParam, bladePoint, shieldPoint
        );
        
    outResult.closestDistance = distance;
        outResult.weaponParameter = bladeParam;
        outResult.weaponContactPoint = bladePoint;
        outResult.shieldContactPoint = shieldPoint;
        
  // Calculate collision point (midpoint)
        outResult.collisionPoint.x = (bladePoint.x + shieldPoint.x) * 0.5f;
        outResult.collisionPoint.y = (bladePoint.y + shieldPoint.y) * 0.5f;
        outResult.collisionPoint.z = (bladePoint.z + shieldPoint.z) * 0.5f;
        
        // Calculate impact angle (angle between blade direction and shield normal)
        NiPoint3 bladeDir;
        bladeDir.x = weapon.tipPosition.x - weapon.basePosition.x;
        bladeDir.y = weapon.tipPosition.y - weapon.basePosition.y;
      bladeDir.z = weapon.tipPosition.z - weapon.basePosition.z;
        bladeDir = Normalize(bladeDir);
        
        float dotProduct = Dot(bladeDir, shield.normal);
     outResult.impactAngle = acos(Clamp(fabs(dotProduct), 0.0f, 1.0f)) * (180.0f / 3.14159f);
      
        // Calculate relative velocity
    // Interpolate weapon velocity at contact point
        NiPoint3 weaponVel;
     weaponVel.x = weapon.baseVelocity.x + bladeParam * (weapon.tipVelocity.x - weapon.baseVelocity.x);
 weaponVel.y = weapon.baseVelocity.y + bladeParam * (weapon.tipVelocity.y - weapon.baseVelocity.y);
 weaponVel.z = weapon.baseVelocity.z + bladeParam * (weapon.tipVelocity.z - weapon.baseVelocity.z);
  
 // Use weapon velocity directly - shield is considered stationary
        // (shield movement should NOT affect collision detection)
    NiPoint3 relVel = weaponVel;
        
     outResult.relativeVelocity = Length(relVel);
 
        // Calculate closing velocity (positive = approaching, negative = separating)
    NiPoint3 separationDir;
    separationDir.x = shieldPoint.x - bladePoint.x;
        separationDir.y = shieldPoint.y - bladePoint.y;
        separationDir.z = shieldPoint.z - bladePoint.z;
        
        float sepLength = Length(separationDir);
        if (sepLength > 0.0001f)
        {
    separationDir.x /= sepLength;
            separationDir.y /= sepLength;
            separationDir.z /= sepLength;
        }
        
    float closingVelocity = Dot(relVel, separationDir);
        
        // Estimate time to collision
     outResult.timeToCollision = EstimateTimeToCollision(distance, closingVelocity);
     
     // Check if weapon is in front of shield face (not behind or to the side)
        // Calculate vector from shield center to blade contact point
   NiPoint3 shieldToWeapon;
        shieldToWeapon.x = bladePoint.x - shield.centerPosition.x;
    shieldToWeapon.y = bladePoint.y - shield.centerPosition.y;
        shieldToWeapon.z = bladePoint.z - shield.centerPosition.z;
        
        // Dot product with shield normal tells us if weapon is in front (positive) or behind (negative)
        float frontFaceDot = Dot(shieldToWeapon, shield.normal);
        bool weaponInFrontOfShield = (frontFaceDot > 0.0f);
        
// Minimum closing velocity to prevent triggering on noise/tiny movements
        // Only consider it "approaching" if moving at least 5 units/sec toward shield
        const float minClosingVelocity = 5.0f;
        bool isApproaching = (closingVelocity > minClosingVelocity);
        
        // Check collision states - only trigger if weapon is in front of shield face
        outResult.isColliding = (distance <= m_collisionThreshold) && weaponInFrontOfShield;
      outResult.isImminent = !outResult.isColliding && 
   (distance <= m_imminentThreshold) && 
   isApproaching &&         // Only imminent if approaching with meaningful velocity
            weaponInFrontOfShield;       // Only imminent if in front of shield
        
        // Debug logging for troubleshooting
        static int debugCounter = 0;
        debugCounter++;
      if (debugCounter % 200 == 0)
        {
       _MESSAGE("ShieldCollision: dist=%.2f, closingVel=%.2f, frontDot=%.2f, inFront=%s, approaching=%s, imminent=%s",
  distance, closingVelocity, frontFaceDot,
                weaponInFrontOfShield ? "YES" : "NO",
      isApproaching ? "YES" : "NO",
   outResult.isImminent ? "YES" : "NO");
   }
    
  return outResult.isColliding || outResult.isImminent;
    }

    float ShieldCollisionTracker::EstimateTimeToCollision(float distance, float closingVelocity)
    {
        // If not approaching (velocity <= 0) or already colliding, return -1
        if (closingVelocity <= 0.0f || distance <= m_collisionThreshold)
            return -1.0f;
        
        // Time = distance / velocity
        float timeToCollision = (distance - m_collisionThreshold) / closingVelocity;
        
        // Cap at a reasonable maximum (e.g., 2 seconds)
        if (timeToCollision > 2.0f)
        return -1.0f;
        
        return timeToCollision;
    }

    float ShieldCollisionTracker::ClosestDistanceBladeToShield(
        const NiPoint3& bladeBase, const NiPoint3& bladeTip,
        const NiPoint3& shieldCenter, const NiPoint3& shieldNormal, float shieldRadius,
        float& outBladeParam, NiPoint3& outBladePoint, NiPoint3& outShieldPoint)
    {
        // We model the shield as a disc (circle in 3D space)
        // Find the closest point on the blade segment to the shield disc
        
        NiPoint3 bladeDir;
 bladeDir.x = bladeTip.x - bladeBase.x;
      bladeDir.y = bladeTip.y - bladeBase.y;
        bladeDir.z = bladeTip.z - bladeBase.z;
      
        float bladeLength = Length(bladeDir);
        if (bladeLength < 0.0001f)
        {
 // Degenerate blade - treat as point
        outBladeParam = 0.0f;
          outBladePoint = bladeBase;
    outShieldPoint = ClampPointToDisc(bladeBase, shieldCenter, shieldNormal, shieldRadius);
  
      NiPoint3 diff;
      diff.x = outBladePoint.x - outShieldPoint.x;
  diff.y = outBladePoint.y - outShieldPoint.y;
      diff.z = outBladePoint.z - outShieldPoint.z;
    return Length(diff);
        }
     
        // Sample multiple points along the blade and find closest to shield
        // This is a simplified approach - a more accurate method would solve analytically
    float minDist = FLT_MAX;
   float bestParam = 0.0f;
        NiPoint3 bestBladePoint, bestShieldPoint;
        
    const int numSamples = 10;
        for (int i = 0; i <= numSamples; i++)
        {
     float t = (float)i / (float)numSamples;
            
            NiPoint3 bladePoint;
     bladePoint.x = bladeBase.x + t * bladeDir.x;
   bladePoint.y = bladeBase.y + t * bladeDir.y;
        bladePoint.z = bladeBase.z + t * bladeDir.z;
          
            // Project blade point onto shield plane, then clamp to disc
            NiPoint3 shieldPoint = ClampPointToDisc(bladePoint, shieldCenter, shieldNormal, shieldRadius);
       
            NiPoint3 diff;
            diff.x = bladePoint.x - shieldPoint.x;
          diff.y = bladePoint.y - shieldPoint.y;
            diff.z = bladePoint.z - shieldPoint.z;
     float dist = Length(diff);

       if (dist < minDist)
 {
         minDist = dist;
  bestParam = t;
    bestBladePoint = bladePoint;
      bestShieldPoint = shieldPoint;
            }
        }
        
        outBladeParam = bestParam;
        outBladePoint = bestBladePoint;
        outShieldPoint = bestShieldPoint;
        
        return minDist;
}

    NiPoint3 ShieldCollisionTracker::ProjectPointOntoPlane(
    const NiPoint3& point, const NiPoint3& planePoint, const NiPoint3& planeNormal)
    {
        NiPoint3 diff;
        diff.x = point.x - planePoint.x;
        diff.y = point.y - planePoint.y;
        diff.z = point.z - planePoint.z;
        
        float dist = Dot(diff, planeNormal);
        
        NiPoint3 result;
        result.x = point.x - dist * planeNormal.x;
        result.y = point.y - dist * planeNormal.y;
     result.z = point.z - dist * planeNormal.z;
      
     return result;
    }

    NiPoint3 ShieldCollisionTracker::ClampPointToDisc(
        const NiPoint3& point, const NiPoint3& center, const NiPoint3& normal, float radius)
    {
        // First project point onto the plane containing the disc
NiPoint3 projected = ProjectPointOntoPlane(point, center, normal);
        
 // Then clamp to disc radius
        NiPoint3 toProjected;
  toProjected.x = projected.x - center.x;
    toProjected.y = projected.y - center.y;
        toProjected.z = projected.z - center.z;
        
        float distFromCenter = Length(toProjected);
        
   if (distFromCenter <= radius)
   {
            return projected;  // Point is within disc
        }
 
     // Clamp to edge of disc
        NiPoint3 direction = Normalize(toProjected);
        NiPoint3 result;
    result.x = center.x + direction.x * radius;
        result.y = center.y + direction.y * radius;
        result.z = center.z + direction.z * radius;
     
   return result;
    }

    // ============================================
    // Helper Functions
    // ============================================

    float ShieldCollisionTracker::Dot(const NiPoint3& a, const NiPoint3& b)
    {
    return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    float ShieldCollisionTracker::Clamp(float value, float min, float max)
  {
    if (value < min) return min;
        if (value > max) return max;
        return value;
    }

    float ShieldCollisionTracker::Length(const NiPoint3& v)
    {
        return sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }

    NiPoint3 ShieldCollisionTracker::Normalize(const NiPoint3& v)
    {
        float len = Length(v);
        if (len < 0.0001f)
    return NiPoint3(0, 0, 1);
        
        NiPoint3 result;
        result.x = v.x / len;
        result.y = v.y / len;
        result.z = v.z / len;
        return result;
    }

    NiPoint3 ShieldCollisionTracker::Cross(const NiPoint3& a, const NiPoint3& b)
    {
    NiPoint3 result;
        result.x = a.y * b.z - a.z * b.y;
    result.y = a.z * b.x - a.x * b.z;
        result.z = a.x * b.y - a.y * b.x;
        return result;
    }

    void ShieldCollisionTracker::LogCollisionState()
    {
        if (m_hasShield)
        {
         const ShieldGeometry& shield = m_shieldInLeftHand ? m_leftHandShield : m_rightHandShield;
    if (shield.isValid)
     {
      _MESSAGE("ShieldCollision: Shield in %s hand - Center(%.2f, %.2f, %.2f) Radius: %.2f",
          m_shieldInLeftHand ? "Left" : "Right",
              shield.centerPosition.x,
          shield.centerPosition.y,
   shield.centerPosition.z,
      shield.radius);
   }
  }
    }

    // ============================================
    // Convenience Functions
    // ============================================

    void InitializeShieldCollisionTracker()
    {
        ShieldCollisionTracker::GetSingleton()->Initialize();
    }

    void UpdateShieldCollision(float deltaTime)
    {
        ShieldCollisionTracker::GetSingleton()->Update(deltaTime);
    }
}
