// Player settings (config.h): allgenurecomp.ini in the game folder (next to the executable, or MHGU_HOME; with a game
// folder argument, the current folder). A missing file is written with the defaults, so
// deleting it resets everything; keys that are missing or not understood keep their defaults (the log says which).
//   MHGU_CONFIG=<path>   another settings file
//   MHGU_WINDOW=WxH      windowed at that size, MHGU_RENDER=WxH  3D resolution, MHGU_MSAA=1|2|4|8, MHGU_FXAA=0|1,
//                        MHGU_PAD_POSITIONAL=0|1 (test runs; override the file)
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL3/SDL.h>

#include "config.h"
#include "rt.h"

Config g_config = {DISPLAY_BORDERLESS, 1920, 1080, 0, 0, 60, 1, 1, 0 /* [AllGenU] fxaa: anti-aliasing off by default */, FACE_POSITION, 39500, "", 1, 1, 1, {{1, 1, 1, 1}, 2}, 1.0f, 0.0f, 1.0f, UI_CENTER, 1,
                   0 /* [AllGenU] dock_mode */, 0 /* [AllGenU] fsr */, 55 /* [AllGenU] fsr_sharp: 0..100 */,
                   3 /* [AllGenU] ui_theme */, 0 /* [AllGenU] ui_sharp */, 0 /* [AllGenU] ui_font */,
                   1 /* [AllGenU] mods_enabled */, 1 /* [AllGenU] render_auto */, 0 /* [AllGenU] show_fps */,
                   // --- [AllGenU] image look, FSR render scale and the keyboard map (host_input.c / kzmui.cpp) ---
                   0 /* hdr */, 0 /* false_dlss */, 100 /* fsr_scale: native by default; the menu slider lowers it */,
                   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} /* keybinds: config_keybinds_default */,
                   SDL_SCANCODE_LALT /* walk_key */};
uint32_t g_render_size[2] = {1920, 1080};
float g_render_aspect = 16.0f / 9.0f;
float g_fov_scale = 1.0f;
uint32_t g_ui_size[2] = {1280, 720};
float g_ui_anchor_aspect = 16.0f / 9.0f;

