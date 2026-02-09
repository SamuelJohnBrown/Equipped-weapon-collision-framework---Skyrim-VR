#include "DaggerFlipTracker.h"
#include "VRInputHandler.h"
#include "EquipManager.h"
#include "Engine.h"
#include "skse64/GameForms.h"
#include "skse64/NiNodes.h"
#include "common/IDebugLog.h"
#include <cmath>

namespace FalseEdgeVR
{
    DaggerFlipTracker* DaggerFlipTracker::GetSingleton()
    {
        static DaggerFlipTracker instance;
        return &instance;
    }

    void DaggerFlipTracker::Initialize()
    {
        if (m_initialized)
        return;

        _MESSAGE("DaggerFlipTracker: Initializing...");
   
   // Clear all state on init
    ClearAllState();
  
        m_initialized = true;
        _MESSAGE("DaggerFlipTracker: Initialized successfully");
    }

    bool DaggerFlipTracker::IsDagger(TESForm* form)
    {
        if (!form)
         return false;

        // Use EquipManager's weapon type detection
        WeaponType type = EquipManager::GetWeaponType(form);
  return (type == WeaponType::Dagger);
    }

    void DaggerFlipTracker::OnDropped(bool isLeftVRController, TESObjectREFR* droppedRefr)
    {
 if (!droppedRefr || !droppedRefr->baseForm)
   return;

  // Only track daggers
        if (!IsDagger(droppedRefr->baseForm))
     return;

UInt32 daggerFormID = droppedRefr->baseForm->formID;
    const char* handName = isLeftVRController ? "LEFT" : "RIGHT";

        // Check if drop protection is disabled (grip spam detected = intentional throw)
        bool dropProtectionDisabled = VRInputHandler::IsDropProtectionDisabled(isLeftVRController);

        if (dropProtectionDisabled)
        {
            _MESSAGE("DaggerFlipTracker: === DAGGER THROWN === %s hand (FormID: %08X)", 
        handName, daggerFormID);
            _MESSAGE("DaggerFlipTracker: Drop protection is DISABLED - flip catch window OPEN");
 
         // Mark that we're in the flip catch window
    if (isLeftVRController)
      {
 m_inFlipWindowLeft = true;
     m_lastDroppedDaggerFormIDLeft = daggerFormID;
            }
        else
    {
   m_inFlipWindowRight = true;
   m_lastDroppedDaggerFormIDRight = daggerFormID;
            }
        }
        else
        {
       _MESSAGE("DaggerFlipTracker: Dagger dropped from %s hand (normal drop, not in flip window)", handName);
  
       // Normal drop - clear the flip window
            if (isLeftVRController)
       {
   m_inFlipWindowLeft = false;
       m_lastDroppedDaggerFormIDLeft = 0;
            }
         else
 {
    m_inFlipWindowRight = false;
         m_lastDroppedDaggerFormIDRight = 0;
            }
      }
    }

