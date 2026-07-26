// LinuxInput.cpp - C_4JInput implementation backed by SDL2 keyboard + game
// controller state. 4J_Input.h has no D3D/Win32-only types in its surface
// (only LPVOID/WCHAR/DWORD/etc, already resolved via LinuxStubs.h/LinuxTypes.h),
// so unlike 4J_Render.h/4J_Storage.h this reuses Windows64's header unchanged -
// see Minecraft.World/stdafx.h's _LINUX64 branch.
//
// Pad 0 always mixes in keyboard state (WASD + arrows + space/enter/escape),
// so the game is controllable with no physical controller attached. Pads 0-3
// additionally read from up to 4 SDL_GameControllers if present.

#include "LinuxTypes.h"
#include "LinuxStubs.h"
#include "../../Windows64/4JLibs/inc/4J_Input.h"

#include <SDL2/SDL.h>

#include <cstring>
#include <cmath>

namespace
{
	const int MAX_PADS = 4;
	const int MAX_ACTIONS = 256;
	const int MAX_MAPS = 3; // MAP_STYLE_0/1/2
	// Sized so that every possible unsigned char action index is in range, which is
	// why the accessors below only need to bounds-check the map index.
	static_assert(MAX_ACTIONS >= 256, "action index is an unsigned char, so MAX_ACTIONS must cover 0..255");

	float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

	// Deadzone/range applied to analog sticks, set via SetDeadzoneAndMovementRange.
	// Miles/4J's units are unspecified beyond "matches XINPUT's ranges" (0-32767);
	// treated as such since every call site in the leak uses XINPUT-shaped values.
	struct AnalogRange
	{
		unsigned int deadzone = 7849; // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
		unsigned int movementRangeMax = 32767;
	};

	struct PadState
	{
		SDL_GameController *controller = nullptr;
		unsigned int currentButtons = 0;
		unsigned int previousButtons = 0;
		unsigned char mapStyle = MAP_STYLE_0;
		float sensitivity = 1.0f;
		bool menuDisplayed = false;
		double idleSince = 0.0;
		double lastInputTime = 0.0;

		// SouthPaw/remap tables: identity by default (AXIS_MAP_LX -> AXIS_MAP_LX, etc).
		unsigned int axisMap[4] = { AXIS_MAP_LX, AXIS_MAP_LY, AXIS_MAP_RX, AXIS_MAP_RY };
		unsigned int triggerMap[2] = { TRIGGER_MAP_0, TRIGGER_MAP_1 };

		// Pad 0 only: smoothed mouse velocity in pixels/second, +X right, +Y up.
		//
		// Velocity rather than a raw per-frame delta because that is what the
		// interface being emulated actually means: ReadAxis() is a "where is the
		// stick right now" sample that several callers take, at different rates,
		// any number of times per tick. Anything consumed-on-read breaks those
		// callers, and a raw per-frame delta is meaningless to a 20Hz reader when
		// Tick() runs at ~100Hz.
		float mouseVelX = 0.0f;
		float mouseVelY = 0.0f;
	};

	PadState g_pads[MAX_PADS];
	unsigned int g_joypadMap[MAX_MAPS][MAX_ACTIONS] = {};
	AnalogRange g_analogRange;

	// Mouse-look tuning. MOUSE_LOOK_GAIN is in stick-units^2 per (pixel/second):
	// Input.cpp:102 turns a stick value into rotation as `tx * abs(tx) * 50` per
	// 20Hz tick, i.e. degrees/sec = 1000 * tx^2. Feeding sqrt() of the velocity
	// through that square therefore makes the turn rate LINEAR in mouse speed,
	// which is what "natural" means for a mouse - passing velocity in directly
	// gave a quadratic curve that felt dead when moving slowly and uncontrollable
	// when moving fast.
	//
	// Because the square cancels, the resulting turn rate is simply
	// degrees/sec = 1000 * velocity * GAIN, i.e. GAIN * 1000 degrees per pixel.
	// 0.0008 gives 0.8 deg/pixel. This is the one number to change if look speed
	// needs adjusting.
	const float MOUSE_LOOK_GAIN = 0.0008f;
	// Light smoothing over Tick()'s ~100Hz sampling so a 20Hz reader sees a
	// stable value rather than whichever single frame it happened to land on.
	const float MOUSE_VEL_SMOOTHING = 0.65f;
	double g_lastMouseSampleTime = 0.0;

