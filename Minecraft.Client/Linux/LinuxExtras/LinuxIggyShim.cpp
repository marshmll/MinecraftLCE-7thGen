// LinuxIggyShim.cpp - no-op bodies for the Iggy/Iggy-perfmon runtime calls
// Minecraft.Client/Common/UI/UIController.cpp makes (grep-confirmed: ~17
// distinct symbols). Iggy (RAD Game Tools' closed-source vector-UI
// middleware) has real SDK headers in the leak (Windows64/Iggy/include/*.h,
// reused as-is for Linux via stdafx.h - the types/prototypes are portable
// C++, only the compiled runtime is Windows/console-only) but no source and
// no Linux .lib. Per the Phase 7 plan, Iggy itself (loading/playing real
// vector-UI movies) is out of scope - this shim exists purely so
// UIController.cpp (which most of Minecraft.Client calls into for
// menu-state queries/tooltips/SFX, not rendering) compiles and links
// unmodified. Every stub is safe precisely because nothing in this port
// calls UIController::postInit()/loadSkins() (the only caller of the
// library-loading functions below) - see LinuxUIController.cpp.
//
// Signatures are copied verbatim from Windows64/Iggy/include/{iggy.h,
// iggyperfmon.h,iggyexpruntime.h} - not guessed.

#include "stdafx.h"

// IggyExpCreate/IggyUseExplorer/IggyPerfmonCreate/IggyInstallPerfmon/
// IggyPerfmonTickAndDraw are deliberately NOT stubbed here: their only call
// sites in UIController.cpp are inside #ifdef ENABLE_IGGY_EXPLORER /
// #ifdef ENABLE_IGGY_PERFMON blocks, and both macros are permanently
// commented out (`//#define ENABLE_IGGY_EXPLORER` etc.) - dead code on
// every platform including Windows64, not just Linux. Defining them would
// need iggyperfmon.h/iggyexpruntime.h included for no reason.

void IggyInit(IggyAllocator *allocator)
{
}

IggyLibrary IggyLibraryCreateFromMemoryUTF16(IggyUTF16 const *url_utf16_null_terminated, void const *data, U32 data_size_in_bytes, IggyPlayerConfig *config)
{
	return IGGY_INVALID_LIBRARY;
}

void IggyLibraryDestroy(IggyLibrary lib)
{
}

void IggySetWarningCallback(Iggy_WarningFunction *error, void *user_callback_data)
{
}

void IggySetTraceCallbackUTF8(Iggy_TraceFunctionUTF8 *trace_utf8, void *user_callback_data)
{
}

void IggySetCustomDrawCallback(Iggy_CustomDrawCallback *custom_draw, void *user_callback_data)
{
}

void IggySetTextureSubstitutionCallbacks(Iggy_TextureSubstitutionCreateCallback *texture_create, Iggy_TextureSubstitutionDestroyCallback *texture_destroy, void *user_callback_data)
{
}

void IggySetAS3ExternalFunctionCallbackUTF16(Iggy_AS3ExternalFunctionUTF16 *as3_external_function_utf16, void *user_callback_data)
{
}

void IggySetFontCachingCalculationBuffer(S32 max_chars, void *optional_temp_buffer, S32 optional_temp_buffer_size_in_bytes)
{
}

void *IggyPlayerGetUserdata(Iggy *player)
{
	return NULL;
}

void IggyFontSetIndirectUTF8(const char *request_name, S32 request_namelen, U32 request_flags, const char *result_name, S32 result_namelen, U32 result_flags)
{
}

rrbool IggyDebugGetMemoryUseInfo(Iggy *player, IggyLibrary lib, char const *category_string, S32 category_stringlen, S32 iteration, IggyMemoryUseInfo *data)
{
	return 0;
}

