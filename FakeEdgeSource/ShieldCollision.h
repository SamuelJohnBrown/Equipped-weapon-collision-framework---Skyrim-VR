#pragma once

#include "skse64/NiNodes.h"
#include "skse64/NiObjects.h"
#include "skse64/NiTypes.h"
#include "skse64/GameReferences.h"
#include "skse64/GameObjects.h"
#include "config.h"
#include "EquipManager.h"
#include "WeaponGeometry.h"

namespace FalseEdgeVR
{
    // Represents the shield geometry data
    struct ShieldGeometry
    {
        NiPoint3 centerPosition;    // World position of shield center
        NiPoint3 normal;            // Shield facing direction (normal)
        NiPoint3 velocity;          // Velocity of shield center
        float radius;              // Approximate shield radius
        bool isValid;               // Whether the geometry data is valid
   
     // Previous frame position for velocity calculation
   NiPoint3 prevCenterPosition;
        
        void Clear()
 {
            centerPosition = NiPoint3(0, 0, 0);
 normal = NiPoint3(0, 0, 0);
            velocity = NiPoint3(0, 0, 0);
            prevCenterPosition = NiPoint3(0, 0, 0);
      radius = 25.0f;  // Default shield radius
       isValid = false;
      }
 
      ShieldGeometry()
        {
      Clear();
        }
    };

  // Shield collision result data
    struct ShieldCollisionResult
    {
        bool isColliding;         // Whether weapon is contacting shield
        bool isImminent;    // Whether collision is imminent
        NiPoint3 collisionPoint;        // World position where contact occurs
        NiPoint3 weaponContactPoint;    // Point on weapon where contact occurs
        NiPoint3 shieldContactPoint;    // Point on shield where contact occurs
     float closestDistance;     // Distance from weapon to shield surface
        float weaponParameter;          // Parameter (0-1) along weapon blade
        float relativeVelocity;         // Relative velocity at collision
   float impactAngle;  // Angle of weapon relative to shield normal (degrees)
   float timeToCollision;          // Estimated time until collision
        bool isLeftHandWeapon;          // Which hand holds the weapon
  bool isLeftHandShield;        // Which hand holds the shield
    
        void Clear()
        {
       isColliding = false;
     isImminent = false;
            collisionPoint = NiPoint3(0, 0, 0);
   weaponContactPoint = NiPoint3(0, 0, 0);
    shieldContactPoint = NiPoint3(0, 0, 0);
            closestDistance = FLT_MAX;
     weaponParameter = 0.0f;
          relativeVelocity = 0.0f;
            impactAngle = 0.0f;
         timeToCollision = -1.0f;
            isLeftHandWeapon = false;
      isLeftHandShield = false;
        }
        
        ShieldCollisionResult()
        {
            Clear();
   }
    };

    // Callback type for shield collision events
    typedef void (*ShieldCollisionCallback)(const ShieldCollisionResult& collision);

    // Tracks shield geometry and weapon-to-shield collisions
    class ShieldCollisionTracker
    {
    public:
        static ShieldCollisionTracker* GetSingleton();
        
        // Initialize the tracker
        void Initialize();
        
   // Update shield geometry and check collisions - call this each frame
        void Update(float deltaTime);
      
      // Get shield geometry for a specific hand
        const ShieldGeometry& GetShieldGeometry(bool isLeftHand) const;
        
     // Check if a shield is equipped in either hand
        bool HasShieldEquipped() const;
        
   // Check which hand has the shield
     bool IsShieldInLeftHand() const { return m_shieldInLeftHand; }

        // ============================================
  // Shield Collision Detection
   // ============================================
        
      // Check if weapon is colliding with shield
      bool CheckWeaponShieldCollision(ShieldCollisionResult& outResult, bool rightHandHiggsGrabbed = false);
      
     // Get the last collision result
        const ShieldCollisionResult& GetLastCollisionResult() const { return m_lastCollision; }
        
