/*
 * gdraw_sdl.c - Iggy's GDraw rendering backend for Linux, on SDL2 + OpenGL.
 *
 * Iggy splits into a portable core plus a per-platform renderer called GDraw, and RAD
 * shipped every GDraw backend as *source*. This tree already contains a complete
 * OpenGL one:
 *
 *   Minecraft.Client/Windows64/Iggy/gdraw/gdraw_gl_shared.inl    (~2400 lines)
 *   Minecraft.Client/Windows64/Iggy/gdraw/gdraw_gl_shaders.inl
 *
 * It was never built by any project file in the leak - only the D3D11 backend was -
 * and the shared .inl contains no Windows API calls at all. All that was missing is
 * the ~100 lines of platform glue around it, which on Windows is gdraw_wgl.c. This is
 * that file for Linux, and it is deliberately a near-transliteration:
 *
 *   wglGetProcAddress   ->  SDL_GL_GetProcAddress
 *   OutputDebugStringA  ->  fprintf(stderr, ...)
 *   <windows.h>/<gl/gl.h> + bundled "glext.h"  ->  <GL/gl.h> + <GL/glext.h>
 *
 * Everything else - the extension list, the texture format table, the capability
 * checks - is the vendor's, unchanged, so that the two stay comparable.
 *
 * gdraw_wgl.h is reused rather than copied: despite the name it only includes
 * rrCore.h and gdraw.h and declares the portable gdraw_GL_* entry points.
 *
 * REQUIRES A COMPATIBILITY PROFILE. This is not a preference:
 *   - create_context() below calls glGetString(GL_EXTENSIONS), which returns NULL
 *     under a core profile and trips the vendor's own assert.
 *   - It resolves GL_ARB_shader_objects entry points (glCreateShaderObjectARB and
 *     friends), which a core profile does not export.
 *   - The texture format table uses GL_INTENSITY8 / GL_LUMINANCE4_ALPHA4 / GL_RGBA4,
 *     which core removed.
 * Linux_App.cpp therefore asks SDL for SDL_GL_CONTEXT_PROFILE_COMPATIBILITY. That is
 * a superset of core, so LinuxRender's GLSL 330 + VAO/VBO path is unaffected.
 */

#define GDRAW_ASSERTS

#include "iggy.h"
#include "gdraw.h"
#include "gdraw_wgl.h"

#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>      /* getenv, for the MCLINUX_GL_DEBUG switch below */
#include <string.h>
#include <math.h>

#define true 1
#define false 0

///////////////////////////////////////////////////////////////////////////////
//
//  Extensions (we map to GL 2.0 function names for a uniform interface
//  across platforms)
//
//  Verbatim from gdraw_wgl.c. Mesa's <GL/glext.h> declares every PFNGL*ARBPROC
//  and PFNGL*EXTPROC typedef named here, and exports the ARB/EXT aliases under a
//  compatibility profile, so the list needs no Linux-specific edits.
//

