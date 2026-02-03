#pragma once

#include "skse64/NiNodes.h"
#include "skse64/NiObjects.h"
#include "skse64/NiTypes.h"
#include "skse64/GameReferences.h"
#include "skse64/GameObjects.h"
#include "config.h"
#include "EquipManager.h"

namespace FalseEdgeVR
{
    // Represents the blade geometry data for a weapon
    struct BladeGeometry
    {
        NiPoint3 tipPosition;       // World position of blade tip
      NiPoint3 basePosition;      // World position of blade base (hilt/handle)
        NiPoint3 tipVelocity;       // Velocity of blade tip (units per second)
        NiPoint3 baseVelocity;  // Velocity of blade base
 float bladeLength;          // Distance from base to tip
        bool isValid;      // Whether the geometry data is valid
        
      // Previous frame positions for velocity calculation
  NiPoint3 prevTipPosition;
        NiPoint3 prevBasePosition;

     void Clear()
        {
     tipPosition = NiPoint3(0, 0, 0);
            basePosition = NiPoint3(0, 0, 0);
  tipVelocity = NiPoint3(0, 0, 0);
      baseVelocity = NiPoint3(0, 0, 0);
      prevTipPosition = NiPoint3(0, 0, 0);
 prevBasePosition = NiPoint3(0, 0, 0);
         bladeLength = 0.0f;
    isValid = false;
        }
        
        BladeGeometry()
        {
       Clear();
     }
    };
    
    // Raycast hit result for blade intersection
    struct BladeRaycastHit
    {
        bool hit;           // Whether the ray hit the other blade
        NiPoint3 hitPoint;          // World position where ray intersected
 float hitDistance;          // Distance from ray origin to hit point
        float rayParameter;   // Parameter (0-1) along the ray where hit occurred
        float bladeParameter;       // Parameter (0-1) along the target blade where hit occurred
        
        void Clear()
        {
    hit = false;
     hitPoint = NiPoint3(0, 0, 0);
  hitDistance = FLT_MAX;
      rayParameter = 0.0f;
            bladeParameter = 0.0f;
        }
        
    BladeRaycastHit()
        {
            Clear();
        }
};
    
    // Blade collision result data
    struct BladeCollisionResult
    {
   bool isColliding;          // Whether blades are currently colliding
bool isImminent;        // Whether collision is imminent (close but not touching)
        bool isGrinding;        // Whether blades are grinding (sustained contact)
        NiPoint3 collisionPoint;      // World position of collision point
   NiPoint3 leftBladeContactPoint; // Point on left blade where contact occurs
        NiPoint3 rightBladeContactPoint;// Point on right blade where contact occurs
        float closestDistance;   // Closest distance between the two blade segments
        float leftBladeParameter;  // Parameter (0-1) along left blade where closest point is
        float rightBladeParameter;      // Parameter (0-1) along right blade where closest point is
        float relativeVelocity;         // Relative velocity at collision point
        float timeToCollision;          // Estimated time until collision (seconds), -1 if moving apart
int raycastHitCount;            // Number of rays that detected collision
    float grindDuration;          // How long blades have been grinding (seconds)

     void Clear()
    {
 isColliding = false;
     isImminent = false;
            isGrinding = false;
       collisionPoint = NiPoint3(0, 0, 0);
   leftBladeContactPoint = NiPoint3(0, 0, 0);
            rightBladeContactPoint = NiPoint3(0, 0, 0);
            closestDistance = FLT_MAX;
            leftBladeParameter = 0.0f;
          rightBladeParameter = 0.0f;
   relativeVelocity = 0.0f;
       timeToCollision = -1.0f;
   raycastHitCount = 0;
    grindDuration = 0.0f;
        }
        
        BladeCollisionResult()
  {
            Clear();
        }
    };
    
    // Weapon geometry data for both hands
    struct WeaponGeometryState
    {
     BladeGeometry leftHand;
  BladeGeometry rightHand;
    };