	// Velocity (px/s) -> stick deflection, pre-compensated for the quadratic above.
	float MouseVelToStick(float vel)
	{
		float mag = std::sqrt(std::fabs(vel) * MOUSE_LOOK_GAIN);
		if (mag > 1.0f) mag = 1.0f;
		return vel < 0.0f ? -mag : mag;
	}

	float g_repeatDelaySecs = 0.3f;
	float g_repeatRateSecs = 0.2f;
	bool g_initialised = false;

	double NowSeconds() { return SDL_GetTicks() / 1000.0; }

	// Raw physical -> _360_JOY_BUTTON_* bitmask (SDL_GameController + keyboard
	// for pad 0). This is the "controller state", independent of the game's
	// action maps - GetValue/ButtonDown etc. resolve action -> bitmask via
	// g_joypadMap, then test against this.
	unsigned int ReadPhysicalButtons(int iPad)
	{
		unsigned int mask = 0;
		PadState &pad = g_pads[iPad];

		if (pad.controller)
		{
			struct { SDL_GameControllerButton sdl; unsigned int bit; } buttons[] = {
				{ SDL_CONTROLLER_BUTTON_A, _360_JOY_BUTTON_A },
				{ SDL_CONTROLLER_BUTTON_B, _360_JOY_BUTTON_B },
				{ SDL_CONTROLLER_BUTTON_X, _360_JOY_BUTTON_X },
				{ SDL_CONTROLLER_BUTTON_Y, _360_JOY_BUTTON_Y },
				{ SDL_CONTROLLER_BUTTON_START, _360_JOY_BUTTON_START },
				{ SDL_CONTROLLER_BUTTON_BACK, _360_JOY_BUTTON_BACK },
				{ SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, _360_JOY_BUTTON_RB },
				{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER, _360_JOY_BUTTON_LB },
				{ SDL_CONTROLLER_BUTTON_RIGHTSTICK, _360_JOY_BUTTON_RTHUMB },
				{ SDL_CONTROLLER_BUTTON_LEFTSTICK, _360_JOY_BUTTON_LTHUMB },
				{ SDL_CONTROLLER_BUTTON_DPAD_UP, _360_JOY_BUTTON_DPAD_UP },
				{ SDL_CONTROLLER_BUTTON_DPAD_DOWN, _360_JOY_BUTTON_DPAD_DOWN },
				{ SDL_CONTROLLER_BUTTON_DPAD_LEFT, _360_JOY_BUTTON_DPAD_LEFT },
				{ SDL_CONTROLLER_BUTTON_DPAD_RIGHT, _360_JOY_BUTTON_DPAD_RIGHT },
			};
			for (auto &b : buttons)
			{
				if (SDL_GameControllerGetButton(pad.controller, b.sdl))
					mask |= b.bit;
			}

			Sint16 lx = SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_LEFTX);
			Sint16 ly = SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_LEFTY);
			Sint16 rx = SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_RIGHTX);
			Sint16 ry = SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_RIGHTY);
			if (lx > (Sint16)g_analogRange.deadzone) mask |= _360_JOY_BUTTON_LSTICK_RIGHT;
			if (lx < -(Sint16)g_analogRange.deadzone) mask |= _360_JOY_BUTTON_LSTICK_LEFT;
			if (ly < -(Sint16)g_analogRange.deadzone) mask |= _360_JOY_BUTTON_LSTICK_UP;
			if (ly > (Sint16)g_analogRange.deadzone) mask |= _360_JOY_BUTTON_LSTICK_DOWN;
			if (rx > (Sint16)g_analogRange.deadzone) mask |= _360_JOY_BUTTON_RSTICK_RIGHT;
			if (rx < -(Sint16)g_analogRange.deadzone) mask |= _360_JOY_BUTTON_RSTICK_LEFT;
			if (ry < -(Sint16)g_analogRange.deadzone) mask |= _360_JOY_BUTTON_RSTICK_UP;
			if (ry > (Sint16)g_analogRange.deadzone) mask |= _360_JOY_BUTTON_RSTICK_DOWN;

			if (SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 4096)
				mask |= _360_JOY_BUTTON_LT;
			if (SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 4096)
				mask |= _360_JOY_BUTTON_RT;
		}

		if (iPad == 0)
		{
			const Uint8 *keys = SDL_GetKeyboardState(nullptr);
			if (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_SPACE]) mask |= _360_JOY_BUTTON_A;
			if (keys[SDL_SCANCODE_BACKSPACE] || keys[SDL_SCANCODE_ESCAPE]) mask |= _360_JOY_BUTTON_B;
			if (keys[SDL_SCANCODE_Q]) mask |= _360_JOY_BUTTON_X;
			if (keys[SDL_SCANCODE_TAB]) mask |= _360_JOY_BUTTON_Y;
			if (keys[SDL_SCANCODE_ESCAPE]) mask |= _360_JOY_BUTTON_START;
			// Movement keys raise the LEFT STICK bits ONLY - never the D-pad.
			//
			// The D-pad is a separate control with its own bindings, and in a non-final
			// build those are debug functions: Minecraft.cpp:1456-1465 remaps
			// DPAD_UP/DOWN/LEFT/RIGHT to FLY_TOGGLE / RENDER_DEBUG / SPAWN_CREEPER /
			// CHANGE_SKIN whenever app.GetUseDPadForDebug() is set, which it is
			// (Consoles_App.cpp:210). Raising both bit groups from one key therefore
			// made W toggle creative flight, S toggle the debug overlay, A spawn a
			// creeper and D change skin, on top of moving - which is what the
			// "WASD glitched", "S shows debug text" and the long-standing spurious
			// "flying is on" reports all were.
			//
			// Menu navigation is unaffected: DefineActions() maps ACTION_MENU_UP and
			// friends to (DPAD_x | LSTICK_x), so the stick bits alone still drive menus.
			if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W]) mask |= _360_JOY_BUTTON_LSTICK_UP;
			if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S]) mask |= _360_JOY_BUTTON_LSTICK_DOWN;
			if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A]) mask |= _360_JOY_BUTTON_LSTICK_LEFT;
			if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) mask |= _360_JOY_BUTTON_LSTICK_RIGHT;
			// SHIFT = sneak, and "descend" while flying. DefineActions() maps
			// MINECRAFT_ACTION_SNEAK_TOGGLE -> RTHUMB, and Minecraft.cpp:1443-1450
			// already reads that action with ButtonDown() (held) when flying and
			// ButtonPressed() (edge toggle) when not - so a held SHIFT descends
			// (LocalPlayer.cpp:400, yd -= 0.15 + 0.42) and a tap toggles sneak on
			// the ground, with no game-side change needed.
			if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) mask |= _360_JOY_BUTTON_RTHUMB;
			// Keyboard alternate for break, alongside left mouse below.
			if (keys[SDL_SCANCODE_LCTRL]) mask |= _360_JOY_BUTTON_RT;

			// Mouse: LEFT = break, RIGHT = place/use, the desktop Minecraft
			// convention. Which trigger that means is not guessable from the names -
			// MINECRAFT_ACTION_ACTION is the break/attack action (it drives
			// handleMouseClick(0) / handleMouseDown(0, ...) at Minecraft.cpp:3204-3225,
			// i.e. mouse button 0) and MINECRAFT_ACTION_USE is place/use. Per
			// DefineActions() (Linux_Minecraft.cpp) ACTION -> RT and USE -> LT, so
			// left mouse must raise RT and right mouse LT. These were previously the
			// other way round, which made left-click place and right-click break.
			Uint32 mouseButtons = SDL_GetMouseState(nullptr, nullptr);
			if (mouseButtons & SDL_BUTTON(SDL_BUTTON_LEFT)) mask |= _360_JOY_BUTTON_RT;
			if (mouseButtons & SDL_BUTTON(SDL_BUTTON_RIGHT)) mask |= _360_JOY_BUTTON_LT;
		}

		return mask;
	}
}