#define GDRAW_GL_EXTENSION_LIST \
   /*  identifier                      import                              procname */ \
   /* GL_ARB_multitexture */ \
   GLE(ActiveTexture,                  "ActiveTextureARB",                 ACTIVETEXTUREARB) \
   /* GL_ARB_texture_compression */ \
   GLE(CompressedTexImage2D,           "CompressedTexImage2DARB",          COMPRESSEDTEXIMAGE2DARB) \
   /* GL_ARB_vertex_buffer_object */ \
   GLE(GenBuffers,                     "GenBuffersARB",                    GENBUFFERSARB) \
   GLE(DeleteBuffers,                  "DeleteBuffersARB",                 DELETEBUFFERSARB) \
   GLE(BindBuffer,                     "BindBufferARB",                    BINDBUFFERARB) \
   GLE(BufferData,                     "BufferDataARB",                    BUFFERDATAARB) \
   GLE(MapBuffer,                      "MapBufferARB",                     MAPBUFFERARB) \
   GLE(UnmapBuffer,                    "UnmapBufferARB",                   UNMAPBUFFERARB) \
   GLE(VertexAttribPointer,            "VertexAttribPointerARB",           VERTEXATTRIBPOINTERARB) \
   GLE(EnableVertexAttribArray,        "EnableVertexAttribArrayARB",       ENABLEVERTEXATTRIBARRAYARB) \
   GLE(DisableVertexAttribArray,       "DisableVertexAttribArrayARB",      DISABLEVERTEXATTRIBARRAYARB) \
   /* GL_ARB_shader_objects */ \
   GLE(CreateShader,                   "CreateShaderObjectARB",            CREATESHADEROBJECTARB) \
   GLE(DeleteShader,                   "DeleteObjectARB",                  DELETEOBJECTARB) \
   GLE(ShaderSource,                   "ShaderSourceARB",                  SHADERSOURCEARB) \
   GLE(CompileShader,                  "CompileShaderARB",                 COMPILESHADERARB) \
   GLE(GetShaderiv,                    "GetObjectParameterivARB",          GETOBJECTPARAMETERIVARB) \
   GLE(GetShaderInfoLog,               "GetInfoLogARB",                    GETINFOLOGARB) \
   GLE(CreateProgram,                  "CreateProgramObjectARB",           CREATEPROGRAMOBJECTARB) \
   GLE(DeleteProgram,                  "DeleteObjectARB",                  DELETEOBJECTARB) \
   GLE(AttachShader,                   "AttachObjectARB",                  ATTACHOBJECTARB) \
   GLE(LinkProgram,                    "LinkProgramARB",                   LINKPROGRAMARB) \
   GLE(GetUniformLocation,             "GetUniformLocationARB",            GETUNIFORMLOCATIONARB) \
   GLE(UseProgram,                     "UseProgramObjectARB",              USEPROGRAMOBJECTARB) \
   GLE(GetProgramiv,                   "GetObjectParameterivARB",          GETOBJECTPARAMETERIVARB) \
   GLE(GetProgramInfoLog,              "GetInfoLogARB",                    GETINFOLOGARB) \
   GLE(Uniform1i,                      "Uniform1iARB",                     UNIFORM1IARB) \
   GLE(Uniform4f,                      "Uniform4fARB",                     UNIFORM4FARB) \
   GLE(Uniform4fv,                     "Uniform4fvARB",                    UNIFORM4FVARB) \
   /* Added for Linux: gdraw_gl_shared.inl:1368 calls glUniform1f in the
      GDRAW_vformat_ihud1 path, but gdraw_wgl.c's list omits it. That is a gap in the
      vendor's own GL backend, not a Linux difference - ihud1 is one of 4J's custom
      additions and only the D3D11 backend was kept current with them, which is
      consistent with gdraw_wgl.c not being referenced by any project file in the
      leak. Without this the file does not compile on Windows either. */ \
   GLE(Uniform1f,                      "Uniform1fARB",                     UNIFORM1FARB) \
   /* GL_ARB_vertex_shader */ \
   GLE(BindAttribLocation,             "BindAttribLocationARB",            BINDATTRIBLOCATIONARB) \
   /* GL_EXT_framebuffer_object */ \
   GLE(GenRenderbuffers,               "GenRenderbuffersEXT",              GENRENDERBUFFERSEXT) \
   GLE(DeleteRenderbuffers,            "DeleteRenderbuffersEXT",           DELETERENDERBUFFERSEXT) \
   GLE(BindRenderbuffer,               "BindRenderbufferEXT",              BINDRENDERBUFFEREXT) \
   GLE(RenderbufferStorage,            "RenderbufferStorageEXT",           RENDERBUFFERSTORAGEEXT) \
   GLE(GenFramebuffers,                "GenFramebuffersEXT",               GENFRAMEBUFFERSEXT) \
   GLE(DeleteFramebuffers,             "DeleteFramebuffersEXT",            DELETEFRAMEBUFFERSEXT) \
   GLE(BindFramebuffer,                "BindFramebufferEXT",               BINDFRAMEBUFFEREXT) \
   GLE(CheckFramebufferStatus,         "CheckFramebufferStatusEXT",        CHECKFRAMEBUFFERSTATUSEXT) \
   GLE(FramebufferRenderbuffer,        "FramebufferRenderbufferEXT",       FRAMEBUFFERRENDERBUFFEREXT) \
   GLE(FramebufferTexture2D,           "FramebufferTexture2DEXT",          FRAMEBUFFERTEXTURE2DEXT) \
   GLE(GenerateMipmap,                 "GenerateMipmapEXT",                GENERATEMIPMAPEXT) \
   /* GL_EXT_framebuffer_blit */ \
   GLE(BlitFramebuffer,                "BlitFramebufferEXT",               BLITFRAMEBUFFEREXT) \
   /* GL_EXT_framebuffer_multisample */ \
   GLE(RenderbufferStorageMultisample, "RenderbufferStorageMultisampleEXT",RENDERBUFFERSTORAGEMULTISAMPLEEXT) \
   /* <end> */

