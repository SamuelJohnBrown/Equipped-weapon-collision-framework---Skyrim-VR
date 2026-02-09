#include "skse64_common/skse_version.h"
#include <shlobj.h>
#include <intrin.h>
#include <string>
#include <xbyak/xbyak.h>

#include "skse64/PluginAPI.h"	
#include "Engine.h"
#include "EquipManager.h"
#include "VRInputHandler.h"
#include "WeaponGeometry.h"
#include "ShieldCollision.h"
#include "DaggerFlipTracker.h"
#include "ActivateHook.h"
#include "skse64/GameEvents.h"
#include "skse64/GameMenus.h"
#include "skse64/PapyrusEvents.h"

#include "skse64_common/BranchTrampoline.h"

namespace FalseEdgeVR
{
	static SKSEMessagingInterface* g_messaging = NULL;
	PluginHandle					g_pluginHandle = kPluginHandle_Invalid;
	static SKSEPapyrusInterface* g_papyrus = NULL;
	static SKSEObjectInterface* g_object = NULL;
	SKSETaskInterface* g_task = NULL;

	SKSEVRInterface* g_vrInterface = nullptr;

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
					_MESSAGE("MenuEventHandler: === PAUSING VR TRACKING === Menu: %s", evn->menuName.data);
					VRInputHandler::GetSingleton()->PauseTracking(true);
				}
				else
				{
					_MESSAGE("MenuEventHandler: === RESUMING VR TRACKING === Menu: %s", evn->menuName.data);
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
							_MESSAGE("MenuEventHandler: Pausing VR tracking (flag) - Menu: %s", evn->menuName.data);
							VRInputHandler::GetSingleton()->PauseTracking(true);
						}
						else
						{
							_MESSAGE("MenuEventHandler: Resuming VR tracking (flag) - Menu: %s", evn->menuName.data);
							VRInputHandler::GetSingleton()->PauseTracking(false);
						}
					}
				}
			}

			// Maintain existing hot-reload behavior for Main Menu close
			BSFixedString mainMenu("Main Menu");
			if (evn->menuName == mainMenu && !evn->opening)
			{
				_MESSAGE("=== Main Menu Closed - Hot reloading config ===");
				FalseEdgeVR::loadConfig();
				_MESSAGE("=== Config hot reload complete ===");
			}

			return kEvent_Continue;
		}

		static MenuEventHandler* GetSingleton()
		{
			static MenuEventHandler instance;
			return &instance;
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
				_MESSAGE("DeathEventHandler: Player died! Clearing all VR tracking state.");
				VRInputHandler::GetSingleton()->ClearAllState();
			}

			return kEvent_Continue;
		}

		static DeathEventHandler* GetSingleton()
		{
			static DeathEventHandler instance;
			return &instance;
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
			
			_MESSAGE("WeaponSwingEventHandler: WEAPON SWING detected! Hand: %s, Weapon FormID: %08X",
				isLeftHand ? "LEFT" : "RIGHT",
				evn->sourceForm ? evn->sourceForm->formID : 0);

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
			
			bool isPowerAttack = (evn->flags & TESHitEvent::kFlag_PowerAttack) != 0;
			bool isSneakAttack = (evn->flags & TESHitEvent::kFlag_SneakAttack) != 0;
			bool isBash = (evn->flags & TESHitEvent::kFlag_Bash) != 0;
			bool isBlocked = (evn->flags & TESHitEvent::kFlag_Blocked) != 0;
			
			_MESSAGE("HitEventHandler: === PLAYER HIT EVENT ===");
			_MESSAGE("HitEventHandler:   Target: %08X, Source Weapon: %08X", 
				evn->target ? evn->target->formID : 0,
				evn->sourceFormID);
			_MESSAGE("HitEventHandler:   Flags: PowerAttack=%s, SneakAttack=%s, Bash=%s, Blocked=%s",
				isPowerAttack ? "YES" : "NO",
				isSneakAttack ? "YES" : "NO",
				isBash ? "YES" : "NO",
				isBlocked ? "YES" : "NO");
			
			// Determine which hand based on the weapon
			PlayerCharacter* player = *g_thePlayer;
			if (player && sourceForm)
			{
				TESForm* leftEquipped = player->GetEquippedObject(true);
				TESForm* rightEquipped = player->GetEquippedObject(false);
				
				bool isLeftHand = (leftEquipped && leftEquipped->formID == sourceForm->formID);
				
				_MESSAGE("HitEventHandler:   Hand: %s", isLeftHand ? "LEFT" : "RIGHT");
				
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
		_MESSAGE("Building Event Sinks...");

		// Register equip event handler
		RegisterEquipEventHandler();
		
		// Register death event handler
		auto* eventDispatcher = GetEventDispatcherList();
		if (eventDispatcher)
		{
			eventDispatcher->deathDispatcher.AddEventSink(DeathEventHandler::GetSingleton());
			_MESSAGE("Death event handler registered");
			
			// Register hit event handler
			eventDispatcher->unk630.AddEventSink(HitEventHandler::GetSingleton());
			_MESSAGE("Hit event handler registered");
		}
		
		// Register weapon swing event handler (SKSE action events)
		g_actionEventDispatcher.AddEventSink(WeaponSwingEventHandler::GetSingleton());
		_MESSAGE("Weapon swing event handler registered");
		
		// Register menu event handler for hot reloading config
		MenuManager* menuManager = MenuManager::GetSingleton();
		if (menuManager)
		{
			menuManager->MenuOpenCloseEventDispatcher()->AddEventSink(MenuEventHandler::GetSingleton());
			_MESSAGE("Menu event handler registered (config hot reload on main menu close)");
		}
	}

	// Called after HIGGS interface is available
	void InitializeVRSystems()
	{
		_MESSAGE("=== Initializing VR Systems ===");
		
		// Initialize VR input handling (HIGGS callbacks) - NOW higgsInterface is available
		_MESSAGE("Calling InitializeVRInput...");
		InitializeVRInput();
		_MESSAGE("InitializeVRInput complete");
		
		// Initialize weapon geometry tracking
		_MESSAGE("Calling InitializeWeaponGeometryTracker...");
		InitializeWeaponGeometryTracker();
		_MESSAGE("InitializeWeaponGeometryTracker complete");

		// Initialize shield collision tracking
		_MESSAGE("Calling InitializeShieldCollisionTracker...");
		InitializeShieldCollisionTracker();
		_MESSAGE("InitializeShieldCollisionTracker complete");

		// Initialize dagger flip tracking
		_MESSAGE("Calling DaggerFlipTracker::Initialize...");
		FalseEdgeVR::DaggerFlipTracker::GetSingleton()->Initialize();
		_MESSAGE("DaggerFlipTracker::Initialize complete");
		
		// Update grab listening based on current equipment
		_MESSAGE("Calling UpdateGrabListening...");
		VRInputHandler::GetSingleton()->UpdateGrabListening();
		_MESSAGE("UpdateGrabListening complete");
		
		_MESSAGE("=== VR Systems initialized successfully ===");
	}

	extern "C" {

		bool SKSEPlugin_Query(const SKSEInterface* skse, PluginInfo* info) {
			gLog.OpenRelative(CSIDL_MYDOCUMENTS, "\\My Games\\Skyrim VR\\SKSE\\FalseEdgeVR.log");
			gLog.SetPrintLevel(IDebugLog::kLevel_Error);
			gLog.SetLogLevel(IDebugLog::kLevel_DebugMessage);

			std::string logMsg("FalseEdgeVR: ");
			logMsg.append(FalseEdgeVR::MOD_VERSION_STR);
			_MESSAGE(logMsg.c_str());

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

					FalseEdgeVR::GameLoad();
					
					// Setup Activate hook to block player from activating grabbed weapons
					SetupActivateHook();
					
					// Initialize equip manager early (doesn't need HIGGS)
					EquipManager::GetSingleton()->Initialize();
					EquipManager::GetSingleton()->UpdateEquipmentState();
				}
				else if (msg->type == SKSEMessagingInterface::kMessage_PostPostLoad)
				{
					// Get HIGGS interface
					higgsInterface = HiggsPluginAPI::GetHiggsInterface001(g_pluginHandle, g_messaging);
					if (higgsInterface)
					{
						_MESSAGE("Got HIGGS interface. Buildnumber: %d", higgsInterface->GetBuildNumber());
					}
					else
					{
						_MESSAGE("Did not get HIGGS interface - VR collision features will be disabled");
					}

					// Get VRIK interface
					vrikInterface = vrikPluginApi::getVrikInterface001(g_pluginHandle, g_messaging);
					if (vrikInterface)
					{
						unsigned int vrikBuildNumber = vrikInterface->getBuildNumber();
						if (vrikBuildNumber < 80400)
						{
							ShowErrorBoxAndTerminate("[CRITICAL] VRIK's older versions are not compatible. Make sure you have VRIK version 0.8.4 or higher.");
						}
						_MESSAGE("Got VRIK interface. Buildnumber: %d", vrikBuildNumber);
					}
					else
					{
						_MESSAGE("Did not get VRIK interface");
					}

					// Get SkyrimVRESL interface
					skyrimVRESLInterface = SkyrimVRESLPluginAPI::GetSkyrimVRESLInterface001(g_pluginHandle, g_messaging);
					if (skyrimVRESLInterface)
					{
						_MESSAGE("Got SkyrimVRESL interface");
					}
					else
					{
						_MESSAGE("Did not get SkyrimVRESL interface");
					}

					// NOW initialize VR systems that depend on HIGGS
					InitializeVRSystems();
				}
				else if (msg->type == SKSEMessagingInterface::kMessage_PostLoadGame)
				{
					if ((bool)(msg->data) == true)
					{
						_MESSAGE("PostLoadGame: Clearing VR tracking state and updating equipment...");
						
						// Clear all VR tracking state first (old references are now invalid)
						VRInputHandler::GetSingleton()->ClearAllState();
						
						FalseEdgeVR::PostLoadGame();
						
						// Update equipment state after loading a save
						EquipManager::GetSingleton()->UpdateEquipmentState();
						VRInputHandler::GetSingleton()->UpdateGrabListening();
						
						_MESSAGE("PostLoadGame: Complete");
					}
				}
			}
		}

		bool SKSEPlugin_Load(const SKSEInterface* skse) {	// Called by SKSE to load this plugin

			g_task = (SKSETaskInterface*)skse->QueryInterface(kInterface_Task);

			g_papyrus = (SKSEPapyrusInterface*)skse->QueryInterface(kInterface_Papyrus);

			g_messaging = (SKSEMessagingInterface*)skse->QueryInterface(kInterface_Messaging);
			g_messaging->RegisterListener(g_pluginHandle, "SKSE", OnSKSEMessage);

			g_vrInterface = (SKSEVRInterface*)skse->QueryInterface(kInterface_VR);
			if (!g_vrInterface) {
				_MESSAGE("[CRITICAL] Couldn't get SKSE VR interface. You probably have an outdated SKSE version.");
				return false;
			}

			return true;
		}
	};
}