void C_4JInput::Initialise(int iInputStateC, unsigned char ucMapC, unsigned char ucActionC, unsigned char ucMenuActionC)
{
	(void)iInputStateC; (void)ucMapC; (void)ucActionC; (void)ucMenuActionC;

	if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
	{
		fprintf(stderr, "InputManager: SDL_InitSubSystem(GAMECONTROLLER) failed: %s\n", SDL_GetError());
	}

	// Confines/hides the cursor and switches SDL_GetRelativeMouseState() to
	// report raw deltas each frame instead of absolute position - the same
	// "mouse becomes the camera" capture every desktop FPS/Minecraft build
	// uses. See ReadAxis()'s pad-0 RX/RY handling below for where the delta
	// this produces actually gets consumed.
	SDL_SetRelativeMouseMode(SDL_TRUE);

	for (int i = 0; i < MAX_PADS; i++)
	{
		g_pads[i] = PadState();
		if (SDL_IsGameController(i))
			g_pads[i].controller = SDL_GameControllerOpen(i);
	}

	memset(g_joypadMap, 0, sizeof(g_joypadMap));
	g_initialised = true;
}

void C_4JInput::Tick(void)
{
	// Pick up controllers connected/disconnected since Initialise/last Tick.
	for (int i = 0; i < MAX_PADS; i++)
	{
		if (!g_pads[i].controller && SDL_IsGameController(i))
			g_pads[i].controller = SDL_GameControllerOpen(i);
		else if (g_pads[i].controller && !SDL_GameControllerGetAttached(g_pads[i].controller))
		{
			SDL_GameControllerClose(g_pads[i].controller);
			g_pads[i].controller = nullptr;
		}
	}

	for (int i = 0; i < MAX_PADS; i++)
	{
		g_pads[i].previousButtons = g_pads[i].currentButtons;
		g_pads[i].currentButtons = ReadPhysicalButtons(i);
		if (g_pads[i].currentButtons != 0)
			g_pads[i].lastInputTime = NowSeconds();
	}

	// Mouse-look: convert this frame's delta into a smoothed velocity that
	// ReadAxis() can sample without consuming. SDL_GetRelativeMouseState() zeroes
	// its own delta on every call, so it must be read exactly once per frame -
	// here - and nowhere else.
	//
	// Y is negated because SDL reports +dy for "mouse moved down" while this
	// interface is up-positive (see ReadAxis).
	int mouseDx = 0, mouseDy = 0;
	SDL_GetRelativeMouseState(&mouseDx, &mouseDy);

	double nowSecs = NowSeconds();
	float dt = (float)(nowSecs - g_lastMouseSampleTime);
	g_lastMouseSampleTime = nowSecs;
	if (dt < 0.001f) dt = 0.001f;      // guard the first call and any clock stall
	if (dt > 0.1f) dt = 0.1f;

	float instX = (float)mouseDx / dt;
	float instY = (float)-mouseDy / dt;
	g_pads[0].mouseVelX += (instX - g_pads[0].mouseVelX) * MOUSE_VEL_SMOOTHING;
	g_pads[0].mouseVelY += (instY - g_pads[0].mouseVelY) * MOUSE_VEL_SMOOTHING;

	// Snap fully to rest once the mouse stops, so Input.cpp's rReset latch (which
	// needs to observe an exactly-zero look axis once) can never be starved by an
	// exponential tail that only asymptotically approaches zero.
	if (mouseDx == 0 && mouseDy == 0 &&
	    std::fabs(g_pads[0].mouseVelX) < 1.0f && std::fabs(g_pads[0].mouseVelY) < 1.0f)
	{
		g_pads[0].mouseVelX = 0.0f;
		g_pads[0].mouseVelY = 0.0f;
	}

	if (mouseDx != 0 || mouseDy != 0)
		g_pads[0].lastInputTime = nowSecs;
}

