//
// dusty_input.cpp — Direct SDL event injection bridge.
//
// The Dusty platform shell owns DOM event capture (shell.js). Mouse/keyboard
// events are postMessaged to the worker, which calls into these helpers from
// JS (via Module._dusty_input_*). Each helper constructs an SDL_Event and
// pushes it onto SDL's thread-safe event queue with SDL_PushEvent. The engine
// pthread's SDL_PollEvent picks it up in its main loop.
//
// We use SDL_PushEvent (not SDL's internal Emscripten event glue) so that
// cross-thread event delivery goes through SDL's own mutex-protected queue
// rather than Emscripten's HTML5 proxy machinery, which has its own bugs
// under -pthread (em_task_queue alignment trap, TLS bootstrap trap, etc.).
//
// We deliberately do NOT call SDL's Emscripten_RegisterEventHandlers path —
// the JS shell does not let SDL register DOM listeners on its mock document.
// All input flows through this file.
//

#include <SDL.h>
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>

// dusty-bytes: test-harness Lua eval queue. JS pushes a string via
// dusty_lua_eval(); engine pthread drains it at the top of GameLogicLoop
// (see mainloop.cpp). Single producer / single consumer → atomic exchange.
std::atomic<char *> g_dustyPendingLuaEval{nullptr};

