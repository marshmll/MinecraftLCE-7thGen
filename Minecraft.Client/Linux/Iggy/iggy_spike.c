/*
 * iggy_spike.c - standalone check that the vendor Iggy core runs on Linux.
 *
 * This is the gate for the whole Iggy-on-Linux effort (Phase 8, step 1). It does not
 * touch the game: it links only the patched vendor archive plus LinuxRadShim.c and
 * linux_iggy_setjmp.S, then asks Iggy to parse real LCE SWF assets.
 *
 * Nothing here draws anything - drawing needs GDraw, which is step 2. What this
 * proves is that the SWF decoder, the AS3 bytecode loader and the allocator work,
 * which is the part that could not be replaced by hand at any reasonable cost.
 *
 * Build/run: see CMakeLists.txt in this directory, or
 *   cmake --build build --target iggy_spike
 *   ./build/.../iggy_spike --media Minecraft.Client <scene>.swf ...
 *
 * The scene SWFs are not self-contained: they use SWF ImportAssets2 to pull symbols
 * out of shared "skin" libraries (878 import records across the asset set), so the
 * libraries have to be registered first, under the URLs the scenes import them by.
 * --media does that, mirroring UIController::loadSkins().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --render brings up a window and draws through Iggy's own GDraw OpenGL backend
   (gdraw_sdl.c). Without it the harness never touches SDL or GL, so it still runs
   headless as a pure parse/AS3 check. */
#include <SDL2/SDL.h>
#include <GL/gl.h>

/* rrCore.h (pulled in by iggy.h) needs to know this is a 64-bit Linux build; it
   detects __linux__ on its own, but the game defines _LINUX64 everywhere and the
   Iggy headers are shared with the rest of the port, so keep it consistent. */
#ifndef _LINUX64
#define _LINUX64 1
#endif

#include "iggy.h"
#include "gdraw.h"
#include "gdraw_wgl.h"    /* portable despite the name: declares gdraw_GL_* */

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

/* Provided by linux_iggy_setjmp.S. Declared here only so the self-test below can
   exercise them; Iggy reaches them through the redefined symbols in the archive. */
extern int  iggy_fbsd_setjmp(void *buf);
extern void iggy_fbsd_longjmp(void *buf, int value);

/* ------------------------------------------------------------------ */

static long long g_bytes_live;
static long long g_bytes_peak;
static int       g_warnings;

static void *RADLINK spike_alloc(void *ud, size_t size, size_t *size_returned)
{
   void *p;
   (void)ud;
   /* Iggy asks how much it actually got; handing back a smaller number than we
      allocated would make it under-use the block, a larger one would corrupt. */
   p = malloc(size);
   if (size_returned)
      *size_returned = p ? size : 0;
   if (p) {
      g_bytes_live += (long long)size;
      if (g_bytes_live > g_bytes_peak)
         g_bytes_peak = g_bytes_live;
   }
   return p;
}

static void RADLINK spike_free(void *ud, void *ptr)
{
   (void)ud;
   free(ptr);
}

static void RADLINK spike_warning(void *ud, Iggy *player, IggyResult code,
                                  char const *message)
{
   (void)ud; (void)player;
   ++g_warnings;
   fprintf(stderr, "    [iggy warning %d] %s\n", (int)code,
           message ? message : "(no message)");
}

static void RADLINK spike_trace(void *ud, Iggy *player, char const *utf8, S32 len)
{
   (void)ud; (void)player;
   fprintf(stderr, "    [iggy trace] %.*s\n", (int)len, utf8 ? utf8 : "");
}

/* ------------------------------------------------------------------ */

/* The reason linux_iggy_setjmp.S exists is that Iggy's structs reserve 96 bytes for
   a jmp_buf. Check both that a round trip works and that we stayed inside 96 bytes,
   because overrunning it is silent and would show up much later as heap corruption. */