static const char DEFAULT_INI[] =
    "; AllGenURecomp settings. Delete this file to get the defaults back: it is written again at the next start.\n"
    "\n"
    "[Display]\n"
    "; Fullscreen, Windowed or Borderless (a borderless window covering the whole screen).\n"
    "; Alt+Enter switches between Windowed and the full-screen mode chosen here.\n"
    "Mode = Borderless\n"
    "\n"
    "; Screen size: WxH (1280x720, 1600x900, 1920x1080, 2560x1440...) or Desktop (the monitor's own resolution).\n"
    "; Fullscreen switches the monitor to this mode, Windowed opens a window this size (it can be resized), and\n"
    "; Borderless always covers the screen, scaling the game's image to it.\n"
    "Resolution = 1920x1080\n"
    "\n"
    "; The game's 3D resolution: Auto (as large as fits Resolution, in the picture's shape below) or WxH. Lower is\n"
    "; faster, higher is sharper; the Switch renders 1920x1080 docked. Above Resolution it supersamples (smoothest at\n"
    "; exactly double, e.g. 3840x2160 for 1920x1080).\n"
    "RenderResolution = Auto\n"
    "\n"
    "; The picture's shape: Auto (the window's) or a ratio such as 21:9. Wider than 16:9 (ultrawide screens) shows\n"
    "; more to the sides, 16:10 a little more above and below. Narrower than 16:10 (4:3) gets 16:10 with bars.\n"
    "AspectRatio = Auto\n"
    "\n"
    "; Field of view: 1.0 is the game's, above 1.0 (1.1, 1.2) shows more, below 1.0 less. 0.5 to 2.\n"
    "FOV = 1.0\n"
    "\n"
    "; Where the HUD goes on a picture wider than 16:9: Center (in the middle 16:9, as the game lays it out) or Edges\n"
    "; (out to the sides of the screen). 21:9 or 32:9 spread it that wide, at most to the sides.\n"
    "UIAnchor = Center\n"
    "\n"
    "; Dock mode: On fixes the 3D resolution to a native profile (720p, or 1080p when Resolution is at least that big)\n"
    "; for a stable frame rate, with the HUD drawn at its native 720-line scale (crisp fonts) and FSR sharpening.\n"
    "; Toggled from the menu (Gráficos -> MODO DOCK), which writes this key. Off: RenderResolution decides. [AllGenU]\n"
    "DockMode = Off\n"
    "\n"
    "[Anti-aliasing]\n"
    "; MSAA: Off, 2x, 4x or 8x. Smooths the edges of 3D geometry and of some cut-out leaves and fences; each step\n"
    "; costs GPU time and video memory, a lot on older cards (an R9 270X at 1080p: 56 fps off, 35 at 2x, 28 at 4x).\n"
    "; F7 cycles through them while playing, to compare. (The Switch version has none.)\n"
    "MSAA = Off\n"
    "\n"
    "; FXAA: On or Off. The game's own edge smoothing: a filter over the finished image, slightly blurry.\n"
    "; Off by default (and not in the menu any more: MSAA below is off too). [AllGenU]\n"
    "FXAA = Off\n"
    "\n"
    "[Performance]\n"
    "; Frame rate cap: 30 (the Switch version's rate) to 240. Game speed stays the same at any rate.\n"
    "; 0 removes the cap (the infinity entry in the menu): the GPU sets the pace. [AllGenU]\n"
    "FPS = 60\n"
    "\n"
    "; ShowFPS: On draws the frame rate (and the frame time) in the top right corner of the screen, over the game.\n"
    "; Toggled from the menu (Gráficos -> CONTADOR DE FPS). [AllGenU]\n"
    "ShowFPS = Off\n"
    "\n"
    "; VSync: On or Off. On: exactly one frame per display retrace, no tearing: a 60 Hz monitor shows 60 (the engine is\n"
    "; paced a hair above the panel so it never misses a retrace), and a target above the refresh is shown at the refresh\n"
    "; rate. Off: frames are shown as soon as they are done, up to FPS (tearing possible). [AllGenU]\n"
    "VSync = On\n"
    "\n"
    "[Controls]\n"
    "; Controller face buttons: Position or Label.\n"
    "; Position: the Switch layout the game is made for - the right button is A (confirm), the bottom one B, the left\n"
    ";   one Y, the top one X, whatever is printed on them (on an Xbox controller the in-game icons then don't match).\n"
    "; Label: by the letter printed on the button (an Xbox controller's A is A; the icons match, the layout is awkward).\n"
    "FaceButtons = Position\n"
    "\n"
    "; The controller while the game's window is in the background (a wiki in front of it, or on another screen):\n"
    "; On (it keeps working) or Off. The keyboard and mouse only work while the window has the focus.\n"
    "BackgroundInput = On\n"
    "\n"
    "; [AllGenU] KeyMap: the keyboard key of each Switch button, as SDL scancode numbers, in this order:\n"
    ";   A, B, X, Y, LStick, RStick, L, R, ZL, ZR, Plus, Minus, Left, Up, Right, Down\n"
    "; 0 leaves a button to the mouse / the pad. Written by the menu (Controles): click a row and press a key.\n"
    "; Defaults: R (right bumper, run) on Shift, ZL on Tab, both as asked; the walk key moved to Alt.\n"
    "KeyMap = 0,44,0,21,224,6,20,225,43,23,40,42,80,82,79,81\n"
    "\n"
    "; [AllGenU] WalkKey: held, the left stick is tilted half way (walking). Left Alt by default.\n"
    "WalkKey = 226\n"
    "\n"
    "[Mouse]\n"
    "; Mouse look: click in the game window to capture the mouse (it then turns the camera, as the right stick), Esc\n"
    "; lets it go. Sensitivity: 1 = full stick tilt at a brisk mouse movement; 0.5 slower, 2 faster (0.1 to 10). The\n"
    "; game's own camera options (speed, inverted axes) apply on top.\n"
    "MouseSensitivity = 1.0\n"
    "\n"
    "[Multiplayer]\n"
    "; Local Play (the Switch's wireless play with people nearby) over the network. Create a hub under Local Play to\n"
    "; host: the others reach your computer on this port. Over the internet the host forwards it in their router,\n"
    "; TCP and UDP (TCP alone works too, but copes worse with a bad connection); nobody else needs to.\n"
    "Port = 39500\n"
    "\n"
    "; Search Hubs finds hubs on your LAN by itself. To reach a host elsewhere, put its address here: an IP or name,\n"
    "; with :port if it isn't the Port above. Several: separate them with commas. Example: Join = 203.0.113.7:39500\n"
    "Join =\n"
    "\n"
    "; Small monsters (Jaggi, Aptonoth...) synced between the players in a zone, like large monsters are: On or Off.\n"
    "; Off is the original game: each player sees them in different places. Everyone in the hunt should use the same.\n"
    "SyncSmallMonsters = On\n"
    "\n"
    "[Monster HP]\n"
    "; Large monster HP in Hub quests by the number of hunters on the quest. Each quest sets its monsters' HP as a\n"
    "; multiplier (most Hub quests are made for about two hunters); these factors multiply it: 0.6 = 60% of the\n"
    "; quest's HP, 1 = the original game. Only HP changes: attack, defense, size and the rest stay as they are.\n"
    "; Applies to every quest but the Village ones (played alone anyway): Hub and G Rank, Arena, Special Permit,\n"
    "; Event and Hub Prowler quests. In a hub, everyone uses the host's settings (each game computes the HP itself).\n"
    "1Player = 1.0\n"
    "2Players = 1.0\n"
    "3Players = 1.0\n"
    "4Players = 1.0\n"
    "\n"
    "; One quest set apart: Quest<number> = factors for 1, 2, 3 and 4 hunters (or one factor for all). The log\n"
    "; (log.txt) names the quest number whenever a monster's HP is set, e.g. \"[hpscale] quest 10107, ...\".\n"
    "; Example: Quest10107 = 1.0, 1.0, 1.2, 1.4\n"
    "\n"
    "[Village Quests]\n"
    "; Village quests at the Hub's and the Hunters' Pub's quest counter, to play them together: On or Off. In the\n"
    "; counter's rank menu or star menu, L or R switches between the Hub's quests and the Village quests (the same\n"
    "; list as at a village: what your village progress has unlocked).\n"
    "VillageQuestsInHub = On\n"
    "\n"
    "; Village quests are made for one hunter: posted in the Hub or Pub, their large monsters' HP is multiplied by this\n"
    "; first, then by the [Monster HP] factor for the number of hunters. 1 = as in the village.\n"
    "HubHP = 2.0\n"
    "\n"
    "[Bonus Content]\n"
    "; The free downloads (event and challenge quests, item packs, Palicos) can't be fetched from Capcom's server any\n"
    "; more. On: they are copied from a save that has them (a \"donor\" save file named system, in a folder called bonus\n"
    "; next to allgenurecomp.exe) into a save that has never had any downloads, such as a new one. Only the download data\n"
    "; is written, once; saves that already have downloads are left alone. Without a donor save nothing happens.\n"
    "; Off: saves are never changed.\n"
    "BonusContent = On\n"
    "\n"
    "[Interface]\n"
    "; KZMUI (the menu): Theme (Bherna, Kokoto, Yukumo, Rathalos Carmesi, Pokke, Soaratorium, Wycademy, Ancestral,\n"
    "; Deviljho, Dunas, Obsidiana, Halloween), Geometry (Rounded or Sharp) and Font (Segoe UI, Consolas, Bahnschrift,\n"
    "; Arial, Verdana). Changed from the menu, Apariencia. [AllGenU]\n"
    "Theme = Rathalos Carmesi\n"
    "Geometry = Rounded\n"
    "Font = Segoe UI\n"
    "\n"
    "[Mods]\n"
    "; ModsEnabled: On loads the mods/ folder at start, Off leaves it out (needs a restart). [AllGenU]\n"
    "ModsEnabled = On\n"
    "\n"
    "[Upscaling]\n"
    "; FSR 1.0 (AMD FidelityFX Super Resolution): Off or On. The present draw upscales the 3D image to the window\n"
    "; with edge-adaptive filtering (EASU) plus a contrast-adaptive sharpening pass (RCAS). Cheap, and toggled from\n"
    "; the menu (Gráficos -> ESCALADO FSR 1.0) at any time, no restart needed. [AllGenU]\n"
    "FSR = Off\n"
    "\n"
    "; FsrScale: the 3D render scale FSR starts from, in percent of the window (50..100). 100 renders at the window's\n"
    "; size (only RCAS sharpening applies); 75 gives EASU a real 1.33x upscale, which is where FSR shows most. It is\n"
    "; the slider in the menu (Gráficos -> ESCALADO FSR 1.0) and applies at start. [AllGenU]\n"
    "FsrScale = 100\n"
    "\n"
    "; Sharpness: how much RCAS sharpens, 0 (softest, safest for thin HUD text) to 100 (Extremo). The menu offers\n"
    "; four presets (Nítido 55, Muy nítido 70, Ultra nítido 85, Extremo 100) and a slider; applied at once. [AllGenU]\n"
    "Sharpness = 55\n"
    "\n"
    "[Image]\n"
    "; HDR: On applies the port's software colour lift (more saturation and a gentle contrast curve) over the finished\n"
    "; image, in the present pass. Off by default. [AllGenU]\n"
    "HDR = Off\n"
    "\n"
    "; FalseDLSS: On applies the \"False DLSS\" look: slightly desaturated, brighter, with the highlights compressed so\n"
    "; the game's glow reads as a clean image instead of blooming. Off by default. [AllGenU]\n"
    "FalseDLSS = Off\n";

