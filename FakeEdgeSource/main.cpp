#include "skse64_common/skse_version.h"
#include <shlobj.h>
#include <intrin.h>
#include <string>
#include <cstring>
#include <xbyak/xbyak.h>

#include "skse64/PluginAPI.h"	
#include "Engine.h"
#include "EquipManager.h"
#include "VRInputHandler.h"
#include "WeaponGeometry.h"
#include "ActivateHook.h"
#include "SkyrimEventSinkCompat.h"
#include "skse64/GameEvents.h"
#include "skse64/GameMenus.h"
#include "skse64/PapyrusEvents.h"

#include "skse64_common/BranchTrampoline.h"
#include "skse64_common/SafeWrite.h"

namespace FalseEdgeVR
{
	static SKSEMessagingInterface* g_messaging = NULL;
	PluginHandle					g_pluginHandle = kPluginHandle_Invalid;
	const PluginInfo* (* g_getPluginInfo)(const char* name) = nullptr;
	static SKSEPapyrusInterface* g_papyrus = NULL;
	static SKSEObjectInterface* g_object = NULL;
	SKSETaskInterface* g_task = NULL;

	SKSEVRInterface* g_vrInterface = nullptr;

	// HIGGS 1.10.10 deliberately rejects a main/right support hand trying to
	// two-hand a two-handed melee weapon equipped in the off/left hand.  Fake
	// Edge's symmetric opposite-grip mode needs that one case.  Patch only the
	// two validated melee-type branches; the crossbow branch and every other
	// HIGGS grab rule remain untouched.
	static constexpr unsigned int kSupportedHiggsBuild = 1101000;
	static constexpr uintptr_t kHiggsCanTwoHandTwoHandSwordBranchRva = 0x278A5;
	static constexpr uintptr_t kHiggsCanTwoHandTwoHandAxeBranchRva = 0x278AE;
	static constexpr uintptr_t kHiggsPlayPhysicsSoundRva = 0x18500;

	using HiggsPlayPhysicsSound = void (*)(
		void* hand, const NiPoint3& location, bool loud);
	static HiggsPlayPhysicsSound s_originalHiggsPlayPhysicsSound = nullptr;

	static void HookHiggsPlayPhysicsSound(
		void* hand, const NiPoint3& location, bool loud)
	{
		if (VRInputHandler::ShouldSuppressHiggsPhysicsSound())
		{
			_MESSAGE(
				"[FalseEdgeVR] Suppressed HIGGS physics sound for opposite-grip 2H transition");
			return;
		}

		if (s_originalHiggsPlayPhysicsSound)
			s_originalHiggsPlayPhysicsSound(hand, location, loud);
	}

	static bool InstallExactHiggsSoundHook(uintptr_t functionAddress)
	{
		// Exact HIGGS 1.10.10 PlayPhysicsSound prologue.  Thirteen bytes ends
		// on an instruction boundary immediately before the first body load.
		static constexpr UInt8 expectedPrologue[] = {
			0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56,
			0x41, 0x57, 0x48, 0x83, 0xEC, 0x38
		};
		if (std::memcmp(
				reinterpret_cast<const void*>(functionAddress),
				expectedPrologue, sizeof(expectedPrologue)) != 0)
		{
			_MESSAGE(
				"[FalseEdgeVR] HIGGS transition-audio hook skipped: "
				"PlayPhysicsSound byte signature did not match");
			return false;
		}

		const size_t prologueSize = sizeof(expectedPrologue);
		UInt8* trampoline = reinterpret_cast<UInt8*>(
			g_localTrampoline.Allocate(prologueSize + 14));
		if (!trampoline)
		{
			_MESSAGE(
				"[FalseEdgeVR] HIGGS transition-audio hook skipped: "
				"local trampoline allocation failed");
			return false;
		}

		std::memcpy(
			trampoline, reinterpret_cast<const void*>(functionAddress), prologueSize);
		trampoline[prologueSize + 0] = 0xFF;
		trampoline[prologueSize + 1] = 0x25;
		trampoline[prologueSize + 2] = 0x00;
		trampoline[prologueSize + 3] = 0x00;
		trampoline[prologueSize + 4] = 0x00;
		trampoline[prologueSize + 5] = 0x00;
		const uintptr_t jumpBack = functionAddress + prologueSize;
		std::memcpy(trampoline + prologueSize + 6, &jumpBack, sizeof(jumpBack));
		s_originalHiggsPlayPhysicsSound =
			reinterpret_cast<HiggsPlayPhysicsSound>(trampoline);

		// HIGGS may be loaded more than 2 GB away from SKSE's shared branch
		// trampoline.  BranchTrampoline::Write5Branch asserts in that case.
		// Hook the nearby FalseEdgeVR module directly instead; SafeWriteJump
		// range-checks the rel32 displacement and fails without crashing.
		if (!SafeWriteJump(
				functionAddress,
				reinterpret_cast<uintptr_t>(&HookHiggsPlayPhysicsSound)))
		{
			s_originalHiggsPlayPhysicsSound = nullptr;
			_MESSAGE(
				"[FalseEdgeVR] HIGGS transition-audio hook skipped: "
				"direct jump outside rel32 range");
			return false;
		}

		_MESSAGE(
			"[FalseEdgeVR] HIGGS transition-audio suppression hook installed");
		return true;
	}

