// Forge's ImGui overlay: its "forge" window (About, Loaded Plugins, Plugin UI: each plugin's settings) and whatever
// plugins draw every frame. On the console Forge draws it with its own NVN renderer onto the image about to be shown;
// here Dear ImGui's Vulkan backend draws it into the window image, over the game (runtime/nvn/vulkan/device.c).
//
// Threads:
//   - the game's thread builds the frame (runtime/forge.c's frame update, after the plugins' forge_onUpdate): the
//     plugins' ImGui code runs there (their calls: runtime/forge_imgui.c); texture uploads too, under the Vulkan lock
//   - the render thread draws a copy of the last frame's draw lists (taken under a lock of its own, so it never
//     waits for plugin code)
//   - the host's main thread passes input on: while the menu is open, keyboard and mouse go to ImGui, and the
//     controller moves through the menu (runtime/host_input.c then gives the game none)
// The menu opens and closes with the key of Forge's config (nativeNX/forge/config.ini [menu] key; Insert by default).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "imgui.h"
#ifndef MHGU_NULL_BACKEND
#include "backends/imgui_impl_vulkan.h"
#else  // (no renderer: no overlay)
struct ImGui_ImplVulkan_InitInfo { int unused; };
static void ImGui_ImplVulkan_NewFrame() {}
static void ImGui_ImplVulkan_UpdateTexture(ImTextureData *) {}
#endif

#include "config.h"  // [AllGenU] g_config (KZMUI's settings, the FPS counter)
#include "forge.h"
#include "kzmui.h"  // [AllGenU] the port's own interface (Escape)
#include "overlay.h"

#define LOG(...) fprintf(stderr, __VA_ARGS__)

extern "C" {
void vk_lock(void);
void vk_unlock(void);
SDL_Window *ui_window(void);
extern volatile uint64_t g_present_count;  // [AllGenU] frames the renderer presented (runtime/ui.c)
}

// [AllGenU] presented frames a second, measured from g_present_count: with V-Sync the engine is paced a hair above the
// panel (runtime/hle/vi.c) while FIFO shows exactly the panel's rate, so ImGui's Framerate would read 62 at a 60 Hz
// screen. The counter and the menu's telemetry must show what the player sees (60), so they use this instead.
static float g_present_fps;
float overlay_fps(void) { return g_present_fps; }

static std::atomic<bool> g_enabled, g_ready, g_open;
static std::atomic<bool> g_kz_open;  // [AllGenU] KZMUI (the port's menu, Escape) open
static int g_menu_key;
static float g_font_size;
static ImGuiStyle g_base_style;  // (unscaled; scaled copies follow the window's height)
static float g_scale;

// ------------------------------------------------------------------ input (main thread -> game thread)
struct Ev {
    enum { POS, BUTTON, WHEEL, KEY, TEXT, FOCUS, CLEAR } kind;
    bool down;
    int button;
    float x, y;
    ImGuiKey key;
    SDL_Keymod mods;
    char text[32];
};
static std::mutex g_in_lock;
static std::vector<Ev> g_in;
static OverlayPad g_pad;

static void queue(const Ev &e) {
    std::lock_guard<std::mutex> l(g_in_lock);
    if (g_in.size() < 4096) g_in.push_back(e);
}