static void ini_path(char *buf, size_t size) {
    const char *e = getenv("MHGU_CONFIG");
    if (e && *e) { snprintf(buf, size, "%s", e); return; }
    snprintf(buf, size, "allgenurecomp.ini");  // (started on its own the game runs in its folder, or MHGU_HOME: main.c)
}

// lower case, spaces and '_' / '-' removed: "Borderless Window" == "borderlesswindow"
static void squash(char *s) {
    char *d = s;
    for (; *s; s++) if (!isspace((unsigned char)*s) && *s != '_' && *s != '-') *d++ = (char)tolower((unsigned char)*s);
    *d = 0;
}

static bool parse_size(const char *v, int *w, int *h) {
    int a, b;
    char x;
    if (sscanf(v, "%d %c %d", &a, &x, &b) != 3 || (x != 'x' && x != 'X' && x != '*')) return false;
    if (a < 320 || b < 180 || a > 16384 || b > 16384) return false;
    *w = a, *h = b;
    return true;
}

// [Monster HP] factors: "0.6", "0.6x", or a comma separated list of up to four (squashed: no spaces). The count read,
// 0 when one is not a number in HP_SCALE_MIN..HP_SCALE_MAX.
static int parse_factors(const char *v, float f[4]) {
    int n = 0;
    while (*v && n < 4) {
        char *end;
        float x = strtof(v, &end);
        if (end == v || !(x >= HP_SCALE_MIN && x <= HP_SCALE_MAX)) return 0;
        f[n++] = x;
        v = end + (*end == 'x');
        if (*v == ',') v++;
        else if (*v) return 0;
    }
    return *v ? 0 : n;
}