void C_4JInput::SetDeadzoneAndMovementRange(unsigned int uiDeadzone, unsigned int uiMovementRangeMax)
{
	g_analogRange.deadzone = uiDeadzone;
	g_analogRange.movementRangeMax = uiMovementRangeMax;
}

void C_4JInput::SetGameJoypadMaps(unsigned char ucMap, unsigned char ucAction, unsigned int uiActionVal)
{
	// ucAction needs no bound check - see the static_assert on MAX_ACTIONS.
	if (ucMap < MAX_MAPS)
		g_joypadMap[ucMap][ucAction] = uiActionVal;
}

unsigned int C_4JInput::GetGameJoypadMaps(unsigned char ucMap, unsigned char ucAction)
{
	if (ucMap < MAX_MAPS)
		return g_joypadMap[ucMap][ucAction];
	return 0;
}

void C_4JInput::SetJoypadMapVal(int iPad, unsigned char ucMap)
{
	if (iPad >= 0 && iPad < MAX_PADS)
		g_pads[iPad].mapStyle = ucMap;
}

unsigned char C_4JInput::GetJoypadMapVal(int iPad)
{
	if (iPad >= 0 && iPad < MAX_PADS)
		return g_pads[iPad].mapStyle;
	return MAP_STYLE_0;
}

