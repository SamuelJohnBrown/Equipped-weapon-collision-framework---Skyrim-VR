#pragma once
#include "skse64/PluginAPI.h"
#include <skse64/GameReferences.h>

namespace spellwheelPluginApi {
	class ISpellWheelInterface001;

	ISpellWheelInterface001* getSpellWheelInterface001(const PluginHandle& pluginHandle, SKSEMessagingInterface* messagingInterface);

	class ISpellWheelInterface001
	{
	public:
		virtual unsigned int getBuildNumber() = 0;
		virtual bool IsMainWheelOpen() = 0;
		virtual bool IsSecondaryWheelOpen() = 0;
		virtual void SpawnConjurationCircle(NiPoint3 pos) = 0;
		virtual void CloseOstimWheels() = 0;
	};
}
