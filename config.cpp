#include "config.h"
#include "skse64/GameData.h"

namespace FalseEdgeVR {

	static UInt32 ParseHexFormID(const std::string& value)
	{
		if (value.empty())
			return 0;

		std::string hex = value;
		if (hex.size() >= 2 && (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')))
			hex = hex.substr(2);

		try
		{
			return static_cast<UInt32>(std::stoul(hex, nullptr, 16));
		}
		catch (...)
		{
			return 0;
		}
	}

	std::vector<WeaponExclusionEntry> weaponExclusionEntries;

	bool IsExcludedWeaponFormID(UInt32 formID)
	{
		if (formID == 0)
			return false;

		UInt8 weaponModIndex = (formID >> 24) & 0xFF;
		UInt32 weaponBaseFormID = formID & 0x00FFFFFF;
		if (weaponModIndex == 0xFE)
			weaponBaseFormID = formID & 0x00000FFF;

		for (const WeaponExclusionEntry& entry : weaponExclusionEntries)
		{
			if (entry.formID == 0)
				continue;

			if (entry.pluginName.empty())
			{
				if (formID == entry.formID)
					return true;
				continue;
			}

			DataHandler* dataHandler = DataHandler::GetSingleton();
			if (!dataHandler)
				continue;

			const ModInfo* modInfo = dataHandler->LookupModByName(entry.pluginName.c_str());
			if (!modInfo || !modInfo->IsActive())
				continue;

			UInt8 modIndex = modInfo->GetPartialIndex();
			if (weaponModIndex != modIndex)
				continue;

			UInt32 entryBaseFormID = entry.formID & 0x00FFFFFF;
			if (modIndex == 0xFE)
				entryBaseFormID = entry.formID & 0x00000FFF;

			if (weaponBaseFormID == entryBaseFormID)
				return true;
		}

		return false;
	}
		
	int logging = 2;  // Default to INFO level
	int leftHandedMode = 0;

	// Blade collision settings - defaults
	float bladeCollisionThreshold = 5.0f;       // Distance at which blades are considered touching
	float bladeImminentThreshold = 25.0f;       // Distance at which collision is imminent (triggers unequip)
	float bladeImminentThresholdBackup = 30.0f; // Backup threshold, larger than primary
	float bladeTimeToCollisionThreshold = 0.15f; // Time-based collision prediction threshold (150ms)
	float bladeReequipCooldown = 0.5f;          // Cooldown after re-equip (500ms)
	float swingVelocityThreshold = 150.0f;      // Swing velocity threshold (units per second)
	
	// Auto-equip grabbed weapon settings
	bool autoEquipGrabbedWeaponEnabled = true;  // Enable/disable auto-equip feature
	float autoEquipGrabbedWeaponDelay = 2.0f;   // Delay before auto-equipping grabbed weapon (2 seconds)

	// Trigger-based weapon hold settings
	float triggerUnequipDelay = 0.1f;     // Delay (seconds) after trigger release before unequipping (100ms default)
	bool tapThenHoldGrabEquip = false;    // Off by default: simple hold equips grabbed weapon

	// Intentional drop settings (grip spam detection)
	int gripSpamThreshold = 4;       // Number of grip releases to trigger intentional drop
	float gripSpamWindow = 2.0f;   // Time window (seconds) for grip releases
	float dropProtectionDisableTime = 3.0f; // How long drop protection is disabled (seconds)

	// Weapon lock settings (trigger spam detection)
	int triggerSpamThreshold = 4;      // Number of trigger presses to toggle weapon lock
	float triggerSpamWindow = 2.0f;     // Time window (seconds) for trigger presses

	// Weapon spawn offset settings (when unequipping for HIGGS grab)
	// Non-mounted: spawn behind player so they can't see it
	float spawnOffsetX = 0.0f;       // X offset (left/right) - usually 0
	float spawnOffsetY = 0.0f;       // Y offset (forward/back adjustment) - usually 0
	float spawnOffsetZ = -20.0f;     // Z offset (up/down) - negative = below player
	float spawnDistance = 150.0f;    // Distance behind player (units, 70 = ~1 meter)
	
	// Mounted: spawn elevated to avoid horse collision
	float spawnOffsetMountedX = 0.0f;   // X offset when mounted
	float spawnOffsetMountedY = 0.0f;   // Y offset when mounted
	float spawnOffsetMountedZ = 50.0f;  // Z offset when mounted (positive = above hand)

	// Collision avoidance hand preference (0 = left hand unequips, 1 = right hand unequips)
	int collisionAvoidanceHand = 0;             // Default: left hand gets unequipped/grabbed during dual-wield collision

	// Shield bash settings - defaults
	bool shieldBashEnabled = true;   // Enable/disable shield bash tracking feature
	int shieldBashThreshold = 3;   // Number of bashes required to trigger effect
	float shieldBashWindow = 6.0f;// Time window (seconds) to register bashes
	float shieldBashLockoutDuration = 240.0f;    // Lockout duration (seconds) after triggering effect (4 minutes)

	// Equipment change grace period
	int equipGraceFrames = 20;    // Frames to wait after equipment change before collision detection (~0.22 sec at 90fps)

	void loadConfig() 
	{
		weaponExclusionEntries.clear();

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
						else if (variableName == "TimeToCollisionThreshold")
						{
							bladeTimeToCollisionThreshold = std::stof(variableValueStr);
						}
						else if (variableName == "ReequipCooldown")
						{
							bladeReequipCooldown = std::stof(variableValueStr);
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
							autoEquipGrabbedWeaponEnabled = (std::stoi(variableValueStr)) != 0;
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
						else if (variableName == "TapThenHoldGrabEquip")
						{
							tapThenHoldGrabEquip = (std::stoi(variableValueStr) != 0);
						}
					}
					else if (currentSection == "IntentionalDrop")
					{
						std::string variableName;
						std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

						if (variableName == "GripSpamThreshold")
						{
							gripSpamThreshold = std::stoi(variableValueStr);
						}
						else if (variableName == "GripSpamWindow")
						{
							gripSpamWindow = std::stof(variableValueStr);
						}
						else if (variableName == "DropProtectionDisableTime")
						{
							dropProtectionDisableTime = std::stof(variableValueStr);
						}
					}
					else if (currentSection == "WeaponLock")
					{
						std::string variableName;
						std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

						if (variableName == "SpamThreshold")
						{
							triggerSpamThreshold = std::stoi(variableValueStr);
						}
						else if (variableName == "SpamWindow")
						{
							triggerSpamWindow = std::stof(variableValueStr);
						}
					}
					else if (currentSection == "WeaponSpawn")
					{
						std::string variableName;
						std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

						if (variableName == "OffsetX")
						{
							spawnOffsetX = std::stof(variableValueStr);
						}
						else if (variableName == "OffsetY")
						{
							spawnOffsetY = std::stof(variableValueStr);
						}
						else if (variableName == "OffsetZ")
						{
							spawnOffsetZ = std::stof(variableValueStr);
						}
						else if (variableName == "Distance")
						{
							spawnDistance = std::stof(variableValueStr);
						}
					}
					else if (currentSection == "WeaponSpawnMounted")
					{
						std::string variableName;
						std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

						if (variableName == "OffsetX")
						{
							spawnOffsetMountedX = std::stof(variableValueStr);
						}
						else if (variableName == "OffsetY")
						{
							spawnOffsetMountedY = std::stof(variableValueStr);
						}
						else if (variableName == "OffsetZ")
						{
							spawnOffsetMountedZ = std::stof(variableValueStr);
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
					else if (currentSection == "WeaponExclusions")
					{
						// LocalFormID=Plugin.esp  or  FullFormID (no plugin)
						size_t equalsPos = line.find('=');
						WeaponExclusionEntry entry;

						if (equalsPos == std::string::npos)
						{
							entry.formID = ParseHexFormID(line);
							if (entry.formID == 0)
								continue;
						}
						else
						{
							std::string formIdStr = line.substr(0, equalsPos);
							std::string pluginName = line.substr(equalsPos + 1);
							trim(formIdStr);
							trim(pluginName);
							entry.formID = ParseHexFormID(formIdStr);
							entry.pluginName = pluginName;
							if (entry.formID == 0)
								continue;
						}

						weaponExclusionEntries.push_back(entry);
					}
				} 
			}

			_MESSAGE("Config loaded successfully.");
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