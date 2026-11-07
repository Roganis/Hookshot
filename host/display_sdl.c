/* (hookshot, Linux) The game's window on SDL2: what host/display.c is on Windows.
   A thread of its own makes the window and runs its events; the renderer (gpu/vk, through rx_start) presents into it,
   with SDL making the Vulkan surface (host/posix/native_window.h). The keys held are kept by their Windows codes
   (w32_key_down: nn_hid.c and the Insert menu ask with GetAsyncKeyState), by position, so the WASD of the controls are
   the same keys on any layout. Mouse: a click in the window captures it (relative motion, cursor hidden), Esc or leaving
   the window releases it (nn_hid.c reads the motion). F11 or the menu's row: fullscreen (borderless, the desktop's
   mode). Closing the window ends the run.
   HOS_HEADLESS=1: no window. HOS_WINDOW_SIZE=<w>x<h>: the window's size (default 1280x720). HOS_FULLSCREEN=1. */
#ifndef _WIN32
#include "hos.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include "../gpu/render/rx.h"
#include "posix/native_window.h"

volatile LONG64 hos_present_count;   /* (nvn_native.c: the game's presents) */
/* (SDL_Init from the window's, the controllers' and the sound's threads, one at a time) */
pthread_mutex_t hos_sdl_mutex = PTHREAD_MUTEX_INITIALIZER;
int hos_sdl_init(uint32_t flags) {
    pthread_mutex_lock(&hos_sdl_mutex);
    const int r = SDL_InitSubSystem(flags);
    pthread_mutex_unlock(&hos_sdl_mutex);
    return r;
}

static SDL_Window *window;
static HookshotNativeWindow native;
static int renderer;
static volatile LONG focused, fullscreen, want_fullscreen = -1;

/* mouse capture: relative motion accumulated here, taken by nn_hid.c */
static volatile LONG mouse_dx, mouse_dy, mouse_captured;
int hos_mouse_take(int32_t *dx, int32_t *dy) {
    *dx = InterlockedExchange(&mouse_dx, 0);
    *dy = InterlockedExchange(&mouse_dy, 0);
    return mouse_captured;
}
static void mouse_capture(int on) {
    if (on == (int)mouse_captured) return;
    mouse_captured = on;
    InterlockedExchange(&mouse_dx, 0);
    InterlockedExchange(&mouse_dy, 0);
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
}

static void set_fullscreen(int on) {
    if (on == (int)fullscreen) return;
    if (SDL_SetWindowFullscreen(window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0)) {
        hos_log("display: fullscreen: %s", SDL_GetError());
        return;
    }
    fullscreen = on;
    hos_log("display: %s", on ? "fullscreen (F11 for a window)" : "windowed");
}

/* ---- what host/posix/win32.c asks of the window (the menu's Fullscreen row, focus, sizes) ---- */
static HWND op_foreground(void) { return focused ? (HWND)&native : NULL; }
static LONG op_style(HWND w) { (void)w; return fullscreen ? 0 : WS_CAPTION | WS_OVERLAPPEDWINDOW; }
static BOOL op_post(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    (void)w, (void)lp;
    if (msg != WM_APP + 7) return FALSE;   /* (the Insert menu's row: fullscreen on or off) */
    want_fullscreen = (LONG)wp;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_USEREVENT;
    SDL_PushEvent(&e);
    return TRUE;
}
static void drawable(void *win, int *w, int *h) {
    *w = *h = 0;
    if (win) SDL_Vulkan_GetDrawableSize((SDL_Window *)win, w, h);
}
static BOOL op_client(HWND hw, RECT *r) {
    (void)hw;
    int w = 0, h = 0;
    drawable(window, &w, &h);
    r->left = r->top = 0;
    r->right = w, r->bottom = h;
    return w > 0;
}
/* (the main screen's size, for the Ultrawide row: asked by hos_main.c before the window's thread starts, so SDL's
   video is opened and closed again here, on the asking thread; once the window is up, the window's screen) */
static void op_screen(int *w, int *h) {
    SDL_DisplayMode m;
    *w = *h = 0;
    pthread_mutex_lock(&hos_sdl_mutex);
    if (window) {
        if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(window), &m) == 0) *w = m.w, *h = m.h;
    } else if (!getenv("HOS_HEADLESS") && SDL_InitSubSystem(SDL_INIT_VIDEO) == 0) {
        if (SDL_GetDesktopDisplayMode(0, &m) == 0) *w = m.w, *h = m.h;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    pthread_mutex_unlock(&hos_sdl_mutex);
}
static void screen_only(int *w, int *h) { op_screen(w, h); }
static HWND no_window(void) { return NULL; }
static LONG no_style(HWND w) { (void)w; return WS_CAPTION; }
static BOOL no_post(HWND w, UINT m, WPARAM wp, LPARAM lp) { (void)w, (void)m, (void)wp, (void)lp; return FALSE; }
static BOOL no_client(HWND w, RECT *r) { (void)w, (void)r; return FALSE; }
/* (before the window: only the screen's size) */
static const W32WindowOps early_ops = {no_window, no_style, no_post, no_client, screen_only};
__attribute__((constructor)) static void early_window_ops(void) { w32_set_window_ops(&early_ops); }
static const W32WindowOps window_ops = {op_foreground, op_style, op_post, op_client, op_screen};

