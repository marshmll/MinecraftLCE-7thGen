#ifndef __MCLINUX_GDRAW_SDL_H__
#define __MCLINUX_GDRAW_SDL_H__

/*
 * Linux-only additions to GDraw's GL backend.
 *
 * The portable gdraw_GL_* entry points are declared by the vendor's own
 * Windows64/Iggy/gdraw/gdraw_wgl.h, which is reused as-is (despite the name it only
 * includes rrCore.h and gdraw.h). This header exists for the one thing that backend
 * is missing relative to D3D11's, so the vendor files stay unmodified.
 */

#include "rrCore.h"
#include "gdraw.h"

RADDEFSTART

struct IggyCustomDrawCallbackRegion;

/*
 * Compute a custom-draw region's object-to-clip matrix *without* touching render
 * state - the equivalent of D3D11's gdraw_D3D11_CalculateCustomDraw_4J.
 *
 * GDraw's GL backend only offers gdraw_GL_BeginCustomDraw, which clears render state
 * and computes the matrix in one go. The game needs them separated:
 * UIScene::customDrawSlotControl caches a CustomDrawData per inventory slot
 * (UIScene.cpp:676) and replays it later, so the matrix has to be filled at
 * calculate-time. Deriving it in beginIggyCustomDraw4J instead would leave every
 * cached slot icon with an uninitialised matrix.
 *
 * Implemented in gdraw_sdl.c, where gdraw_shared.inl's static
 * gdraw_GetObjectSpaceMatrix() is in scope.
 *
 * These pass out_col_major = 0 (ROW-major), matching D3D11/Orbis/PS3/PSVita and the
 * shared UIController::setupCustomDrawMatrices that consumes the result - NOT the 1 that
 * the vendor's own gdraw_GL_BeginCustomDraw passes. See the long comment in gdraw_sdl.c;
 * getting this wrong puts every item icon and skin preview in the centre of the screen.
 *
 * gdraw_GL_BeginCustomDraw_4J is the twin of gdraw_D3D11_BeginCustomDraw_4J: it clears
 * render state as well as computing the matrix. Call it in preference to the vendor's
 * gdraw_GL_BeginCustomDraw, which would recompute the matrix column-major.
 */
extern void gdraw_GL_CalculateCustomDraw(struct IggyCustomDrawCallbackRegion *region,
                                         F32 *matrix);
extern void gdraw_GL_BeginCustomDraw_4J(struct IggyCustomDrawCallbackRegion *region,
                                        F32 *matrix);

/*
 * Save/restore the GL state GDraw is about to trample, around a frame of Iggy
 * drawing.
 *
 * GDraw and LinuxRender share one GL context and each assumes it owns the state it
 * set. LinuxRender mostly self-heals - ApplyStateAndDraw re-uploads its program, all
 * uniforms, texture binding and VAO on every draw, and its StateSet* methods issue
 * their GL calls unconditionally with no shadow-compare early-outs. What does not heal
 * is state the game sets once and expects to persist, such as Minecraft.cpp:381's
 * glCullFace(GL_BACK).
 *
 * Rather than enumerate what GDraw touches - a list that would rot silently - this
 * brackets it wholesale with glPushAttrib/glPopAttrib (available because the context
 * is a compatibility profile), plus a manual save of the shader program and the
 * VAO/buffer bindings, which glPushAttrib does not cover.
 *
 * These live here rather than in LinuxUIController.cpp because that translation unit
 * gets stdafx.h, which pulls in the game's own legacy free-function gl* shim
 * (Minecraft.Client/stubs.h). Those declarations conflict with the real <GL/gl.h>, so
 * game code cannot include it.
 *
 * Call Save before, and Restore after, exactly one region. Not nestable.
 */
extern void gdraw_GL_SaveHostState(void);
extern void gdraw_GL_RestoreHostState(void);

/*
 * Re-apply GDraw's viewport before the game renders 3D into a custom-draw region.
 *
 * UIController::setupCustomDrawGameState calls the per-backend equivalent of this
 * (gdraw_D3D11_setViewport_4J on Windows64/Durango, gdraw_orbis_setViewport_4J on PS4)
 * so that the orthographic projection it then sets up lands in the same pixel space
 * Iggy just drew into. That #ifdef chain had no Linux case, so on Linux the game drew
 * item icons and player-skin previews with whatever viewport happened to be current -
 * which is why UIScene_SkinSelectMenu showed no skins.
 *
 * Like the D3D11 and Orbis versions this is a 4J addition, not vendor API, so it is
 * declared here rather than in gdraw_wgl.h.
 */
extern void gdraw_GL_setViewport_4J(void);

RADDEFEND

#endif
