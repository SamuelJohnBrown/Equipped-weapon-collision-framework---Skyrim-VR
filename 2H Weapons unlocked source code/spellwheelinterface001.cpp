#include "spellwheelinterface001.h"

struct SpellWheelMessage
{
	enum { kMessage_GetInterface = 0xFA27C15D };
	void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
};

static spellwheelPluginApi::ISpellWheelInterface001* g_spellwheelInterface = nullptr;

spellwheelPluginApi::ISpellWheelInterface001* spellwheelPluginApi::getSpellWheelInterface001(
	const PluginHandle& pluginHandle,
	SKSEMessagingInterface* messagingInterface)
{
	if (g_spellwheelInterface)
	{
		return g_spellwheelInterface;
	}

	SpellWheelMessage swMessage;
	if (messagingInterface->Dispatch(
		pluginHandle,
		SpellWheelMessage::kMessage_GetInterface,
		reinterpret_cast<void*>(&swMessage),
		sizeof(SpellWheelMessage*),
		"SpellWheelVR"))
	{
		_MESSAGE("BarebonesVR: SpellWheelVR dispatch message returned true");
	}
	else
	{
		_MESSAGE("BarebonesVR: SpellWheelVR dispatch message returned false");
	}

	if (!swMessage.GetApiFunction)
	{
		return nullptr;
	}

	g_spellwheelInterface = static_cast<ISpellWheelInterface001*>(swMessage.GetApiFunction(1));
	return g_spellwheelInterface;
}