#define gdraw_GLx_(id)     gdraw_GL_##id
#define GDRAW_GLx_(id)     GDRAW_GL_##id
#define GDRAW_SHADERS      "gdraw_gl_shaders.inl"

typedef GLhandleARB GLhandle;
typedef gdraw_gl_resourcetype gdraw_resourcetype;

/*
 * Mesa's <GL/gl.h> declares GL through 1.3 as real prototypes; Windows' stops at 1.1.
 * Two names in the list above therefore already exist as functions here and cannot be
 * redeclared as function pointers. Renaming just those two - rather than the whole
 * list - keeps the change minimal, and because these are object-like macros the
 * vendor .inl's own calls get rewritten along with our declarations.
 *
 * Why not simply call Mesa's versions? Because GDraw resolves the ARB entry points
 * against the *current* context via SDL_GL_GetProcAddress, which is what the rest of
 * the list does; mixing the two would work by accident today and is exactly the kind
 * of inconsistency that bites later. If a future <GL/gl.h> adds more of these, the
 * result is a compile error naming the symbol, not silent misbehaviour.
 */
#define glActiveTexture        gdraw_sdl_glActiveTexture
#define glCompressedTexImage2D gdraw_sdl_glCompressedTexImage2D

// Extensions
#define GLE(id, import, procname) static PFNGL##procname##PROC gl##id;
GDRAW_GL_EXTENSION_LIST
#undef GLE

/* Report anything that failed to resolve rather than leaving a NULL function
   pointer to be called later - a null glBufferData crashes a long way from the
   cause, and this is the one place that knows the name. */
static int gdraw_sdl_missing_procs;

static void load_extensions(void)
{
#define GLE(id, import, procname)                                                   \
   gl##id = (PFNGL##procname##PROC) SDL_GL_GetProcAddress("gl" import);              \
   if (!gl##id) {                                                                    \
      ++gdraw_sdl_missing_procs;                                                      \
      fprintf(stderr, "[gdraw_sdl] missing GL entry point gl%s\n", import);          \
   }
   GDRAW_GL_EXTENSION_LIST
#undef GLE
}

static void clear_renderstate_platform_specific(void)
{
   glDisable(GL_ALPHA_TEST);
}

static void error_msg_platform_specific(const char *msg)
{
   fputs(msg, stderr);
}

/*
 * Non-fatal GL error reporting, enabled by setting MCLINUX_GL_DEBUG=1.
 *
 * This exists because GDraw's own error checking is unusable here. Its opengl_check()
 * is wrapped in `#ifdef _DEBUG`, and _DEBUG is deliberately NOT defined for this target
 * (see Iggy/CMakeLists.txt): the _DEBUG path calls break_on_err() -> RR_BREAK(), which
 * on x86-64 is `int $3`, so the first GL error anywhere in GDraw would abort the whole
 * client rather than report itself. The result is that GDraw silently swallows every GL
 * error, which is exactly how a whole class of draws can go missing with a completely
 * clean log.
 *
 * KHR_debug gives us the same information without the breakpoint, so it can be left in
 * and switched on from the environment whenever something is not drawing.
 */
typedef void (APIENTRY *GDrawSdlDebugProc)(GLenum, GLenum, GLuint, GLenum, GLsizei,
                                           const GLchar *, const void *);

static void APIENTRY gdraw_sdl_gl_debug_cb(GLenum source, GLenum type, GLuint id,
                                           GLenum severity, GLsizei length,
                                           const GLchar *message, const void *user)
{
   (void)source; (void)id; (void)length; (void)user;
   if (severity == GL_DEBUG_SEVERITY_NOTIFICATION)
      return;
   fprintf(stderr, "[gdraw_sdl][GL 0x%04x sev 0x%04x] %s\n",
           (unsigned)type, (unsigned)severity, message ? message : "(null)");
   fflush(stderr);
}