// The vertical field of view (radians) a camera of this aspect gets (tools/gen.py PATCHES: the three callers of the
// perspective matrix that load a camera's). One made for the screen (its 16:9 aspect replaced with g_render_aspect)
// gets the FOV factor and, on a picture narrower than 16:9 (16:10), the height that keeps 16:9's width (Vert+): more
// above and below instead of less to the sides. Other cameras keep their own.
float camera_fovy(float fovy, float aspect) {
    if (aspect != g_render_aspect) return fovy;
    fovy *= g_fov_scale;
    if (aspect < 16.0f / 9.0f) fovy = 2.0f * atanf(tanf(fovy * 0.5f) * (16.0f / 9.0f) / aspect);
    return fovy;
}

// A picture shape: "Auto" (0), "21:9" / "21/9", or a number (0.5 .. 10).
static bool parse_aspect(const char *v, float *out) {
    float n, d, a;
    char sep, *end;
    if (!strcasecmp(v, "auto")) { *out = 0; return true; }
    if (sscanf(v, "%f %c %f", &n, &sep, &d) == 3 && (sep == ':' || sep == '/') && n > 0 && d > 0) {
        a = n / d;
    } else {
        a = strtof(v, &end);
        if (end == v || *end) return false;
    }
    if (a < 0.5f || a > 10.0f) return false;
    *out = a;
    return true;
}

// UIAnchor: Center (also 1, 16:9), 21:9 (2), 32:9 (3) or Edges, as Config.ui_anchor's UI_*
static bool parse_anchor(const char *v, int *out) {
    if (!strcasecmp(v, "center") || !strcmp(v, "1") || !strcmp(v, "16:9")) *out = UI_CENTER;
    else if (!strcmp(v, "2") || !strcmp(v, "21:9")) *out = UI_21_9;
    else if (!strcmp(v, "3") || !strcmp(v, "32:9")) *out = UI_32_9;
    else if (!strcasecmp(v, "edges")) *out = UI_EDGES;
    else return false;
    return true;
}

static int desktop_w, desktop_h;
static bool render_auto = true;
static bool keymap_seen, walk_seen;  // [AllGenU] (a settings file from an older build has neither: defaults below)

// [AllGenU] the keyboard map's defaults: R (right bumper, run) on Shift, ZL on Tab as asked, RStick on C (Alt is the
// walk key now), everything else where the port always had it.
void config_keybinds_default(int *out16) {
    static const int d[16] = {SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_SPACE, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_R,
                              SDL_SCANCODE_LCTRL, SDL_SCANCODE_C, SDL_SCANCODE_Q, SDL_SCANCODE_LSHIFT,
                              SDL_SCANCODE_TAB, SDL_SCANCODE_T, SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE,
                              SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN};
    memcpy(out16, d, sizeof d);
}

