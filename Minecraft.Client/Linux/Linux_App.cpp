#include "Linux_App.h"

#include <cstdio>

// Defined in LinuxExtras/LinuxInput.cpp. Declared rather than included: this file
// deliberately depends on nothing but SDL2, and 4J_Input.h needs the whole
// LinuxTypes.h/LinuxStubs.h Win32-shim chain behind it.
void LinuxInput_NotifyMouseWheel(int iNotches);

CLinuxApp::CLinuxApp()
	: m_window(nullptr)
	, m_glContext(nullptr)
	, m_width(0)
	, m_height(0)
{
}

CLinuxApp::~CLinuxApp()
{
	Shutdown();
}

bool CLinuxApp::Init(int width, int height, const char *title)
{
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
	{
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return false;
	}

	// COMPATIBILITY, not CORE - required by Iggy's GDraw OpenGL backend, which is a
	// 2011-era GL 2.x renderer (see Minecraft.Client/Linux/Iggy/gdraw_sdl.c and
	// .claude/linux-port/IGGY.md). Under a core profile it fails three ways:
	// glGetString(GL_EXTENSIONS) returns NULL and trips its own assert; the
	// GL_ARB_shader_objects entry points it resolves don't exist; and its texture
	// format table uses GL_INTENSITY8/GL_LUMINANCE4_ALPHA4, which core removed.
	//
	// Compatibility is a strict superset of core, so LinuxRender's GLSL 330 +
	// VAO/VBO path is unaffected - it keeps asking for 3.3 and keeps getting it.
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	// GDraw uses the stencil buffer for Flash masks (gdraw_DrawMaskBegin/End). The
	// default is 0 bits, which would silently disable masking rather than fail.
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

	m_window = SDL_CreateWindow(title,
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		width, height,
		SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);

	if (!m_window)
	{
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		return false;
	}

	m_glContext = SDL_GL_CreateContext(m_window);
	if (!m_glContext)
	{
		fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
		return false;
	}

	SDL_GL_SetSwapInterval(1);

	// Requesting an attribute is not getting it. A 0-bit stencil buffer does not fail
	// context creation - GL_STENCIL_TEST simply passes unconditionally, so every Flash
	// mask silently clips nothing and the only evidence is artwork drawn where it
	// should have been cut off. Say so out loud rather than leaving it to be inferred.
	{
		int stencilBits = 0, depthBits = 0;
		SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &stencilBits);
		SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &depthBits);
		fprintf(stderr, "GL context: depth=%d bits, stencil=%d bits\n", depthBits, stencilBits);
		if (stencilBits < 8)
			fprintf(stderr, "  WARNING: fewer than 8 stencil bits - Iggy/GDraw masks will not clip.\n");
	}

	m_width = width;
	m_height = height;

	return true;
}

void CLinuxApp::Shutdown()
{
	if (m_glContext)
	{
		SDL_GL_DeleteContext(m_glContext);
		m_glContext = nullptr;
	}
	if (m_window)
	{
		SDL_DestroyWindow(m_window);
		m_window = nullptr;
	}
	SDL_Quit();
}

bool CLinuxApp::PollEvents()
{
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		switch (event.type)
		{
		case SDL_QUIT:
			return false;

		case SDL_WINDOWEVENT:
			if (event.window.event == SDL_WINDOWEVENT_CLOSE)
				return false;
			if (event.window.event == SDL_WINDOWEVENT_RESIZED)
			{
				m_width = event.window.data1;
				m_height = event.window.data2;
			}
			break;

		case SDL_KEYDOWN:
			// ESCAPE alone must NOT quit. LinuxInput maps it to the pad's
			// pause/start button, so quitting on it meant every attempt to open
			// the pause menu silently exited the main loop instead - which then
			// looked like a mid-game freeze rather than a quit, because the
			// window is not destroyed until MainThreadHandleShutdown() returns.
			//
			// Quit is the window close button (SDL_QUIT / WINDOWEVENT_CLOSE
			// above). SHIFT+ESCAPE is kept as an explicit developer quit, since
			// relative-mouse capture makes the close button awkward to reach.
			if (event.key.keysym.sym == SDLK_ESCAPE && (event.key.keysym.mod & KMOD_SHIFT))
				return false;
			break;

		case SDL_MOUSEWHEEL:
			// Hand the wheel to LinuxInput. It has to be pushed rather than polled:
			// SDL reports the wheel only as an event, and this loop has already taken
			// it off the queue by the time C_4JInput::Tick() runs, so there is no
			// state left for LinuxInput to sample.
			//
			// SDL_MOUSEWHEEL_FLIPPED means the platform already inverted y (natural
			// scrolling), so undo it and always report "positive = away from the user".
			LinuxInput_NotifyMouseWheel(event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
			                            ? -event.wheel.y : event.wheel.y);
			break;

		default:
			break;
		}
	}
	return true;
}

void CLinuxApp::SwapBuffers()
{
	SDL_GL_SwapWindow(m_window);
}
