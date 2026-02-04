#include "config.h"

namespace FalseEdgeVR {
		
	int logging = 2;  // Default to INFO level
	int leftHandedMode = 0;

	// Blade collision settings - defaults
	float bladeCollisionThreshold = 5.0f;       // Distance at which blades are considered touching
	float bladeImminentThreshold = 25.0f;       // Distance at which collision is imminent (triggers unequip)
	float bladeImminentThresholdBackup = 30.0f; // Backup threshold, larger than primary
	float bladeReequipThreshold = 35.0f;        // Distance required before re-equipping weapon
	float bladeCollisionTimeout = 0.9f;       // Time (seconds) without collision before considered separated
	float bladeTimeToCollisionThreshold = 0.15f; // Time-based collision prediction threshold (150ms)
	float bladeReequipCooldown = 0.5f;          // Cooldown after re-equip (500ms)
	float reequipDelay = 0.002f;      // Delay after activating weapon before equipping (2ms)
	float swingVelocityThreshold = 150.0f;      // Swing velocity threshold (units per second)
	
	// Auto-equip grabbed weapon settings
	bool autoEquipGrabbedWeaponEnabled = true;  // Enable/disable auto-equip feature
	float autoEquipGrabbedWeaponDelay = 2.0f;   // Delay before auto-equipping grabbed weapon (2 seconds)

	// Trigger-based weapon hold settings
	float triggerUnequipDelay = 0.1f;     // Delay (seconds) after trigger release before unequipping (100ms default)

	// Collision avoidance hand preference (0 = left hand unequips, 1 = right hand unequips)
	int collisionAvoidanceHand = 0;             // Default: left hand gets unequipped/grabbed during dual-wield collision

	// Close combat settings
	float closeCombatEnterDistance = 70.0f;     // Enter close combat mode at 70 units (~1 meter)
	float closeCombatExitDistance = 90.0f;      // Exit close combat mode at 90 units (buffer to prevent rapid switching)

	// Shield collision settings - defaults same as blade collision
	float shieldCollisionThreshold = 5.0f;       // Distance at which weapon is considered touching shield
	float shieldImminentThreshold = 25.0f; // Distance at which collision is imminent (triggers unequip)
	float shieldImminentThresholdBackup = 30.0f; // Backup threshold, larger safety net
	float shieldReequipThreshold = 35.0f;        // Distance required before re-equipping weapon
	float shieldCollisionTimeout = 0.9f;         // Time (seconds) without collision before considered separated
	float shieldTimeToCollisionThreshold = 0.15f; // Time-based collision prediction threshold (150ms)
	float shieldReequipCooldown = 0.5f;  // Cooldown after re-equip (500ms)
	float shieldReequipDelay = 0.002f;           // Delay after activating weapon before equipping (2ms)
	float shieldSwingVelocityThreshold = 150.0f; // Swing velocity threshold (units per second)
	float shieldRadius = 15.0f;                // Shield face detection radius (units)

	// Shield bash settings - defaults
	bool shieldBashEnabled = true;   // Enable/disable shield bash tracking feature
	int shieldBashThreshold = 3;   // Number of bashes required to trigger effect
	float shieldBashWindow = 6.0f;// Time window (seconds) to register bashes
	float shieldBashLockoutDuration = 240.0f;    // Lockout duration (seconds) after triggering effect (4 minutes)

	// Equipment change grace period
	int equipGraceFrames = 20;    // Frames to wait after equipment change before collision detection (~0.22 sec at 90fps)

	// Grabbed weapon scaling settings
	bool weaponScalingEnabled = true;   // Enable weapon scaling when HIGGS grabbed (default enabled)
	float weaponGrabbedScale = 0.85f; // Scale factor for grabbed weapons (0.85 = 85% of original size)

