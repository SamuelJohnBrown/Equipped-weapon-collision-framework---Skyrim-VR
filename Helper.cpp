#include "Helper.h"

namespace FalseEdgeVR
{
	// RemoveItem native function address (Papyrus ObjectReference.RemoveItem)
	typedef void(*_RemoveItem_Native)(VMClassRegistry* registry, UInt32 stackId, TESObjectREFR* akSource, TESForm* akItemToRemove, SInt32 aiCount, bool abSilent, TESObjectREFR* akOtherContainer);
	RelocAddr<_RemoveItem_Native> RemoveItem_Native(0x009D1190);

	std::uintptr_t Write5Call(std::uintptr_t a_src, std::uintptr_t a_dst)
	{
		const auto disp = reinterpret_cast<std::int32_t*>(a_src + 1);
		const auto nextOp = a_src + 5;
		const auto func = nextOp + *disp;
		g_branchTrampoline.Write5Call(a_src, a_dst);
		return func;
	}

	void LeftHandedModeChange()
	{
		const int value = vlibGetSetting("bLeftHandedMode:VRInput");
		if (value != leftHandedMode)
		{
			leftHandedMode = value;

			LOG("Left Handed Mode is %s.", leftHandedMode ? "ON" : "OFF");
		}
	}

	void ShowErrorBox(const char* errorString)
	{
		int msgboxID = MessageBox(
			NULL,
			(LPCTSTR)errorString,
			(LPCTSTR)"Immersive Crossbow Reload VR Fatal Error",
			MB_ICONERROR | MB_OK | MB_TASKMODAL
		);
	}

	void ShowErrorBoxAndLog(const char* errorString)
	{
		_ERROR(errorString);
		ShowErrorBox(errorString);
	}

	void ShowErrorBoxAndTerminate(const char* errorString)
	{
		ShowErrorBoxAndLog(errorString);
		*((int*)0) = 0xDEADBEEF; // crash
	}

	template<typename T>
	T* LoadFormAndLog(const std::string& pluginName, UInt32& fullFormId, UInt32 baseFormId, const char* formName) 
	{
		fullFormId = GetFullFormIdFromEspAndFormId(pluginName.c_str(), GetBaseFormID(baseFormId));
		if (fullFormId > 0) 
		{
			TESForm* form = LookupFormByID(fullFormId);
			if (form) 
			{
				T* castedForm = nullptr;
				if constexpr (std::is_same_v<T, BGSProjectile>) 
				{
					castedForm = DYNAMIC_CAST(form, TESForm, BGSProjectile);
				}
				else if constexpr (std::is_same_v<T, TESAmmo>) 
				{
					castedForm = DYNAMIC_CAST(form, TESForm, TESAmmo);
				}
				else if constexpr (std::is_same_v<T, TESObjectWEAP>) 
				{
					castedForm = DYNAMIC_CAST(form, TESForm, TESObjectWEAP);
				}
				else if constexpr (std::is_same_v<T, TESObjectREFR>) 
				{
					castedForm = DYNAMIC_CAST(form, TESForm, TESObjectREFR);
				}
				else if constexpr (std::is_same_v<T, BGSSoundDescriptorForm>) 
				{
					castedForm = DYNAMIC_CAST(form, TESForm, BGSSoundDescriptorForm);
				}

				if (castedForm) 
				{
					LOG_ERR("%s found. formid: %x", formName, fullFormId);
					return castedForm;
				}
				else 
				{
					LOG_ERR("%s null. formid: %x", formName, fullFormId);
				}
			}
			else 
			{
				LOG_ERR("%s not found. formid: %x", formName, fullFormId);
			}
		}
		return nullptr;
	}

	void GameLoad()
	{
		LeftHandedModeChange();





	}


	void PostLoadGame()
	{
		if ((*g_thePlayer) && (*g_thePlayer)->loadedState)
		{
			
		}
	}

	UInt32 GetFullFormIdMine(const char* espName, UInt32 baseFormId)
	{
		UInt32 fullFormID = 0;

		std::string espNameStr = espName;
		std::transform(espNameStr.begin(), espNameStr.end(), espNameStr.begin(), ::tolower);

		if (espNameStr == "skyrim.esm")
		{
			fullFormID = baseFormId;
		}
		else
		{
			DataHandler* dataHandler = DataHandler::GetSingleton();

			if (dataHandler)
			{
				std::pair<const char*, UInt32> formIdPair = { espName, baseFormId };
				
				const ModInfo* modInfo = NEWLookupAllLoadedModByName(formIdPair.first);
				if (modInfo)
				{
					if (IsValidModIndex(modInfo->modIndex)) //If plugin is in the load order.
					{
						fullFormID = GetFullFormID(modInfo, GetBaseFormID(formIdPair.second));
					}
				}
			}
		}
		return fullFormID;
	}

	void RemoveItemFromInventory(TESObjectREFR* target, TESForm* item, SInt32 count, bool silent)
	{
		if (!target || !item)
			return;
		
		RemoveItem_Native(nullptr, 0, target, item, count, silent, nullptr);
	}
}