static void set_key(const char *key, char *val, int line) {
    char k[64], v[256];
    snprintf(k, sizeof k, "%s", key);
    snprintf(v, sizeof v, "%s", val);
    squash(k);
    squash(v);
    bool ok = true;
    if (!strcmp(k, "mode")) {
        if (!strcmp(v, "fullscreen") || !strcmp(v, "exclusive") || !strcmp(v, "exclusivefullscreen"))
            g_config.display_mode = DISPLAY_FULLSCREEN;
        else if (!strcmp(v, "windowed") || !strcmp(v, "window")) g_config.display_mode = DISPLAY_WINDOWED;
        else if (!strncmp(v, "borderless", 10)) g_config.display_mode = DISPLAY_BORDERLESS;
        else ok = false;
    } else if (!strcmp(k, "resolution")) {
        if (!strcmp(v, "desktop") || !strcmp(v, "auto")) g_config.width = g_config.height = 0;
        else ok = parse_size(v, &g_config.width, &g_config.height);
    } else if (!strcmp(k, "renderresolution")) {
        if (!strcmp(v, "auto")) render_auto = true;
        else if ((ok = parse_size(v, &g_config.render_w, &g_config.render_h))) render_auto = false;
    } else if (!strcmp(k, "aspectratio")) {
        ok = parse_aspect(v, &g_config.aspect_ratio);
    } else if (!strcmp(k, "fov")) {
        float f = strtof(v, NULL);
        if ((ok = f >= 0.5f && f <= 2.0f)) g_config.fov = f;
    } else if (!strcmp(k, "uianchor")) {
        ok = parse_anchor(v, &g_config.ui_anchor);
    } else if (!strcmp(k, "msaa")) {
        if (!strcmp(v, "off") || !strcmp(v, "none") || !strcmp(v, "0") || !strcmp(v, "1") || !strcmp(v, "1x"))
            g_config.msaa = 1;
        else if (!strcmp(v, "2x") || !strcmp(v, "2")) g_config.msaa = 2;
        else if (!strcmp(v, "4x") || !strcmp(v, "4")) g_config.msaa = 4;
        else if (!strcmp(v, "8x") || !strcmp(v, "8")) g_config.msaa = 8;
        else ok = false;
    } else if (!strcmp(k, "backgroundinput")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.background_input = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.background_input = 0;
        else ok = false;
    } else if (!strcmp(k, "fxaa")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.fxaa = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.fxaa = 0;
        else ok = false;
    } else if (!strcmp(k, "vsync")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.vsync = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.vsync = 0;
        else ok = false;
    } else if (!strcmp(k, "fsr")) {  // [AllGenU] FSR 1.0 in the present draw (nvn/vulkan/exec.c)
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.fsr = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.fsr = 0;
        else ok = false;
    } else if (!strcmp(k, "sharpness")) {  // [AllGenU] FSR RCAS amount: 0..100, or the old Soft / Balanced / Sharp
        if (!strcmp(v, "soft") || !strcmp(v, "low")) g_config.fsr_sharp = 0;
        else if (!strcmp(v, "balanced") || !strcmp(v, "medium") || !strcmp(v, "default")) g_config.fsr_sharp = 55;
        else if (!strcmp(v, "sharp") || !strcmp(v, "high")) g_config.fsr_sharp = 85;
        else if (isdigit((unsigned char)v[0]) && atoi(v) >= 0 && atoi(v) <= 100) g_config.fsr_sharp = atoi(v);
        else ok = false;
    } else if (!strcmp(k, "fsrscale")) {  // [AllGenU] FSR's internal render scale, percent of the window
        int n = atoi(v);
        if ((ok = isdigit((unsigned char)v[0]) && n >= 50 && n <= 100)) g_config.fsr_scale = n;
    } else if (!strcmp(k, "hdr")) {  // [AllGenU] the HDR look of the present pass (runtime/nvn/vulkan/shaders.c)
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.hdr = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.hdr = 0;
        else ok = false;
    } else if (!strcmp(k, "falsedlss")) {  // [AllGenU] the "False DLSS" look
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.false_dlss = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.false_dlss = 0;
        else ok = false;
    } else if (!strcmp(k, "keymap")) {  // [AllGenU] 16 SDL scancodes, one per Switch button (runtime/host_input.c)
        int got = 0, val = 0;
        for (const char *p = v;; p++) {
            if (isdigit((unsigned char)*p)) {
                val = val * 10 + (*p - '0');
                if (val > 1000) break;
            } else if (*p == ',' || !*p) {
                if (got < 16) g_config.keybinds[got] = val < SDL_SCANCODE_COUNT ? val : 0;
                got++, val = 0;
                if (!*p) break;
            } else {
                break;
            }
        }
        if (got >= 16) keymap_seen = true;
        else ok = false;
    } else if (!strcmp(k, "walkkey")) {  // [AllGenU] the half-tilt (walk) key
        int n = atoi(v);
        if ((ok = isdigit((unsigned char)v[0]) && n >= 0 && n < SDL_SCANCODE_COUNT)) g_config.walk_key = n, walk_seen = true;
    } else if (!strcmp(k, "dockmode")) {  // [AllGenU] Dock mode: fixed native resolution profile (kzmui.cpp)
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.dock_mode = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.dock_mode = 0;
        else ok = false;
    } else if (!strcmp(k, "facebuttons")) {
        if (!strcmp(v, "position") || !strcmp(v, "positional") || !strcmp(v, "nintendo"))
            g_config.face_buttons = FACE_POSITION;
        else if (!strcmp(v, "label") || !strcmp(v, "labels") || !strcmp(v, "xbox"))
            g_config.face_buttons = FACE_LABEL;
        else ok = false;
    } else if (!strcmp(k, "mousesensitivity")) {
        float f = strtof(v, NULL);
        if ((ok = f >= 0.1f && f <= 10.0f)) g_config.mouse_sensitivity = f;
    } else if (!strcmp(k, "port")) {
        int p = atoi(v);
        if (p > 0 && p < 65536) g_config.ldn_port = p;
        else ok = false;
    } else if (!strcmp(k, "join")) {
        snprintf(g_config.ldn_join, sizeof g_config.ldn_join, "%s", val);  // (as written: names keep their dashes)
    } else if (!strcmp(k, "bonuscontent")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.bonus_content = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.bonus_content = 0;
        else ok = false;
    } else if (!strcmp(k, "villagequestsinhub")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.village_in_hub = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.village_in_hub = 0;
        else ok = false;
    } else if (!strcmp(k, "hubhp")) {
        float f[4];
        if ((ok = parse_factors(v, f) == 1)) g_config.hp.village = f[0];
    } else if (!strcmp(k, "syncsmallmonsters")) {
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.sync_small_monsters = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.sync_small_monsters = 0;
        else ok = false;
    } else if (!strcmp(k, "theme")) {  // [AllGenU] KZMUI: the theme by the key it writes (runtime/kzmui.cpp)
        static const char *const names[12] = {"bherna", "kokoto", "yukumo", "rathaloscarmesi", "pokke", "soaratorium",
                                              "wycademy", "ancestral", "deviljho", "dunas", "obsidiana", "halloween"};
        ok = false;
        for (int i = 0; i < 12; i++)
            if (!strcmp(v, names[i])) g_config.ui_theme = i, ok = true;
        if (!ok && isdigit((unsigned char)v[0]) && atoi(v) >= 0 && atoi(v) < 12) g_config.ui_theme = atoi(v), ok = true;
    } else if (!strcmp(k, "geometry")) {  // [AllGenU] KZMUI: Rounded or Sharp
        if (!strcmp(v, "rounded")) g_config.ui_sharp = 0;
        else if (!strcmp(v, "sharp")) g_config.ui_sharp = 1;
        else ok = false;
    } else if (!strcmp(k, "font")) {  // [AllGenU] KZMUI: Segoe UI, Consolas, Bahnschrift, Arial or Verdana
        static const char *const names[5] = {"segoeui", "consolas", "bahnschrift", "arial", "verdana"};
        ok = false;
        for (int i = 0; i < 5; i++)
            if (!strcmp(v, names[i])) g_config.ui_font = i, ok = true;
    } else if (!strcmp(k, "showfps")) {  // [AllGenU] the FPS counter over the game (runtime/overlay.cpp)
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.show_fps = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.show_fps = 0;
        else ok = false;
    } else if (!strcmp(k, "modsenabled")) {  // [AllGenU] KZMUI: the mods/ folder at start
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) g_config.mods_enabled = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) g_config.mods_enabled = 0;
        else ok = false;
    } else if (k[0] >= '1' && k[0] <= '4' && (!strcmp(k + 1, "player") || !strcmp(k + 1, "players"))) {
        float f[4];
        if ((ok = parse_factors(v, f) == 1)) g_config.hp.scale[k[0] - '1'] = f[0];
    } else if (!strncmp(k, "quest", 5) && isdigit((unsigned char)k[5])) {
        HpScale *h = &g_config.hp;
        char *end;
        unsigned long q = strtoul(k + 5, &end, 10);
        float f[4];
        int n = parse_factors(v, f);
        if ((ok = !*end && q && (n == 1 || n == 4))) {
            int i = 0;
            while (i < h->quests && h->quest[i].quest != q) i++;
            if (i == HP_QUESTS_MAX) { LOG("[config] line %d: more than %d quests set apart\n", line, HP_QUESTS_MAX); return; }
            h->quest[i].quest = (uint32_t)q;
            for (int j = 0; j < 4; j++) h->quest[i].scale[j] = f[n == 4 ? j : 0];
            if (i == h->quests) h->quests++;
        }
    } else if (!strcmp(k, "fps") || !strcmp(k, "framerate")) {
        int f = atoi(v);
        if (f == 0 || (f >= 30 && f <= 240)) g_config.fps = f;  // 0 = no cap ([AllGenU] "∞" in the menu)
        else ok = false;
    } else {
        LOG("[config] line %d: unknown setting \"%s\" (ignored)\n", line, key);
        return;
    }
    if (!ok) LOG("[config] line %d: \"%s\" is not a value %s takes; using the default\n", line, val, key);
}