	static void InstallHiggsTransitionAudioCompatibility()
	{
		if (!higgsInterface ||
			higgsInterface->GetBuildNumber() != kSupportedHiggsBuild)
		{
			return;
		}

		HMODULE higgsModule = GetModuleHandleA("higgs_vr.dll");
		if (!higgsModule)
			return;

		InstallExactHiggsSoundHook(
			reinterpret_cast<uintptr_t>(higgsModule) +
			kHiggsPlayPhysicsSoundRva);
	}

	static void InstallHiggsOppositeGripCompatibility()
	{
		if (!higgsInterface)
			return;

		const unsigned int build = higgsInterface->GetBuildNumber();
		if (build != kSupportedHiggsBuild)
		{
			_MESSAGE(
				"[FalseEdgeVR] HIGGS left-owned 2H compatibility skipped: "
				"unsupported build %u (expected %u)",
				build, kSupportedHiggsBuild);
			return;
		}

		HMODULE higgsModule = GetModuleHandleA("higgs_vr.dll");
		if (!higgsModule)
		{
			_MESSAGE(
				"[FalseEdgeVR] HIGGS left-owned 2H compatibility skipped: "
				"higgs_vr.dll module not found");
			return;
		}

		const uintptr_t base = reinterpret_cast<uintptr_t>(higgsModule);
		UInt8* swordBranch = reinterpret_cast<UInt8*>(
			base + kHiggsCanTwoHandTwoHandSwordBranchRva);
		UInt8* axeBranch = reinterpret_cast<UInt8*>(
			base + kHiggsCanTwoHandTwoHandAxeBranchRva);
		static constexpr UInt8 expectedSwordBranch[] = {
			0x0F, 0x84, 0x99, 0x00, 0x00, 0x00
		};
		static constexpr UInt8 expectedAxeBranch[] = {
			0x0F, 0x84, 0x90, 0x00, 0x00, 0x00
		};

		if (std::memcmp(
				swordBranch, expectedSwordBranch, sizeof(expectedSwordBranch)) != 0 ||
			std::memcmp(
				axeBranch, expectedAxeBranch, sizeof(expectedAxeBranch)) != 0)
		{
			_MESSAGE(
				"[FalseEdgeVR] HIGGS left-owned 2H compatibility skipped: "
				"CanTwoHand byte signature did not match");
			return;
		}

		// Retarget HIGGS's two melee-type true branches from its shared false
		// return to its shared true return.  Only the 32-bit branch displacement
		// changes; instruction lengths and stack behavior are identical.
		SafeWrite32(
			base + kHiggsCanTwoHandTwoHandSwordBranchRva + 2, 0x0000018E);
		SafeWrite32(
			base + kHiggsCanTwoHandTwoHandAxeBranchRva + 2, 0x00000185);
		_MESSAGE(
			"[FalseEdgeVR] HIGGS left-owned 2H support compatibility installed "
			"(build %u; sword/axe only)", build);
	}

	#pragma comment(lib, "Ws2_32.lib")

