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
	extern float bladeReequipThreshold;      // Distance required before re-equipping weapon
	extern float bladeCollisionTimeout;         // Time (seconds) without collision before considered separated
	extern float bladeTimeToCollisionThreshold; // Time-based collision prediction threshold
	extern float bladeReequipCooldown;          // Cooldown after re-equip before another unequip can trigger
	extern float reequipDelay;                  // Delay after activating weapon before equipping
	extern float swingVelocityThreshold;     // Swing velocity threshold
	
	// Auto-equip grabbed weapon settings
	extern bool autoEquipGrabbedWeaponEnabled;  // Enable/disable auto-equip feature
	extern float autoEquipGrabbedWeaponDelay;   // Delay before auto-equipping grabbed weapon

	// Trigger-based weapon hold settings
	extern float triggerUnequipDelay;           // Delay (seconds) after trigger release before unequipping weapon

	// Collision avoidance hand preference (0 = left hand unequips, 1 = right hand unequips)
	extern int collisionAvoidanceHand;          // Which hand gets unequipped/grabbed during dual-wield collision

	// Close combat settings
	extern float closeCombatEnterDistance;      // Distance to enemy at which close combat mode activates
	extern float closeCombatExitDistance;       // Distance to enemy at which close combat mode deactivates (buffer)

	// Shield collision settings
	extern float shieldCollisionThreshold;       // Distance at which weapon is considered touching shield
	extern float shieldImminentThreshold;        // Distance at which collision is imminent (triggers unequip)
	extern float shieldImminentThresholdBackup;  // Backup threshold, larger safety net
	extern float shieldReequipThreshold;       // Distance required before re-equipping weapon
	extern float shieldCollisionTimeout;         // Time (seconds) without collision before considered separated
	extern float shieldTimeToCollisionThreshold; // Time-based collision prediction threshold
	extern float shieldReequipCooldown;    // Cooldown after re-equip before another unequip can trigger
	extern float shieldReequipDelay;     // Delay after activating weapon before equipping
	extern float shieldSwingVelocityThreshold;   // Swing velocity threshold for shield collision
	extern float shieldRadius;     // Shield face detection radius

	// Shield bash settings
	extern bool shieldBashEnabled;   // Enable/disable shield bash tracking feature
	extern int shieldBashThreshold;              // Number of bashes required to trigger effect
	extern float shieldBashWindow;      // Time window (seconds) to register bashes
	extern float shieldBashLockoutDuration;      // Lockout duration (seconds) after triggering effect

	// Equipment change grace period
	extern int equipGraceFrames;         // Frames to wait after equipment change before collision detection

	void loadConfig();
	
	void Log(const int msgLogLevel, const char* fmt, ...);
	enum eLogLevels
	{
		LOGLEVEL_ERR = 0,
		LOGLEVEL_WARN,
		LOGLEVEL_INFO,
	};


#define LOG(fmt, ...) Log(LOGLEVEL_WARN, fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) Log(LOGLEVEL_ERR, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) Log(LOGLEVEL_INFO, fmt, ##__VA_ARGS__)


}