// [AllGenU] KZMUI: persist one key of the settings file, keeping every other line (comments included) as it is. The
// menu has already changed g_config; this only makes the change survive a restart.
void config_set(const char *section, const char *key, const char *value) {
    char path[512], tmp[520], line[1024];
    ini_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *in = fopen(path, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) {
        if (in) fclose(in);
        LOG("[config] %s cannot be written\n", path);
        return;
    }
    bool in_section = false, wrote = false, section_seen = false;
    while (in && fgets(line, sizeof line, in)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') {
            if (in_section && !wrote) {  // (the key was not in its section: added before the next one)
                fprintf(out, "%s = %s\n", key, value);
                wrote = true;
            }
            char name[128];
            size_t n = 0;
            for (const char *p = s + 1; *p && *p != ']' && n + 1 < sizeof name; p++) name[n++] = *p;
            name[n] = 0;
            in_section = !strcmp(name, section);
            if (in_section) section_seen = true;
            fputs(line, out);
            continue;
        }
        if (in_section && !wrote) {
            char k[128];
            size_t n = 0;
            const char *p = s;
            while (*p && *p != '=' && *p != '\n' && *p != '\r' && n + 1 < sizeof k) k[n++] = *p++;
            while (n && (k[n - 1] == ' ' || k[n - 1] == '\t')) n--;
            k[n] = 0;
            squash(k);
            char want[128];
            snprintf(want, sizeof want, "%s", key);
            squash(want);
            if (*p == '=' && !strcmp(k, want)) {  // (this key: replaced, its comment stays above it)
                fprintf(out, "%s = %s\n", key, value);
                wrote = true;
                continue;
            }
        }
        fputs(line, out);
    }
    if (!wrote) {
        if (in_section) fprintf(out, "%s = %s\n", key, value);  // (the last section, key missing)
        else if (!section_seen) fprintf(out, "\n[%s]\n%s = %s\n", section, key, value);  // (a new section)
    }
    if (in) fclose(in);
    fclose(out);
    remove(path);
    if (rename(tmp, path) && (in = fopen(tmp, "rb"))) fclose(in);  // (Windows: rename needs the target gone, above)
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

// The largest size inside w x h matching the given aspect ratio, even numbers.
static void fit_aspect(int w, int h, float aspect, int *rw, int *rh) {
    if (aspect <= 0.0f) aspect = 16.0f / 9.0f;
    if ((float)w / (float)h > aspect)
        w = (int)((float)h * aspect + 0.5f);
    else
        h = (int)((float)w / aspect + 0.5f);
    *rw = w & ~1, *rh = h & ~1;
}