// IggyPlayer* - actual movie-instance playback (drawing, ticking, event
// dispatch). Called from UIScene.cpp/UIGroup.cpp/the "component" overlays
// (UIComponent_Tooltips etc) this port does compile, but since no real Iggy
// library ever loads (IggyLibraryCreateFromMemoryUTF16 above always returns
// IGGY_INVALID_LIBRARY), no code path actually has a valid Iggy* to hand
// these - they exist purely so those callers link, matching the plan's
// "skip Iggy rendering entirely" scope for this phase.
Iggy *IggyPlayerCreateFromMemory(void const *data, U32 data_size_in_bytes, IggyPlayerConfig *config)
{
	return NULL;
}

void IggyPlayerDestroy(Iggy *player)
{
}

void IggyPlayerSetUserdata(Iggy *player, void *userdata)
{
}

IggyProperties *IggyPlayerProperties(Iggy *player)
{
	return NULL;
}

void IggyPlayerInitializeAndTickRS(Iggy *player)
{
}

rrbool IggyPlayerReadyToTick(Iggy *player)
{
	return 0;
}

void IggyPlayerTickRS(Iggy *player)
{
}

void IggyPlayerSetDisplaySize(Iggy *f, S32 w, S32 h)
{
}

void IggyPlayerDraw(Iggy *f)
{
}

void IggyPlayerDrawTilesStart(Iggy *f)
{
}

void IggyPlayerDrawTile(Iggy *f, S32 x0, S32 y0, S32 x1, S32 y1, S32 padding)
{
}

void IggyPlayerDrawTilesEnd(Iggy *f)
{
}

IggyValuePath *IggyPlayerRootPath(Iggy *f)
{
	return NULL;
}

IggyName IggyPlayerCreateFastName(Iggy *f, IggyUTF16 const *name, S32 len)
{
	return NULL;
}

IggyResult IggyPlayerCallMethodRS(Iggy *f, IggyDataValue *result, IggyValuePath *target, IggyName methodname, S32 numargs, IggyDataValue *args)
{
	return IGGY_RESULT_SUCCESS;
}

void IggyMakeEventKey(IggyEvent *event, IggyKeyevent event_type, IggyKeycode keycode, IggyKeyloc keyloc)
{
}

rrbool IggyPlayerDispatchEventRS(Iggy *player, IggyEvent *event, IggyEventResult *result)
{
	return 0;
}

// The remaining symbols below are called from Common/UI/UIControl.cpp,
// UIBitmapFont.cpp, and UITTFFont.cpp (font install/removal, and
// IggyValuePath get/set - used to read/write values on a live Iggy movie
// instance). Same rationale as above: since no real Iggy library or player
// instance is ever created (IggyLibraryCreateFromMemoryUTF16/
// IggyPlayerCreateFromMemory always return invalid/NULL), these bodies are
// never exercised on a real value tree - they exist purely for linkage.
// Signatures copied verbatim from Windows64/Iggy/include/iggy.h.

rrbool IggyValuePathMakeNameRef(IggyValuePath *result, IggyValuePath *parent, char const *text_utf8)
{
	return 0;
}

IggyResult IggyValueGetF64RS(IggyValuePath *var, IggyName sub_name, char const *sub_name_utf8, F64 *result)
{
	return IGGY_RESULT_Error_ValuePath;
}

IggyResult IggyValueGetBooleanRS(IggyValuePath *var, IggyName sub_name, char const *sub_name_utf8, rrbool *result)
{
	return IGGY_RESULT_Error_ValuePath;
}

rrbool IggyValueSetBooleanRS(IggyValuePath *var, IggyName sub_name, char const *sub_name_utf8, rrbool value)
{
	return 0;
}

void IggyFontInstallTruetypeUTF8(const void *truetype_storage, S32 ttc_index, const char *fontname, S32 namelen_in_bytes, U32 fontflags)
{
}

void IggyFontInstallTruetypeFallbackCodepointUTF8(const char *fontname, S32 len, U32 fontflags, S32 fallback_codepoint)
{
}

void IggyFontInstallBitmapUTF8(const IggyBitmapFontProvider *bmf, const char *fontname, S32 namelen_in_bytes, U32 fontflags)
{
}

void IggyFontRemoveUTF8(const char *fontname, S32 namelen_in_bytes, U32 fontflags)
{
}