	// ============================================
	// Menu Event Handler - Hot reload config on main menu close
	// Also handles safe tracking pause during crafting/smithing menus
	// ============================================
	class MenuEventHandler : public BSTEventSink<MenuOpenCloseEvent>
	{
	public:
		virtual EventResult ReceiveEvent(MenuOpenCloseEvent* evn, EventDispatcher<MenuOpenCloseEvent>* dispatcher) override
		{
			if (!evn)
				return kEvent_Continue;

			// ============================================
			// SAFE TRACKING: Check for dangerous menus by NAME
			// These menus can cause CTD if HIGGS/VR tracking runs during them
			// ============================================
			static BSFixedString craftingMenu("Crafting Menu");
			static BSFixedString raceSexMenu("RaceSex Menu");
			static BSFixedString containerMenu("ContainerMenu");
			static BSFixedString barterMenu("BarterMenu");
			static BSFixedString giftMenu("GiftMenu");
			static BSFixedString lockpickingMenu("Lockpicking Menu");
			static BSFixedString bookMenu("Book Menu");
			static BSFixedString sleepWaitMenu("Sleep/Wait Menu");
			static BSFixedString loadingMenu("Loading Menu");
			static BSFixedString faderMenu("Fader Menu");
			static BSFixedString journalMenu("Journal Menu");
			static BSFixedString mapMenu("MapMenu");
			static BSFixedString inventoryMenu("InventoryMenu");
			static BSFixedString magicMenu("MagicMenu");
			static BSFixedString favoritesMenu("FavoritesMenu");
			static BSFixedString statsMenu("StatsMenu");
			static BSFixedString trainingMenu("Training Menu");
			
			// Check if this is a dangerous menu that requires tracking pause
			bool isDangerousMenu = (
				evn->menuName == craftingMenu ||      // Smithing, Alchemy, Enchanting
				evn->menuName == raceSexMenu ||       // Character creation
				evn->menuName == containerMenu ||     // Containers
				evn->menuName == barterMenu ||  // Trading
				evn->menuName == giftMenu ||          // Gift giving
				evn->menuName == lockpickingMenu ||   // Lockpicking
				evn->menuName == bookMenu ||          // Reading books
				evn->menuName == sleepWaitMenu ||     // Sleep/Wait
				evn->menuName == loadingMenu ||       // Loading screen
				evn->menuName == faderMenu ||         // Cell transition fade
				evn->menuName == journalMenu ||       // Journal/Quest menu
				evn->menuName == mapMenu ||           // Map
				evn->menuName == inventoryMenu ||     // Inventory
				evn->menuName == magicMenu ||   // Magic menu
				evn->menuName == favoritesMenu ||     // Favorites
				evn->menuName == statsMenu ||     // Stats/Perk menu
				evn->menuName == trainingMenu         // Training menu
			);

			if (isDangerousMenu)
			{
				if (evn->opening)
				{
					VRInputHandler::GetSingleton()->PauseTracking(true);
				}
				else
				{
					VRInputHandler::GetSingleton()->PauseTracking(false);
				}
				
				// Continue to check for other menu behaviors
			}
			
			// FALLBACK: Also pause for any menu with kFlag_PausesGame that we didn't catch above
			MenuManager* mm = MenuManager::GetSingleton();
			if (mm && !isDangerousMenu)
			{
				IMenu* menu = mm->GetMenu(&evn->menuName);
				if (menu)
				{
					bool pausesGame = (menu->flags & IMenu::kFlag_PausesGame) != 0;
					if (pausesGame)
					{
						if (evn->opening)
						{
							VRInputHandler::GetSingleton()->PauseTracking(true);
						}
						else
						{
							VRInputHandler::GetSingleton()->PauseTracking(false);
						}
					}
				}
			}

			// Maintain existing hot-reload behavior for Main Menu close
			BSFixedString mainMenu("Main Menu");
			if (evn->menuName == mainMenu && !evn->opening)
			{
				FalseEdgeVR::loadConfig();
			}

			return kEvent_Continue;
		}

		static MenuEventHandler* GetSingleton()
		{
			static MenuEventHandler instance;
			return MakeSkyrimEventSinkCompatible(&instance);
		}

	private:
		MenuEventHandler() = default;
	};

	// ============================================
	// Death Event Handler
	// ============================================
	class DeathEventHandler : public BSTEventSink<TESDeathEvent>
	{
	public:
		virtual EventResult ReceiveEvent(TESDeathEvent* evn, EventDispatcher<TESDeathEvent>* dispatcher) override
		{
			if (!evn || !evn->source)
				return kEvent_Continue;

			// Check if the player died
			Actor* actor = DYNAMIC_CAST(evn->source, TESObjectREFR, Actor);
			if (actor && actor == *g_thePlayer)
			{
				VRInputHandler::GetSingleton()->ClearAllState();
			}

			return kEvent_Continue;
		}

		static DeathEventHandler* GetSingleton()
		{
			static DeathEventHandler instance;
			return MakeSkyrimEventSinkCompatible(&instance);
		}

	private:
		DeathEventHandler() = default;
	};