// [AllGenU] Dock mode's profile: a native 720 lines, or 1080 when the window has the pixels, widened to the picture's
// shape (the same vertical resolution the menu promises, whatever RenderResolution says).
static void dock_render_size(int tw, int th, float aspect, int *w, int *h) {
    if (aspect <= 0.0f) aspect = 16.0f / 9.0f;
    int bh = (tw >= 1920 && th >= 1080) ? 1080 : 720;
    int bw = (int)((float)bh * aspect + 0.5f);
    *w = bw & ~1, *h = bh & ~1;
}

// Sections of DEFAULT_INI the file lacks (written by an older build) are added with their defaults at its end; the
// player's own settings stay as they are.
static void append_missing_sections(const char *path, const char *seen) {
    FILE *o = NULL;
    for (const char *p = strstr(DEFAULT_INI, "\n["); p;) {
        const char *start = p + 1, *end = strstr(start, "\n["), *close = strchr(start, ']');
        p = end;
        char name[64], key[70];
        snprintf(name, sizeof name, "%.*s", (int)(close - start - 1), start + 1);
        for (char *q = name; *q; q++) *q = (char)tolower((unsigned char)*q);
        snprintf(key, sizeof key, "|%s|", name);
        if (strstr(seen, key)) continue;
        if (!o && !(o = fopen(path, "a"))) return;
        fprintf(o, "\n%.*s", (int)(end ? end - start : (long)strlen(start)), start);
        LOG("[config] added the [%s] section to %s\n", name, path);
    }
    if (o) fclose(o);
}