  // Check if weapon is currently contacting shield
        bool IsWeaponContactingShield() const { return m_weaponContactingShield; }
        
      // Check if collision is imminent
        bool IsCollisionImminent() const { return m_collisionImminent; }
        
        // Set collision threshold distance (default ~8 units)
  void SetCollisionThreshold(float threshold) { m_collisionThreshold = threshold; }
      float GetCollisionThreshold() const { return m_collisionThreshold; }
        
        // Set imminent threshold distance
        void SetImminentThreshold(float threshold) { m_imminentThreshold = threshold; }
        float GetImminentThreshold() const { return m_imminentThreshold; }
     
        // Register callback for shield collision events
        void SetCollisionCallback(ShieldCollisionCallback callback) { m_collisionCallback = callback; }
        
    private:
        ShieldCollisionTracker() = default;
        ~ShieldCollisionTracker() = default;
        ShieldCollisionTracker(const ShieldCollisionTracker&) = delete;
 ShieldCollisionTracker& operator=(const ShieldCollisionTracker&) = delete;
        
  // Update geometry for shield
        void UpdateShieldGeometry(bool isLeftHand, float deltaTime);
     
     // Update geometry for HIGGS-grabbed weapon
        void UpdateHiggsGrabbedWeaponGeometry(TESObjectREFR* grabbedRef, float deltaTime);
 
        // Get the shield node from player skeleton
        NiAVObject* GetShieldNode(bool isLeftHand);
        
        // Get the appropriate shield offset node name
        const char* GetShieldOffsetNodeName(bool isLeftHand);
        
        // Calculate closest distance from blade segment to shield disc
    float ClosestDistanceBladeToShield(
   const NiPoint3& bladeBase, const NiPoint3& bladeTip,
   const NiPoint3& shieldCenter, const NiPoint3& shieldNormal, float shieldRadius,
       float& outBladeParam, NiPoint3& outBladePoint, NiPoint3& outShieldPoint
        );
        
        // Estimate time to collision
        float EstimateTimeToCollision(float distance, float closingVelocity);
    
        // Helper: project point onto plane
    NiPoint3 ProjectPointOntoPlane(const NiPoint3& point, const NiPoint3& planePoint, const NiPoint3& planeNormal);
        
        // Helper: clamp point to disc
        NiPoint3 ClampPointToDisc(const NiPoint3& point, const NiPoint3& center, const NiPoint3& normal, float radius);
        
      // Helper functions
        static float Dot(const NiPoint3& a, const NiPoint3& b);
        static float Clamp(float value, float min, float max);
        static float Length(const NiPoint3& v);
        static NiPoint3 Normalize(const NiPoint3& v);
        static NiPoint3 Cross(const NiPoint3& a, const NiPoint3& b);
 
        // Log state for debugging
        void LogCollisionState();
        
        ShieldGeometry m_leftHandShield;
     ShieldGeometry m_rightHandShield;
        BladeGeometry m_higgsGrabbedWeapon;   // For tracking HIGGS-grabbed right hand weapon
        ShieldCollisionResult m_lastCollision;
    ShieldCollisionCallback m_collisionCallback = nullptr;
 
        bool m_initialized = false;
        bool m_shieldInLeftHand = true;     // Which hand has shield
        bool m_hasShield = false;       // Whether shield is equipped
        bool m_weaponContactingShield = false;
bool m_wasContacting = false;       // Previous frame contact state
 bool m_collisionImminent = false;
        bool m_wasImminent = false;             // Previous frame imminent state
        float m_collisionThreshold = 8.0f;      // Distance threshold for collision
  float m_imminentThreshold = 15.0f;  // Distance threshold for imminent collision
    };
    
    // Convenience function to initialize shield collision tracking
    void InitializeShieldCollisionTracker();
    
    // Convenience function to update shield collision (call each frame)
    void UpdateShieldCollision(float deltaTime);
}
