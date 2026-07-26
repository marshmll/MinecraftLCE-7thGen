#pragma once

#include <SDL2/SDL.h>

// Minimal SDL2-backed window/GL-context wrapper for the Linux client shell.
// Windowing/GL-context/event-pump only; LinuxRender.cpp (Phase 4) is the
// real C4JRender/RenderManager implementation that renders into this
// window. It does not yet drive any Minecraft.Client game logic, since that
// code calls directly into the InputManager/StorageManager globals, which
// have no Linux implementation until Phase 5.
class CLinuxApp
{
public:
	CLinuxApp();
	~CLinuxApp();

	bool Init(int width, int height, const char *title);
	void Shutdown();

	// Pumps the SDL event queue. Returns false once the window should close.
	bool PollEvents();

	void SwapBuffers();

	int GetWidth() const { return m_width; }
	int GetHeight() const { return m_height; }
	SDL_Window *GetWindow() const { return m_window; }

private:
	SDL_Window *m_window;
	SDL_GLContext m_glContext;
	int m_width;
	int m_height;
};
