#include "stdafx.h"
#include "LinuxUIController.h"

LinuxUIController ui;

void LinuxUIController::render()
{
	// Real Iggy scene rendering (menus/pause/inventory) is out of scope for
	// Phase 7 - the in-game HUD (Gui.cpp) and 3D world (LevelRenderer) render
	// through a completely separate, Iggy-free path and don't call this.
}

CustomDrawData *LinuxUIController::setupCustomDraw(UIScene *scene, IggyCustomDrawCallbackRegion *region)
{
	return NULL;
}

CustomDrawData *LinuxUIController::calculateCustomDraw(IggyCustomDrawCallbackRegion *region)
{
	return NULL;
}

void LinuxUIController::endCustomDraw(IggyCustomDrawCallbackRegion *region)
{
}

void LinuxUIController::beginIggyCustomDraw4J(IggyCustomDrawCallbackRegion *region, CustomDrawData *customDrawRegion)
{
}

void LinuxUIController::setTileOrigin(S32 xPos, S32 yPos)
{
}