    void DaggerFlipTracker::OnGrabbed(bool isLeftVRController, TESObjectREFR* grabbedRefr)
 {
        if (!grabbedRefr || !grabbedRefr->baseForm)
        return;

        // Only track daggers
      if (!IsDagger(grabbedRefr->baseForm))
         return;

        UInt32 daggerFormID = grabbedRefr->baseForm->formID;
const char* handName = isLeftVRController ? "LEFT" : "RIGHT";

        // Check if we're in the flip catch window
        bool inFlipWindow = isLeftVRController ? m_inFlipWindowLeft : m_inFlipWindowRight;
        UInt32 lastDroppedFormID = isLeftVRController ? m_lastDroppedDaggerFormIDLeft : m_lastDroppedDaggerFormIDRight;
        bool isFlipped = isLeftVRController ? m_isFlippedLeft : m_isFlippedRight;

        _MESSAGE("DaggerFlipTracker: === DAGGER GRABBED === %s hand (FormID: %08X)", handName, daggerFormID);
        _MESSAGE("DaggerFlipTracker:   In flip window: %s", inFlipWindow ? "YES" : "NO");
     _MESSAGE("DaggerFlipTracker:   Last dropped FormID: %08X", lastDroppedFormID);
        _MESSAGE("DaggerFlipTracker:   Same dagger: %s", (lastDroppedFormID == daggerFormID) ? "YES" : "NO");
        _MESSAGE("DaggerFlipTracker:   Currently flipped: %s", isFlipped ? "YES" : "NO");

        // === LOG WORLD OBJECT ORIENTATION ===
        LogGrabbedObjectOrientation(isLeftVRController, grabbedRefr);

        // Check if we caught the same dagger we just threw
  if (inFlipWindow && lastDroppedFormID == daggerFormID)
        {
       _MESSAGE("DaggerFlipTracker: *** FLIP CATCH DETECTED! ***");

        // Toggle the flip state
            if (isFlipped)
     {
      _MESSAGE("DaggerFlipTracker: >>> WOULD RESTORE to NORMAL orientation <<<");
       
            if (isLeftVRController)
          m_isFlippedLeft = false;
       else
      m_isFlippedRight = false;
            }
  else
 {
          _MESSAGE("DaggerFlipTracker: >>> WOULD FLIP to INVERTED orientation <<<");
 
         if (isLeftVRController)
              m_isFlippedLeft = true;
              else
     m_isFlippedRight = true;
 }
        }
        else
        {
            _MESSAGE("DaggerFlipTracker: Normal grab (not a flip catch)");
        }

     // Clear the flip window (we've processed the catch)
  if (isLeftVRController)
        {
     m_inFlipWindowLeft = false;
        m_lastDroppedDaggerFormIDLeft = 0;
        }
        else
    {
            m_inFlipWindowRight = false;
        m_lastDroppedDaggerFormIDRight = 0;
 }
    }

    void DaggerFlipTracker::LogGrabbedObjectOrientation(bool isLeftVRController, TESObjectREFR* grabbedRefr)
    {
        if (!grabbedRefr)
      return;

        const char* handName = isLeftVRController ? "LEFT" : "RIGHT";

        // Log the world position of the grabbed object
        _MESSAGE("DaggerFlipTracker: --- WORLD OBJECT ORIENTATION ---");
        _MESSAGE("DaggerFlipTracker:   World Position: (%.2f, %.2f, %.2f)", 
   grabbedRefr->pos.x, grabbedRefr->pos.y, grabbedRefr->pos.z);
        _MESSAGE("DaggerFlipTracker:   World Rotation (radians): (%.3f, %.3f, %.3f)", 
            grabbedRefr->rot.x, grabbedRefr->rot.y, grabbedRefr->rot.z);
        
        // Convert to degrees for easier reading
 float rotXDeg = grabbedRefr->rot.x * (180.0f / 3.14159f);
        float rotYDeg = grabbedRefr->rot.y * (180.0f / 3.14159f);
        float rotZDeg = grabbedRefr->rot.z * (180.0f / 3.14159f);
        _MESSAGE("DaggerFlipTracker:   World Rotation (degrees): (%.1f, %.1f, %.1f)", 
 rotXDeg, rotYDeg, rotZDeg);

        // Check if the dagger is upside down based on X rotation
        // Normal grip: X rotation close to 0
   // Inverted grip: X rotation close to 180 or -180 (or PI/-PI in radians)
        bool isUpsideDown = false;
        
        // X rotation determines if blade is pointing up or down
   // If X rotation is between 90-270 degrees (or -90 to -270), it's likely upside down
        float absRotX = fabs(rotXDeg);
        if (absRotX > 90.0f && absRotX < 270.0f)
        {
    isUpsideDown = true;
    }
        
        // Also check based on the actual rotation value being close to PI
        float absRotXRad = fabs(grabbedRefr->rot.x);
        if (absRotXRad > 1.57f && absRotXRad < 4.71f)  // Between PI/2 and 3*PI/2
        {
     isUpsideDown = true;
     }

        _MESSAGE("DaggerFlipTracker:   Upside down check (X rotation): %s", isUpsideDown ? "YES - INVERTED" : "NO - NORMAL");

      // Also log the HIGGS grab transform if available
        if (higgsInterface)
        {
NiTransform grabTransform = higgsInterface->GetGrabTransform(isLeftVRController);
   
      _MESSAGE("DaggerFlipTracker: --- HIGGS GRAB TRANSFORM ---");
    _MESSAGE("DaggerFlipTracker:   Position offset: (%.2f, %.2f, %.2f)", 
    grabTransform.pos.x, grabTransform.pos.y, grabTransform.pos.z);
          _MESSAGE("DaggerFlipTracker:   Rotation matrix row 0: (%.3f, %.3f, %.3f)", 
   grabTransform.rot.data[0][0], grabTransform.rot.data[0][1], grabTransform.rot.data[0][2]);
          _MESSAGE("DaggerFlipTracker:   Rotation matrix row 1: (%.3f, %.3f, %.3f)", 
  grabTransform.rot.data[1][0], grabTransform.rot.data[1][1], grabTransform.rot.data[1][2]);
  _MESSAGE("DaggerFlipTracker:   Rotation matrix row 2: (%.3f, %.3f, %.3f)", 
     grabTransform.rot.data[2][0], grabTransform.rot.data[2][1], grabTransform.rot.data[2][2]);
   
   // Check if grab transform indicates inverted grip
            // The Z column of the rotation matrix (data[x][2]) indicates the "up" direction
            // If Z is pointing down (negative values in row 2), it's inverted
    float upZ = grabTransform.rot.data[2][2];
            _MESSAGE("DaggerFlipTracker:   Grab transform up-axis Z: %.3f (%s)", 
    upZ, (upZ < 0) ? "INVERTED" : "NORMAL");
    
       if (upZ < 0)
   {
          _MESSAGE("DaggerFlipTracker: >>> GRABBED UPSIDE DOWN - WOULD BE INVERTED GRIP <<<");
 }
        }
        
        _MESSAGE("DaggerFlipTracker: ---------------------------------");
    }