void C_4JInput::SetJoypadSensitivity(int iPad, float fSensitivity)
{
	if (iPad >= 0 && iPad < MAX_PADS)
		g_pads[iPad].sensitivity = fSensitivity;
}

unsigned int C_4JInput::GetValue(int iPad, unsigned char ucAction, bool bRepeat)
{
	if (iPad < 0 || iPad >= MAX_PADS) return 0;
	unsigned int actionMask = g_joypadMap[g_pads[iPad].mapStyle][ucAction];
	bool down = (g_pads[iPad].currentButtons & actionMask) != 0;
	if (!down) return 0;

	if (!bRepeat)
		return 1;

	// Simple repeat: fire once immediately, then at g_repeatRateSecs intervals
	// after g_repeatDelaySecs has elapsed since the button first went down.
	bool wasDown = (g_pads[iPad].previousButtons & actionMask) != 0;
	if (!wasDown)
		return 1;
	double held = NowSeconds() - g_pads[iPad].lastInputTime;
	if (held < g_repeatDelaySecs)
		return 0;
	return 1;
}

bool C_4JInput::ButtonPressed(int iPad, unsigned char ucAction)
{
	if (iPad < 0 || iPad >= MAX_PADS) return false;
	if (ucAction == 255)
		return g_pads[iPad].currentButtons != 0 && g_pads[iPad].previousButtons == 0;
	unsigned int actionMask = g_joypadMap[g_pads[iPad].mapStyle][ucAction];
	return (g_pads[iPad].currentButtons & actionMask) != 0 && (g_pads[iPad].previousButtons & actionMask) == 0;
}

bool C_4JInput::ButtonReleased(int iPad, unsigned char ucAction)
{
	if (iPad < 0 || iPad >= MAX_PADS) return false;
	unsigned int actionMask = g_joypadMap[g_pads[iPad].mapStyle][ucAction];
	return (g_pads[iPad].currentButtons & actionMask) == 0 && (g_pads[iPad].previousButtons & actionMask) != 0;
}

bool C_4JInput::ButtonDown(int iPad, unsigned char ucAction)
{
	if (iPad < 0 || iPad >= MAX_PADS) return false;
	if (ucAction == 255)
		return g_pads[iPad].currentButtons != 0;
	unsigned int actionMask = g_joypadMap[g_pads[iPad].mapStyle][ucAction];
	return (g_pads[iPad].currentButtons & actionMask) != 0;
}

void C_4JInput::SetJoypadStickAxisMap(int iPad, unsigned int uiFrom, unsigned int uiTo)
{
	if (iPad >= 0 && iPad < MAX_PADS && uiFrom < 4)
		g_pads[iPad].axisMap[uiFrom] = uiTo;
}

void C_4JInput::SetJoypadStickTriggerMap(int iPad, unsigned int uiFrom, unsigned int uiTo)
{
	if (iPad >= 0 && iPad < MAX_PADS && uiFrom < 2)
		g_pads[iPad].triggerMap[uiFrom] = uiTo;
}

void C_4JInput::SetKeyRepeatRate(float fRepeatDelaySecs, float fRepeatRateSecs)
{
	g_repeatDelaySecs = fRepeatDelaySecs;
	g_repeatRateSecs = fRepeatRateSecs;
}

void C_4JInput::SetDebugSequence(const char *chSequenceA, int (*Func)(LPVOID), LPVOID lpParam)
{
	// Xbox 360 debug cheat-code sequence (e.g. "LRLRYYY" on the d-pad) used to
	// unlock debug menus during development. Not wired up on Linux - no
	// equivalent input ceremony is needed since debug builds can just use a
	// command-line flag or #ifdef instead. Parameters intentionally unused.
	(void)chSequenceA; (void)Func; (void)lpParam;
}

FLOAT C_4JInput::GetIdleSeconds(int iPad)
{
	if (iPad < 0 || iPad >= MAX_PADS) return 0.0f;
	return (float)(NowSeconds() - g_pads[iPad].lastInputTime);
}

bool C_4JInput::IsPadConnected(int iPad)
{
	if (iPad < 0 || iPad >= MAX_PADS) return false;
	if (iPad == 0) return true; // keyboard is always available on pad 0
	return g_pads[iPad].controller != nullptr;
}

