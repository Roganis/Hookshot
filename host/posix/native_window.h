/* (hookshot, Linux) What rx_start is given on Linux in place of an HWND: the game's SDL window, with SDL's own way to
   make a Vulkan surface for it (X11, Wayland or whatever SDL runs on), so the Vulkan back end needs no window system
   of its own. host/display_sdl.c fills it; gpu/vk/vk_main.cpp reads it. */
#pragma once
#include <stdint.h>

#define HOOKSHOT_NATIVE_WINDOW_MAGIC 0x484b5357u   /* "HKSW" */
typedef struct HookshotNativeWindow {
    uint32_t magic;
    void *sdl_window;
    /* the instance extensions the surface needs: their count, and the names when names is not null */
    int (*instance_extensions)(void *sdl_window, unsigned *count, const char **names);
    /* a VkSurfaceKHR for the window, of a VkInstance (both as integers: no Vulkan header here); 1 when made */
    int (*create_surface)(void *sdl_window, uint64_t instance, uint64_t *surface);
    /* the size in pixels to draw at */
    void (*drawable_size)(void *sdl_window, int *width, int *height);
} HookshotNativeWindow;