static int test_setjmp(void)
{
   struct {
      unsigned char buf[96];
      unsigned char canary[64];
   } frame;
   int rc;

   memset(&frame, 0xAB, sizeof frame);

   rc = iggy_fbsd_setjmp(frame.buf);
   if (rc == 0)
      iggy_fbsd_longjmp(frame.buf, 42);

   if (rc != 42) {
      fprintf(stderr, "FAIL: setjmp/longjmp round trip returned %d, expected 42\n", rc);
      return 0;
   }
   for (size_t i = 0; i < sizeof frame.canary; ++i) {
      if (frame.canary[i] != 0xAB) {
         fprintf(stderr, "FAIL: setjmp wrote %zu bytes past the 96-byte jmp_buf\n",
                 i + 1);
         return 0;
      }
   }
   /* longjmp(buf, 0) must not appear to return 0. */
   rc = iggy_fbsd_setjmp(frame.buf);
   if (rc == 0)
      iggy_fbsd_longjmp(frame.buf, 0);
   if (rc != 1) {
      fprintf(stderr, "FAIL: longjmp(buf, 0) surfaced as %d, expected 1\n", rc);
      return 0;
   }

   printf("setjmp/longjmp: round trip OK, no overrun past 96 bytes\n");
   return 1;
}

/* ------------------------------------------------------------------ */

static unsigned char *read_file(const char *path, unsigned *out_size)
{
   FILE *f = fopen(path, "rb");
   long size;
   unsigned char *data;

   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
   size = ftell(f);
   if (size <= 0) { fclose(f); return NULL; }
   rewind(f);

   data = (unsigned char *)malloc((size_t)size);
   if (!data) { fclose(f); return NULL; }
   if (fread(data, 1, (size_t)size, f) != (size_t)size) {
      free(data);
      fclose(f);
      return NULL;
   }
   fclose(f);
   *out_size = (unsigned)size;
   return data;
}

/* ------------------------------------------------------------------ *
 *  Shared skin libraries                                             *
 * ------------------------------------------------------------------ *
 *
 * Straight from UIController::loadSkins() (Common/UI/UIController.cpp:421), the
 * 1080p set. Two things about it matter:
 *
 *  - The platform skin must be registered FIRST: the comment in loadSkin() notes
 *    "we need to load the platformskin before the normal skin, as the normal skin
 *    requires some elements from the platform skin".
 *  - It is registered under a *different* URL than its filename. Scenes import
 *    "platformskinHD.swf"; the file backing it is per-platform (skinHDWin.swf,
 *    skinHDOrbis.swf, ...). Linux has no branch for this in loadSkins() yet - the
 *    #ifdef chain there lists every console and Windows64 but not _LINUX64 - so the
 *    real fix belongs in that function. Here we just use the Windows64 asset, which
 *    is what the Linux port already does for the media archive itself.
 *
 * skinHDGraphicsTooltips.swf and skinHDTooltips.swf are listed by loadSkins() but do
 * not exist in this tree; loadSkin() tolerates that via hasArchiveFile(), so a
 * missing file here is reported and skipped rather than treated as a failure.
 */
typedef struct SkinEntry { const char *file; const char *url; } SkinEntry;

/*
 * Both resolution sets, in loadSkins()' order: platform skins first, then the
 * standard-definition set, then the HD set.
 *
 * Registering *both* is not belt-and-braces - it is required. A handful of 1080p
 * scenes import the SD URLs (NewUpdateMessage1080.swf imports "platformskin.swf",
 * HorseInventoryMenu1080.swf imports "skinGraphics.swf"), so with only the HD set
 * registered they fail outright. loadSkins() does the same thing, via the
 * eLibraryFallback_* block it loads under `#ifndef _FINAL_BUILD` with the comment
 * "Load the 720/480 skins so that we have something to fallback on during
 * development".
 */
static const SkinEntry SKINS[] = {
   /* platform skins - registered under a URL that differs from the filename */
   { "skinHDWin.swf",                "platformskinHD.swf"            },
   { "skinWin.swf",                  "platformskin.swf"              },

   /* standard-definition set */
   { "skinGraphics.swf",             "skinGraphics.swf"              },
   { "skinGraphicsHud.swf",          "skinGraphicsHud.swf"           },
   { "skinGraphicsInGame.swf",       "skinGraphicsInGame.swf"        },
   { "skinGraphicsTooltips.swf",     "skinGraphicsTooltips.swf"      },
   { "skinGraphicsLabels.swf",       "skinGraphicsLabels.swf"        },
   { "skinLabels.swf",               "skinLabels.swf"                },
   { "skinInGame.swf",               "skinInGame.swf"                },
   { "skinHud.swf",                  "skinHud.swf"                   },
   { "skinTooltips.swf",             "skinTooltips.swf"              },
   { "skin.swf",                     "skin.swf"                      },

   /* HD set */
   { "skinHDGraphics.swf",           "skinHDGraphics.swf"            },
   { "skinHDGraphicsHud.swf",        "skinHDGraphicsHud.swf"         },
   { "skinHDGraphicsInGame.swf",     "skinHDGraphicsInGame.swf"      },
   { "skinHDGraphicsTooltips.swf",   "skinHDGraphicsTooltips.swf"    },
   { "skinHDGraphicsLabels.swf",     "skinHDGraphicsLabels.swf"      },
   { "skinHDLabels.swf",             "skinHDLabels.swf"              },
   { "skinHDInGame.swf",             "skinHDInGame.swf"              },
   { "skinHDHud.swf",                "skinHDHud.swf"                 },
   { "skinHDTooltips.swf",           "skinHDTooltips.swf"            },
   { "skinHD.swf",                   "skinHD.swf"                    },
};