static void gdraw_sdl_install_gl_debug(void)
{
   void (*msg_cb)(GDrawSdlDebugProc, const void *);
   const char *env = getenv("MCLINUX_GL_DEBUG");

   if (!env || env[0] == '0')
      return;

   msg_cb = (void (*)(GDrawSdlDebugProc, const void *))
      SDL_GL_GetProcAddress("glDebugMessageCallback");
   if (!msg_cb)
      msg_cb = (void (*)(GDrawSdlDebugProc, const void *))
         SDL_GL_GetProcAddress("glDebugMessageCallbackARB");
   if (!msg_cb) {
      fputs("[gdraw_sdl] MCLINUX_GL_DEBUG set but no glDebugMessageCallback\n", stderr);
      return;
   }

   glEnable(GL_DEBUG_OUTPUT);
   glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);   /* so the message lands on the guilty call */
   msg_cb(gdraw_sdl_gl_debug_cb, NULL);
   fputs("[gdraw_sdl] GL debug output enabled\n", stderr);
}

///////////////////////////////////////////////////////////////////////////////
//
//  Shared code
//

#define GDRAW_MULTISAMPLING
#include "gdraw_gl_shared.inl"

/* ------------------------------------------------------------------ *
 *  Per-draw tracing, enabled by MCLINUX_GDRAW_TRACE=1                *
 * ------------------------------------------------------------------ *
 *
 * Wraps two entries of the GDrawFunctions vtable that gdraw_GL_CreateContext returns,
 * so the vendor .inl stays untouched. This is the only way to see what Iggy actually
 * asks for: GDraw's own instrumentation is compiled out (opengl_check() is _DEBUG-only
 * and _DEBUG cannot be defined here - its error path is RR_BREAK() = int $3), and it
 * swallows GL errors in two places (gdraw_gl_shared.inl:413-424 and :523-542 read
 * glGetError() *before* their own calls and then eat_gl_err(), so a stale error from
 * LinuxRender is misattributed and the evidence destroyed).
 *
 * Trace the texture-creation result and the render state of every textured draw. The
 * discriminator worth watching: a bitmap fill and a *gradient* fill select the identical
 * shader and the identical GDRAW_TEXTURE_normal mode, but a bitmap fill computes its UVs
 * through the texgen matrix (s0_texgen/t0_texgen, vformat v2c4), whereas a directly
 * placed image carries explicit UVs (vformat v2tc2) and never touches texgen. With
 * GL_CLAMP_TO_EDGE and an icon whose border texels are transparent, a wrong texgen scale
 * samples nothing but transparency - which is indistinguishable from "not drawn" in the
 * framebuffer and raises no GL error.
 */
static gdraw_draw_indexed_triangles *gdraw_sdl_real_draw;
static gdraw_make_texture_end       *gdraw_sdl_real_maketex_end;
static int                           gdraw_sdl_trace_draws;

static void RADLINK gdraw_sdl_trace_draw(GDrawRenderState *r, GDrawPrimitive *prim,
                                        GDrawVertexBuffer *buf, GDrawStats *stats)
{
   /* Untextured draws are traced too: a Flash solid/gradient fill - a slider's value
      bar, a focus highlight - carries no texture, so filtering on tex0_mode hides
      exactly the draws you look at when something is drawn in the wrong PLACE rather
      than with the wrong pixels. stencil_set/stencil_test matter for the same reason:
      together with scissor they are the only two ways GDraw clips anything, so an
      overflowing fill is a draw that arrives with all three saying "clip nothing". */
   if (r && prim) {
      fprintf(stderr,
              "[gdraw] draw#%d tex0mode=%u tex0=%p texgen=%u vfmt=%d idx=%d "
              "scissor=%u[%d,%d,%d,%d] sten_set=%u sten_test=%u "
              "test_id=%u set_id=%u wrap0=%u near0=%u "
              "s=[%.4f %.4f %.4f %.4f] t=[%.4f %.4f %.4f %.4f]\n",
              gdraw_sdl_trace_draws++,
              (unsigned)r->tex0_mode, (void *)r->tex[0],
              (unsigned)r->texgen0_enabled, prim->vertex_format, prim->num_indices,
              (unsigned)r->scissor,
              r->scissor_rect.x0, r->scissor_rect.y0,
              r->scissor_rect.x1, r->scissor_rect.y1,
              (unsigned)r->stencil_set, (unsigned)r->stencil_test,
              (unsigned)r->test_id, (unsigned)r->set_id,
              (unsigned)r->wrap0, (unsigned)r->nearest0,
              r->s0_texgen[0], r->s0_texgen[1], r->s0_texgen[2], r->s0_texgen[3],
              r->t0_texgen[0], r->t0_texgen[1], r->t0_texgen[2], r->t0_texgen[3]);
   }
   gdraw_sdl_real_draw(r, prim, buf, stats);
}

