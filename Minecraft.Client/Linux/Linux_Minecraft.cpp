// Linux_Minecraft.cpp : entry point for the Linux client.
//
// Phase 7b: replaces the Phase 3-6 smoke-test main() (a rotating test
// triangle + throwaway input/storage/audio self-tests) with the real
// per-frame loop shape from Windows64_Minecraft.cpp, and a direct non-UI
// world launch via CConsoleMinecraftApp::TemporaryCreateGameStart() (see
// Linux_MinecraftApp.cpp) instead of going through the title screen /
// UIScene_CreateWorldMenu - Common/UI's ~100 concrete UIScene_*.cpp menu
// screens are Iggy-only and still out of scope (see LinuxUIController.h).
//
// Pass --smoke-test on the command line to run the old Phase 3-5
// standalone triangle/input/storage self-test instead (kept as a
// regression check for those subsystems in isolation from real game logic).
// Its audio leg is gone: it tested the OpenAL AIL_* shim, which the real Miles
// runtime replaced. Miles has its own harness - Linux/Miles/miles_spike.

#include "stdafx.h"
#include "Linux_App.h"

#include "../PS3/PS3Extras/ShutdownManager.h"
#include "../MinecraftServer.h"
#include "../LocalPlayer.h"
#include "../ClientConnection.h"
#include "../User.h"
#include "../Common/Consoles_App.h"
#include "../Common/Network/GameNetworkManager.h"
#include "../Options.h"
#include "../../Minecraft.World/Mth.h"
#include "../../Minecraft.World/IntCache.h"
#include "../../Minecraft.World/AABB.h"
#include "../../Minecraft.World/Vec3.h"
#include "../../Minecraft.World/compression.h"
#include "../../Minecraft.World/OldChunkStorage.h"
#include "../../Minecraft.World/Level.h"
#include "../Tesselator.h"
#include "../../Minecraft.World/net.minecraft.world.level.tile.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
	struct TestVertex
	{
		float x, y, z;
		float u, v;
		unsigned char colour[4];
		signed char normal[4];
		unsigned int pad;
	};

	const unsigned char ACTION_TEST_LEFT = 0;
	const unsigned char ACTION_TEST_RIGHT = 1;
	const unsigned char ACTION_TEST_FIRE = 2;

	bool RunStorageSmokeTest()
	{
		const char *payload = "Linux port Phase 5 storage smoke test payload";
		unsigned int len = (unsigned int)strlen(payload) + 1;

		StorageManager.Init(1, L"LinuxSmokeTest", (char *)"minecraft-lce", 0, nullptr, nullptr, "linux-port");
		StorageManager.SetSaveUniqueFilename((char *)"phase5_smoketest");

		void *buf = StorageManager.AllocateSaveData(len);
		memcpy(buf, payload, len);

		bool saveOk = false;
		StorageManager.SaveSaveData([](LPVOID param, const bool success) -> int {
			*(bool *)param = success;
			return 0;
		}, &saveOk);

		SAVE_INFO info = {};
		strncpy(info.UTF8SaveFilename, "phase5_smoketest", MAX_SAVEFILENAME_LENGTH - 1);

		bool loadOk = false;
		StorageManager.LoadSaveData(&info, [](LPVOID param, const bool success, const bool) -> int {
			*(bool *)param = success;
			return 0;
		}, &loadOk);

		unsigned int readBackLen = StorageManager.GetSaveSize();
		std::vector<unsigned char> readBack(readBackLen);
		unsigned int readBackLenCopy = readBackLen;
		StorageManager.GetSaveData(readBack.data(), &readBackLenCopy);

		bool matches = saveOk && loadOk && readBackLen == len &&
			memcmp(readBack.data(), payload, len) == 0;

		printf("StorageManager smoke test: save=%s load=%s bytes=%u round-trip-match=%s\n",
			saveOk ? "ok" : "FAIL", loadOk ? "ok" : "FAIL", readBackLen, matches ? "yes" : "NO");
		return matches;
	}

	int RunSmokeTest(CLinuxApp &app)
	{
		RenderManager.Initialise(app.GetWindow());

		float clearColour[4] = {0.0f, 0.125f, 0.3f, 1.0f};
		RenderManager.SetClearColour(clearColour);

		InputManager.Initialise(0, 3, 3, 0);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0, ACTION_TEST_LEFT, _360_JOY_BUTTON_DPAD_LEFT | _360_JOY_BUTTON_LSTICK_LEFT);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0, ACTION_TEST_RIGHT, _360_JOY_BUTTON_DPAD_RIGHT | _360_JOY_BUTTON_LSTICK_RIGHT);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0, ACTION_TEST_FIRE, _360_JOY_BUTTON_A);
		InputManager.SetJoypadMapVal(0, MAP_STYLE_0);
		printf("InputManager: pad 0 connected=%s\n", InputManager.IsPadConnected(0) ? "yes" : "no");

		RunStorageSmokeTest();

		// Audio has its own, better harness now that the real Miles runtime is
		// linked: Minecraft.Client/Linux/Miles/miles_spike loads the soundbank and
		// plays a named event. The test tone that used to be here belonged to the
		// OpenAL shim that Miles replaced.

		TestVertex triangle[3] = {
			{  0.0f,  0.6f, -3.0f,  0.5f, 1.0f, {255,  64,  64, 255}, {0, 0, 127, 0}, 0},
			{ -0.6f, -0.6f, -3.0f,  0.0f, 0.0f, { 64, 255,  64, 255}, {0, 0, 127, 0}, 0},
			{  0.6f, -0.6f, -3.0f,  1.0f, 0.0f, { 64,  64, 255, 255}, {0, 0, 127, 0}, 0},
		};

		bool running = true;
		float angle = 0.0f;
		while (running)
		{
			running = app.PollEvents();
			InputManager.Tick();

			float turn = 0.01f;
			if (InputManager.ButtonDown(0, ACTION_TEST_LEFT)) turn = -0.05f;
			else if (InputManager.ButtonDown(0, ACTION_TEST_RIGHT)) turn = 0.05f;
			turn += InputManager.GetJoypadStick_LX(0) * 0.05f;

			RenderManager.StartFrame();
			RenderManager.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

			RenderManager.MatrixMode(GL_PROJECTION);
			RenderManager.MatrixSetIdentity();
			float aspect = app.GetHeight() > 0 ? (float)app.GetWidth() / (float)app.GetHeight() : 1.0f;
			// Degrees, matching gluPerspective (this call previously converted to radians,
			// which is what led MatrixPerspective to be written against the wrong unit).
			RenderManager.MatrixPerspective(60.0f, aspect, 0.1f, 100.0f);

			RenderManager.MatrixMode(GL_MODELVIEW);
			RenderManager.MatrixSetIdentity();
			RenderManager.MatrixRotate(angle, 0.0f, 1.0f, 0.0f);
			angle += turn;

			RenderManager.DrawVertices(C4JRender::PRIMITIVE_TYPE_TRIANGLE_LIST, 3, triangle,
				C4JRender::VERTEX_TYPE_PF3_TF2_CB4_NB4_XW1, C4JRender::PIXEL_SHADER_TYPE_STANDARD);

			RenderManager.Present();
		}

		return 0;
	}

	// Verbatim from Windows64_Minecraft.cpp's DefineActions() - purely a
	// table of InputManager.SetGameJoypadMaps(...) calls against the shared
	// _360_JOY_BUTTON_*/MINECRAFT_ACTION_*/ACTION_MENU_* vocabulary, nothing
	// Windows-specific. LinuxInput.cpp (Phase 5) implements the generic
	// C_4JInput joypad-map layer these calls populate, so real gameplay
	// actions resolve through it once this table is set up, the same way
	// they do on every other platform.
	void DefineActions(void)
	{
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_A,							_360_JOY_BUTTON_A);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_B,							_360_JOY_BUTTON_B);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_X,							_360_JOY_BUTTON_X);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_Y,							_360_JOY_BUTTON_Y);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_OK,							_360_JOY_BUTTON_A);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_CANCEL,						_360_JOY_BUTTON_B);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_UP,							_360_JOY_BUTTON_DPAD_UP | _360_JOY_BUTTON_LSTICK_UP);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_DOWN,						_360_JOY_BUTTON_DPAD_DOWN | _360_JOY_BUTTON_LSTICK_DOWN);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_LEFT,						_360_JOY_BUTTON_DPAD_LEFT | _360_JOY_BUTTON_LSTICK_LEFT);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_RIGHT,						_360_JOY_BUTTON_DPAD_RIGHT | _360_JOY_BUTTON_LSTICK_RIGHT);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_PAGEUP,						_360_JOY_BUTTON_LT);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_PAGEDOWN,					_360_JOY_BUTTON_RT);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_RIGHT_SCROLL,				_360_JOY_BUTTON_RB);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_LEFT_SCROLL,					_360_JOY_BUTTON_LB);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_PAUSEMENU,					_360_JOY_BUTTON_START);

		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_STICK_PRESS,					_360_JOY_BUTTON_LTHUMB);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_OTHER_STICK_PRESS,			_360_JOY_BUTTON_RTHUMB);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_OTHER_STICK_UP,				_360_JOY_BUTTON_RSTICK_UP);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_OTHER_STICK_DOWN,			_360_JOY_BUTTON_RSTICK_DOWN);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_OTHER_STICK_LEFT,			_360_JOY_BUTTON_RSTICK_LEFT);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,ACTION_MENU_OTHER_STICK_RIGHT,			_360_JOY_BUTTON_RSTICK_RIGHT);

		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_JUMP,					_360_JOY_BUTTON_A);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_FORWARD,				_360_JOY_BUTTON_LSTICK_UP);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_BACKWARD,				_360_JOY_BUTTON_LSTICK_DOWN);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_LEFT,					_360_JOY_BUTTON_LSTICK_LEFT);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_RIGHT,					_360_JOY_BUTTON_LSTICK_RIGHT);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_LOOK_LEFT,				_360_JOY_BUTTON_RSTICK_LEFT);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_LOOK_RIGHT,				_360_JOY_BUTTON_RSTICK_RIGHT);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_LOOK_UP,				_360_JOY_BUTTON_RSTICK_UP);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_LOOK_DOWN,				_360_JOY_BUTTON_RSTICK_DOWN);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_USE,					_360_JOY_BUTTON_LT);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_ACTION,					_360_JOY_BUTTON_RT);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_RIGHT_SCROLL,			_360_JOY_BUTTON_RB);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_LEFT_SCROLL,			_360_JOY_BUTTON_LB);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_INVENTORY,				_360_JOY_BUTTON_Y);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_PAUSEMENU,				_360_JOY_BUTTON_START);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_DROP,					_360_JOY_BUTTON_B);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_SNEAK_TOGGLE,			_360_JOY_BUTTON_RTHUMB);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_CRAFTING,				_360_JOY_BUTTON_X);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_RENDER_THIRD_PERSON,	_360_JOY_BUTTON_LTHUMB);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_GAME_INFO,				_360_JOY_BUTTON_BACK);

		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_DPAD_LEFT,				_360_JOY_BUTTON_DPAD_LEFT);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_DPAD_RIGHT,				_360_JOY_BUTTON_DPAD_RIGHT);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_DPAD_UP,				_360_JOY_BUTTON_DPAD_UP);
		InputManager.SetGameJoypadMaps(MAP_STYLE_0,MINECRAFT_ACTION_DPAD_DOWN,				_360_JOY_BUTTON_DPAD_DOWN);
	}
}