static float ReadAxis(int iPad, unsigned int axisSlot, bool bCheckMenuDisplay)
{
	if (iPad < 0 || iPad >= 4) return 0.0f;
	PadState &pad = g_pads[iPad];
	if (bCheckMenuDisplay && pad.menuDisplayed) return 0.0f;

	// Pad 0 has no physical controller in the common desktop case (keyboard+
	// mouse only). Movement (Input.cpp) reads GetJoypadStick_LX/LY directly
	// rather than GetValue(MINECRAFT_ACTION_FORWARD/...) - the digital
	// LSTICK_*/RSTICK_* bits ReadPhysicalButtons sets from WASD are never
	// otherwise consumed as movement, so without this, keyboard movement
	// silently does nothing even though the buttons register correctly.
	// Synthesize a full-deflection analog value from those same digital
	// bits for LX/LY; RX/RY (camera look) instead use the mouse-look delta
	// Tick() captured, since keyboard has no natural look-axis equivalent.
	if (!pad.controller)
	{
		unsigned int physical = pad.axisMap[axisSlot];

		// Mouse look. This MUST be a pure read with no side effects: RX/RY are
		// sampled by more than one caller. Minecraft.cpp:1735's idle check reads
		// them every frame, and it is a short-circuiting || chain starting with
		// LY - so consuming the value here made look input work only while a
		// movement key happened to be held (LY != 0 short-circuited the chain
		// before it could eat the mouse motion). Tick() keeps a smoothed velocity
		// instead, which any number of readers can sample harmlessly.
		if (iPad == 0 && physical == AXIS_MAP_RX) return MouseVelToStick(pad.mouseVelX);
		if (iPad == 0 && physical == AXIS_MAP_RY) return MouseVelToStick(pad.mouseVelY);

		if (iPad == 0 && physical == AXIS_MAP_LX)
		{
			if (pad.currentButtons & _360_JOY_BUTTON_LSTICK_RIGHT) return 1.0f;
			if (pad.currentButtons & _360_JOY_BUTTON_LSTICK_LEFT) return -1.0f;
		}
		if (iPad == 0 && physical == AXIS_MAP_LY)
		{
			// Forward is POSITIVE here. Input.cpp:39 assigns ya = LY unmodified and
			// treats positive as forward, so returning the raw XINPUT convention
			// (up = negative) made W walk backwards and S forwards.
			if (pad.currentButtons & _360_JOY_BUTTON_LSTICK_UP) return 1.0f;
			if (pad.currentButtons & _360_JOY_BUTTON_LSTICK_DOWN) return -1.0f;
		}
		return 0.0f;
	}

	// axisMap[] lets SetJoypadStickAxisMap remap which physical stick feeds
	// AXIS_MAP_LX/LY/RX/RY (SouthPaw control scheme swaps left/right sticks).
	unsigned int physical = pad.axisMap[axisSlot];
	SDL_GameControllerAxis sdlAxis = SDL_CONTROLLER_AXIS_LEFTX;
	switch (physical)
	{
	case AXIS_MAP_LX: sdlAxis = SDL_CONTROLLER_AXIS_LEFTX; break;
	case AXIS_MAP_LY: sdlAxis = SDL_CONTROLLER_AXIS_LEFTY; break;
	case AXIS_MAP_RX: sdlAxis = SDL_CONTROLLER_AXIS_RIGHTX; break;
	case AXIS_MAP_RY: sdlAxis = SDL_CONTROLLER_AXIS_RIGHTY; break;
	}

	Sint16 raw = SDL_GameControllerGetAxis(pad.controller, sdlAxis);
	// SDL reports -Y for "stick pushed up/forward"; this interface is
	// forward/up-positive (see the AXIS_MAP_LY note above), so flip the Y axes.
	if (physical == AXIS_MAP_LY || physical == AXIS_MAP_RY)
		raw = (Sint16)-(int)raw;
	float normalised = raw / 32768.0f;
	if (std::fabs((float)raw) < (float)g_analogRange.deadzone)
		normalised = 0.0f;
	return Clamp(normalised * pad.sensitivity, -1.0f, 1.0f);
}