extern "C" {

// All six event types here correspond to events shell.js forwards via
// postMessage({action:'event'}): mousemove, mousedown, mouseup, wheel,
// keydown, keyup. Touch is translated to synthetic mouse events upstream.
// No other DOM event ever reaches the worker as an 'event' action — visibility,
// focus, resize, beforeunload are owned by shell.js at page level and never
// proxied into the engine. So this is the complete input surface.

EMSCRIPTEN_KEEPALIVE
void dusty_input_mouse_motion(int x, int y, int xrel, int yrel, int buttonState)
{
	SDL_Event evt;
	memset(&evt, 0, sizeof(evt));
	evt.motion.type = SDL_MOUSEMOTION;
	evt.motion.timestamp = SDL_GetTicks();
	evt.motion.windowID = 1;
	evt.motion.which = 0;
	evt.motion.state = (Uint32)buttonState;
	evt.motion.x = x;
	evt.motion.y = y;
	evt.motion.xrel = xrel;
	evt.motion.yrel = yrel;
	SDL_PushEvent(&evt);
}

EMSCRIPTEN_KEEPALIVE
void dusty_input_mouse_button(int button, int down, int x, int y)
{
	SDL_Event evt;
	memset(&evt, 0, sizeof(evt));
	evt.button.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	evt.button.timestamp = SDL_GetTicks();
	evt.button.windowID = 1;
	evt.button.which = 0;
	evt.button.button = (Uint8)button;          // SDL button: 1=left, 2=middle, 3=right
	evt.button.state = down ? SDL_PRESSED : SDL_RELEASED;
	evt.button.clicks = 1;
	evt.button.x = x;
	evt.button.y = y;
	SDL_PushEvent(&evt);
}

EMSCRIPTEN_KEEPALIVE
void dusty_input_mouse_wheel(int x, int y)
{
	SDL_Event evt;
	memset(&evt, 0, sizeof(evt));
	evt.wheel.type = SDL_MOUSEWHEEL;
	evt.wheel.timestamp = SDL_GetTicks();
	evt.wheel.windowID = 1;
	evt.wheel.which = 0;
	evt.wheel.x = x;
	evt.wheel.y = y;
	evt.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
	SDL_PushEvent(&evt);
}

// JS keyCode → SDL_Scancode minimal translation table for the keys WC2 cares
// about. Covers ASCII alphanumerics, function keys, navigation, modifiers.
// Anything not in the table is dropped (returns SDL_SCANCODE_UNKNOWN).
static SDL_Scancode js_keycode_to_scancode(int jsKey)
{
	if (jsKey >= 'A' && jsKey <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (jsKey - 'A'));
	if (jsKey >= '0' && jsKey <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + ((jsKey - '0' + 9) % 10));
	if (jsKey >= 112 && jsKey <= 123) return (SDL_Scancode)(SDL_SCANCODE_F1 + (jsKey - 112));
	switch (jsKey) {
		case 8:   return SDL_SCANCODE_BACKSPACE;
		case 9:   return SDL_SCANCODE_TAB;
		case 13:  return SDL_SCANCODE_RETURN;
		case 16:  return SDL_SCANCODE_LSHIFT;
		case 17:  return SDL_SCANCODE_LCTRL;
		case 18:  return SDL_SCANCODE_LALT;
		case 27:  return SDL_SCANCODE_ESCAPE;
		case 32:  return SDL_SCANCODE_SPACE;
		case 33:  return SDL_SCANCODE_PAGEUP;
		case 34:  return SDL_SCANCODE_PAGEDOWN;
		case 35:  return SDL_SCANCODE_END;
		case 36:  return SDL_SCANCODE_HOME;
		case 37:  return SDL_SCANCODE_LEFT;
		case 38:  return SDL_SCANCODE_UP;
		case 39:  return SDL_SCANCODE_RIGHT;
		case 40:  return SDL_SCANCODE_DOWN;
		case 45:  return SDL_SCANCODE_INSERT;
		case 46:  return SDL_SCANCODE_DELETE;
		case 187: return SDL_SCANCODE_EQUALS;
		case 188: return SDL_SCANCODE_COMMA;
		case 189: return SDL_SCANCODE_MINUS;
		case 190: return SDL_SCANCODE_PERIOD;
		case 191: return SDL_SCANCODE_SLASH;
		case 192: return SDL_SCANCODE_GRAVE;
		case 219: return SDL_SCANCODE_LEFTBRACKET;
		case 220: return SDL_SCANCODE_BACKSLASH;
		case 221: return SDL_SCANCODE_RIGHTBRACKET;
		case 222: return SDL_SCANCODE_APOSTROPHE;
	}
	return SDL_SCANCODE_UNKNOWN;
}

EMSCRIPTEN_KEEPALIVE
void dusty_input_key(int jsKeyCode, int modMask, int down, int repeat)
{
	SDL_Scancode scancode = js_keycode_to_scancode(jsKeyCode);
	if (scancode == SDL_SCANCODE_UNKNOWN) return;

	SDL_Event evt;
	memset(&evt, 0, sizeof(evt));
	evt.key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	evt.key.timestamp = SDL_GetTicks();
	evt.key.windowID = 1;
	evt.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	evt.key.repeat = (Uint8)repeat;
	evt.key.keysym.scancode = scancode;
	evt.key.keysym.sym = SDL_GetKeyFromScancode(scancode);
	evt.key.keysym.mod = (Uint16)modMask;
	SDL_PushEvent(&evt);
}

// dusty_lua_eval: queue a Lua string for engine pthread to execute at the
// top of the next GameLogicLoop tick. Returns 0 on queued, -1 if a previous
// eval is still pending. The engine pthread frees the buffer after running.
EMSCRIPTEN_KEEPALIVE
int dusty_lua_eval(const char *code)
{
	if (!code) return -1;
	size_t n = strlen(code) + 1;
	char *buf = (char *)malloc(n);
	if (!buf) return -1;
	memcpy(buf, code, n);
	// Only queue if currently nullptr (don't overwrite a pending eval).
	char *expected = nullptr;
	if (!g_dustyPendingLuaEval.compare_exchange_strong(expected, buf,
	                                                   std::memory_order_release,
	                                                   std::memory_order_relaxed)) {
		free(buf);
		return -1;
	}
	return 0;
}

// dusty-bytes: trap-survival diagnostic trace. Writes to a fixed-size
// in-WASM ring buffer. JS reads it directly from shared HEAPU8 via the
// pointer exports below — no cross-thread proxy (MAIN_THREAD_EM_ASM hits
// the em_task_queue alignment bug from pthread context). Atomic head
// makes single-producer-single-consumer safe.
#define DUSTY_TRACE_SLOTS 16384
#define DUSTY_TRACE_SLOT_BYTES 128
static char g_dustyTrace[DUSTY_TRACE_SLOTS * DUSTY_TRACE_SLOT_BYTES];
static std::atomic<int> g_dustyTraceHead{0};

EMSCRIPTEN_KEEPALIVE
void dusty_trace_push(const char *msg)
{
	if (!msg) return;
	int slot = g_dustyTraceHead.fetch_add(1, std::memory_order_acq_rel) % DUSTY_TRACE_SLOTS;
	char *dst = g_dustyTrace + (size_t)slot * DUSTY_TRACE_SLOT_BYTES;
	size_t n = strlen(msg);
	if (n >= DUSTY_TRACE_SLOT_BYTES) n = DUSTY_TRACE_SLOT_BYTES - 1;
	memcpy(dst, msg, n);
	dst[n] = '\0';
}

EMSCRIPTEN_KEEPALIVE uint32_t dusty_trace_buf_addr(void) { return (uint32_t)(uintptr_t)g_dustyTrace; }
EMSCRIPTEN_KEEPALIVE int dusty_trace_head(void) { return g_dustyTraceHead.load(std::memory_order_acquire); }
EMSCRIPTEN_KEEPALIVE int dusty_trace_slots(void) { return DUSTY_TRACE_SLOTS; }
EMSCRIPTEN_KEEPALIVE int dusty_trace_slot_bytes(void) { return DUSTY_TRACE_SLOT_BYTES; }

} // extern "C"