static ImGuiKey key_of(SDL_Scancode sc, SDL_Keycode kc) {
    if (kc >= SDLK_A && kc <= SDLK_Z) return (ImGuiKey)(ImGuiKey_A + (int)(kc - SDLK_A));  // (letters as labelled)
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return (ImGuiKey)(ImGuiKey_A + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return (ImGuiKey)(ImGuiKey_1 + (sc - SDL_SCANCODE_1));
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return (ImGuiKey)(ImGuiKey_F1 + (sc - SDL_SCANCODE_F1));
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return (ImGuiKey)(ImGuiKey_Keypad1 + (sc - SDL_SCANCODE_KP_1));
    switch (sc) {
    case SDL_SCANCODE_0: return ImGuiKey_0;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_INSERT: return ImGuiKey_Insert;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_APOSTROPHE: return ImGuiKey_Apostrophe;
    case SDL_SCANCODE_COMMA: return ImGuiKey_Comma;
    case SDL_SCANCODE_MINUS: return ImGuiKey_Minus;
    case SDL_SCANCODE_PERIOD: return ImGuiKey_Period;
    case SDL_SCANCODE_SLASH: return ImGuiKey_Slash;
    case SDL_SCANCODE_SEMICOLON: return ImGuiKey_Semicolon;
    case SDL_SCANCODE_EQUALS: return ImGuiKey_Equal;
    case SDL_SCANCODE_LEFTBRACKET: return ImGuiKey_LeftBracket;
    case SDL_SCANCODE_BACKSLASH: return ImGuiKey_Backslash;
    case SDL_SCANCODE_RIGHTBRACKET: return ImGuiKey_RightBracket;
    case SDL_SCANCODE_GRAVE: return ImGuiKey_GraveAccent;
    case SDL_SCANCODE_CAPSLOCK: return ImGuiKey_CapsLock;
    case SDL_SCANCODE_SCROLLLOCK: return ImGuiKey_ScrollLock;
    case SDL_SCANCODE_NUMLOCKCLEAR: return ImGuiKey_NumLock;
    case SDL_SCANCODE_PRINTSCREEN: return ImGuiKey_PrintScreen;
    case SDL_SCANCODE_PAUSE: return ImGuiKey_Pause;
    case SDL_SCANCODE_KP_0: return ImGuiKey_Keypad0;
    case SDL_SCANCODE_KP_PERIOD: return ImGuiKey_KeypadDecimal;
    case SDL_SCANCODE_KP_DIVIDE: return ImGuiKey_KeypadDivide;
    case SDL_SCANCODE_KP_MULTIPLY: return ImGuiKey_KeypadMultiply;
    case SDL_SCANCODE_KP_MINUS: return ImGuiKey_KeypadSubtract;
    case SDL_SCANCODE_KP_PLUS: return ImGuiKey_KeypadAdd;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_KP_EQUALS: return ImGuiKey_KeypadEqual;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_LALT: return ImGuiKey_LeftAlt;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_RALT: return ImGuiKey_RightAlt;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_APPLICATION: return ImGuiKey_Menu;
    default: return ImGuiKey_None;
    }
}

static void set_open(bool open) {
    g_open = open;
    SDL_Window *w = ui_window();
    if (open) SDL_ShowCursor();  // (full screen hides it: runtime/ui.c)
    else if (w && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN)) SDL_HideCursor();
    Ev e = {};
    e.kind = Ev::CLEAR;  // (keys and buttons held as it opens or closes: released)
    queue(e);
    {   // [AllGenU] the controller state kept from the last time the menu was open must not be replayed on opening:
        // a stale "A" activated the focused item (a resolution / theme segment changed by itself).
        std::lock_guard<std::mutex> l(g_in_lock);
        memset(&g_pad, 0, sizeof g_pad);
    }
    LOG("[forge] menu %s\n", open ? "open" : "closed");
}

// [AllGenU] KZMUI's open state, beside Forge's window: Escape toggles it, the game gets no input while it is open
// (overlay_menu_open), the pointer comes back, and the keys held as it opens are released.
static void set_kz_open(bool open) {
    g_kz_open = open;
    SDL_Window *w = ui_window();
    if (open) SDL_ShowCursor();
    else if (w && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN)) SDL_HideCursor();
    Ev e = {};
    e.kind = Ev::CLEAR;
    queue(e);
    {   // [AllGenU] same as Forge's window: no stale controller press on opening
        std::lock_guard<std::mutex> l(g_in_lock);
        memset(&g_pad, 0, sizeof g_pad);
    }
    LOG("[kzmui] menu %s\n", open ? "open" : "closed");
}