float C_4JInput::GetJoypadStick_LX(int iPad, bool bCheckMenuDisplay) { return ReadAxis(iPad, AXIS_MAP_LX, bCheckMenuDisplay); }
float C_4JInput::GetJoypadStick_LY(int iPad, bool bCheckMenuDisplay) { return ReadAxis(iPad, AXIS_MAP_LY, bCheckMenuDisplay); }
float C_4JInput::GetJoypadStick_RX(int iPad, bool bCheckMenuDisplay) { return ReadAxis(iPad, AXIS_MAP_RX, bCheckMenuDisplay); }
float C_4JInput::GetJoypadStick_RY(int iPad, bool bCheckMenuDisplay) { return ReadAxis(iPad, AXIS_MAP_RY, bCheckMenuDisplay); }

static unsigned char ReadTrigger(int iPad, unsigned int triggerSlot, bool bCheckMenuDisplay)
{
	if (iPad < 0 || iPad >= 4) return 0;
	PadState &pad = g_pads[iPad];
	if (bCheckMenuDisplay && pad.menuDisplayed) return 0;
	if (!pad.controller) return 0;

	unsigned int physical = pad.triggerMap[triggerSlot];
	SDL_GameControllerAxis sdlAxis = (physical == TRIGGER_MAP_0) ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
	Sint16 raw = SDL_GameControllerGetAxis(pad.controller, sdlAxis); // 0..32767
	return (unsigned char)Clamp(raw / 128.0f, 0.0f, 255.0f);
}

unsigned char C_4JInput::GetJoypadLTrigger(int iPad, bool bCheckMenuDisplay) { return ReadTrigger(iPad, TRIGGER_MAP_0, bCheckMenuDisplay); }
unsigned char C_4JInput::GetJoypadRTrigger(int iPad, bool bCheckMenuDisplay) { return ReadTrigger(iPad, TRIGGER_MAP_1, bCheckMenuDisplay); }

void C_4JInput::SetMenuDisplayed(int iPad, bool bVal)
{
	// Deliberately ignored on this build. The flag exists to suppress gameplay
	// stick/trigger input while a menu covers the screen, but no Iggy scene ever
	// reaches the screen here, so it can only ever be set spuriously - and it
	// latches: UIController::SetMenuDisplayed() forwards true to us from
	// NavigateToScene() (UIController.cpp:1481/1820) *before* the scene is built,
	// and only NavigateBack() would clear it. Scene construction fails here, so
	// nothing ever clears it.
	//
	// The consequence was severe because ReadAxis()/ReadTrigger() return 0 for
	// *every* axis when it is set: one failed navigation permanently killed both
	// camera look and WASD movement. See LinuxUIController::GetMenuDisplayed() for
	// the same reasoning applied to the game-facing query.
	(void)iPad; (void)bVal;
}

EKeyboardResult C_4JInput::RequestKeyboard(LPCWSTR Title, LPCWSTR Text, DWORD dwPad, UINT uiMaxChars, int (*Func)(LPVOID, const bool), LPVOID lpParam, C_4JInput::EKeyboardMode eMode)
{
	// No on-screen keyboard exists on desktop (there's a physical one) and no
	// Minecraft.Client UI is wired up yet to host a text-entry widget - see
	// Linux_Minecraft.cpp's header comment on why UI integration is deferred.
	// Decline immediately so callers don't block waiting on a result that will
	// never arrive, rather than silently hanging.
	(void)Title; (void)Text; (void)dwPad; (void)uiMaxChars; (void)lpParam; (void)eMode;
	if (Func) Func(lpParam, false);
	return EKeyboard_ResultDecline;
}

void C_4JInput::GetText(uint16_t *UTF16String)
{
	if (UTF16String) UTF16String[0] = 0;
}

bool C_4JInput::VerifyStrings(WCHAR **pwStringA, int iStringC, int (*Func)(LPVOID, STRING_VERIFY_RESPONSE *), LPVOID lpParam)
{
	// TCR 92's Xbox LIVE profanity-filter service has no Linux/offline
	// equivalent and no online service is being stood up for this port -
	// accept all strings unconditionally rather than fabricating a filter.
	(void)pwStringA;
	if (Func)
	{
		STRING_VERIFY_RESPONSE response;
		response.wNumStrings = (WORD)iStringC;
		response.pStringResult = nullptr;
		Func(lpParam, &response);
	}
	return true;
}

void C_4JInput::CancelQueuedVerifyStrings(int (*Func)(LPVOID, STRING_VERIFY_RESPONSE *), LPVOID lpParam)
{
	(void)Func; (void)lpParam;
}

void C_4JInput::CancelAllVerifyInProgress(void)
{
}

C_4JInput InputManager;