static GDrawTexture * RADLINK gdraw_sdl_trace_maketex_end(GDraw_MakeTexture_ProcessingInfo *info,
                                                          GDrawStats *stats)
{
   GDrawTexture *t = gdraw_sdl_real_maketex_end(info, stats);
   if (!t)
      fputs("[gdraw] MakeTextureEnd returned NULL - texture creation FAILED\n", stderr);
   return t;
}

static void gdraw_sdl_install_trace(GDrawFunctions *funcs)
{
   const char *env = getenv("MCLINUX_GDRAW_TRACE");
   if (!funcs || !env || env[0] == '0')
      return;
   gdraw_sdl_real_draw         = funcs->DrawIndexedTriangles;
   gdraw_sdl_real_maketex_end  = funcs->MakeTextureEnd;
   funcs->DrawIndexedTriangles = gdraw_sdl_trace_draw;
   funcs->MakeTextureEnd       = gdraw_sdl_trace_maketex_end;
   fputs("[gdraw] per-draw tracing enabled\n", stderr);
}

///////////////////////////////////////////////////////////////////////////////
//
//  Initialization and platform-specific functionality
//

GDrawFunctions *gdraw_GL_CreateContext(S32 w, S32 h, S32 msaa_samples)
{
   static const TextureFormatDesc tex_formats[] = {
      { IFT_FORMAT_rgba_8888,    1, 1,  4,   GL_RGBA,                            GL_RGBA,               GL_UNSIGNED_BYTE },
      { IFT_FORMAT_rgba_4444_LE, 1, 1,  2,   GL_RGBA4,                           GL_RGBA,               GL_UNSIGNED_SHORT_4_4_4_4 },
      { IFT_FORMAT_rgba_5551_LE, 1, 1,  2,   GL_RGB5_A1,                         GL_RGBA,               GL_UNSIGNED_SHORT_5_5_5_1 },
      { IFT_FORMAT_la_88,        1, 1,  2,   GL_LUMINANCE8_ALPHA8,               GL_LUMINANCE_ALPHA,    GL_UNSIGNED_BYTE },
      { IFT_FORMAT_la_44,        1, 1,  1,   GL_LUMINANCE4_ALPHA4,               GL_LUMINANCE_ALPHA,    GL_UNSIGNED_BYTE },
      { IFT_FORMAT_i_8,          1, 1,  1,   GL_INTENSITY8,                      GL_ALPHA,              GL_UNSIGNED_BYTE },
      { IFT_FORMAT_i_4,          1, 1,  1,   GL_INTENSITY4,                      GL_ALPHA,              GL_UNSIGNED_BYTE },
      { IFT_FORMAT_l_8,          1, 1,  1,   GL_LUMINANCE8,                      GL_LUMINANCE,          GL_UNSIGNED_BYTE },
      { IFT_FORMAT_l_4,          1, 1,  1,   GL_LUMINANCE4,                      GL_LUMINANCE,          GL_UNSIGNED_BYTE },
      { IFT_FORMAT_DXT1,         4, 4,  8,   GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,   0,                     GL_UNSIGNED_BYTE },
      { IFT_FORMAT_DXT3,         4, 4, 16,   GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,   0,                     GL_UNSIGNED_BYTE },
      { IFT_FORMAT_DXT5,         4, 4, 16,   GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,   0,                     GL_UNSIGNED_BYTE },
      { 0,                       0, 0,  0,   0,                                  0,                     0 },
   };

   GDrawFunctions *funcs;
   const char *s;
   GLint n;

   // check for the extensions we need
   s = (const char *) glGetString(GL_EXTENSIONS);
   if (s == NULL) {
      // A core-profile context returns NULL here (glGetString(GL_EXTENSIONS) is
      // deprecated), so this fires for a wrong profile just as readily as for a
      // context that does not exist yet. Say so, because the vendor's assert alone
      // sent us looking in the wrong place.
      fprintf(stderr, "[gdraw_sdl] glGetString(GL_EXTENSIONS) returned NULL.\n"
                      "            Either no GL context is current yet, or this is a "
                      "core profile - GDraw needs a compatibility profile.\n");
      assert(s != NULL);
      return NULL;
   }

   // check for the extensions we won't work without
   if (!hasext(s, "GL_ARB_multitexture") ||
       !hasext(s, "GL_ARB_texture_compression") ||
       !hasext(s, "GL_ARB_texture_mirrored_repeat") ||
       !hasext(s, "GL_ARB_texture_non_power_of_two") || // with caveats - see below!
       !hasext(s, "GL_ARB_vertex_buffer_object") ||
       !hasext(s, "GL_EXT_framebuffer_object") ||
       !hasext(s, "GL_ARB_shader_objects") ||
       !hasext(s, "GL_ARB_vertex_shader") ||
       !hasext(s, "GL_ARB_fragment_shader")) {
      fprintf(stderr, "[gdraw_sdl] a required GL extension is missing; "
                      "GDraw cannot start.\n");
      return NULL;
   }

   // if user requests multisampling and HW doesn't support it, bail
   if (!hasext(s, "GL_EXT_framebuffer_multisample") && msaa_samples > 1)
      return NULL;

   load_extensions();
   if (gdraw_sdl_missing_procs) {
      fprintf(stderr, "[gdraw_sdl] %d GL entry points could not be resolved; "
                      "refusing to start GDraw.\n", gdraw_sdl_missing_procs);
      return NULL;
   }

   funcs = create_context(w, h);
   if (!funcs)
      return NULL;

   gdraw->tex_formats = tex_formats;

   // check for optional extensions
   gdraw->has_mapbuffer = true; // part of core VBO extension on regular GL
   gdraw->has_depth24 = true;   // we just assume.
   gdraw->has_texture_max_level = true; // core on regular GL

   if (hasext(s, "GL_EXT_packed_depth_stencil"))      gdraw->has_packed_depth_stencil = true;

   // we require ARB_texture_non_power_of_two - on actual HW, this may either give us
   // "full" non-power-of-two support, or "conditional" non-power-of-two (wrap mode must
   // be CLAMP_TO_EDGE, no mipmaps). figure out which it is using this heuristic by
   // Unity's Aras Pranckevicius (thanks!):
   //   http://www.aras-p.info/blog/2012/10/17/non-power-of-two-textures/
   //
   // we use the second heuristic (texture size <8192 for cards without full NPOT support)
   // since we don't otherwise use ARB_fragment_program and don't want to create a program
   // just to be able to query MAX_PROGRAM_NATIVE_INSTRUCTIONS_ARB!
   glGetIntegerv(GL_MAX_TEXTURE_SIZE, &n);
   gdraw->has_conditional_non_power_of_two = n < 8192;

   // clamp number of multisampling levels to max supported
   if (msaa_samples > 1) {
      glGetIntegerv(GL_MAX_SAMPLES, &n);
      gdraw->multisampling = RR_MIN(msaa_samples, n);
   }

   opengl_check();

   gdraw_sdl_install_gl_debug();
   gdraw_sdl_install_trace(funcs);

   return funcs;
}

