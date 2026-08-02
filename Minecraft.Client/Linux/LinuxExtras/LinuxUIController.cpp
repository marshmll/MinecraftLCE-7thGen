#include "stdafx.h"
#include "LinuxUIController.h"
#include "LinuxRender.h"

#include "../Iggy/gdraw_sdl.h"

// Note: no <GL/gl.h> here. stdafx.h pulls in the game's own legacy free-function gl*
// shim (Minecraft.Client/stubs.h), whose declarations conflict with the real header.
// Anything needing genuine GL calls lives in gdraw_sdl.c instead.

LinuxUIController ui;

void LinuxUIController::init(S32 width, S32 height)
{
	// Shared init, exactly as every platform's own init() does it.
	preInit(width, height);

	// Resource cache sizes. Windows64 passes the same numbers with a comment that for
	// D3D these are only approximate because D3D manages the storage; GDraw's GL
	// backend allocates real handle tables from them, so they matter more here. These
	// are generous on purpose - the skin libraries alone hold several thousand
	// symbols, and undersizing shows up as symbols silently failing to draw.
	gdraw_GL_SetResourceLimits(GDRAW_GL_RESOURCE_vertexbuffer, 5000,  16 * 1024 * 1024);
	gdraw_GL_SetResourceLimits(GDRAW_GL_RESOURCE_texture,      5000, 128 * 1024 * 1024);
	gdraw_GL_SetResourceLimits(GDRAW_GL_RESOURCE_rendertarget,   10,  32 * 1024 * 1024);

	// No device/context to pass: GDraw's GL backend binds to the current GL context,
	// which CLinuxApp::Init() has already created. It must be a compatibility profile
	// - see Linux_App.cpp and gdraw_sdl.c. MSAA off (1 sample); the game does not use
	// it either.
	m_gdrawFuncs = gdraw_GL_CreateContext(width, height, 1);

	if (!m_gdrawFuncs)
	{
		// Fail loudly. A null GDraw would leave every Iggy draw call silently doing
		// nothing, which reads as "the UI is broken" rather than "the UI never
		// started" - and that ambiguity cost a whole phase on this port already.
		app.DebugPrintf("Failed to initialise GDraw! The UI will not render.\n");
		app.DebugPrintf("  (gdraw_sdl.c prints the specific reason above - usually a "
		                "core-profile GL context or a missing extension.)\n");
		return;
	}

	// GDraw is ready, so point Iggy at it. Must happen before postInit(), which loads
	// the skin libraries and navigates to the first scene.
	IggySetGDraw(m_gdrawFuncs);

	// Deliberately no IggyAudioUse*(). Iggy's mixer is only for audio embedded in the
	// SWFs; the game drives its own sound, Windows64 is the only platform that turns
	// Iggy's on, and Durango_UIController.cpp:57 has it commented out with "Iggy
	// crashes if I have audio enabled". Consistent with that, LinuxRadShim.c stubs
	// RADSS_SonyInstallDriver to return "no driver".

	// Shared init
	postInit();
}

void LinuxUIController::render()
{
	if (!m_gdrawFuncs)
		return;

	// GDraw and LinuxRender share one GL context and each assumes it owns the state it
	// set, so bracket the whole thing - see gdraw_GL_SaveHostState's comment in
	// gdraw_sdl.h for what is and isn't at risk.
	gdraw_GL_SaveHostState();

	// Iggy's high-quality AA encodes an object id in the depth buffer: GDraw draws with
	// test_id/set_id set and glDepthFunc(GL_LESS) (gdraw_gl_shared.inl:1479-1490), the
	// depth value coming from depth_from_id(). Traced draws confirm the button-icon quads
	// arrive with test_id=1 set_id=1.
	//
	// That only works if the depth buffer starts each frame cleared. In-game the world
	// renderer clears it (GameRenderer.cpp:1291) and endCustomDrawGameState clears it
	// again, but in the frontend - no world - nothing does, so ids accumulate across
	// frames and every depth-tested draw fails from the second frame on. That is why a
	// button prompt kept its letter (not depth-tested) but lost the disc behind it, and
	// why everything worked as soon as a world was running.
	RenderManager.Clear(GL_DEPTH_BUFFER_BIT);

	// Point GDraw at the render target and origin, draw every scene, then tell it the
	// frame is over so it can flush. Framebuffer 0 is the default one - unlike D3D11
	// there is no render-target view to hand over.
	gdraw_GL_SetTileOrigin(0, 0, 0);

	renderScenes();

	gdraw_GL_NoMoreGDrawThisFrame();

	gdraw_GL_RestoreHostState();
}

void LinuxUIController::setTileOrigin(S32 xPos, S32 yPos)
{
	if (!m_gdrawFuncs)
		return;
	gdraw_GL_SetTileOrigin(xPos, yPos, 0);
}