	// ============================================
	// Weapon Swing Event Handler - Track game-registered weapon swings
	// ============================================
	class WeaponSwingEventHandler : public BSTEventSink<SKSEActionEvent>
	{
	public:
		virtual EventResult ReceiveEvent(SKSEActionEvent* evn, EventDispatcher<SKSEActionEvent>* dispatcher) override
		{
			if (!evn)
				return kEvent_Continue;

			// Only track weapon swings from the player
			if (!evn->actor || evn->actor != *g_thePlayer)
				return kEvent_Continue;

			// Only track weapon swing events
			if (evn->type != SKSEActionEvent::kType_WeaponSwing)
				return kEvent_Continue;

			bool isLeftHand = (evn->slot == SKSEActionEvent::kSlot_Left);

			// Notify VRInputHandler
			VRInputHandler::GetSingleton()->OnWeaponSwing(isLeftHand, evn->sourceForm);

			return kEvent_Continue;
		}

		static WeaponSwingEventHandler* GetSingleton()
		{
			static WeaponSwingEventHandler instance;
			return &instance;
		}

	private:
		WeaponSwingEventHandler() = default;
	};

	// ============================================
	// Weapon Sheathe Event Handler - Block sheathing while weapons stay equipped
	// ============================================
	class WeaponSheatheEventHandler : public BSTEventSink<SKSEActionEvent>
	{
	public:
		virtual EventResult ReceiveEvent(SKSEActionEvent* evn, EventDispatcher<SKSEActionEvent>* dispatcher) override
		{
			if (!evn)
				return kEvent_Continue;

			if (!evn->actor || evn->actor != *g_thePlayer)
				return kEvent_Continue;

			if (evn->type != SKSEActionEvent::kType_BeginSheathe &&
				evn->type != SKSEActionEvent::kType_EndSheathe)
				return kEvent_Continue;

			PlayerCharacter* player = *g_thePlayer;
			if (!player)
				return kEvent_Continue;

			TESForm* leftEquipped = player->GetEquippedObject(true);
			TESForm* rightEquipped = player->GetEquippedObject(false);
			bool hasEquippedWeapon =
				(leftEquipped && EquipManager::IsWeapon(leftEquipped)) ||
				(rightEquipped && EquipManager::IsWeapon(rightEquipped));

			if (!hasEquippedWeapon)
				return kEvent_Continue;

			if (leftEquipped && EquipManager::IsWeapon(leftEquipped))
			{
				const char* weaponName = leftEquipped->GetName();
				_MESSAGE("[FalseEdgeVR] Weapon redrawn in LEFT game hand: %s (0x%08X)",
					weaponName ? weaponName : "(unnamed)", leftEquipped->formID);
			}
			if (rightEquipped && EquipManager::IsWeapon(rightEquipped))
			{
				const char* weaponName = rightEquipped->GetName();
				_MESSAGE("[FalseEdgeVR] Weapon redrawn in RIGHT game hand: %s (0x%08X)",
					weaponName ? weaponName : "(unnamed)", rightEquipped->formID);
			}

			EquipManager::s_suppressDrawSound = true;
			player->DrawSheatheWeapon(true);
			EquipManager::s_suppressDrawSound = false;

			return kEvent_Continue;
		}

		static WeaponSheatheEventHandler* GetSingleton()
		{
			static WeaponSheatheEventHandler instance;
			return &instance;
		}

	private:
		WeaponSheatheEventHandler() = default;
	};

	// ============================================
	// Hit Event Handler - Track when player hits something
	// ============================================
	class HitEventHandler : public BSTEventSink<TESHitEvent>
	{
	public:
		virtual EventResult ReceiveEvent(TESHitEvent* evn, EventDispatcher<TESHitEvent>* dispatcher) override
		{
			if (!evn)
				return kEvent_Continue;

			// Only track hits from the player
			if (!evn->caster || evn->caster != *g_thePlayer)
				return kEvent_Continue;

			TESForm* sourceForm = evn->sourceFormID ? LookupFormByID(evn->sourceFormID) : nullptr;
			
			// Determine which hand based on the weapon
			PlayerCharacter* player = *g_thePlayer;
			if (player && sourceForm)
			{
				TESForm* leftEquipped = player->GetEquippedObject(true);
				TESForm* rightEquipped = player->GetEquippedObject(false);
				
				bool isLeftHand = (leftEquipped && leftEquipped->formID == sourceForm->formID);
				
				// Notify VRInputHandler of the hit/swing
				VRInputHandler::GetSingleton()->OnWeaponSwing(isLeftHand, sourceForm);
			}

			return kEvent_Continue;
		}