// Every platform's own *_Minecraft.cpp entry point defines this itself
// (e.g. Windows64_Minecraft.cpp:286, under #else when MEMORY_TRACKING isn't
// defined) rather than it living in any shared file - Minecraft.World's
// gameplay code (Villager.cpp, BiomeSource.cpp, ...) calls it unconditionally.
void MemSect(int sect)
{
}

int main(int argc, char *argv[])
{
	CLinuxApp linuxApp;
	if (!linuxApp.Init(1280, 720, "Minecraft"))
	{
		fprintf(stderr, "Failed to initialise Linux client shell.\n");
		return 1;
	}

	// --direct-world bypasses the Iggy frontend and launches a world immediately,
	// which is what this port did for all of Phase 7. See its use further down.
	bool bDirectWorld = false;
	for (int i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "--direct-world") == 0)
			bDirectWorld = true;
	}

	if (argc > 1 && strcmp(argv[1], "--smoke-test") == 0)
	{
		int rc = RunSmokeTest(linuxApp);
		linuxApp.Shutdown();
		return rc;
	}

	printf("Minecraft.World linkage check: Mth::sqrt(4.0f) = %f\n", Mth::sqrt(4.0f));

	// --- Real boot sequence, mirroring Windows64_Minecraft.cpp's live path ---

	RenderManager.Initialise(linuxApp.GetWindow());
	float clearColour[4] = {0.0f, 0.125f, 0.3f, 1.0f};
	RenderManager.SetClearColour(clearColour);

	app.loadMediaArchive();
	app.loadStringTable();

	// The real thing now: preInit(w,h) + GDraw setup + IggySetGDraw + postInit(),
	// mirroring ConsoleUIController::init (Windows64_UIController.cpp:12-67). Unlike
	// the other platforms there is no device/context to pass - GDraw's GL backend
	// binds to the GL context CLinuxApp already created above.
	//
	// postInit() loads the skin libraries and ends with
	// NavigateToScene(0, eUIScene_Intro) (UIController.cpp:303), so the frontend
	// starts here. This must therefore run *after* app.loadMediaArchive(), which is
	// where the SWFs come from.
	ui.init(linuxApp.GetWidth(), linuxApp.GetHeight());

	// Needed as early as possible so HasStarted()/ShouldRun() are valid for
	// any background thread that might start soon after (GameRenderer's
	// chunk-update thread, MinecraftServer's threads, etc.) - see
	// LinuxShutdownManager.cpp for why this exists.
	ShutdownManager::Initialise();

	InputManager.Initialise(1, 3, MINECRAFT_ACTION_MAX, ACTION_MAX_MENU);
	DefineActions();
	InputManager.SetJoypadMapVal(0, 0);
	InputManager.SetKeyRepeatRate(0.3f, 0.2f);

	// Allocates ProfileManager's per-pad GameSettings buffers (Extrax64Stubs.cpp's
	// profileData[]) - CMinecraftApp::InitGameSettings() dereferences these
	// unconditionally below, so skipping this call (as an earlier iteration of
	// this bootstrap did) segfaults there. dwProfileSettingsA is the Xbox-Live
	// profile-option ID table; all-zero is correct here, same as
	// Windows64_Minecraft.cpp's own #else branch for non-Xbox platforms.
	static DWORD dwProfileSettingsA[5] = {0, 0, 0, 0, 0};
	ProfileManager.Initialise(TITLEID_MINECRAFT, app.m_dwOfferID, PROFILE_VERSION_10,
		5, 4, dwProfileSettingsA,
		app.GAME_DEFINED_PROFILE_DATA_BYTES * XUSER_MAX_COUNT,
		&app.uiGameDefinedDataChangedBitmask);

	// StorageManager.Init(...) is never reached on Windows64's own live path
	// either (its one real call site there is inside an #if 0 block) - real
	// per-world save naming happens via TemporaryCreateGameStart()'s
	// ResetSaveData()/SetSaveTitle() calls below. This just needs sane
	// defaults so LinuxStorage.cpp's save/load path has a save-pack/group
	// identity to work against.
	StorageManager.Init(1, L"World", (char *)"minecraft-lce", 0, nullptr, nullptr, "linux-port");

	// Per-thread storage every platform's own entry point sets up on the
	// main thread before Minecraft::main() (see Windows64_Minecraft.cpp
	// ~line 822) - Tile::staticCtor()/RailTile etc, called from inside
	// Minecraft::main(), dereference Tile's TLS unconditionally.
	Tesselator::CreateNewThreadStorage(1024 * 1024);
	AABB::CreateNewThreadStorage();
	Vec3::CreateNewThreadStorage();
	IntCache::CreateNewThreadStorage();
	Compression::CreateNewThreadStorage();
	OldChunkStorage::CreateNewThreadStorage();
	Level::enableLightingCache();
	Tile::CreateNewThreadStorage();

	// Minecraft::main() must run first - it constructs the Minecraft
	// singleton (and its `user`/etc members) that TemporaryCreateGameStart()
	// below dereferences. Matches Windows64_Minecraft.cpp's real ordering
	// (Minecraft::main() at line ~832; its own now-commented-out
	// TemporaryCreateGameStart() call sits much later, near line 888).
	Minecraft::main();
	Minecraft *pMinecraft = Minecraft::GetInstance();
	app.InitGameSettings();
	app.InitialiseTips();

	// Constructs s_pPlatformNetworkManager (PlatformNetworkManagerStub on
	// desktop) - TemporaryCreateGameStart()'s HostGame()/FakeLocalPlayerJoined()
	// dereference it unconditionally. Matches Windows64_Minecraft.cpp:794.
	g_NetworkManager.Initialise();

	// Direct non-UI world launch - see Linux_MinecraftApp.cpp. Ported verbatim from
	// Windows64_App.cpp's own dev-shortcut of the same name; skips
	// UIScene_CreateWorldMenu and the Iggy progress scene entirely.
	//
	// This was the boot path for all of Phase 7, when there was no working UI to
	// launch a world from. Now that there is (Phase 8), it would fight the frontend:
	// ui.init() ends in NavigateToScene(eUIScene_Intro), so the intro/main menu is
	// already up, and starting a world underneath it puts the game in two states at
	// once. Kept behind a flag because it is still the quickest way to get straight
	// into a world, and the only way to test gameplay if the UI regresses.
	if (bDirectWorld)
	{
		app.DebugPrintf("--direct-world: closing the frontend, launching a world directly.\n");

		// ui.init() ended in NavigateToScene(eUIScene_Intro), so the intro and its
		// autosave message box are already on screen. Leaving them there does not just
		// look wrong - a displayed menu legitimately blocks gameplay input
		// (Minecraft.cpp:2238 gates the whole in-game input block on
		// ui.GetMenuDisplayed(), which is how breaking and placing get suppressed), so
		// the world would be unplayable underneath it. Close the scenes first.
		ui.CloseAllPlayersScenes();

		app.TemporaryCreateGameStart();
	}

	pMinecraft->options->set(Options::Option::MUSIC, 1.0f);
	pMinecraft->options->set(Options::Option::SOUND, 1.0f);

	bool running = true;
	while (running)
	{
		running = linuxApp.PollEvents();

		RenderManager.StartFrame();

		app.UpdateTime();
		InputManager.Tick();
		StorageManager.Tick();
		RenderManager.Tick();

		if (app.GetGameStarted())
		{
			pMinecraft->run_middle();
			app.SetAppPaused(g_NetworkManager.IsLocalGame() && g_NetworkManager.GetPlayerCount() == 1 &&
				ui.IsPauseMenuDisplayed(ProfileManager.GetPrimaryPad()));
		}
		else
		{
			pMinecraft->soundEngine->tick(NULL, 0.0f);
			pMinecraft->textures->tick(true, false);
			IntCache::Reset();
		}

		pMinecraft->soundEngine->playMusicTick();

		ui.tick();
		ui.render();

		RenderManager.Present();

		// Dispatch queued app actions. app.SetAction() only *records* an action;
		// CMinecraftApp::HandleXuiActions() is the sole consumer of the queue, and every
		// other platform's main loop pumps it once per frame (Windows64_Minecraft.cpp:1128,
		// Durango:950, Orbis:1416, PS3:1316, PSVita:1010, Xbox:824 - all live code, each
		// just past a closing #endif).
		//
		// Without it the whole exit chain is dead after the confirmation dialogs: the pause
		// menu records eAppAction_ExitWorld and nothing ever runs it, so SetGameStarted(false)
		// - which lives two hops later in case eAppAction_ExitWorldCapturedThumbnail - never
		// happens and the loop above keeps calling run_middle(). That is the reported
		// "exiting to the main menu leaves the world running underneath".
		//
		// It also silently disabled Save Game, autosave, death-menu Respawn, per-player exit,
		// dimension-change completion and eAppAction_ReloadTexturePack.
		app.HandleXuiActions();

		ui.CheckMenuDisplayed();
	}

	// Without this, background threads gated on ShutdownManager::ShouldRun()
	// (GameRenderer's chunk-update thread, MinecraftServer's post-process/
	// server threads, C4JThread's event-queue threads, Connection's read/
	// write threads) are still running when main() returns and races the
	// process's static-destructor teardown - this was the crash on window
	// close. StartShutdown() flips the flag; MainThreadHandleShutdown()
	// requests each thread to stop and blocks until they've all confirmed.
	ShutdownManager::StartShutdown();
	ShutdownManager::MainThreadHandleShutdown();

	linuxApp.Shutdown();
	return 0;
}