/* The loose assets live in two directories; the shipped build reads them out of
   MediaWindows64.arc instead, which contains both. */
static const char *MEDIA_SUBDIRS[] = { "Common/Media", "Windows64Media/Media" };

static void ascii_to_utf16(const char *in, unsigned short *out, size_t out_max)
{
   size_t i = 0;
   for (; in[i] && i + 1 < out_max; ++i)
      out[i] = (unsigned short)(unsigned char)in[i];
   out[i] = 0;
}

static int load_skins(const char *media_root)
{
   size_t i;
   int loaded = 0, missing = 0;

   for (i = 0; i < sizeof SKINS / sizeof SKINS[0]; ++i) {
      char path[1024];
      unsigned short url[256];
      unsigned size = 0;
      unsigned char *data = NULL;
      size_t d;
      IggyLibrary lib;

      for (d = 0; d < sizeof MEDIA_SUBDIRS / sizeof MEDIA_SUBDIRS[0] && !data; ++d) {
         snprintf(path, sizeof path, "%s/%s/%s",
                  media_root, MEDIA_SUBDIRS[d], SKINS[i].file);
         data = read_file(path, &size);
      }
      if (!data) {
         printf("  %-30s -- not present, skipped\n", SKINS[i].file);
         ++missing;
         continue;
      }

      ascii_to_utf16(SKINS[i].url, url, sizeof url / sizeof url[0]);
      lib = IggyLibraryCreateFromMemoryUTF16((IggyUTF16 *)url, data, size, NULL);
      free(data);

      if (lib == IGGY_INVALID_LIBRARY) {
         printf("  %-30s FAIL (registered as %s)\n",
                SKINS[i].file, SKINS[i].url);
         return -1;
      }
      printf("  %-30s -> %-28s (%u bytes)\n",
             SKINS[i].file, SKINS[i].url, size);
      ++loaded;
   }

   printf("  %d libraries registered, %d absent\n", loaded, missing);
   return loaded;
}

/* ------------------------------------------------------------------ *
 *  --render : draw a scene through Iggy's GDraw OpenGL backend        *
 * ------------------------------------------------------------------ */

static int      g_render_w = 1920, g_render_h = 1080;
static SDL_Window    *g_window;
static SDL_GLContext  g_glctx;
static GDrawFunctions *g_gdraw;
static int      g_gl_errors;

static void APIENTRY gl_debug_cb(GLenum source, GLenum type, GLuint id,
                                 GLenum severity, GLsizei length,
                                 const GLchar *message, const void *user)
{
   (void)source; (void)id; (void)length; (void)user;
   if (type == GL_DEBUG_TYPE_ERROR_ARB || severity == GL_DEBUG_SEVERITY_HIGH_ARB) {
      ++g_gl_errors;
      fprintf(stderr, "    [GL error] %s\n", message ? message : "?");
   }
}

/* Compatibility profile, deliberately - see the header comment in gdraw_sdl.c. Under
   a core profile GDraw's own capability check fails at glGetString(GL_EXTENSIONS). */
