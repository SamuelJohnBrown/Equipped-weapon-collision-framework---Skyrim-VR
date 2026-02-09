#pragma once

#include "skse64/GameReferences.h"
#include "skse64/NiNodes.h"
#include "higgsinterface001.h"
#include "vrikinterface001.h"
#include "EquipManager.h"

namespace FalseEdgeVR
{
    // ============================================
    // DaggerFlipTracker
    // ============================================
    // Detects when player rapid-grip-releases a dagger, throws it in the air,
    // and catches it during the drop protection window.
    // When caught in this state, the dagger's transform is inverted (upside-down).
    // Catching again while inverted returns it to normal orientation.
    // ============================================

    class DaggerFlipTracker
    {
    public:
        static DaggerFlipTracker* GetSingleton();

        // Initialize the tracker (register callbacks)
        void Initialize();

        // Called when an object is grabbed by HIGGS
        void OnGrabbed(bool isLeftVRController, TESObjectREFR* grabbedRefr);

        // Called when an object is dropped by HIGGS
        void OnDropped(bool isLeftVRController, TESObjectREFR* droppedRefr);

        // Called every frame from PostVrikPostHiggs callback to apply flipped transform
        // This ensures our transform is applied AFTER HIGGS sets its transform
        void ApplyFlippedTransformIfNeeded();

        // Check if a form is a dagger
        static bool IsDagger(TESForm* form);

        // Check if dagger is currently flipped for a hand
        bool IsDaggerFlipped(bool isLeftVRController) const;

        // Clear flip state for a hand
        void ClearFlipState(bool isLeftVRController);

        // Clear all state
        void ClearAllState();

    private:
        DaggerFlipTracker() = default;
        ~DaggerFlipTracker() = default;
        DaggerFlipTracker(const DaggerFlipTracker&) = delete;
        DaggerFlipTracker& operator=(const DaggerFlipTracker&) = delete;

        // Log the orientation of a grabbed object to determine if it's upside down
        void LogGrabbedObjectOrientation(bool isLeftVRController, TESObjectREFR* grabbedRefr);

        // Store the original transform before flipping
        void StoreOriginalTransform(bool isLeftVRController);

        // Apply flipped transform to grabbed dagger
        void ApplyFlippedTransform(bool isLeftVRController);

        // Apply normal transform to grabbed dagger
        void ApplyNormalTransform(bool isLeftVRController);

        // Disable VRIK hand control so flipped transform works properly
        void DisableVrikHandControl(bool isLeftVRController);

        // Restore VRIK hand control when dagger is unflipped
        void RestoreVrikHandControl(bool isLeftVRController);

        // Store the original grab transform for each hand
        NiTransform m_originalTransformLeft;
        NiTransform m_originalTransformRight;
        bool m_hasOriginalTransformLeft = false;
        bool m_hasOriginalTransformRight = false;

        // Track if dagger is currently flipped for each hand
        bool m_isFlippedLeft = false;
        bool m_isFlippedRight = false;

        // Track if transform needs to be applied this frame (set in OnGrabbed, applied in PostVrikPostHiggs)
        bool m_needsTransformApplyLeft = false;
        bool m_needsTransformApplyRight = false;

        // Track the last dropped dagger FormID for each hand (to detect catch)
        UInt32 m_lastDroppedDaggerFormIDLeft = 0;
        UInt32 m_lastDroppedDaggerFormIDRight = 0;

        // Track if we're in the "flip catch" window for each hand
        // This is true when drop protection is disabled (grip spam detected)
        // and a dagger was just dropped
        bool m_inFlipWindowLeft = false;
        bool m_inFlipWindowRight = false;

        bool m_initialized = false;
    };

    // Helper function to check if drop protection is currently disabled
    bool IsDropProtectionDisabledForHand(bool isLeftVRController);
}