bool overlay_event(const SDL_Event *e) {
    if (!g_enabled) return false;
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && (int)e->key.scancode == g_menu_key) {
        set_open(!g_open);
        return true;
    }
    // [AllGenU] Escape: KZMUI opens / closes (the port's menu, as the player expects). Taken before the game sees it.
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && e->key.scancode == SDL_SCANCODE_ESCAPE) {
        set_kz_open(!g_kz_open);
        return true;
    }
    // [AllGenU] both menus take the input: KZMUI's own state (Escape) and Forge's window were counted apart here, so
    // with only KZMUI open every mouse and key event went to the game and the menu could not be used at all.
    if (!g_open && !g_kz_open) return false;
    SDL_Window *w = ui_window();
    float density = w ? SDL_GetWindowPixelDensity(w) : 1.0f;  // (ImGui works in the image's pixels)
    Ev v = {};
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        v.kind = Ev::POS, v.x = e->motion.x * density, v.y = e->motion.y * density;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
        static const int map[] = {-1, ImGuiMouseButton_Left, ImGuiMouseButton_Middle, ImGuiMouseButton_Right, 3, 4};
        if (e->button.button >= 6 || map[e->button.button] < 0) return true;
        v.kind = Ev::BUTTON, v.button = map[e->button.button], v.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        v.x = e->button.x * density, v.y = e->button.y * density;
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        v.kind = Ev::WHEEL, v.x = -e->wheel.x, v.y = e->wheel.y;
        break;
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        v.kind = Ev::KEY, v.down = e->type == SDL_EVENT_KEY_DOWN, v.mods = e->key.mod;
        v.key = key_of(e->key.scancode, e->key.key);
        queue(v);
        // the port's own keys still work (screenshots, Alt+Enter full screen)
        bool port_key = (e->key.scancode >= SDL_SCANCODE_F1 && e->key.scancode <= SDL_SCANCODE_F12) ||
                        ((e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) &&
                         (e->key.mod & SDL_KMOD_ALT));
        return !port_key;
    }
    case SDL_EVENT_TEXT_INPUT:
        v.kind = Ev::TEXT;
        SDL_strlcpy(v.text, e->text.text, sizeof v.text);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: case SDL_EVENT_WINDOW_FOCUS_GAINED:
        v.kind = Ev::FOCUS, v.down = e->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        queue(v);
        return false;  // (the game's input sees it too)
    default:
        return false;
    }
    queue(v);
    return true;
}

void overlay_pad(const OverlayPad *p) {
    std::lock_guard<std::mutex> l(g_in_lock);
    g_pad = *p;
}

bool overlay_enabled(void) { return g_enabled; }
bool overlay_menu_open(void) { return g_open || g_kz_open; }

static void apply_input(ImGuiIO &io) {
    std::vector<Ev> evs;
    OverlayPad pad;
    {
        std::lock_guard<std::mutex> l(g_in_lock);
        evs.swap(g_in);
        pad = g_pad;
    }
    for (const Ev &e : evs) {
        switch (e.kind) {
        case Ev::POS: io.AddMousePosEvent(e.x, e.y); break;
        case Ev::BUTTON:
            io.AddMousePosEvent(e.x, e.y);
            io.AddMouseButtonEvent(e.button, e.down);
            break;
        case Ev::WHEEL: io.AddMouseWheelEvent(e.x, e.y); break;
        case Ev::KEY:
            io.AddKeyEvent(ImGuiMod_Ctrl, (e.mods & SDL_KMOD_CTRL) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (e.mods & SDL_KMOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (e.mods & SDL_KMOD_ALT) != 0);
            io.AddKeyEvent(ImGuiMod_Super, (e.mods & SDL_KMOD_GUI) != 0);
            if (e.key != ImGuiKey_None) io.AddKeyEvent(e.key, e.down);
            break;
        case Ev::TEXT: io.AddInputCharactersUTF8(e.text); break;
        case Ev::FOCUS: io.AddFocusEvent(e.down); break;
        case Ev::CLEAR:
            io.ClearInputKeys();
            io.ClearInputMouse();
            memset(&g_pad, 0, sizeof g_pad);
            pad = {};
            break;
        }
    }
    // the controller (as Forge maps it: A activates, B cancels, the left stick and the d-pad move)
    if (!g_open && !g_kz_open) pad = {};
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, pad.a);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, pad.b);
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, pad.x);
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, pad.y);
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, pad.up);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, pad.down);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, pad.left);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, pad.right);
    io.AddKeyEvent(ImGuiKey_GamepadL1, pad.l);
    io.AddKeyEvent(ImGuiKey_GamepadR1, pad.r);
    io.AddKeyEvent(ImGuiKey_GamepadStart, pad.plus);
    io.AddKeyEvent(ImGuiKey_GamepadBack, pad.minus);
    auto axis = [&](ImGuiKey k, float v) { io.AddKeyAnalogEvent(k, v > 0.2f, std::min(v, 1.0f)); };
    axis(ImGuiKey_GamepadLStickLeft, -pad.lx);
    axis(ImGuiKey_GamepadLStickRight, pad.lx);
    axis(ImGuiKey_GamepadLStickUp, pad.ly);
    axis(ImGuiKey_GamepadLStickDown, -pad.ly);
}

