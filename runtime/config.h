// Player settings: allgenurecomp.ini in the game folder (written with the defaults when it is missing).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {  // (runtime/kzmui.cpp, C++: the same symbols the C files define)
#endif

enum { DISPLAY_WINDOWED, DISPLAY_FULLSCREEN, DISPLAY_BORDERLESS };
enum { FACE_POSITION, FACE_LABEL };

// [Monster HP]: factors on the HP multiplier of Hub-type quests' large monsters
#define HP_QUESTS_MAX 256
#define HP_SCALE_MIN 0.05f
#define HP_SCALE_MAX 20.0f
typedef struct {
    float scale[4];  // by the number of hunters (index: hunters - 1)
    float village;   // Village quests posted in the Hub / Pub: this first, then scale ([Village Quests] HubHP)
    int quests;
    struct { uint32_t quest; float scale[4]; } quest[HP_QUESTS_MAX];  // per quest, in place of scale
} HpScale;

typedef struct {
    int display_mode;         // DISPLAY_*
    int width, height;        // screen / window size (Fullscreen: the monitor mode)
    int render_w, render_h;   // the game's 3D resolution (16:9)
    int fps;                  // frame rate cap: 30..240
    int vsync;                // 1: present without tearing (mailbox / FIFO, at most the refresh rate), 0: immediate
    int msaa;                 // samples of the 3D scene: 1 (off), 2, 4, 8
    int fxaa;                 // the game's own FXAA pass: 1 on, 0 off
    int face_buttons;         // FACE_*: controller face buttons by position (the Switch layout) or by their labels
    int ldn_port;             // local play: the port a hosted hub listens on (TCP; UDP for LAN search)
    char ldn_join[256];       // local play: hosts to search besides the LAN (host[:port], comma separated)
    int sync_small_monsters;  // multiplayer: small monsters synced like large ones (runtime/net/emsync.c)
    int bonus_content;        // free downloads copied from a donor save into saves without any (runtime/services/bonus.c)
    int village_in_hub;       // L / R at the Hub's / Pub's quest counter switch to Village quests (runtime/hle/villagehub.c)
    HpScale hp;               // large monster HP in Hub-type quests (runtime/hle/hpscale.c)
    float mouse_sensitivity;  // mouse look: the speed for full right stick tilt is divided by it (runtime/host_input.c)
    float aspect_ratio;       // the picture's shape (width / height), 0: the window's; never below 16:10 (config.c)
    float fov;                // 3D camera field of view factor (1: the game's)
    int ui_anchor;            // where the HUD goes on a picture wider than 16:9: UI_*
    int background_input;     // the controller keeps working while the window is in the background (runtime/ui.c)
    // --- [AllGenU] added fields: appended so the existing layout and defaults above stay untouched ---
    int dock_mode;            // Dock mode: fix the 3D resolution to a native 720p/1080p profile (kzmui.cpp / config.c)
    int fsr;                  // FSR 1.0 upscaling + sharpening in the present pass: 0 off, 1 on (nvn/vulkan/exec.c)
    int fsr_sharp;            // [AllGenU] FSR sharpening amount: 0 (soft) .. 100 (maximum), RCAS in exec.c
    // --- [AllGenU] KZMUI (runtime/kzmui.cpp) settings, appended like the ones above ---
    int ui_theme;             // KZMUI theme: index into its 12 themes (3: metal red, the default)
    int ui_sharp;             // KZMUI shape: 0 rounded, 1 sharp
    int ui_font;              // KZMUI font: 0 Segoe UI, 1 Consolas, 2 Bahnschrift, 3 Arial, 4 Verdana
    int mods_enabled;         // mods/ loaded at start (1); 0 leaves them out (needs a restart)
    int render_auto;          // 3D resolution: 1 Auto, 0 the RenderResolution value (runtime/config.c)
    int show_fps;             // an FPS counter over the game, top right (runtime/overlay.cpp)
    // --- [AllGenU] image look, FSR render scale and the keyboard map, appended last ---
    int hdr;                  // [AllGenU] HDR look (software colour grading in the present shader)
    int false_dlss;           // [AllGenU] "False DLSS" look: desaturated, brighter, highlights compressed
    int fsr_scale;            // [AllGenU] FSR's internal render scale, percent of the window (50..100; 100 = native)
    int keybinds[16];         // [AllGenU] keyboard key (SDL scancode) per guest button, BTN_* order (host_input.c)
    int walk_key;             // [AllGenU] key held for a half left-stick tilt (walk; was Left Shift)
} Config;
enum { UI_CENTER = 1, UI_21_9, UI_32_9, UI_EDGES };  // the middle 16:9, 21:9, 32:9, the picture's sides

extern Config g_config;
extern uint32_t g_render_size[2];  // read by the game code (tools/gen.py PATCHES: its docked render size)
extern float g_render_aspect;      // the picture's shape, 16:10 or wider: the 3D cameras' aspect (tools/gen.py PATCHES)
extern float g_fov_scale;          // 3D camera field of view factor (tools/gen.py PATCHES)
extern uint32_t g_ui_size[2];      // the game's 2D canvas size (tools/gen.py PATCHES: 0xdbcd48, 0xdbcd54)
extern float g_ui_anchor_aspect;   // the HUD's width / height, 16:9 .. g_render_aspect (nvn/vulkan/exec.c)
float camera_fovy(float fovy, float aspect);  // a 3D camera's vertical field of view for its aspect (gen.py PATCHES)

void config_load(void);  // before the window and the guest start
// [AllGenU] KZMUI: persist one key of allgenurecomp.ini (the menu has already changed g_config; a restart keeps it).
void config_set(const char *section, const char *key, const char *value);
// [AllGenU] the keyboard map Back to its defaults (KZMUI's "Restaurar" and a fresh settings file call it).
void config_keybinds_default(int *out16);

#ifdef __cplusplus
}
#endif