static int render_init(void)
{
   PFNGLDEBUGMESSAGECALLBACKARBPROC debug_cb;

   if (SDL_Init(SDL_INIT_VIDEO) != 0) {
      fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
      return 0;
   }
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
   SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
   SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
   SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
   SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);   /* GDraw masks need stencil */

   g_window = SDL_CreateWindow("Iggy on Linux", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, g_render_w, g_render_h,
                               SDL_WINDOW_OPENGL);
   if (!g_window) {
      fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
      return 0;
   }
   g_glctx = SDL_GL_CreateContext(g_window);
   if (!g_glctx) {
      fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
      return 0;
   }

   printf("GL_VERSION  %s\nGL_RENDERER %s\n",
          (const char *)glGetString(GL_VERSION),
          (const char *)glGetString(GL_RENDERER));

   /* Read GL's own complaints rather than inferring from the picture. */
   debug_cb = (PFNGLDEBUGMESSAGECALLBACKARBPROC)
              SDL_GL_GetProcAddress("glDebugMessageCallbackARB");
   if (debug_cb) {
      glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB);
      debug_cb(gl_debug_cb, NULL);
   } else {
      printf("(no GL_ARB_debug_output; GL errors will not be reported)\n");
   }

   /* Resource limits mirror Windows64_UIController.cpp's, scaled the same way. */
   gdraw_GL_SetResourceLimits(GDRAW_GL_RESOURCE_vertexbuffer, 5000,  16 * 1024 * 1024);
   gdraw_GL_SetResourceLimits(GDRAW_GL_RESOURCE_texture,      5000, 128 * 1024 * 1024);
   gdraw_GL_SetResourceLimits(GDRAW_GL_RESOURCE_rendertarget,   10,  32 * 1024 * 1024);

   g_gdraw = gdraw_GL_CreateContext(g_render_w, g_render_h, 1 /* no MSAA */);
   if (!g_gdraw) {
      fprintf(stderr, "gdraw_GL_CreateContext failed\n");
      return 0;
   }
   IggySetGDraw(g_gdraw);
   printf("gdraw_GL_CreateContext: OK, GDraw installed\n");
   return 1;
}

static void save_screenshot(const char *path)
{
   int w = g_render_w, h = g_render_h, y;
   unsigned char *px = (unsigned char *)malloc((size_t)w * h * 4);
   unsigned char *flipped = (unsigned char *)malloc((size_t)w * h * 4);

   if (!px || !flipped) { free(px); free(flipped); return; }

   glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
   /* GL's origin is bottom-left, image files are top-down. */
   for (y = 0; y < h; ++y)
      memcpy(flipped + (size_t)y * w * 4, px + (size_t)(h - 1 - y) * w * 4,
             (size_t)w * 4);

   if (stbi_write_png(path, w, h, 4, flipped, w * 4))
      printf("wrote %s\n", path);
   else
      fprintf(stderr, "failed to write %s\n", path);

   free(px);
   free(flipped);
}

static int render_scene(const char *path, int frames, const char *shot)
{
   unsigned size = 0;
   unsigned char *data = read_file(path, &size);
   Iggy *player;
   int f;

   if (!data) {
      fprintf(stderr, "cannot read %s\n", path);
      return 0;
   }
   player = IggyPlayerCreateFromMemory(data, size, NULL);
   free(data);
   if (!player) {
      fprintf(stderr, "IggyPlayerCreateFromMemory failed for %s\n", path);
      return 0;
   }

   IggyPlayerSetDisplaySize(player, g_render_w, g_render_h);
   IggyPlayerInitializeAndTickRS(player);

   for (f = 0; f < frames; ++f) {
      SDL_Event ev;
      while (SDL_PollEvent(&ev)) { /* drain, so the WM sees us as responsive */ }

      glViewport(0, 0, g_render_w, g_render_h);
      glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

      while (IggyPlayerReadyToTick(player))
         IggyPlayerTickRS(player);

      /* The two calls that bracket Iggy drawing on every platform: point GDraw at
         the target, draw, then tell it the frame is done so it can flush. */
      gdraw_GL_SetTileOrigin(0, 0, 0 /* default framebuffer */);
      IggyPlayerDraw(player);
      gdraw_GL_NoMoreGDrawThisFrame();

      if (shot && f == frames - 1)
         save_screenshot(shot);

      SDL_GL_SwapWindow(g_window);
   }

   IggyPlayerDestroy(player);
   return 1;
}

/* ------------------------------------------------------------------ */