		static HitEventHandler* GetSingleton()
		{
			static HitEventHandler instance;
			return &instance;
		}

	private:
		HitEventHandler() = default;
	};

	void SetupReceptors()
	{
		// Register equip event handler
		RegisterEquipEventHandler();
		
		// Register death event handler
		auto* eventDispatcher = GetEventDispatcherList();
		if (eventDispatcher)
		{
			eventDispatcher->deathDispatcher.AddEventSink(DeathEventHandler::GetSingleton());
			// The published SKSEVR dispatcher-list layout does not expose a safe
			// TESHitEvent source here.  This handler only forwards to the current
			// no-op OnWeaponSwing stub, so do not risk corrupting startup by
			// registering against the ambiguous unk630 slot.
			_MESSAGE("[FalseEdgeVR] TESHitEvent sink skipped (unsafe SKSEVR dispatcher slot)");
		}
		
		g_actionEventDispatcher.AddEventSink(WeaponSwingEventHandler::GetSingleton());
		g_actionEventDispatcher.AddEventSink(WeaponSheatheEventHandler::GetSingleton());
		RegisterPromptActivationSupport(g_messaging);
		
		MenuManager* menuManager = MenuManager::GetSingleton();
		if (menuManager)
		{
			menuManager->MenuOpenCloseEventDispatcher()->AddEventSink(MenuEventHandler::GetSingleton());
		}
	}

	void InitializeVRSystems()
	{
		InitializeVRInput();
		InitializeWeaponGeometryTracker();
		VRInputHandler::GetSingleton()->UpdateGrabListening();
	}