    // Callback type for blade collision events
    typedef void (*BladeCollisionCallback)(const BladeCollisionResult& collision);
    
    // Callback type for imminent collision events (about to collide)
    typedef void (*BladeImminentCallback)(const BladeCollisionResult& collision);

    // Tracks weapon geometry (blade tip/base positions) each frame
    class WeaponGeometryTracker
    {
    public:
        static WeaponGeometryTracker* GetSingleton();
   
     // Initialize the tracker
        void Initialize();
  
        // Update weapon geometry - call this each frame
    void Update(float deltaTime);
        
      // Get current geometry state
        const WeaponGeometryState& GetGeometryState() const { return m_geometryState; }
        
        // Get blade geometry for a specific hand
        const BladeGeometry& GetBladeGeometry(bool isLeftHand) const;
        
        // Get the weapon node for a hand
        NiAVObject* GetWeaponNode(bool isLeftHand);
        
   // Calculate blade tip position based on weapon type and reach
        NiPoint3 CalculateBladeTip(NiAVObject* weaponNode, TESObjectWEAP* weapon, bool isLeftHand);
        
        // Calculate blade base position (handle/hilt)
   NiPoint3 CalculateBladeBase(NiAVObject* weaponNode, bool isLeftHand);
   
        // ============================================
        // Blade Collision Detection
        // ============================================
        
        // Check if blades are colliding and get collision info
  bool CheckBladeCollision(BladeCollisionResult& outResult);
      
        // Get the last collision result
  const BladeCollisionResult& GetLastCollisionResult() const { return m_lastCollision; }
        
        // Check if blades are currently in contact
        bool AreBladesInContact() const { return m_bladesInContact; }
 
        // Check if collision is imminent (close but not touching)
  bool IsCollisionImminent() const { return m_collisionImminent; }
        
        // Check if blades are grinding (sustained contact)
      bool AreBladesGrinding() const { return m_bladesGrinding; }
        
   // Set collision threshold distance (default ~5 units)
        void SetCollisionThreshold(float threshold) { m_collisionThreshold = threshold; }
        float GetCollisionThreshold() const { return m_collisionThreshold; }
        
   // Set imminent collision threshold (default ~15 units)
   void SetImminentThreshold(float threshold) { m_imminentThreshold = threshold; }
 float GetImminentThreshold() const { return m_imminentThreshold; }
        
   // Register callback for blade collision events
        void SetCollisionCallback(BladeCollisionCallback callback) { m_collisionCallback = callback; }
  
   // Register callback for imminent collision events
        void SetImminentCallback(BladeImminentCallback callback) { m_imminentCallback = callback; }
        
    private:
        WeaponGeometryTracker() = default;
        ~WeaponGeometryTracker() = default;
WeaponGeometryTracker(const WeaponGeometryTracker&) = delete;
        WeaponGeometryTracker& operator=(const WeaponGeometryTracker&) = delete;
      
        // Update geometry for a single hand (equipped weapon)
        void UpdateHandGeometry(bool isLeftHand, float deltaTime);
        
        // Update geometry for a HIGGS-grabbed weapon
      void UpdateHiggsGrabbedGeometry(bool isLeftHand, TESObjectREFR* grabbedRef, float deltaTime);
   
        // Check for X-pose (crossed blades facing forward)
    void CheckXPose(const BladeGeometry& leftBlade, const BladeGeometry& rightBlade);
        
        // Get the appropriate weapon offset node name
        const char* GetWeaponOffsetNodeName(bool isLeftHand);
        
        // Log geometry state for debugging
     void LogGeometryState();
        
      // ============================================
        // Raycasting Collision Detection
        // ============================================
        
        // Cast multiple rays along a blade to detect intersection with another blade
        // Returns the number of rays that hit
     int RaycastBladeIntersection(
        const BladeGeometry& sourceBlade,
            const BladeGeometry& targetBlade,
      float bladeRadius,      // Thickness/radius of blade for hit detection
 BladeRaycastHit* outHits,    // Array to store hit results
  int maxHits// Maximum number of hits to return
        );
 
