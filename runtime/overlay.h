// Forge's ImGui overlay (runtime/overlay.cpp): its menu window with the plugins' settings, drawn over the game.
// C and C++.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// From runtime/forge.c once plugins are loaded: font (a .ttf; NULL: ImGui's own), its size in pixels at 720 lines
// (scaled with the window), the key that opens the menu (SDL scancode = HID usage, as Forge's config has it).
void overlay_init(const char *font_path, float font_size, int menu_key);
bool overlay_enabled(void);
bool overlay_menu_open(void);

// Host main thread: an event for ImGui while the menu is open (true: taken, not for the game) - and the menu key.
bool overlay_event(const SDL_Event *e);
// Host main thread, while the menu is open: the controller, which moves through the menu instead of the game.
typedef struct { bool a, b, x, y, up, down, left, right, l, r, plus, minus; float lx, ly; } OverlayPad;
void overlay_pad(const OverlayPad *p);

// The game's thread, once a frame (after the plugins' forge_onUpdate): the overlay's frame, plugins' UI included.
void overlay_frame(void *cpu);
// [AllGenU] the rate frames are actually presented at (FIFO / mailbox make it differ from the engine's own pace, which
// is what ImGui's Framerate measures); the FPS counter and KZMUI's telemetry show this one.
float overlay_fps(void);

// Render thread, inside the window image's render pass (runtime/nvn/vulkan/device.c, under the Vulkan lock).
typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qfam;
    VkQueue queue;
    VkPipelineCache cache;
    VkRenderPass pass;
    uint32_t width, height;
} OverlayTarget;
void overlay_draw(VkCommandBuffer cb, const OverlayTarget *t);

#ifdef __cplusplus
}
#endif
