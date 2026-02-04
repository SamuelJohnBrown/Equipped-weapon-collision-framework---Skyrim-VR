#pragma once

#include "Helper.h"
#include "skse64/PluginAPI.h"

namespace FalseEdgeVR
{
	extern SKSETrampolineInterface* g_trampolineInterface;
	extern HiggsPluginAPI::IHiggsInterface001* higgsInterface;
	extern vrikPluginApi::IVrikInterface001* vrikInterface;
	extern SkyrimVRESLPluginAPI::ISkyrimVRESLInterface001* skyrimVRESLInterface;
	extern SKSEVRInterface* g_vrInterface;
	extern PluginHandle g_pluginHandle;
	extern SKSETaskInterface* g_task;

	void StartMod();

	// ============================================
	// Left-Handed Mode Support
	// ============================================
	
	// Check if the game is in left-handed VR mode
	// In left-handed mode, VR controllers are inverted:
	//   - Left VR controller = Right game hand
	//   - Right VR controller = Left game hand
	bool IsLeftHandedMode();
	
	// Convert VR controller hand (from HIGGS callbacks) to game hand
	// HIGGS isLeft parameter refers to VR controller, not game hand
	// Returns: true = game left hand, false = game right hand
	bool VRControllerToGameHand(bool isLeftVRController);
	
	// Convert game hand to VR controller
	// Returns: true = left VR controller, false = right VR controller
	bool GameHandToVRController(bool isLeftGameHand);

	// Get which game hand should be unequipped during dual-wield collision (from INI)
	bool GetCollisionAvoidanceHandIsLeft();

	// ============================================
	// Spell Casting
	// ============================================
	
	// Cast a spell on the player (self-cast)
	// formId is the full FormID (e.g., 0x000AA026 for Skyrim.esm spell)
	void CastSpellOnPlayer(UInt32 formId);

	// ============================================
	// Sound Playing
	// ============================================
	
	// Play a sound at the player's location
	// soundFormId is the full FormID of the SOUN record
	void PlaySoundAtPlayer(UInt32 soundFormId);

	// Play a sound at any actor's location (NPC or player)
	// soundFormId is the full FormID of the SOUN record
	void PlaySoundAtActor(UInt32 soundFormId, Actor* actor);

	// Set the ownership of an object reference to the player
	// This prevents the item from being flagged as stolen when picked up
	void SetOwnerToPlayer(TESObjectREFR* objRef);

	// Delete a spawned world object (removes it from the world)
	// Used to clean up PlaceAtMe spawned objects that are no longer needed
	void DeleteWorldObject(TESObjectREFR* objRef);

	// Remove an item from player inventory after a short delay
	// This is used to clean up the duplicate spawned weapon after activation
	// Only removes 1 item and won't remove equipped items
	void DelayedRemoveItemFromInventory(UInt32 itemFormId, int delayMs = 100);

	// ============================================
	// Blocking (X-Pose)
	// ============================================
	
	// Start blocking on the player (for X-pose dual wield block)
	void StartBlocking();
	
	// Stop blocking on the player
	void StopBlocking();
	
	// Check if player is currently blocking
	bool IsBlocking();

	// ============================================
	// Weapon Scaling (HIGGS grabbed weapons)
	// ============================================
	
	// Scale a world object (weapon) to a target scale
	// This sets both local and world transform scales and updates all child nodes
	void ScaleWorldObject(TESObjectREFR* refr, float targetScale);
	
	// Track a grabbed weapon for continuous scale updates
	// isLeftVRController: which VR controller hand grabbed it
	void TrackGrabbedWeapon(TESObjectREFR* weaponRefr, bool isLeftVRController);
	
	// Clear tracking for a grabbed weapon when dropped
	void ClearGrabbedWeapon(bool isLeftVRController);
	
	// Update scales for all tracked grabbed weapons (call each frame)
	void UpdateGrabbedWeaponScales();
	
	// Get the currently tracked grabbed weapon for a hand
	TESObjectREFR* GetGrabbedWeapon(bool isLeftVRController);
}