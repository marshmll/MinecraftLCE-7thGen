/*
 * LinuxIggyUtf16.cpp - UTF-16 marshalling between the game and Iggy, for Linux.
 *
 * THE PROBLEM
 *
 * `IggyUTF16` is `unsigned short` (iggy.h:111) - genuine 16-bit UTF-16. On MSVC
 * `wchar_t` is also 16-bit, so all of Common/UI passes wide strings to Iggy with a
 * plain reinterpret cast:
 *
 *     stringVal.string = (IggyUTF16 *)label.c_str();
 *     lib = IggyLibraryCreateFromMemoryUTF16((IggyUTF16 *)skinName.c_str(), ...);
 *
 * On Linux `wchar_t` is 32-bit, so those casts hand Iggy UTF-32 bytes. Iggy reads the
 * low half of the first code point, then hits the zero high half and sees a
 * one-character string. Nothing errors: skin libraries register under a garbage URL,
 * so every scene fails with "Attempted to import undefined library skinGraphics.swf"
 * even though it loaded seconds earlier; labels come out as a single glyph; and
 * AS3 callback names never match their wcscmp.
 *
 * WHY THIS FILE RATHER THAN FIXING THE CALL SITES
 *
 * There are 93 `string16` assignments across 26 files in Common/UI, plus the library
 * URL, the fast-name lookup, the AS3 callback names and the texture-substitution
 * names. Editing all of them would mean ~100 `#ifdef _LINUX64` blocks in shared code
 * that Windows and the consoles also build.
 *
 * They all funnel through a handful of Iggy entry points, so the conversion happens
 * there instead. patch_orbis_iggy.py renames the vendor's implementations
 * (IggyPlayerCallMethodRS -> iggy_vendor_IggyPlayerCallMethodRS, and so on) exactly as
 * it already does for setjmp, and the wrappers below take the original names. Shared
 * code is untouched; Iggy's own internal calls still reach the vendor functions
 * directly, so they are not double-converted.
 *
 * -fshort-wchar was considered and rejected: it would make wchar_t 16-bit to match
 * MSVC, but the tree uses glibc's wcscmp/swprintf/wcslen throughout, and those expect
 * 32-bit wchar_t. That trade breaks far more than it fixes.
 */

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <vector>

#include "iggy.h"

/* The vendor implementations, renamed by patch_orbis_iggy.py. */
extern "C" {
IggyLibrary iggy_vendor_IggyLibraryCreateFromMemoryUTF16(IggyUTF16 const *url_utf16,
                                                        void const *data,
                                                        U32 data_size_in_bytes,
                                                        IggyPlayerConfig *config);
IggyName   iggy_vendor_IggyPlayerCreateFastName(Iggy *player, IggyUTF16 const *name,
                                                S32 length);
IggyResult iggy_vendor_IggyPlayerCallMethodRS(Iggy *player, IggyDataValue *result,
                                              IggyValuePath *path, IggyName method,
                                              S32 num_args, IggyDataValue *args);
void       iggy_vendor_IggySetAS3ExternalFunctionCallbackUTF16(
                                              Iggy_AS3ExternalFunctionUTF16 *callback,
                                              void *user_callback_data);
void       iggy_vendor_IggySetTextureSubstitutionCallbacks(
                                 Iggy_TextureSubstitutionCreateCallback *create,
                                 Iggy_TextureSubstitutionDestroyCallback *destroy,
                                 void *user_callback_data);
void       iggy_vendor_IggySetCustomDrawCallback(Iggy_CustomDrawCallback *custom_draw,
                                                 void *user_callback_data);
}

/* ------------------------------------------------------------------ *
 *  Conversion                                                        *
 * ------------------------------------------------------------------ */

namespace
{

// Scratch storage for converted strings. One arena per thread, reset at the start of
// each wrapped call: Iggy consumes the strings synchronously inside the call, and the
// game copies anything it gets back before doing anything else, so nothing needs to
// outlive that. Deliberately not a fixed buffer - a long chat line or world name
// should grow it, not overflow it.
class Arena
{
public:
	void reset() { m_used = 0; }

