#pragma once
#include <iostream>
#include <string>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <skse64/NiProperties.h>
#include <skse64/NiNodes.h>

#include "skse64\GameSettings.h"
#include "Utility.hpp"

#include <skse64/GameData.h>

#include "higgsinterface001.h"
#include "vrikinterface001.h"
#include "SkyrimVRESLAPI.h"

namespace FalseEdgeVR {

	const UInt32 MOD_VERSION = 0x10000;
	const std::string MOD_VERSION_STR = "1.0.0";
	extern int leftHandedMode;

	extern int logging;

	// Blade collision settings
	extern float bladeCollisionThreshold;       // Distance at which blades are considered touching
	extern float bladeImminentThreshold;        // Distance at which collision is imminent (triggers unequip)
	extern float bladeImminentThresholdBackup;  // Backup threshold, larger than primary
	extern float bladeTimeToCollisionThreshold; // Time-based collision prediction threshold
	extern float bladeReequipCooldown;          // Cooldown after re-equip before another unequip can trigger
	extern float swingVelocityThreshold;     // Swing velocity threshold
	
	// Auto-equip grabbed weapon settings
	extern bool autoEquipGrabbedWeaponEnabled;  // Enable/disable auto-equip feature
	extern float autoEquipGrabbedWeaponDelay;   // Delay before auto-equipping grabbed weapon

	// Trigger-based weapon hold settings
	extern float triggerUnequipDelay;           // Delay (seconds) after trigger release before unequipping weapon

	// When true, equipping a grabbed weapon requires a tap (press+release)
	// followed by an immediate press-and-hold. When false (default), a simple
	// trigger hold equips the grabbed weapon.
	extern bool tapThenHoldGrabEquip;

	// Intentional drop settings (grip spam detection)
	extern int gripSpamThreshold;    // Number of grip releases to trigger intentional drop
	extern float gripSpamWindow;         // Time window (seconds) for grip releases
	extern float dropProtectionDisableTime;     // How long drop protection is disabled (seconds)

	// Weapon lock settings (trigger spam detection)
	extern int triggerSpamThreshold;            // Number of trigger presses to toggle weapon lock
	extern float triggerSpamWindow;           // Time window (seconds) for trigger presses
	extern float vanillaDefaultRecoverySeconds; // After both hands fall back to vanilla equip, holster to grab

	// Weapon spawn offset settings (when unequipping for HIGGS grab)
	// Non-mounted: spawn behind player so they can't see it
	extern float spawnOffsetX;       // X offset from player (negative = behind based on facing)
	extern float spawnOffsetY;      // Y offset from player (negative = behind based on facing)
	extern float spawnOffsetZ;           // Z offset from player (negative = below)
	extern float spawnDistance; // Distance behind player (units, 70 = ~1 meter)
	
	// Mounted: spawn elevated to avoid horse collision
	extern float spawnOffsetMountedX;  // X offset when mounted
	extern float spawnOffsetMountedY;       // Y offset when mounted
	extern float spawnOffsetMountedZ;           // Z offset when mounted (positive = above)

	// Collision avoidance hand preference (0 = left hand unequips, 1 = right hand unequips)
	extern int collisionAvoidanceHand;          // Which hand gets unequipped/grabbed during dual-wield collision

	// Mount: equip grabbed weapons while riding; holster to grab on dismount
	extern bool mountWeaponHandlingEnabled;

	// Staff: include in trigger-hold / holster-to-grab when enabled
	extern bool staffTrackingEnabled;
	extern float staffTriggerReleaseUnequipDelay; // Delay after trigger release before holstering staff

	// Two-handed melee: auto-enabled when 2hWeaponsUnlocked is loaded
	extern bool twoHandedTrackingEnabled;

	// Door stow: when grabbing a 2H weapon in each hand while facing a nearby door,
	// stow both to inventory until the player turns away, then re-equip them.
	extern bool doorStowDual2HEnabled;       // Enable/disable the feature
	extern float doorStowDual2HProximity;    // How close (units) the player must be to the door

	// Set twoHandedTrackingEnabled from load order (call after PostLoad)
	void InitTwoHandedTrackingFromLoadOrder();
	// Shield bash settings
	extern bool shieldBashEnabled;
	extern int shieldBashThreshold;
	extern float shieldBashWindow;
	extern float shieldBashLockoutDuration;

	// Equipment change grace period
	extern int equipGraceFrames;

	// Weapon exclusion list (mod items excluded from tracking/sounds/trigger system)
	struct WeaponExclusionEntry
	{
		std::string pluginName;  // empty = match full FormID directly
		UInt32 formID = 0;       // local FormID (with pluginName) or full FormID
	};

	extern std::vector<WeaponExclusionEntry> weaponExclusionEntries;
	bool IsExcludedWeaponFormID(UInt32 formID);

	// Load configuration from INI file
	void loadConfig();
	
	// Logging
	void Log(const int msgLogLevel, const char* fmt, ...);
	#define LOG(fmt, ...) Log(2, fmt, __VA_ARGS__)
}