// ------------------------------------------------------------------ setup
static void forge_style(ImGuiStyle &style) {  // Forge's own (source/forge/graphics.cpp, Graphics::setupStyle)
    style.WindowPadding = {12.0f, 12.0f};
    style.WindowRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowMinSize = {20.0f, 20.0f};
    style.WindowTitleAlign = {0.5f, 0.5f};
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ChildRounding = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = {6.0f, 6.0f};
    style.FrameRounding = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = {12.0f, 6.0f};
    style.ItemInnerSpacing = {6.0f, 3.0f};
    style.CellPadding = {12.0f, 6.0f};
    style.IndentSpacing = 20.0f;
    style.ColumnsMinSpacing = 6.0f;
    style.ScrollbarSize = 12.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabMinSize = 12.0f;
    style.GrabRounding = 1.0f;
    style.TabRounding = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = {0.5f, 0.5f};
    style.SelectableTextAlign = {0.0f, 0.0f};
    const ImVec4 bg = {0.078f, 0.086f, 0.102f, 1.0f}, dark = {0.047f, 0.055f, 0.071f, 1.0f},
                 frame = {0.118f, 0.133f, 0.149f, 1.0f}, line = {0.157f, 0.169f, 0.192f, 1.0f},
                 hover = {0.196f, 0.176f, 0.545f, 1.0f}, active = {0.235f, 0.216f, 0.596f, 1.0f},
                 accent = {0.498f, 0.514f, 1.0f, 1.0f}, bar = {0.098f, 0.106f, 0.122f, 1.0f},
                 dim = {0.196f, 0.176f, 0.545f, 0.502f}, sep = {0.157f, 0.184f, 0.251f, 1.0f};
    ImVec4 *c = style.Colors;
    c[ImGuiCol_Text] = {1.0f, 1.0f, 1.0f, 1.0f};
    c[ImGuiCol_TextDisabled] = {0.275f, 0.318f, 0.451f, 1.0f};
    c[ImGuiCol_WindowBg] = c[ImGuiCol_ChildBg] = c[ImGuiCol_PopupBg] = c[ImGuiCol_BorderShadow] = bg;
    c[ImGuiCol_Border] = line;
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = line;
    c[ImGuiCol_FrameBgActive] = active;
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = dark;
    c[ImGuiCol_TitleBgCollapsed] = bg;
    c[ImGuiCol_MenuBarBg] = bar;
    c[ImGuiCol_ScrollbarBg] = dark;
    c[ImGuiCol_ScrollbarGrab] = c[ImGuiCol_ScrollbarGrabActive] = frame;
    c[ImGuiCol_ScrollbarGrabHovered] = line;
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = {0.537f, 0.553f, 1.0f, 1.0f};
    c[ImGuiCol_Button] = {0.257f, 0.267f, 0.554f, 1.0f};
    c[ImGuiCol_ButtonHovered] = c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ResizeGripHovered] = hover;
    c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderActive] = c[ImGuiCol_ResizeGripActive] = active;
    c[ImGuiCol_Header] = c[ImGuiCol_ResizeGrip] = frame;
    c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = sep;
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = dark;
    c[ImGuiCol_TabHovered] = frame;
    c[ImGuiCol_TabSelected] = bar;
    c[ImGuiCol_TabDimmedSelected] = bg;
    c[ImGuiCol_PlotLines] = {0.522f, 0.600f, 0.702f, 1.0f};
    c[ImGuiCol_PlotLinesHovered] = {0.039f, 0.980f, 0.980f, 1.0f};
    c[ImGuiCol_PlotHistogram] = {1.0f, 0.290f, 0.596f, 1.0f};
    c[ImGuiCol_PlotHistogramHovered] = {0.996f, 0.475f, 0.698f, 1.0f};
    c[ImGuiCol_TableHeaderBg] = c[ImGuiCol_TableBorderStrong] = dark;
    c[ImGuiCol_TableBorderLight] = {0.0f, 0.0f, 0.0f, 1.0f};
    c[ImGuiCol_TableRowBg] = frame;
    c[ImGuiCol_TableRowBgAlt] = bar;
    c[ImGuiCol_TextSelectedBg] = active;
    c[ImGuiCol_DragDropTarget] = c[ImGuiCol_NavCursor] = c[ImGuiCol_NavWindowingHighlight] = accent;
    c[ImGuiCol_NavWindowingDimBg] = c[ImGuiCol_ModalWindowDimBg] = dim;
}