	void *alloc(size_t bytes)
	{
		// Keep 8-byte alignment; IggyUTF16 only needs 2 but this costs nothing.
		bytes = (bytes + 7u) & ~size_t(7u);
		if (m_used + bytes > m_store.size())
			m_store.resize(m_used + bytes + 4096u);
		void *p = m_store.data() + m_used;
		m_used += bytes;
		return p;
	}

private:
	std::vector<unsigned char> m_store;
	size_t m_used = 0;
};

thread_local Arena t_arena;

// Number of UTF-16 code units needed for a UTF-32 run (surrogate pairs count as 2).
S32 utf16LengthOf(const wchar_t *src, S32 chars)
{
	S32 units = 0;
	for (S32 i = 0; i < chars; ++i)
		units += ((unsigned long)src[i] > 0xFFFFul) ? 2 : 1;
	return units;
}

// UTF-32 -> UTF-16 into the arena. Always NUL-terminated: Iggy treats these as
// null-terminated even where a length is also supplied. outUnits, if given, receives
// the code-unit count excluding the terminator.
IggyUTF16 *toUtf16(const wchar_t *src, S32 chars, S32 *outUnits)
{
	if (!src)
	{
		if (outUnits) *outUnits = 0;
		return NULL;
	}
	if (chars < 0)
		chars = (S32)wcslen(src);

	S32 units = utf16LengthOf(src, chars);
	IggyUTF16 *dst = (IggyUTF16 *)t_arena.alloc((size_t)(units + 1) * sizeof(IggyUTF16));

	S32 w = 0;
	for (S32 i = 0; i < chars; ++i)
	{
		unsigned long cp = (unsigned long)src[i];
		if (cp > 0xFFFFul)
		{
			// Outside the BMP: encode as a surrogate pair.
			cp -= 0x10000ul;
			dst[w++] = (IggyUTF16)(0xD800ul + (cp >> 10));
			dst[w++] = (IggyUTF16)(0xDC00ul + (cp & 0x3FFul));
		}
		else
		{
			dst[w++] = (IggyUTF16)cp;
		}
	}
	dst[w] = 0;

	if (outUnits) *outUnits = units;
	return dst;
}

// UTF-16 -> UTF-32 into the arena, for values coming back out of Iggy. Returns a
// wchar_t buffer the game can read through its usual (wchar_t *) cast. outChars, if
// given, receives the character count excluding the terminator.
wchar_t *toWide(const IggyUTF16 *src, S32 units, S32 *outChars)
{
	if (!src)
	{
		if (outChars) *outChars = 0;
		return NULL;
	}
	if (units < 0)
	{
		units = 0;
		while (src[units]) ++units;
	}

	wchar_t *dst = (wchar_t *)t_arena.alloc((size_t)(units + 1) * sizeof(wchar_t));

	S32 w = 0;
	for (S32 i = 0; i < units; ++i)
	{
		unsigned int u = src[i];
		if (u >= 0xD800u && u <= 0xDBFFu && i + 1 < units &&
		    src[i + 1] >= 0xDC00u && src[i + 1] <= 0xDFFFu)
		{
			unsigned long cp = 0x10000ul
			                 + (((unsigned long)u - 0xD800ul) << 10)
			                 + ((unsigned long)src[i + 1] - 0xDC00ul);
			dst[w++] = (wchar_t)cp;
			++i;
		}
		else
		{
			dst[w++] = (wchar_t)u;
		}
	}
	dst[w] = 0;

	if (outChars) *outChars = w;
	return dst;
}

// Rewrite every UTF-16 string in an IggyDataValue array from the game's UTF-32 into
// real UTF-16. Operates on a copy - the caller's array belongs to game code that may
// reuse it.
void convertArgsToUtf16(IggyDataValue *dst, const IggyDataValue *src, S32 count)
{
	for (S32 i = 0; i < count; ++i)
	{
		dst[i] = src[i];
		if (src[i].type != IGGY_DATATYPE_string_UTF16)
			continue;

		S32 units = 0;
		// The game fills .length from wstring::length(), i.e. a character count; the
		// UTF-16 unit count can be larger, so recompute rather than reuse it.
		dst[i].string16.string = toUtf16((const wchar_t *)src[i].string16.string,
		                                 src[i].string16.length, &units);
		dst[i].string16.length = units;
	}
}

}  // namespace

/* ------------------------------------------------------------------ *
 *  Wrappers taking the public names                                  *
 * ------------------------------------------------------------------ */