	extern "C" {

		bool SKSEPlugin_Query(const SKSEInterface* skse, PluginInfo* info) {
			gLog.OpenRelative(CSIDL_MYDOCUMENTS, "\\My Games\\Skyrim VR\\SKSE\\FalseEdgeVR.log");
			gLog.SetPrintLevel(IDebugLog::kLevel_Error);
			gLog.SetLogLevel(IDebugLog::kLevel_DebugMessage);

			std::string logMsg("FalseEdgeVR: ");
			logMsg.append(FalseEdgeVR::MOD_VERSION_STR);
			_MESSAGE(logMsg.c_str());
			_MESSAGE("[FalseEdgeVR] Custom build: OppositeGrip2H-r17 (tidied stable opposite-grip build)");

			// populate info structure
			info->infoVersion = PluginInfo::kInfoVersion;
			info->name = "FalseEdgeVR";
			info->version = FalseEdgeVR::MOD_VERSION;

			// store plugin handle so we can identify ourselves later
			g_pluginHandle = skse->GetPluginHandle();

			std::string skseVers = "SKSE Version: ";
			skseVers += std::to_string(skse->runtimeVersion);
			_MESSAGE(skseVers.c_str());

			if (skse->isEditor)
			{
				_MESSAGE("loaded in editor, marking as incompatible");

				return false;
			}
			else if (skse->runtimeVersion < CURRENT_RELEASE_RUNTIME)
			{
				_MESSAGE("unsupported runtime version %08X", skse->runtimeVersion);

				return false;
			}

			// ### do not do anything else in this callback
			// ### only fill out PluginInfo and return true/false

			// supported runtime version
			return true;
		}

		inline bool file_exists(const std::string& name) {
			struct stat buffer;
			return (stat(name.c_str(), &buffer) == 0);
		}

		static const size_t TRAMPOLINE_SIZE = 256;

		//Listener for SKSE Messages
		void OnSKSEMessage(SKSEMessagingInterface::Message* msg)
		{
			if (msg)
			{
				if (msg->type == SKSEMessagingInterface::kMessage_PostLoad)
				{

				}
				else if (msg->type == SKSEMessagingInterface::kMessage_InputLoaded)
					SetupReceptors();
				else if (msg->type == SKSEMessagingInterface::kMessage_DataLoaded)
				{
					FalseEdgeVR::loadConfig();

					// NEW SKSEVR feature: trampoline interface object from QueryInterface() - Use SKSE existing process code memory pool - allow Skyrim to run without ASLR
					if (FalseEdgeVR::g_trampolineInterface)
					{
						void* branch = FalseEdgeVR::g_trampolineInterface->AllocateFromBranchPool(g_pluginHandle, TRAMPOLINE_SIZE);
						if (!branch) {
							_ERROR("couldn't acquire branch trampoline from SKSE. this is fatal. skipping remainder of init process.");
							return;
						}

						g_branchTrampoline.SetBase(TRAMPOLINE_SIZE, branch);

						void* local = FalseEdgeVR::g_trampolineInterface->AllocateFromLocalPool(g_pluginHandle, TRAMPOLINE_SIZE);
						if (!local) {
							_ERROR("couldn't acquire codegen buffer from SKSE. this is fatal. skipping remainder of init process.");
							return;
						}

						g_localTrampoline.SetBase(TRAMPOLINE_SIZE, local);

						_MESSAGE("Using new SKSEVR trampoline interface memory pool alloc for codegen buffers.");
					}
					else  // otherwise if using an older SKSEVR version, fall back to old code
					{

						if (!g_branchTrampoline.Create(TRAMPOLINE_SIZE))  // don't need such large buffers
						{
							_FATALERROR("[ERROR] couldn't create branch trampoline. this is fatal. skipping remainder of init process.");
							return;
						}

						if (!g_localTrampoline.Create(TRAMPOLINE_SIZE, nullptr))
						{
							_FATALERROR("[ERROR] couldn't create codegen buffer. this is fatal. skipping remainder of init process.");
							return;
						}

						_MESSAGE("Using legacy SKSE trampoline creation.");
					}

					// The HIGGS audio detour needs the local code-generation pool,
					// which becomes available here rather than at PostPostLoad.
					InstallHiggsTransitionAudioCompatibility();

					FalseEdgeVR::GameLoad();
					
					// Setup Activate hook to block player from activating grabbed weapons
					SetupActivateHook();
					SetupEquipItemHook();
					
					// Initialize equip manager early (doesn't need HIGGS)
					EquipManager::GetSingleton()->Initialize();
					EquipManager::GetSingleton()->UpdateEquipmentState();
				}
				else if (msg->type == SKSEMessagingInterface::kMessage_PostPostLoad)
				{
					FalseEdgeVR::InitTwoHandedTrackingFromLoadOrder();

					// Get HIGGS interface
					higgsInterface = HiggsPluginAPI::GetHiggsInterface001(g_pluginHandle, g_messaging);
					if (!higgsInterface)
					{
						_MESSAGE("Did not get HIGGS interface - VR collision features will be disabled");
					}
					else
					{
						InstallHiggsOppositeGripCompatibility();
					}

					vrikInterface = vrikPluginApi::getVrikInterface001(g_pluginHandle, g_messaging);
					if (vrikInterface)
					{
						unsigned int vrikBuildNumber = vrikInterface->getBuildNumber();
						if (vrikBuildNumber < 80400)
						{
							ShowErrorBoxAndTerminate("[CRITICAL] VRIK's older versions are not compatible. Make sure you have VRIK version 0.8.4 or higher.");
						}
					}

					skyrimVRESLInterface = SkyrimVRESLPluginAPI::GetSkyrimVRESLInterface001(g_pluginHandle, g_messaging);

					InitializeVRSystems();
				}
				else if (msg->type == SKSEMessagingInterface::kMessage_PostLoadGame)
				{
					if ((bool)(msg->data) == true)
					{
						EquipManager::GetSingleton()->CaptureDroppedWeaponsForLoadRecovery();
						VRInputHandler::GetSingleton()->ClearAllState();
						FalseEdgeVR::PostLoadGame();
						EquipManager::GetSingleton()->UpdateEquipmentState();
						VRInputHandler::GetSingleton()->UpdateGrabListening();
					}
				}
			}
		}

		bool SKSEPlugin_Load(const SKSEInterface* skse) {	// Called by SKSE to load this plugin

			g_getPluginInfo = skse->GetPluginInfo;

			g_task = (SKSETaskInterface*)skse->QueryInterface(kInterface_Task);

			g_papyrus = (SKSEPapyrusInterface*)skse->QueryInterface(kInterface_Papyrus);

			g_messaging = (SKSEMessagingInterface*)skse->QueryInterface(kInterface_Messaging);
			g_messaging->RegisterListener(g_pluginHandle, "SKSE", OnSKSEMessage);

			g_vrInterface = (SKSEVRInterface*)skse->QueryInterface(kInterface_VR);
			if (!g_vrInterface) {
				_MESSAGE("[CRITICAL] Couldn't get SKSE VR interface. You probably have an outdated SKSE version.");
				return false;
			}

			SKSESerializationInterface* serialization = (SKSESerializationInterface*)skse->QueryInterface(kInterface_Serialization);
			if (serialization)
				EquipManager::RegisterSerialization(serialization);

			return true;
		}
	};
}