/*
 * The custom-draw pair, matching gdraw_d3d1x_shared.inl's gdraw_D3D1X_BeginCustomDraw_4J
 * and gdraw_D3D1X_CalculateCustomDraw_4J (4J additions, not vendor API):
 * Begin = clear render state + compute the matrix, Calculate = matrix only. The game needs
 * them separated because UIScene::customDrawSlotControl caches a CustomDrawData per
 * inventory slot and replays it later, so the matrix has to be filled at calculate-time.
 *
 * NOTE THE FINAL ARGUMENT: out_col_major must be 0, i.e. ROW-major.
 *
 * The vendor's own gdraw_GL_BeginCustomDraw passes 1 (gdraw_gl_shared.inl:1885) and is the
 * only caller in the whole tree that does - D3D11/D3D10/D3D9, Orbis, PS3 and PSVita all
 * pass 0. Row-major is therefore the contract that the shared consumer,
 * UIController::setupCustomDrawMatrices, is written against: it reads mat[3] and mat[7]
 * for the translation. Under column-major those two elements are a hard 0.0f, so the
 * translation collapses to exactly (screenWidth/2, screenHeight/2) and every custom-draw
 * region - every item icon, every player-skin preview - stacks in the middle of the
 * screen at the right size. (The scale terms mat[0]/mat[5] are on the diagonal and so are
 * layout-invariant, which is why only the position looked wrong.)
 *
 * This is also why gdraw_GL_BeginCustomDraw_4J below has to exist rather than the
 * UIController calling the vendor gdraw_GL_BeginCustomDraw: that function would recompute
 * the matrix column-major and undo the fix.
 */
