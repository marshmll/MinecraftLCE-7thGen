#pragma once

#include "../../Common/UI/UIController.h"

// Linux platform UIController, the counterpart of ConsoleUIController on Windows64 /
// Durango / Orbis / PS3 / PSVita. Structurally a transliteration of
// Windows64_UIController.cpp with gdraw_GL_* in place of gdraw_D3D11_*.
//
// This used to be a no-op stand-in, because Iggy was believed to be unavailable on
// Linux. It isn't: Iggy's OpenGL GDraw backend ships as source in this tree and the
// PS4 build of its portable core is ELF x86-64. See .claude/linux-port/IGGY.md.
//
// Unlike the other platforms, init() takes no device/context arguments - GDraw's GL
// backend binds to whatever GL context is current, so CLinuxApp's context just has to
// exist first (and be a *compatibility* profile - see Linux_App.cpp).
class LinuxUIController : public UIController
{
public:
	// preInit()/postInit() are protected on the base class; every platform's own
	// init() calls them from inside the hierarchy, and this is that function for
	// Linux. Between them it creates the GDraw context and hands it to Iggy, because
	// postInit() goes on to load skins and navigate to the first scene, both of which
	// need a live GDraw.
	void init(S32 width, S32 height);
	void shutdown();

	void render();
	CustomDrawData *setupCustomDraw(UIScene *scene, IggyCustomDrawCallbackRegion *region);
	CustomDrawData *calculateCustomDraw(IggyCustomDrawCallbackRegion *region);
	void endCustomDraw(IggyCustomDrawCallbackRegion *region);
	void beginIggyCustomDraw4J(IggyCustomDrawCallbackRegion *region, CustomDrawData *customDrawRegion);

	GDrawTexture *getSubstitutionTexture(int textureId);
	void destroySubstitutionTexture(void *destroyCallBackData, GDrawTexture *handle);

	// True once init() has a GDraw context and Iggy can be asked to draw. The frame
	// loop and any early-boot code that might reach render() need this, because
	// UIController's constructor runs long before the GL context exists.
	bool IsReady() const { return m_gdrawFuncs != NULL; }

protected:
	void setTileOrigin(S32 xPos, S32 yPos);

private:
	GDrawFunctions *m_gdrawFuncs = NULL;
};

extern LinuxUIController ui;
