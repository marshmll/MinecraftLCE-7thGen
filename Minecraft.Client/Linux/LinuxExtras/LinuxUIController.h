#pragma once

#include "../../Common/UI/UIController.h"

// Linux stand-in for the platform UIController subclasses (ConsoleUIController
// on Windows64, etc). Per the Phase 7 plan, real Iggy-backed menu/pause/
// inventory UI is out of scope here (see LinuxIggyShim.cpp) - this class
// exists purely to satisfy UIController's small set of actual pure virtuals
// (render/setTileOrigin/setupCustomDraw/calculateCustomDraw/endCustomDraw/
// beginIggyCustomDraw4J) with no-op bodies, so the real UIController/UIGroup/
// UILayer/UIScene(base) infrastructure - which most of Minecraft.Client's
// game logic calls into for menu-state queries, tooltips, and UI SFX, not
// rendering - can compile and run unmodified. It deliberately never calls
// UIController::postInit()/loadSkins() (the only path that would touch the
// Iggy library-loading stubs in LinuxIggyShim.cpp in a way that matters).
class LinuxUIController : public UIController
{
public:
	// preInit()/postInit() are protected on the base class (every real
	// platform's own *UIController::init() override calls them from inside
	// the class hierarchy) - this just re-exposes them for Linux_Minecraft.cpp's
	// direct, non-D3D11/gdraw boot sequence. See Linux_Minecraft.cpp's call
	// site for why calling postInit() here is safe (no real Iggy loading).
	void Boot(S32 width, S32 height) { preInit(width, height); postInit(); }

	// No Iggy scene ever actually appears on screen in this build, so nothing is
	// ever "displaying a menu" - report that truthfully.
	//
	// The base class can't: UIController::NavigateToScene() sets the flag *before*
	// it tries to build the scene (UIController.cpp:1481) and only NavigateBack()
	// clears it. Here the scene construction fails ("WARNING: Scene 1 was not
	// created" in the log) and nothing ever navigates back, so the flag latches on
	// permanently. That silently disabled a large amount of gameplay, because
	// Minecraft::tick() guards its whole in-game input block on it
	// (Minecraft.cpp:2238) - block breaking and placing among others - and Gui.cpp:175
	// gates the entire HUD on it too.
	bool GetMenuDisplayed(int iPad) { return false; }

	void render();
	CustomDrawData *setupCustomDraw(UIScene *scene, IggyCustomDrawCallbackRegion *region);
	CustomDrawData *calculateCustomDraw(IggyCustomDrawCallbackRegion *region);
	void endCustomDraw(IggyCustomDrawCallbackRegion *region);
	void beginIggyCustomDraw4J(IggyCustomDrawCallbackRegion *region, CustomDrawData *customDrawRegion);

protected:
	void setTileOrigin(S32 xPos, S32 yPos);
};

extern LinuxUIController ui;