        // Cast a single ray from a point on one blade toward the other blade
        bool RaycastTowardBlade(
        const NiPoint3& rayOrigin,
            const NiPoint3& rayDirection,
            const BladeGeometry& targetBlade,
float maxDistance,
     BladeRaycastHit& outHit
        );
        
   // Check if a ray intersects a cylinder (blade approximated as cylinder)
bool RayIntersectsCylinder(
      const NiPoint3& rayOrigin,
   const NiPoint3& rayDirection,
 const NiPoint3& cylinderBase,
            const NiPoint3& cylinderTip,
            float cylinderRadius,
            float& outDistance,
        NiPoint3& outHitPoint
        );
        
   // ============================================
        // Collision Detection Helpers
     // ============================================
        
  // Calculate closest distance between two line segments (blade edges)
 float ClosestDistanceBetweenSegments(
      const NiPoint3& p1, const NiPoint3& q1,
        const NiPoint3& p2, const NiPoint3& q2,
            float& outParam1, float& outParam2,
       NiPoint3& outClosestPoint1,
   NiPoint3& outClosestPoint2
        );
     
  // Estimate time to collision based on closing velocity
        float EstimateTimeToCollision(float distance, float closingVelocity);
    
        // Estimate time to collision with scaled threshold (for dynamic blade length scaling)
        float EstimateTimeToCollisionScaled(float distance, float closingVelocity, float scaledCollisionThreshold);
      
        // Helper: dot product
        static float Dot(const NiPoint3& a, const NiPoint3& b);
   
      // Helper: cross product
        static NiPoint3 Cross(const NiPoint3& a, const NiPoint3& b);
        
        // Helper: vector length
        static float Length(const NiPoint3& v);
        
     // Helper: normalize vector
        static NiPoint3 Normalize(const NiPoint3& v);
 
     // Helper: clamp value
    static float Clamp(float value, float min, float max);
        
        // Helper: point along segment
        static NiPoint3 PointAlongSegment(const NiPoint3& start, const NiPoint3& end, float t);
        
    WeaponGeometryState m_geometryState;
        BladeCollisionResult m_lastCollision;
        BladeCollisionCallback m_collisionCallback = nullptr;
        BladeImminentCallback m_imminentCallback = nullptr;
        
    bool m_initialized = false;
        bool m_bladesInContact = false;
  bool m_wasInContact = false;
     bool m_collisionImminent = false;
        bool m_wasImminent = false;
  bool m_bladesGrinding = false;   // Blades are in sustained contact (grinding)
        bool m_wasGrinding = false;
    float m_grindStartTime = 0.0f;      // When grinding started
        float m_grindDuration = 0.0f;       // How long blades have been grinding
        bool m_inXPose = false;             // Currently in X-pose
        bool m_wasInXPose = false;          // Was in X-pose last frame
        float m_lastUpdateTime = 0.0f;
        
        // Collision detection parameters (use config values)
        float m_collisionThreshold = 5.0f;      // Will be updated from config
        float m_imminentThreshold = 15.0f;      // Will be updated from config
        
        // Raycast configuration
        static const int RAYCAST_SAMPLES = 5;   // Number of rays to cast along each blade
        static const float BLADE_RADIUS;        // Approximated blade thickness for raycast
        
        // Grace period tracking - don't trigger collision right after equipping
      int m_framesSinceEquipChange = 0;
        UInt32 m_lastLeftWeaponFormID = 0;
   UInt32 m_lastRightWeaponFormID = 0;
    };
    
  // Convenience function to initialize weapon geometry tracking
    void InitializeWeaponGeometryTracker();
    
    // Convenience function to update weapon geometry (call each frame)
    void UpdateWeaponGeometry(float deltaTime);
}
