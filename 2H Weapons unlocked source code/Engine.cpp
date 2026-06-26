#include "Engine.h"

#include <skse64/PapyrusActor.cpp>
#include "SpellWheelTwoHandLog.h"

namespace BarebonesVR
{
	SKSETrampolineInterface* g_trampolineInterface = nullptr;

	HiggsPluginAPI::IHiggsInterface001* higgsInterface;
	vrikPluginApi::IVrikInterface001* vrikInterface;

	SkyrimVRESLPluginAPI::ISkyrimVRESLInterface001* skyrimVRESLInterface;

	void StartMod()
	{
		SetupSpellWheelTwoHandLog();
	}
}