/* ---- the surface, for the Vulkan back end ---- */
static int surface_extensions(void *win, unsigned *count, const char **names) {
    return SDL_Vulkan_GetInstanceExtensions((SDL_Window *)win, count, names) == SDL_TRUE;
}
static int surface_create(void *win, uint64_t instance, uint64_t *surface) {
    VkSurfaceKHR s = 0;
    if (!SDL_Vulkan_CreateSurface((SDL_Window *)win, (VkInstance)(uintptr_t)instance, &s)) {
        hos_log("display: no Vulkan surface: %s", SDL_GetError());
        return 0;
    }
    *surface = (uint64_t)s;
    return 1;
}

/* ---- keys: SDL's scancodes (positions) as Windows' codes ---- */
static int vk_of(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s == SDL_SCANCODE_0) return '0';
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return VK_F1 + (s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return VK_NUMPAD0 + 1 + (s - SDL_SCANCODE_KP_1);
    switch (s) {
    case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
    case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
    case SDL_SCANCODE_BACKSPACE: return VK_BACK;
    case SDL_SCANCODE_TAB: return VK_TAB;
    case SDL_SCANCODE_SPACE: return VK_SPACE;
    case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
    case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
    case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;
    case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
    case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
    case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
    case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
    case SDL_SCANCODE_GRAVE: return VK_OEM_3;
    case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
    case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;
    case SDL_SCANCODE_SLASH: return VK_OEM_2;
    case SDL_SCANCODE_CAPSLOCK: return VK_CAPITAL;
    case SDL_SCANCODE_PRINTSCREEN: return VK_SNAPSHOT;
    case SDL_SCANCODE_PAUSE: return VK_PAUSE;
    case SDL_SCANCODE_INSERT: return VK_INSERT;
    case SDL_SCANCODE_HOME: return VK_HOME;
    case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
    case SDL_SCANCODE_DELETE: return VK_DELETE;
    case SDL_SCANCODE_END: return VK_END;
    case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
    case SDL_SCANCODE_RIGHT: return VK_RIGHT;
    case SDL_SCANCODE_LEFT: return VK_LEFT;
    case SDL_SCANCODE_DOWN: return VK_DOWN;
    case SDL_SCANCODE_UP: return VK_UP;
    case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
    case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY;
    case SDL_SCANCODE_KP_MINUS: return VK_SUBTRACT;
    case SDL_SCANCODE_KP_PLUS: return VK_ADD;
    case SDL_SCANCODE_KP_PERIOD: return VK_DECIMAL;
    case SDL_SCANCODE_LCTRL: return VK_LCONTROL;
    case SDL_SCANCODE_RCTRL: return VK_RCONTROL;
    case SDL_SCANCODE_LSHIFT: return VK_LSHIFT;
    case SDL_SCANCODE_RSHIFT: return VK_RSHIFT;
    case SDL_SCANCODE_LALT: return VK_LMENU;
    case SDL_SCANCODE_RALT: return VK_RMENU;
    default: return 0;
    }
}
static void key(SDL_Scancode s, int down) {
    const int vk = vk_of(s);
    if (!vk) return;
    w32_key_down[vk] = (uint8_t)down;
    /* (the either-side codes, from both sides) */
    w32_key_down[VK_SHIFT] = w32_key_down[VK_LSHIFT] | w32_key_down[VK_RSHIFT];
    w32_key_down[VK_CONTROL] = w32_key_down[VK_LCONTROL] | w32_key_down[VK_RCONTROL];
    w32_key_down[VK_MENU] = w32_key_down[VK_LMENU] | w32_key_down[VK_RMENU];
}
static void keys_up(void) {
    for (int i = 0; i < 256; i++) w32_key_down[i] = 0;
}