CustomDrawData *LinuxUIController::setupCustomDraw(UIScene *scene, IggyCustomDrawCallbackRegion *region)
{
	CustomDrawData *customDrawRegion = new CustomDrawData();
	customDrawRegion->x0 = region->x0;
	customDrawRegion->x1 = region->x1;
	customDrawRegion->y0 = region->y0;
	customDrawRegion->y1 = region->y1;

	// Gets the object-to-world matrix from GDraw and resets render state to a normal
	// one, so the game can draw 3D items inside a Flash region.
	//
	// The _4J variant, not the vendor gdraw_GL_BeginCustomDraw: that one writes the matrix
	// column-major, which the shared setupCustomDrawMatrices below misreads. See gdraw_sdl.h.
	if (m_gdrawFuncs)
		gdraw_GL_BeginCustomDraw_4J(region, customDrawRegion->mat);

	setupCustomDrawGameStateAndMatrices(scene, customDrawRegion);

	return customDrawRegion;
}

CustomDrawData *LinuxUIController::calculateCustomDraw(IggyCustomDrawCallbackRegion *region)
{
	CustomDrawData *customDrawRegion = new CustomDrawData();
	customDrawRegion->x0 = region->x0;
	customDrawRegion->x1 = region->x1;
	customDrawRegion->y0 = region->y0;
	customDrawRegion->y1 = region->y1;

	// Matrix only, no render-state change - the caller pairs this with
	// beginIggyCustomDraw4J, or caches the result and replays it later
	// (UIScene.cpp:676). GDraw's GL backend bundles both into
	// gdraw_GL_BeginCustomDraw, so gdraw_sdl.c splits the matrix half out; see
	// gdraw_sdl.h.
	if (m_gdrawFuncs)
		gdraw_GL_CalculateCustomDraw(region, customDrawRegion->mat);

	return customDrawRegion;
}

void LinuxUIController::beginIggyCustomDraw4J(IggyCustomDrawCallbackRegion *region, CustomDrawData *customDrawRegion)
{
	if (!m_gdrawFuncs)
		return;
	// Recomputes the matrix as well as clearing render state, exactly as
	// gdraw_D3D11_BeginCustomDraw_4J does.
	//
	// It must be the _4J variant. Calling the vendor gdraw_GL_BeginCustomDraw here was not
	// "redundant but harmless" as this comment used to claim: UIScene::customDrawSlotControl
	// calls calculateCustomDraw and then immediately this, so the vendor version's
	// column-major recompute silently overwrote the correct matrix on the cached-slot path.
	gdraw_GL_BeginCustomDraw_4J(region, customDrawRegion->mat);
}

void LinuxUIController::endCustomDraw(IggyCustomDrawCallbackRegion *region)
{
	endCustomDrawGameStateAndMatrices();

	if (m_gdrawFuncs)
		gdraw_GL_EndCustomDraw(region);
}

GDrawTexture *LinuxUIController::getSubstitutionTexture(int textureId)
{
	// Wraps a texture the *game* owns so Iggy can draw with it, without GDraw taking
	// ownership or ever freeing it. This is how world thumbnails and texture-pack
	// icons get into the UI (UIScene::registerSubstitutionTexture).
	unsigned int glName = 0;
	int width = 0, height = 0;

	if (!m_gdrawFuncs)
		return NULL;

	// Windows64 goes through C4JRender::TextureGetTexture() and a D3D11 resource
	// query; on Linux that returns nullptr by design, so ask LinuxRender directly.
	if (!LinuxRender_GetGLTexture(textureId, &glName, &width, &height))
	{
		app.DebugPrintf("getSubstitutionTexture: unknown C4JRender texture id %d\n", textureId);
		return NULL;
	}
	if (width <= 0 || height <= 0)
	{
		// A created-but-never-uploaded texture. Wrapping it at 0x0 would have Iggy
		// sample nothing, so say so instead.
		app.DebugPrintf("getSubstitutionTexture: texture id %d has no data yet\n", textureId);
		return NULL;
	}

	// has_mipmaps is false: the substitution textures are UI thumbnails uploaded as a
	// single level (see LinuxRender's TextureData()).
	return gdraw_GL_WrappedTextureCreate((S32)glName, width, height, false);
}

void LinuxUIController::destroySubstitutionTexture(void *destroyCallBackData, GDrawTexture *handle)
{
	// Frees GDraw's wrapper only. The underlying GL texture belongs to the game and is
	// released through C4JRender::TextureFree() like any other.
	if (m_gdrawFuncs && handle)
		gdraw_GL_WrappedTextureDestroy(handle);
}

void LinuxUIController::shutdown()
{
	if (!m_gdrawFuncs)
		return;
	// Only safe once every Iggy player has been destroyed - the same caveat every
	// platform's shutdown() carries.
	gdraw_GL_DestroyContext();
	m_gdrawFuncs = NULL;
}