void config_load(void) {
    char path[1100];
    ini_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) {
        FILE *o = fopen(path, "w");
        if (o) { fputs(DEFAULT_INI, o); fclose(o); LOG("[config] wrote the default settings to %s\n", path); }
        else LOG("[config] %s is missing and could not be written; using the defaults\n", path);
    } else {
        char buf[512], seen[512] = "|";
        int line = 0;
        while (fgets(buf, sizeof buf, f)) {
            line++;
            char *s = trim(buf);
            if (*s == '[' && strchr(s, ']') && strlen(seen) + strlen(s) + 2 < sizeof seen) {
                for (char *q = s + 1; *q != ']'; q++) {
                    size_t n = strlen(seen);
                    seen[n] = (char)tolower((unsigned char)*q), seen[n + 1] = 0;
                }
                strcat(seen, "|");
            }
            if (!*s || *s == ';' || *s == '#' || *s == '[') continue;
            char *eq = strchr(s, '=');
            if (!eq) { LOG("[config] line %d: no '=' (ignored)\n", line); continue; }
            *eq = 0;
            char *val = trim(eq + 1), *cmt = strpbrk(val, ";#");
            if (cmt) *cmt = 0;
            set_key(trim(s), trim(val), line);
        }
        fclose(f);
        append_missing_sections(path, seen);
    }
    if (!keymap_seen) config_keybinds_default(g_config.keybinds);  // [AllGenU]
    if (!walk_seen) g_config.walk_key = SDL_SCANCODE_LALT;        // [AllGenU]

    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    desktop_w = dm ? dm->w : 1920, desktop_h = dm ? dm->h : 1080;
    if (!g_config.width) g_config.width = desktop_w, g_config.height = desktop_h;

    const char *e = getenv("MHGU_WINDOW");
    int w, h;
    if (e && parse_size(e, &w, &h)) g_config.display_mode = DISPLAY_WINDOWED, g_config.width = w, g_config.height = h;
    if (getenv("MHGU_HIDDEN")) g_config.display_mode = DISPLAY_WINDOWED;
    if ((e = getenv("MHGU_MSAA"))) g_config.msaa = atoi(e) >= 8 ? 8 : atoi(e) >= 4 ? 4 : atoi(e) >= 2 ? 2 : 1;
    if ((e = getenv("MHGU_FXAA"))) g_config.fxaa = atoi(e) != 0;
    if ((e = getenv("MHGU_BACKGROUND_INPUT"))) g_config.background_input = atoi(e) != 0;
    if ((e = getenv("MHGU_PAD_POSITIONAL"))) g_config.face_buttons = atoi(e) ? FACE_POSITION : FACE_LABEL;
    if ((e = getenv("MHGU_LDN_PORT")) && atoi(e) > 0) g_config.ldn_port = atoi(e);
    if ((e = getenv("MHGU_SYNC_ZAKO"))) g_config.sync_small_monsters = atoi(e) != 0;
    if ((e = getenv("MHGU_LDN_JOIN"))) snprintf(g_config.ldn_join, sizeof g_config.ldn_join, "%s", e);
    if ((e = getenv("MHGU_ASPECT"))) parse_aspect(e, &g_config.aspect_ratio);
    if ((e = getenv("MHGU_FOV")) && strtof(e, NULL) >= 0.5f && strtof(e, NULL) <= 2.0f) g_config.fov = strtof(e, NULL);
    if ((e = getenv("MHGU_UI_ANCHOR"))) parse_anchor(e, &g_config.ui_anchor);
    e = getenv("MHGU_RENDER");
    if (e && parse_size(e, &w, &h)) render_auto = false, g_config.render_w = w, g_config.render_h = h;

    // [AllGenU] the real window: Fullscreen switches to the closest monitor mode (1920x1080 asked on a 1366x768 panel
    // shows 1366x768), Borderless covers the desktop. Using the requested size here made the 3D resolution larger than
    // the swapchain, so FSR was skipped (downscale) even though the menu said it was on.
    int tw = g_config.width, th = g_config.height;
    if (g_config.display_mode != DISPLAY_WINDOWED) tw = tw < desktop_w ? tw : desktop_w, th = th < desktop_h ? th : desktop_h;
    // The picture's shape: the window's or AspectRatio's, at least 16:10. The game lays its HUD out for 16:9, which
    // still fits down to 16:10 (a little flatter, nvn/vulkan/exec.c), so a narrower window (4:3) shows 16:10 with bars.
    // Wider than 16:9 shows more to the sides, 16:9 to 16:10 more above and below (camera_fovy, gen.py PATCHES).
    float aspect = g_config.aspect_ratio > 0 ? g_config.aspect_ratio : (float)tw / (float)th;
    if (aspect < 1.6f - 0.005f) {
        if (g_config.aspect_ratio > 0)
            LOG("[config] AspectRatio %.3f is narrower than 16:10, which the HUD needs: using 16:10\n", aspect);
        aspect = 1.6f;
    }
    if (fabsf(aspect - 16.0f / 9.0f) < 0.01f) aspect = 16.0f / 9.0f;  // (1366x768 and the like)
    if (g_config.dock_mode) {
        dock_render_size(tw, th, aspect, &w, &h);  // [AllGenU] Dock mode: its profile wins over RenderResolution
    } else if (render_auto) {
        fit_aspect(tw, th, aspect, &w, &h);
    } else {
        fit_aspect(g_config.render_w, g_config.render_h, aspect, &w, &h);
        if (w != g_config.render_w || h != g_config.render_h)
            LOG("[config] RenderResolution %dx%d is not the picture's shape (%.3f): using %dx%d\n", g_config.render_w,
                g_config.render_h, aspect, w, h);
    }
    // [AllGenU] FSR renders the 3D scene at FsrScale percent of the window, so EASU has a real upscale to do (at 100%
    // only RCAS sharpening applies). It wins over Dock mode / RenderResolution while FSR is on.
    if (g_config.fsr && g_config.fsr_scale > 0 && g_config.fsr_scale < 100) {
        int sw = (int)((float)tw * g_config.fsr_scale / 100.0f + 0.5f);
        int sh = (int)((float)th * g_config.fsr_scale / 100.0f + 0.5f);
        fit_aspect(sw, sh, aspect, &w, &h);
    }
    g_config.render_w = w, g_config.render_h = h;
    g_config.render_auto = render_auto ? 1 : 0;  // [AllGenU] (the menu shows and sets it: runtime/kzmui.cpp)
    g_render_size[0] = (uint32_t)w;
    g_render_size[1] = (uint32_t)h;
    g_render_aspect = aspect;
    g_fov_scale = g_config.fov;

    // The HUD's width: 16:9 (Center), 21:9, 32:9 or the picture's (Edges), at most the picture's; the game's 2D canvas
    // gets that width (gen.py PATCHES), exec.c places the HUD's parts in it.
    float ui = g_config.ui_anchor == UI_EDGES  ? aspect
             : g_config.ui_anchor == UI_32_9 ? 32.0f / 9.0f
             : g_config.ui_anchor == UI_21_9 ? 21.0f / 9.0f
                                             : 16.0f / 9.0f;
    g_ui_anchor_aspect = ui < aspect ? ui : aspect;
    g_ui_size[1] = 720;
    g_ui_size[0] = (uint32_t)(720.0f * g_ui_anchor_aspect + 0.5f) & ~1u;
    if (g_ui_size[0] < 1280) g_ui_size[0] = 1280;

    static const char *const modes[] = {"windowed", "fullscreen", "borderless"};
    LOG("[config] %s: %s %dx%d, 3D %dx%d (shape %.3f, FOV x%.2f, HUD %.3f wide), %d fps, vsync %s, MSAA %dx, FXAA %s, "
        "face buttons by %s, controller %s in the background\n", path, modes[g_config.display_mode], g_config.width,
        g_config.height, g_config.render_w, g_config.render_h, g_render_aspect, g_fov_scale, g_ui_anchor_aspect,
        g_config.fps, g_config.vsync ? "on" : "off",
        g_config.msaa, g_config.fxaa ? "on" : "off", g_config.face_buttons == FACE_LABEL ? "label" : "position",
        g_config.background_input ? "on" : "off");
    const HpScale *hp = &g_config.hp;
    LOG("[config] monster HP in Hub quests: x%.2f / %.2f / %.2f / %.2f for 1-4 hunters, %d quest(s) set apart\n",
        hp->scale[0], hp->scale[1], hp->scale[2], hp->scale[3], hp->quests);
    LOG("[config] Village quests at the Hub counter: %s (their HP x%.2f first)\n", g_config.village_in_hub ? "on" : "off",
        hp->village);
}