static HANDLE renderer_up;
static void *display_main(void *p) {
    (void)p;
    void hos_host_thread_register_self(const char *name);   /* (hos_prof.c) */
    hos_host_thread_register_self("window (SDL)");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    SDL_SetHint("SDL_MOUSE_FOCUS_CLICKTHROUGH", "1");
    if (hos_sdl_init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        hos_log("display: SDL could not open the video: %s; no window", SDL_GetError());
        rx_start(NULL);
        SetEvent(renderer_up);
        return NULL;
    }
    extern float hos_aspect_ratio;
    int win_w = hos_aspect_ratio > 1.8f ? (int)(720 * hos_aspect_ratio + 0.5f) : 1280, win_h = 720;
    if (getenv("HOS_WINDOW_SIZE")) {
        int w = 0, h = 0;
        if (sscanf(getenv("HOS_WINDOW_SIZE"), "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64) win_w = w, win_h = h;
    }
    window = SDL_CreateWindow(HOS_GAME_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        hos_log("display: no window: %s", SDL_GetError());
        rx_start(NULL);
        SetEvent(renderer_up);
        return NULL;
    }
    {   /* the icon (assets/hookshot.png is not read here: the window manager's own one stays when there is none) */
        const char *icon = getenv("HOS_ICON_BMP");
        SDL_Surface *s = icon ? SDL_LoadBMP(icon) : NULL;
        if (s) SDL_SetWindowIcon(window, s), SDL_FreeSurface(s);
    }
    native.magic = HOOKSHOT_NATIVE_WINDOW_MAGIC;
    native.sdl_window = window;
    native.instance_extensions = surface_extensions;
    native.create_surface = surface_create;
    native.drawable_size = drawable;
    w32_set_window_ops(&window_ops);
    focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    if (getenv("HOS_FULLSCREEN") && atoi(getenv("HOS_FULLSCREEN"))) set_fullscreen(1);
    renderer = rx_start(&native);
    if (!renderer) hos_log("display: no Vulkan renderer: the window stays black");
    SetEvent(renderer_up);
    SDL_Event e;
    for (;;) {
        if (!SDL_WaitEventTimeout(&e, 250)) continue;
        do {
            switch (e.type) {
            case SDL_QUIT:
                hos_log("display: window closed, exiting");
                hos_log_flush();
                ExitProcess(0);
                break;
            case SDL_USEREVENT:
                if (want_fullscreen >= 0) set_fullscreen((int)InterlockedExchange(&want_fullscreen, -1));
                break;
            case SDL_KEYDOWN:
                if (!e.key.repeat) {
                    if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) mouse_capture(0);
                    if (e.key.keysym.scancode == SDL_SCANCODE_F11) set_fullscreen(!fullscreen);
                }
                key(e.key.keysym.scancode, 1);
                break;
            case SDL_KEYUP:
                key(e.key.keysym.scancode, 0);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    w32_key_down[VK_LBUTTON] = 1;
                    if (renderer && !mouse_captured) mouse_capture(1);
                }
                if (e.button.button == SDL_BUTTON_RIGHT) w32_key_down[VK_RBUTTON] = 1;
                if (e.button.button == SDL_BUTTON_MIDDLE) w32_key_down[VK_MBUTTON] = 1;
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT) w32_key_down[VK_LBUTTON] = 0;
                if (e.button.button == SDL_BUTTON_RIGHT) w32_key_down[VK_RBUTTON] = 0;
                if (e.button.button == SDL_BUTTON_MIDDLE) w32_key_down[VK_MBUTTON] = 0;
                break;
            case SDL_MOUSEMOTION:
                if (mouse_captured) {
                    InterlockedExchangeAdd(&mouse_dx, e.motion.xrel);
                    InterlockedExchangeAdd(&mouse_dy, e.motion.yrel);
                }
                break;
            case SDL_WINDOWEVENT:
                switch (e.window.event) {
                case SDL_WINDOWEVENT_FOCUS_GAINED: focused = 1; break;
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    focused = 0;
                    keys_up();
                    mouse_capture(0);
                    break;
                case SDL_WINDOWEVENT_SIZE_CHANGED: {
                    int w = 0, h = 0;
                    drawable(window, &w, &h);
                    if (renderer && w > 0 && h > 0) rx_resize(w, h);
                    break;
                }
                case SDL_WINDOWEVENT_CLOSE:
                    hos_log("display: window closed, exiting");
                    hos_log_flush();
                    ExitProcess(0);
                    break;
                }
                break;
            }
        } while (SDL_PollEvent(&e));
    }
    return NULL;
}

/* (hookshot) The game's first work for the GPU waits for the renderer (host/display.c says why) */
void hos_renderer_wait(void) {
    if (renderer_up) WaitForSingleObject(renderer_up, INFINITE);
}

void hos_display_start(void) {
    if (getenv("HOS_HEADLESS")) {   /* no window: the renderer still draws, offscreen (screenshots, frame dumps) */
        rx_start(NULL);
        return;
    }
    renderer_up = CreateEventA(NULL, TRUE, FALSE, NULL);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8 << 20);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, display_main, NULL);
    pthread_attr_destroy(&a);
}
#endif