void gdraw_GL_CalculateCustomDraw(IggyCustomDrawCallbackRegion *region, F32 *matrix)
{
   gdraw_GetObjectSpaceMatrix(matrix, region->o2w, gdraw->projection, depth_from_id(0), 0);
}

void gdraw_GL_BeginCustomDraw_4J(IggyCustomDrawCallbackRegion *region, F32 *matrix)
{
   clear_renderstate();
   gdraw_GetObjectSpaceMatrix(matrix, region->o2w, gdraw->projection, depth_from_id(0), 0);
}

/*
 * Re-apply GDraw's own viewport, so the game's renderer can draw 3D inside a movie.
 *
 * This is the GL twin of gdraw_d3d11.cpp's gdraw_D3D11_setViewport_4J (a 4J addition,
 * not vendor API) and of gdraw_orbis's equivalent - and like both of them the body is
 * just a call to the backend's own static set_viewport(), which is in scope here
 * because gdraw_gl_shared.inl is included above.
 */
void gdraw_GL_setViewport_4J(void)
{
   set_viewport();
}

/* See gdraw_sdl.h for why these exist and why they live here rather than in the
   UIController. */
static struct
{
   GLint program;
   GLint vao;
   GLint array_buffer;
   GLint element_buffer;
   GLint active_texture;
   int   valid;
} gdraw_sdl_host_state;

/* Vertex-array objects are GL 3.0 and have no ARB entry in GDraw's extension list
   above (GDraw itself predates them and does not use one). LinuxRender does bind a
   VAO, so restoring it matters; resolve the one function lazily. */
static PFNGLBINDVERTEXARRAYPROC gdraw_sdl_BindVertexArray;
static int gdraw_sdl_vao_checked;

static void gdraw_sdl_resolve_vao(void)
{
   if (gdraw_sdl_vao_checked)
      return;
   gdraw_sdl_vao_checked = 1;
   gdraw_sdl_BindVertexArray =
      (PFNGLBINDVERTEXARRAYPROC) SDL_GL_GetProcAddress("glBindVertexArray");
}

void gdraw_GL_SaveHostState(void)
{
   gdraw_sdl_resolve_vao();

   glGetIntegerv(GL_CURRENT_PROGRAM,              &gdraw_sdl_host_state.program);
   glGetIntegerv(GL_ARRAY_BUFFER_BINDING,         &gdraw_sdl_host_state.array_buffer);
   glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &gdraw_sdl_host_state.element_buffer);
   glGetIntegerv(GL_ACTIVE_TEXTURE,               &gdraw_sdl_host_state.active_texture);
   gdraw_sdl_host_state.vao = 0;
   if (gdraw_sdl_BindVertexArray)
      glGetIntegerv(GL_VERTEX_ARRAY_BINDING,      &gdraw_sdl_host_state.vao);

   glPushAttrib(GL_ALL_ATTRIB_BITS);
   gdraw_sdl_host_state.valid = 1;
}

void gdraw_GL_RestoreHostState(void)
{
   if (!gdraw_sdl_host_state.valid)
      return;                 /* unbalanced call - do nothing rather than pop garbage */
   gdraw_sdl_host_state.valid = 0;

   glPopAttrib();
   /* glActiveTexture and glBindBuffer are the ARB pointers from the extension list;
      glUseProgram is glUseProgramObjectARB, which is the same entry point (GLhandleARB
      is a GLuint here). */
   glActiveTexture((GLenum)gdraw_sdl_host_state.active_texture);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)gdraw_sdl_host_state.element_buffer);
   glBindBuffer(GL_ARRAY_BUFFER,         (GLuint)gdraw_sdl_host_state.array_buffer);
   if (gdraw_sdl_BindVertexArray)
      gdraw_sdl_BindVertexArray((GLuint)gdraw_sdl_host_state.vao);
   glUseProgram((GLhandle)gdraw_sdl_host_state.program);
}