void overlay_init(const char *font_path, float font_size, int menu_key) {
#ifdef MHGU_NULL_BACKEND
    return;
#endif
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = "forge_imgui.ini";  // (window positions; in the game folder, beside allgenurecomp.ini)
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "allgenurecomp";
    g_font_size = font_size > 0 ? font_size : 12.0f;
    ImFontConfig cfg;
    cfg.SizePixels = g_font_size;
    ImFont *font = font_path ? io.Fonts->AddFontFromFileTTF(font_path, g_font_size, &cfg) : nullptr;
    if (!font) io.Fonts->AddFontDefaultVector(&cfg);
    ImGui::StyleColorsDark();
    forge_style(ImGui::GetStyle());
    ImGui::GetStyle().FontSizeBase = g_font_size;
    g_base_style = ImGui::GetStyle();
    kzmui_init();  // [AllGenU] KZMUI's fonts and base style (before the first frame builds the font atlas)
    g_menu_key = menu_key;
    g_enabled = true;
    LOG("[forge] ImGui %s overlay: %s opens the menu (font %s, %.0f px)\n", IMGUI_VERSION,
        SDL_GetScancodeName((SDL_Scancode)menu_key), font ? font_path : "ImGui's", g_font_size);
}

// ------------------------------------------------------------------ frames (game thread)
static std::mutex g_snap_lock;  // the copy the render thread draws
static ImDrawData g_snap;
static std::atomic<uint32_t> g_disp_w, g_disp_h;
static bool g_plugins_init;
static uint64_t g_last_ns;

static void snap_clear(void) {
    for (ImDrawList *l : g_snap.CmdLists) IM_DELETE(l);
    g_snap.CmdLists.resize(0);
    g_snap.CmdListsCount = g_snap.TotalIdxCount = g_snap.TotalVtxCount = 0;
    g_snap.Valid = false;
}