extern "C" {

IggyLibrary IggyLibraryCreateFromMemoryUTF16(IggyUTF16 const *url_utf16,
                                             void const *data,
                                             U32 data_size_in_bytes,
                                             IggyPlayerConfig *config)
{
	// The URL is what scene SWFs match their SWF ImportAssets2 records against, so
	// getting this wrong silently breaks every screen. This is the single most
	// important conversion in the file.
	t_arena.reset();
	IggyUTF16 *url = toUtf16((const wchar_t *)url_utf16, -1, NULL);
	return iggy_vendor_IggyLibraryCreateFromMemoryUTF16(url, data, data_size_in_bytes,
	                                                    config);
}

IggyName IggyPlayerCreateFastName(Iggy *player, IggyUTF16 const *name, S32 length)
{
	// Every property and method lookup goes through a fast name (UIScene.cpp:526), so
	// without this nothing on a movie can be found by name.
	t_arena.reset();
	S32 units = 0;
	IggyUTF16 *converted = toUtf16((const wchar_t *)name, length, &units);
	// The vendor takes -1 to mean "null-terminated"; preserve that, otherwise pass the
	// recomputed unit count rather than the caller's character count.
	return iggy_vendor_IggyPlayerCreateFastName(player, converted,
	                                            length < 0 ? -1 : units);
}

IggyResult IggyPlayerCallMethodRS(Iggy *player, IggyDataValue *result,
                                  IggyValuePath *path, IggyName method,
                                  S32 num_args, IggyDataValue *args)
{
	t_arena.reset();

	IggyDataValue *converted = NULL;
	if (num_args > 0 && args)
	{
		converted = (IggyDataValue *)t_arena.alloc(sizeof(IggyDataValue) * (size_t)num_args);
		convertArgsToUtf16(converted, args, num_args);
	}

	IggyResult rc = iggy_vendor_IggyPlayerCallMethodRS(player, result, path, method,
	                                                   num_args, converted);

	// A returned string is Iggy's real UTF-16, but callers read it as wchar_t - e.g.
	// UIControl_Base::getLabel() does wstring((wchar_t *)result.string16.string, len).
	// Hand back UTF-32 so that works. The buffer lives in the arena until the next
	// wrapped call, which is long enough: callers copy immediately.
	if (result && result->type == IGGY_DATATYPE_string_UTF16 && result->string16.string)
	{
		S32 chars = 0;
		wchar_t *wide = toWide(result->string16.string, result->string16.length, &chars);
		result->string16.string = (IggyUTF16 *)wide;
		result->string16.length = chars;
	}

	return rc;
}

/* --- callbacks: Iggy -> game, so convert the other way --- */

static Iggy_AS3ExternalFunctionUTF16 *s_gameAS3Callback;
static void                          *s_gameAS3UserData;

static rrbool RADLINK as3ExternalTrampoline(void *user_callback_data, Iggy *player,
                                            IggyExternalFunctionCallUTF16 *call)
{
	(void)user_callback_data;
	if (!s_gameAS3Callback)
		return 0;
	if (!call)
		return s_gameAS3Callback(s_gameAS3UserData, player, call);

	t_arena.reset();

	// UIScene::externalCallback dispatches with wcscmp against L"handlePress" and
	// friends, so the name has to be UTF-32 or nothing ever matches - every button
	// press, focus change and slider move would be silently dropped.
	//
	// Rebuild the whole call struct: it is variable-length (arguments[] holds
	// num_arguments entries), and it belongs to Iggy, so it is not ours to edit.
	S32 argc = call->num_arguments;
	if (argc < 0)
		argc = 0;
	size_t bytes = sizeof(IggyExternalFunctionCallUTF16)
	             + (argc > 0 ? sizeof(IggyDataValue) * (size_t)(argc - 1) : 0);
	IggyExternalFunctionCallUTF16 *copy =
		(IggyExternalFunctionCallUTF16 *)t_arena.alloc(bytes);

	copy->num_arguments = call->num_arguments;
	copy->padding = call->padding;

	S32 chars = 0;
	copy->function_name.string =
		(IggyUTF16 *)toWide(call->function_name.string, call->function_name.length, &chars);
	copy->function_name.length = chars;

	for (S32 i = 0; i < argc; ++i)
	{
		copy->arguments[i] = call->arguments[i];
		if (call->arguments[i].type == IGGY_DATATYPE_string_UTF16 &&
		    call->arguments[i].string16.string)
		{
			S32 n = 0;
			copy->arguments[i].string16.string =
				(IggyUTF16 *)toWide(call->arguments[i].string16.string,
				                    call->arguments[i].string16.length, &n);
			copy->arguments[i].string16.length = n;
		}
	}

	return s_gameAS3Callback(s_gameAS3UserData, player, copy);
}

void IggySetAS3ExternalFunctionCallbackUTF16(Iggy_AS3ExternalFunctionUTF16 *callback,
                                             void *user_callback_data)
{
	s_gameAS3Callback = callback;
	s_gameAS3UserData = user_callback_data;
	iggy_vendor_IggySetAS3ExternalFunctionCallbackUTF16(
		callback ? &as3ExternalTrampoline : NULL, NULL);
}

static Iggy_CustomDrawCallback *s_gameCustomDraw;
static void                    *s_gameCustomDrawUserData;

static void RADLINK customDrawTrampoline(void *user_callback_data, Iggy *player,
                                         IggyCustomDrawCallbackRegion *region)
{
	(void)user_callback_data;
	if (!s_gameCustomDraw)
		return;
	if (!region || !region->name)
	{
		s_gameCustomDraw(s_gameCustomDrawUserData, player, region);
		return;
	}

	// region->name is real UTF-16, and 12 sites in Common/UI parse it with glibc's
	// swscanf/wcscmp through a (wchar_t *) cast: UIScene_HUD's "slot_%d" (the hotbar),
	// the container/crafting/enchanting/trading slot families,
	// UIScene_SkinSelectMenu's "Character%d", UIScene_MainMenu's "Splash". Without
	// widening, none of them ever match, the slot id stays -1, and the icon is skipped -
	// which is why the hotbar and the skin previews drew nothing while the tutorial
	// popup's icon (which never parses the name) drew fine.
	//
	// Copy the struct rather than editing Iggy's: it is not ours, and the caller may
	// still need the original name. The arena buffer lives until the next wrapped call,
	// which is long past the end of this callback.
	IggyCustomDrawCallbackRegion copy = *region;

	S32 units = 0;
	while (region->name[units])
		++units;

	S32 chars = 0;
	copy.name = (IggyUTF16 *)toWide(region->name, units, &chars);

	s_gameCustomDraw(s_gameCustomDrawUserData, player, &copy);
}

void IggySetCustomDrawCallback(Iggy_CustomDrawCallback *custom_draw, void *user_callback_data)
{
	s_gameCustomDraw = custom_draw;
	s_gameCustomDrawUserData = user_callback_data;
	iggy_vendor_IggySetCustomDrawCallback(custom_draw ? &customDrawTrampoline : NULL, NULL);
}

static Iggy_TextureSubstitutionCreateCallback  *s_gameTexCreate;
static Iggy_TextureSubstitutionDestroyCallback *s_gameTexDestroy;
static void                                    *s_gameTexUserData;

static GDrawTexture *RADLINK texSubstitutionTrampoline(void *user_callback_data,
                                                       IggyUTF16 *texture_name,
                                                       S32 *width, S32 *height,
                                                       void **destroy_callback_data)
{
	(void)user_callback_data;
	if (!s_gameTexCreate)
		return NULL;

	// UIController::TextureSubstitutionCreateCallback looks this name up in a
	// wstring-keyed map (the one UIScene::registerSubstitutionTexture filled), so it
	// has to be UTF-32 to match. Affects world thumbnails and texture-pack icons.
	t_arena.reset();
	wchar_t *wide = toWide(texture_name, -1, NULL);
	return s_gameTexCreate(s_gameTexUserData, (IggyUTF16 *)wide, width, height,
	                       destroy_callback_data);
}

void IggySetTextureSubstitutionCallbacks(Iggy_TextureSubstitutionCreateCallback *create,
                                         Iggy_TextureSubstitutionDestroyCallback *destroy,
                                         void *user_callback_data)
{
	s_gameTexCreate   = create;
	s_gameTexDestroy  = destroy;
	s_gameTexUserData = user_callback_data;
	// Only the create callback carries a string; destroy takes the opaque cookie the
	// create callback returned, so it needs no marshalling and is passed straight
	// through.
	iggy_vendor_IggySetTextureSubstitutionCallbacks(
		create ? &texSubstitutionTrampoline : NULL, destroy, NULL);
}

}  // extern "C"