	void loadConfig() 
	{
		std::string runtimeDirectory = GetRuntimeDirectory();

		if (!runtimeDirectory.empty()) 
		{
			std::string filepath = runtimeDirectory + "Data\\SKSE\\Plugins\\FalseEdgeVR.ini";
			std::ifstream file(filepath);

			if (!file.is_open()) 
			{
				transform(filepath.begin(), filepath.end(), filepath.begin(), ::tolower);
				file.open(filepath);
			}

			if (file.is_open()) 
			{
				std::string line;
				std::string currentSection;

				while (std::getline(file, line)) 
				{
					trim(line);
					skipComments(line);

					if (line.empty()) continue;

					if (line[0] == '[') 
					{
						// New section
						size_t endBracket = line.find(']');
						if (endBracket != std::string::npos) 
						{
							currentSection = line.substr(1, endBracket - 1);
							trim(currentSection);  
						}
					}
					else if (currentSection == "Settings") 
					{
						std::string variableName;
						std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

						if (variableName == "Logging") 
						{
							logging = std::stoi(variableValueStr);
						}
					}  
					else if (currentSection == "BladeCollision")
					{
						std::string variableName;
						std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

						if (variableName == "CollisionThreshold")
						{
							bladeCollisionThreshold = std::stof(variableValueStr);
						}
						else if (variableName == "ImminentThreshold")
						{
							bladeImminentThreshold = std::stof(variableValueStr);
						}
						else if (variableName == "ImminentThresholdBackup")
						{
							bladeImminentThresholdBackup = std::stof(variableValueStr);
						}
						else if (variableName == "ReequipThreshold")
						{
							bladeReequipThreshold = std::stof(variableValueStr);
						}
						else if (variableName == "CollisionTimeout")
						{
							bladeCollisionTimeout = std::stof(variableValueStr);
						}
						else if (variableName == "TimeToCollisionThreshold")
						{
							bladeTimeToCollisionThreshold = std::stof(variableValueStr);
						}
						else if (variableName == "ReequipCooldown")
						{
							bladeReequipCooldown = std::stof(variableValueStr);
						}
						else if (variableName == "ReequipDelay")
						{
							reequipDelay = std::stof(variableValueStr);
						}
						else if (variableName == "SwingVelocityThreshold")
						{
							swingVelocityThreshold = std::stof(variableValueStr);
						}
						else if (variableName == "CollisionAvoidanceHand")
						{
							collisionAvoidanceHand = std::stoi(variableValueStr);
						}
					}
					else if (currentSection == "AutoEquip")
					{
						std::string variableName;
						std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

						if (variableName == "Enabled")
						{
							autoEquipGrabbedWeaponEnabled = (std::stoi(variableValueStr) != 0);
						}
						else if (variableName == "Delay")
						{
							autoEquipGrabbedWeaponDelay = std::stof(variableValueStr);
						}
					}
					else if (currentSection == "TriggerHold")
					{
						std::string variableName;
						std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

						if (variableName == "UnequipDelay")
						{
							triggerUnequipDelay = std::stof(variableValueStr);
						}
					}
					else if (currentSection == "CloseCombat")
					{
						std::string variableName;
						std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

						if (variableName == "EnterDistance")
						{
							closeCombatEnterDistance = std::stof(variableValueStr);
						}
						else if (variableName == "ExitDistance")
						{
							closeCombatExitDistance = std::stof(variableValueStr);
						}
					}
					else if (currentSection == "ShieldCollision")
					{
						std::string variableName;
						std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

						if (variableName == "CollisionThreshold")
						{
							shieldCollisionThreshold = std::stof(variableValueStr);
						}
						else if (variableName == "ImminentThreshold")
						{
							shieldImminentThreshold = std::stof(variableValueStr);
						}
						else if (variableName == "ImminentThresholdBackup")
						{
							shieldImminentThresholdBackup = std::stof(variableValueStr);
						}
						else if (variableName == "ReequipThreshold")
						{
							shieldReequipThreshold = std::stof(variableValueStr);
						}
						else if (variableName == "CollisionTimeout")
						{
							shieldCollisionTimeout = std::stof(variableValueStr);
						}
						else if (variableName == "TimeToCollisionThreshold")
						{
							shieldTimeToCollisionThreshold = std::stof(variableValueStr);
						}
						else if (variableName == "ReequipCooldown")
						{
							shieldReequipCooldown = std::stof(variableValueStr);
						}
						else if (variableName == "ReequipDelay")
						{
							shieldReequipDelay = std::stof(variableValueStr);
						}
						else if (variableName == "SwingVelocityThreshold")
						{
							shieldSwingVelocityThreshold = std::stof(variableValueStr);
						}
						else if (variableName == "ShieldRadius")
						{
							shieldRadius = std::stof(variableValueStr);
						}
					}
					else if (currentSection == "ShieldBash")
					{
						std::string variableName;
						std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

						if (variableName == "Enabled")
						{
							shieldBashEnabled = (std::stoi(variableValueStr) != 0);
						}
						else if (variableName == "BashThreshold")
						{
							shieldBashThreshold = std::stoi(variableValueStr);
						}
						else if (variableName == "BashWindow")
						{
							shieldBashWindow = std::stof(variableValueStr);
						}
						else if (variableName == "LockoutDuration")
						{
							shieldBashLockoutDuration = std::stof(variableValueStr);
						}
					}
					else if (currentSection == "General")
					{
						std::string variableName;
						std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

						if (variableName == "EquipGraceFrames")
						{
							equipGraceFrames = std::stoi(variableValueStr);
						}
					}
					else if (currentSection == "WeaponScaling")
					{
						std::string variableName;
						std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

						if (variableName == "Enabled")
						{
							weaponScalingEnabled = (std::stoi(variableValueStr) != 0);
						}
						else if (variableName == "GrabbedScale")
						{
							weaponGrabbedScale = std::stof(variableValueStr);
						}
					}
				} 
			}
			_MESSAGE("Config loaded successfully.");
			_MESSAGE("BladeCollision settings:");
			_MESSAGE("  CollisionThreshold=%.2f, ImminentThreshold=%.2f, ImminentThresholdBackup=%.2f",
				bladeCollisionThreshold, bladeImminentThreshold, bladeImminentThresholdBackup);
			_MESSAGE("  ReequipThreshold=%.2f, CollisionTimeout=%.3f, TimeToCollisionThreshold=%.3f",
				bladeReequipThreshold, bladeCollisionTimeout, bladeTimeToCollisionThreshold);
			_MESSAGE("  ReequipCooldown=%.3f, ReequipDelay=%.4f, SwingVelocityThreshold=%.1f",
				bladeReequipCooldown, reequipDelay, swingVelocityThreshold);
			_MESSAGE("  CollisionAvoidanceHand=%d (%s hand unequips during dual-wield collision)",
				collisionAvoidanceHand, collisionAvoidanceHand == 0 ? "LEFT" : "RIGHT");
			_MESSAGE("AutoEquip settings: Enabled=%s, Delay=%.2f",
				autoEquipGrabbedWeaponEnabled ? "true" : "false", autoEquipGrabbedWeaponDelay);
			_MESSAGE("TriggerHold settings: UnequipDelay=%.3f",
				triggerUnequipDelay);
			_MESSAGE("CloseCombat settings: EnterDistance=%.1f, ExitDistance=%.1f",
				closeCombatEnterDistance, closeCombatExitDistance);
			_MESSAGE("ShieldCollision settings:");
			_MESSAGE("  CollisionThreshold=%.2f, ImminentThreshold=%.2f, ImminentThresholdBackup=%.2f",
				shieldCollisionThreshold, shieldImminentThreshold, shieldImminentThresholdBackup);
			_MESSAGE("  ReequipThreshold=%.2f, CollisionTimeout=%.3f, TimeToCollisionThreshold=%.3f",
				shieldReequipThreshold, shieldCollisionTimeout, shieldTimeToCollisionThreshold);
			_MESSAGE("  ReequipCooldown=%.3f, ReequipDelay=%.4f, SwingVelocityThreshold=%.1f, ShieldRadius=%.1f",
				shieldReequipCooldown, shieldReequipDelay, shieldSwingVelocityThreshold, shieldRadius);
			_MESSAGE("ShieldBash settings: Enabled=%s, BashThreshold=%d, BashWindow=%.1f, LockoutDuration=%.0f",
				shieldBashEnabled ? "true" : "false", shieldBashThreshold, shieldBashWindow, shieldBashLockoutDuration);
			_MESSAGE("General settings: EquipGraceFrames=%d", equipGraceFrames);
			_MESSAGE("WeaponScaling settings: Enabled=%s, GrabbedScale=%.2f",
				weaponScalingEnabled ? "true" : "false", weaponGrabbedScale);
			return;
		}
		return;
	}

	void Log(const int msgLogLevel, const char* fmt, ...)
	{
		if (msgLogLevel > logging)
		{
			return;
		}

		va_list args;
		char logBuffer[4096];

		va_start(args, fmt);
		vsprintf_s(logBuffer, sizeof(logBuffer), fmt, args);
		va_end(args);

		_MESSAGE(logBuffer);
	}

}