    void DaggerFlipTracker::ApplyFlippedTransformIfNeeded()
    {
        // DISABLED - just logging for now
    }

    void DaggerFlipTracker::StoreOriginalTransform(bool isLeftVRController)
    {
   // DISABLED - just logging for now
    }

    void DaggerFlipTracker::DisableVrikHandControl(bool isLeftVRController)
    {
        // DISABLED - just logging for now
    }

    void DaggerFlipTracker::RestoreVrikHandControl(bool isLeftVRController)
    {
    // DISABLED - just logging for now
    }

    void DaggerFlipTracker::ApplyFlippedTransform(bool isLeftVRController)
    {
        // DISABLED - just logging for now
    }

    void DaggerFlipTracker::ApplyNormalTransform(bool isLeftVRController)
    {
        // DISABLED - just logging for now
    }

 bool DaggerFlipTracker::IsDaggerFlipped(bool isLeftVRController) const
    {
    return isLeftVRController ? m_isFlippedLeft : m_isFlippedRight;
    }

 void DaggerFlipTracker::ClearFlipState(bool isLeftVRController)
    {
   if (isLeftVRController)
        {
    m_isFlippedLeft = false;
            m_needsTransformApplyLeft = false;
   m_inFlipWindowLeft = false;
            m_lastDroppedDaggerFormIDLeft = 0;
            m_hasOriginalTransformLeft = false;
        }
        else
        {
    m_isFlippedRight = false;
 m_needsTransformApplyRight = false;
      m_inFlipWindowRight = false;
  m_lastDroppedDaggerFormIDRight = 0;
        m_hasOriginalTransformRight = false;
      }
    }

void DaggerFlipTracker::ClearAllState()
    {
    _MESSAGE("DaggerFlipTracker: Clearing all state");

        m_isFlippedLeft = false;
  m_isFlippedRight = false;
        m_needsTransformApplyLeft = false;
   m_needsTransformApplyRight = false;
  m_inFlipWindowLeft = false;
        m_inFlipWindowRight = false;
      m_lastDroppedDaggerFormIDLeft = 0;
  m_lastDroppedDaggerFormIDRight = 0;
     m_hasOriginalTransformLeft = false;
        m_hasOriginalTransformRight = false;
    }

    // Helper function implementation
  bool IsDropProtectionDisabledForHand(bool isLeftVRController)
    {
    return VRInputHandler::IsDropProtectionDisabled(isLeftVRController);
    }
}