static int load_one(const char *path)
{
   unsigned size = 0;
   unsigned char *data = read_file(path, &size);
   const char *name = strrchr(path, '/');
   Iggy *player;
   IggyProperties *props;
   int warnings_before = g_warnings;
   int ticks = 0;

   name = name ? name + 1 : path;

   if (!data) {
      printf("  %-34s SKIP (cannot read)\n", name);
      return 1;                       /* not a failure of Iggy */
   }

   /* IggyPlayerCreateFromMemory does the real work we care about: it decompresses
      the SWF, walks its tags, and loads the AS3 bytecode. */
   player = IggyPlayerCreateFromMemory(data, size, NULL);
   if (!player) {
      printf("  %-34s FAIL (%u bytes, player is NULL)\n", name, size);
      free(data);
      return 0;
   }

   props = IggyPlayerProperties(player);

   /* Run the movie for a few frames so the AS3 VM actually executes: frame 1 of a
      4J screen constructs its fourj.* document class. Parsing alone would not touch
      the interpreter. */
   IggyPlayerSetDisplaySize(player, 1920, 1080);
   IggyPlayerInitializeAndTickRS(player);
   while (ticks < 4) {
      IggyPlayerTickRS(player);
      ++ticks;
   }

   printf("  %-34s OK  %5ux%-5u swf v%d  %.1f fps  %u bytes, %d ticks",
          name,
          props ? (unsigned)props->movie_width_in_pixels : 0u,
          props ? (unsigned)props->movie_height_in_pixels : 0u,
          props ? (int)props->swf_major_version_number : 0,
          props ? (double)props->movie_frame_rate_from_file_in_fps : 0.0,
          size, ticks);
   if (g_warnings != warnings_before)
      printf("  (%d warnings)", g_warnings - warnings_before);
   printf("\n");

   IggyPlayerDestroy(player);
   free(data);
   return 1;
}

int main(int argc, char **argv)
{
   IggyAllocator allocator;
   int i, failures = 0, attempted = 0;

   printf("=== Iggy-on-Linux spike ===\n");

   if (!test_setjmp())
      return 1;

   memset(&allocator, 0, sizeof allocator);
   allocator.user_callback_data = NULL;
   allocator.mem_alloc = spike_alloc;
   allocator.mem_free  = spike_free;

   IggyInit(&allocator);
   IggySetWarningCallback(spike_warning, NULL);
   IggySetTraceCallbackUTF8(spike_trace, NULL);
   printf("IggyInit: OK\n");

   /* --media <root> registers the shared skin libraries first. Scene SWFs import
      symbols from them, so without this every scene fails with
      "Attempted to import undefined library ...". */
   for (i = 1; i < argc; ++i) {
      if (strcmp(argv[i], "--media") == 0 && i + 1 < argc) {
         printf("\nRegistering shared skin libraries from %s:\n", argv[i + 1]);
         if (load_skins(argv[i + 1]) < 0) {
            IggyShutdown();
            return 1;
         }
         ++i;
      }
   }

   if (argc < 2) {
      printf("\nNo SWFs given. Pass some, e.g.\n"
             "  %s --media Minecraft.Client \\\n"
             "     Minecraft.Client/Common/Media/MainMenu1080.swf\n", argv[0]);
      IggyShutdown();
      return 0;
   }

   /* --render <scene.swf> [shot.png] draws instead of just parsing. */
   for (i = 1; i < argc; ++i) {
      if (strcmp(argv[i], "--render") == 0 && i + 1 < argc) {
         const char *scene = argv[i + 1];
         const char *shot  = (i + 2 < argc && argv[i + 2][0] != '-') ? argv[i + 2] : NULL;
         int ok;

         printf("\nRendering %s through GDraw-GL:\n", scene);
         if (!render_init()) {
            IggyShutdown();
            return 1;
         }
         ok = render_scene(scene, 30, shot);
         printf("%s, %d GL errors\n", ok ? "drew 30 frames" : "render FAILED",
                g_gl_errors);

         gdraw_GL_DestroyContext();
         IggyShutdown();
         if (g_glctx) SDL_GL_DeleteContext(g_glctx);
         if (g_window) SDL_DestroyWindow(g_window);
         SDL_Quit();
         return (ok && g_gl_errors == 0) ? 0 : 1;
      }
   }

   printf("\nLoading scenes:\n");
   for (i = 1; i < argc; ++i) {
      if (strcmp(argv[i], "--media") == 0) { ++i; continue; }
      ++attempted;
      if (!load_one(argv[i]))
         ++failures;
   }

   printf("\npeak allocation: %.1f MB\n", (double)g_bytes_peak / (1024.0 * 1024.0));
   printf("%d/%d loaded, %d warnings total\n",
          attempted - failures, attempted, g_warnings);

   IggyShutdown();
   return failures ? 1 : 0;
}