void overlay_frame(void *cpu) {
    if (!g_ready) return;
    {   // [AllGenU] the presented rate (see overlay_fps): a 250 ms window, so the counter settles quickly
        static uint64_t f_ns, f_frames;
        uint64_t now = SDL_GetTicksNS();
        if (!f_ns) f_ns = now, f_frames = g_present_count;
        else if (now - f_ns >= 250000000ull) {
            g_present_fps = (float)((double)(g_present_count - f_frames) * 1e9 / (double)(now - f_ns));
            f_ns = now, f_frames = g_present_count;
        }
    }
    bool open = g_open || g_kz_open, free_render = forge_plugins_have(FORGE_IMGUI_FREE_RENDER);
    ImGuiIO &io = ImGui::GetIO();
    if (!open && !free_render && !g_config.show_fps) {  // (nothing to draw)
        std::lock_guard<std::mutex> l(g_snap_lock);
        if (g_snap.Valid) snap_clear();
        apply_input(io);  // (keep the queue short)
        g_last_ns = 0;
        return;
    }
    uint32_t w = g_disp_w, h = g_disp_h;
    if (!w || !h) return;
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    float scale = std::max(1.0f, h / 720.0f);  // (Forge's sizes are for the console's 720p screen)
    if (scale != g_scale) {
        ImGuiStyle &style = ImGui::GetStyle();
        style = g_base_style;
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale;
        g_scale = scale;
    }
    uint64_t now = SDL_GetTicksNS();
    io.DeltaTime = g_last_ns ? std::clamp((now - g_last_ns) / 1e9f, 1e-4f, 0.1f) : 1.0f / 30;
    g_last_ns = now;
    apply_input(io);

    if (!g_plugins_init) {  // (Forge: right after its context is made)
        g_plugins_init = true;
        forge_imgui_init(cpu);
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (open) {
        bool keep = true;
        if (g_open) {  // Forge's window (its key comes from Forge's config; kept as it was)
        ImGui::SetNextWindowPos(ImVec2(40 * scale, 40 * scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(420 * scale, 360 * scale), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("forge", &keep)) {
            if (ImGui::CollapsingHeader("About Forge")) {
                ImGui::Text("Forge API 1.5.1, by the port (AllGenURecomp)");
                ImGui::SeparatorText("Forge developers");
                ImGui::BulletText("Fexty");
                ImGui::BulletText("jeffi2287");
                ImGui::SeparatorText("Libraries used");
                ImGui::BulletText("Dear ImGui %s", IMGUI_VERSION);
                ImGui::BulletText("cimgui");
            }
            if (ImGui::CollapsingHeader("Loaded Plugins")) {
                const char *names[16];
                int n = forge_plugin_titles(names, 16);
                for (int i = 0; i < n; i++) ImGui::BulletText("%s", names[i]);
            }
            if (ImGui::CollapsingHeader("Plugin UI")) forge_imgui_run(cpu, FORGE_IMGUI_RENDER);
        }
        ImGui::End();
        if (!keep) set_open(false);
        }
        if (g_kz_open) {  // [AllGenU] KZMUI: its pages (graphics: FSR, Dock mode, resolution, FPS...)
            bool kz_keep = true;
            kzmui_draw(&kz_keep, cpu, scale);
            if (!kz_keep) set_kz_open(false);
        }
    }
    // [AllGenU] the FPS counter (menu: Gráficos -> CONTADOR DE FPS): top right, over the game, never taking input
    if (!open && g_config.show_fps) {
        float fps = g_present_fps;
        char text[64];
        snprintf(text, sizeof text, "%.0f FPS   %.1f ms", fps, fps > 0.5f ? 1000.0f / fps : 0.0f);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10.0f * scale, 10.0f * scale), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9 * scale, 4 * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.42f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.93f, 0.76f, 1.0f));
        ImGui::Begin("##fpscount", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::TextUnformatted(text);
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
    forge_imgui_run(cpu, FORGE_IMGUI_FREE_RENDER);
    ImGui::Render();

    ImDrawData *dd = ImGui::GetDrawData();
    if (dd->Textures)  // (texture uploads: on this thread, between the render thread's submissions)
        for (ImTextureData *tex : *dd->Textures)
            if (tex->Status != ImTextureStatus_OK) {
                vk_lock();
                ImGui_ImplVulkan_UpdateTexture(tex);
                vk_unlock();
            }
    std::lock_guard<std::mutex> l(g_snap_lock);
    snap_clear();
    for (ImDrawList *list : dd->CmdLists) g_snap.CmdLists.push_back(list->CloneOutput());
    g_snap.CmdListsCount = dd->CmdListsCount;
    g_snap.TotalIdxCount = dd->TotalIdxCount;
    g_snap.TotalVtxCount = dd->TotalVtxCount;
    g_snap.DisplayPos = dd->DisplayPos;
    g_snap.DisplaySize = dd->DisplaySize;
    g_snap.FramebufferScale = dd->FramebufferScale;
    g_snap.Textures = nullptr;  // (done above)
    g_snap.Valid = true;
}

// ------------------------------------------------------------------ drawing (render thread)
static VkRenderPass g_pass;

static void vk_check(VkResult r) {
    if (r < 0) LOG("[forge] ImGui Vulkan error %d\n", (int)r);
}

void overlay_draw(VkCommandBuffer cb, const OverlayTarget *t) {
#ifndef MHGU_NULL_BACKEND
    if (!g_enabled) return;
    g_disp_w = t->width, g_disp_h = t->height;
    if (t->pass != g_pass) {  // (the window's render pass: made once, again only if the window's format changes)
        if (g_pass) {
            g_ready = false;
            ImGui_ImplVulkan_Shutdown();
        }
        ImGui_ImplVulkan_InitInfo info = {};
        info.ApiVersion = VK_API_VERSION_1_2;
        info.Instance = t->inst;
        info.PhysicalDevice = t->phys;
        info.Device = t->dev;
        info.QueueFamily = t->qfam;
        info.Queue = t->queue;
        info.PipelineCache = t->cache;
        info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE * 2;
        info.MinImageCount = 2;
        info.ImageCount = 8;  // (vertex buffers in turn: more than the frames ever in flight)
        info.PipelineInfoMain.RenderPass = t->pass;
        info.PipelineInfoMain.Subpass = 0;
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.CheckVkResultFn = vk_check;
        if (!ImGui_ImplVulkan_Init(&info)) {
            LOG("[forge] ImGui's Vulkan renderer did not start: no overlay\n");
            g_enabled = false;
            return;
        }
        g_pass = t->pass;
        g_ready = true;
    }
    std::lock_guard<std::mutex> l(g_snap_lock);
    if (g_snap.Valid && g_snap.CmdListsCount) ImGui_ImplVulkan_RenderDrawData(&g_snap, cb);
#endif
}
