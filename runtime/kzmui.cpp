// KZMUI: the AllGenU menu. Drawn with Dear ImGui by runtime/overlay.cpp over the game. Everything here runs on the
// game's thread, inside overlay_frame()'s NewFrame/Render pair; the render thread only sees the finished draw lists.
//
// Branding: AllGenU (the port) / KZMUI (this interface). The pages are the ones the project uses: graphics, mods,
// system, controls, appearance and save vault. No online page: local play is configured by the game itself.
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <SDL3/SDL.h>

#include "imgui.h"

#include "config.h"
#include "forge.h"
#include "kzmui.h"
#include "mods.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

extern "C" {
extern const char *g_save_dir;  // runtime/services/fs.c
void vk_set_msaa(uint32_t n);   // runtime/nvn/vulkan/device.c
void vk_set_vsync(int on);      // runtime/nvn/vulkan/device.c
void vi_set_framerate(int fps);  // runtime/hle/vi.c
void ui_apply_display(void);     // runtime/ui.c (window mode/size, on the host's main thread)
void host_bind_start(int slot);  // runtime/host_input.c (Controles: the next key becomes this binding)
int host_bind_active(void);
void host_keymap_save(void);
float overlay_fps(void);  // runtime/overlay.cpp (presented frames a second)
extern uint32_t g_render_size[2];  // runtime/config.c (the 3D resolution the game asks for)
}

namespace fs = std::filesystem;

// ------------------------------------------------------------------ theme
struct Theme {
    const char *name;
    const char *desc;
    ImU32 accent, accent2;
    ImU32 bg, panel, card, card2, border, text, dim;
    ImU32 swatch[3];
};

static const Theme kThemes[12] = {
    {"Gremio de Bherna", "Bronce real y pergamino", IM_COL32(217, 138, 74, 255), IM_COL32(242, 192, 120, 255),
     IM_COL32(15, 12, 11, 255), IM_COL32(21, 16, 14, 255), IM_COL32(35, 24, 20, 255), IM_COL32(43, 29, 24, 255),
     IM_COL32(81, 55, 43, 255), IM_COL32(245, 232, 219, 255), IM_COL32(169, 143, 124, 255),
     {IM_COL32(217, 138, 74, 255), IM_COL32(242, 192, 120, 255), IM_COL32(45, 31, 25, 255)}},
    {"Kokoto Ancestral", "Forja volcánica y ámbar", IM_COL32(240, 122, 42, 255), IM_COL32(255, 176, 90, 255),
     IM_COL32(16, 11, 8, 255), IM_COL32(22, 16, 12, 255), IM_COL32(41, 25, 16, 255), IM_COL32(50, 31, 20, 255),
     IM_COL32(92, 56, 34, 255), IM_COL32(255, 233, 215, 255), IM_COL32(184, 148, 120, 255),
     {IM_COL32(240, 122, 42, 255), IM_COL32(255, 196, 105, 255), IM_COL32(44, 26, 16, 255)}},
    {"Yukumo Onsen", "Aguas termales y neblina", IM_COL32(79, 209, 197, 255), IM_COL32(155, 232, 224, 255),
     IM_COL32(8, 17, 20, 255), IM_COL32(12, 23, 26, 255), IM_COL32(18, 35, 41, 255), IM_COL32(24, 48, 56, 255),
     IM_COL32(44, 81, 89, 255), IM_COL32(226, 246, 244, 255), IM_COL32(133, 170, 168, 255),
     {IM_COL32(79, 209, 197, 255), IM_COL32(155, 232, 224, 255), IM_COL32(20, 42, 48, 255)}},
    {"Rathalos Carmesí", "Cielo de ceniza y fuego", IM_COL32(255, 90, 54, 255), IM_COL32(255, 170, 107, 255),
     IM_COL32(19, 10, 10, 255), IM_COL32(23, 13, 13, 255), IM_COL32(38, 19, 20, 255), IM_COL32(48, 24, 25, 255),
     IM_COL32(94, 42, 36, 255), IM_COL32(255, 227, 218, 255), IM_COL32(180, 138, 126, 255),
     {IM_COL32(255, 90, 54, 255), IM_COL32(255, 170, 107, 255), IM_COL32(40, 19, 18, 255)}},
    {"Pokke Nival", "Tundra y cristal", IM_COL32(127, 191, 255, 255), IM_COL32(191, 224, 255, 255),
     IM_COL32(8, 14, 23, 255), IM_COL32(11, 19, 31, 255), IM_COL32(20, 33, 50, 255), IM_COL32(26, 44, 66, 255),
     IM_COL32(45, 74, 102, 255), IM_COL32(226, 239, 255, 255), IM_COL32(138, 163, 189, 255),
     {IM_COL32(127, 191, 255, 255), IM_COL32(191, 224, 255, 255), IM_COL32(22, 40, 61, 255)}},
    {"Soaratorium", "Cielo alto y academia", IM_COL32(176, 140, 255, 255), IM_COL32(216, 196, 255, 255),
     IM_COL32(14, 11, 23, 255), IM_COL32(18, 15, 29, 255), IM_COL32(30, 24, 48, 255), IM_COL32(39, 31, 61, 255),
     IM_COL32(69, 52, 105, 255), IM_COL32(238, 231, 255, 255), IM_COL32(169, 155, 196, 255),
     {IM_COL32(176, 140, 255, 255), IM_COL32(216, 196, 255, 255), IM_COL32(32, 25, 51, 255)}},
    {"Wycademy", "Relámpago de jade", IM_COL32(139, 212, 78, 255), IM_COL32(197, 238, 138, 255),
     IM_COL32(10, 18, 10, 255), IM_COL32(13, 23, 12, 255), IM_COL32(23, 38, 22, 255), IM_COL32(31, 51, 29, 255),
     IM_COL32(54, 86, 44, 255), IM_COL32(234, 248, 223, 255), IM_COL32(154, 176, 134, 255),
     {IM_COL32(139, 212, 78, 255), IM_COL32(197, 238, 138, 255), IM_COL32(27, 44, 25, 255)}},
    {"Bosque Ancestral", "Aurora boreal helada", IM_COL32(66, 214, 174, 255), IM_COL32(155, 240, 214, 255),
     IM_COL32(7, 18, 17, 255), IM_COL32(10, 23, 22, 255), IM_COL32(18, 38, 36, 255), IM_COL32(25, 50, 46, 255),
     IM_COL32(42, 81, 73, 255), IM_COL32(224, 248, 242, 255), IM_COL32(133, 170, 160, 255),
     {IM_COL32(66, 214, 174, 255), IM_COL32(155, 240, 214, 255), IM_COL32(21, 44, 41, 255)}},
    {"Deviljho Voraz", "Furia Draco Abisal", IM_COL32(194, 220, 74, 255), IM_COL32(232, 245, 140, 255),
     IM_COL32(13, 16, 8, 255), IM_COL32(17, 22, 11, 255), IM_COL32(31, 40, 20, 255), IM_COL32(41, 53, 27, 255),
     IM_COL32(72, 90, 36, 255), IM_COL32(243, 248, 218, 255), IM_COL32(159, 174, 122, 255),
     {IM_COL32(194, 220, 74, 255), IM_COL32(232, 245, 140, 255), IM_COL32(36, 47, 24, 255)}},
    {"Dunas Ardientes", "Ocaso del Gran Desierto", IM_COL32(243, 179, 78, 255), IM_COL32(255, 217, 140, 255),
     IM_COL32(18, 14, 7, 255), IM_COL32(23, 17, 10, 255), IM_COL32(40, 29, 16, 255), IM_COL32(52, 38, 22, 255),
     IM_COL32(91, 68, 32, 255), IM_COL32(255, 240, 216, 255), IM_COL32(191, 163, 123, 255),
     {IM_COL32(243, 179, 78, 255), IM_COL32(255, 217, 140, 255), IM_COL32(45, 32, 17, 255)}},
    {"Obsidiana Real", "Carbón y oro imperial", IM_COL32(255, 135, 56, 255), IM_COL32(255, 192, 107, 255),
     IM_COL32(14, 11, 9, 255), IM_COL32(19, 15, 12, 255), IM_COL32(33, 26, 21, 255), IM_COL32(44, 35, 28, 255),
     IM_COL32(78, 58, 42, 255), IM_COL32(250, 234, 221, 255), IM_COL32(179, 155, 136, 255),
     {IM_COL32(255, 135, 56, 255), IM_COL32(255, 192, 107, 255), IM_COL32(36, 27, 21, 255)}},
    {"Halloween", "Noche de Brujas ✦", IM_COL32(255, 122, 24, 255), IM_COL32(181, 108, 255, 255),
     IM_COL32(11, 7, 18, 255), IM_COL32(16, 10, 24, 255), IM_COL32(29, 18, 41, 255), IM_COL32(40, 24, 56, 255),
     IM_COL32(90, 44, 110, 255), IM_COL32(244, 232, 255, 255), IM_COL32(179, 155, 196, 255),
     {IM_COL32(255, 122, 24, 255), IM_COL32(181, 108, 255, 255), IM_COL32(34, 20, 48, 255)}},
};

static const char *const kThemeKeys[12] = {"Bherna", "Kokoto", "Yukumo", "Rathalos Carmesi", "Pokke", "Soaratorium",
                                           "Wycademy", "Ancestral", "Deviljho", "Dunas", "Obsidiana", "Halloween"};

static float g_s = 1.0f;  // design units -> pixels (window height / 720)
static ImFont *g_body, *g_bold;
static ImFont *g_fontBody[5], *g_fontBold[5];  // every family is loaded once: the menu switches them live
static int g_fontUsed = -1;
static int g_page;
static bool g_restart_needed;

// Last setting applied, shown for a few seconds at the bottom of the panel (feedback that it reached the game).
static char g_status[96];
static uint64_t g_status_until;
static float g_fpsHist[40];  // telemetry sparkline (filled in kzmui_draw)
static int g_fpsIdx, g_fpsCount;
static void SetStatus(const char *text) {
    snprintf(g_status, sizeof g_status, "%s", text);
    g_status_until = SDL_GetTicks() + 3200;
}

static const Theme &T() {
    int i = g_config.ui_theme;
    if (i < 0 || i > 11) i = 3;
    return kThemes[i];
}

static ImU32 A(ImU32 c, int a) {
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return (c & 0x00FFFFFFu) | ((ImU32)a << 24);
}
static ImU32 Mix(ImU32 a, ImU32 b, float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    int ar = a & 255, ag = (a >> 8) & 255, ab = (a >> 16) & 255;
    int br = b & 255, bg = (b >> 8) & 255, bb = (b >> 16) & 255;
    return IM_COL32((int)(ar + (br - ar) * t), (int)(ag + (bg - ag) * t), (int)(ab + (bb - ab) * t), 255);
}
static ImVec4 V4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
static float R(float v) { return v * g_s; }  // a design length in pixels
static ImVec2 V(float x, float y) { return ImVec2(x * g_s, y * g_s); }

// [AllGenU] (UI bugfix) AddRectFilledMultiColor has no corner rounding, so every gradient drawn with it squared off the
// shape it filled: the rounded look of the menu was overlaid with square corners (the reported "botones con esquinas
// cuadradas"). GradV splits a vertical gradient into bands whose outer edges carry the radius and whose middle strips
// stay square inside the straight sides, so the radius survives with no visible seam. MixA blends alpha too: these
// fills are glass (semi-transparent), unlike Mix.
static ImU32 MixA(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    int ar = a & 255, ag = (a >> 8) & 255, ab = (a >> 16) & 255, aa = (int)(a >> 24);
    int br = b & 255, bg = (b >> 8) & 255, bb = (b >> 16) & 255, ba = (int)(b >> 24);
    return IM_COL32((int)(ar + (br - ar) * t), (int)(ag + (bg - ag) * t), (int)(ab + (bb - ab) * t),
                    (int)(aa + (ba - aa) * t));
}
static void GradV(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bot, float round,
                  ImDrawFlags corners = ImDrawFlags_RoundCornersAll, int bands = 10) {
    if (round <= 0.6f || bands < 2 || b.y - a.y < 2.0f) {
        dl->AddRectFilledMultiColor(a, b, top, top, bot, bot);
        return;
    }
    for (int i = 0; i < bands; i++) {
        float y0 = a.y + (b.y - a.y) * (float)i / bands;
        float y1 = (i + 1 == bands) ? b.y : a.y + (b.y - a.y) * (float)(i + 1) / bands;
        // ImDrawFlags 0 means "all corners rounded" in ImGui, so the square middle bands ask for None explicitly.
        ImDrawFlags f = ImDrawFlags_RoundCornersNone;
        if (i == 0) f |= corners & ImDrawFlags_RoundCornersTop;
        if (i + 1 == bands) f |= corners & ImDrawFlags_RoundCornersBottom;
        dl->AddRectFilled(ImVec2(a.x, y0), ImVec2(b.x, y1), MixA(top, bot, (i + 0.5f) / bands), round, f);
    }
}

// ------------------------------------------------------------------ fonts and base style
static const char *font_regular(int i) {
    switch (i) {
    case 1: return "C:/Windows/Fonts/consola.ttf";
    case 2: return "C:/Windows/Fonts/bahnschrift.ttf";
    case 3: return "C:/Windows/Fonts/arial.ttf";
    case 4: return "C:/Windows/Fonts/verdana.ttf";
    default: return "C:/Windows/Fonts/segoeui.ttf";
    }
}
static const char *font_bold_file(int i) {
    switch (i) {
    case 1: return "C:/Windows/Fonts/consolab.ttf";
    case 2: return "C:/Windows/Fonts/bahnschrift.ttf";
    case 3: return "C:/Windows/Fonts/arialbd.ttf";
    case 4: return "C:/Windows/Fonts/verdanab.ttf";
    default: return "C:/Windows/Fonts/segoeuib.ttf";
    }
}

static void use_fonts(int f) {
    if (f < 0 || f > 4) f = 0;
    g_body = g_fontBody[f] ? g_fontBody[f] : g_fontBody[0];
    g_bold = g_fontBold[f] ? g_fontBold[f] : g_body;
    if (g_body) ImGui::GetIO().FontDefault = g_body;
    g_fontUsed = f;
}

static void apply_style() {
    ImGuiStyle &st = ImGui::GetStyle();
    const Theme &t = T();
    const float round = (g_config.ui_sharp ? 2.0f : 10.0f) * g_s;
    st.WindowRounding = round;
    st.ChildRounding = round;
    st.PopupRounding = round;
    st.FrameRounding = (g_config.ui_sharp ? 1.0f : 5.0f) * g_s;
    st.GrabRounding = st.FrameRounding;
    st.ScrollbarRounding = round;
    st.WindowBorderSize = 0.0f;
    st.ChildBorderSize = 1.0f;
    st.FrameBorderSize = 0.0f;
    st.WindowPadding = ImVec2(0, 0);
    st.FramePadding = ImVec2(R(10), R(6));
    st.ItemSpacing = ImVec2(R(10), R(8));
    st.ItemInnerSpacing = ImVec2(R(7), R(4));
    st.CellPadding = ImVec2(R(8), R(6));
    st.ScrollbarSize = R(9);
    st.GrabMinSize = R(10);
    st.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    st.SelectableTextAlign = ImVec2(0.0f, 0.5f);

    ImVec4 *c = st.Colors;
    c[ImGuiCol_Text] = V4(t.text);
    c[ImGuiCol_TextDisabled] = V4(t.dim);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = V4(t.card);
    c[ImGuiCol_PopupBg] = V4(t.panel);
    c[ImGuiCol_Border] = V4(t.border);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = V4(t.card2);
    c[ImGuiCol_FrameBgHovered] = V4(Mix(t.card2, t.accent, 0.16f));
    c[ImGuiCol_FrameBgActive] = V4(Mix(t.card2, t.accent, 0.24f));
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = V4(t.panel);
    c[ImGuiCol_MenuBarBg] = V4(t.panel);
    c[ImGuiCol_ScrollbarBg] = V4(t.panel);
    c[ImGuiCol_ScrollbarGrab] = V4(t.border);
    c[ImGuiCol_ScrollbarGrabHovered] = V4(Mix(t.border, t.accent, 0.4f));
    c[ImGuiCol_ScrollbarGrabActive] = V4(t.accent);
    c[ImGuiCol_CheckMark] = V4(t.accent);
    c[ImGuiCol_SliderGrab] = V4(t.accent);
    c[ImGuiCol_SliderGrabActive] = V4(t.accent2);
    c[ImGuiCol_Button] = V4(t.card2);
    c[ImGuiCol_ButtonHovered] = V4(Mix(t.card2, t.accent, 0.25f));
    c[ImGuiCol_ButtonActive] = V4(Mix(t.card2, t.accent, 0.40f));
    c[ImGuiCol_Header] = V4(Mix(t.card2, t.accent, 0.12f));
    c[ImGuiCol_HeaderHovered] = V4(Mix(t.card2, t.accent, 0.24f));
    c[ImGuiCol_HeaderActive] = V4(Mix(t.card2, t.accent, 0.34f));
    c[ImGuiCol_Separator] = V4(t.border);
    c[ImGuiCol_SeparatorHovered] = V4(t.accent);
    c[ImGuiCol_SeparatorActive] = V4(t.accent2);
    c[ImGuiCol_ResizeGrip] = V4(t.card2);
    c[ImGuiCol_ResizeGripHovered] = V4(t.accent);
    c[ImGuiCol_ResizeGripActive] = V4(t.accent2);
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = V4(t.panel);
    c[ImGuiCol_TabHovered] = V4(Mix(t.card2, t.accent, 0.24f));
    c[ImGuiCol_TabSelected] = V4(Mix(t.card2, t.accent, 0.35f));
    c[ImGuiCol_TabDimmedSelected] = V4(t.card2);
    c[ImGuiCol_TextSelectedBg] = V4(A(t.accent, 80));
    c[ImGuiCol_DragDropTarget] = V4(t.accent2);
    c[ImGuiCol_NavCursor] = V4(t.accent);
    c[ImGuiCol_NavWindowingHighlight] = V4(t.accent2);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.55f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.72f);
    st.FontSizeBase = 17.0f;
}

void kzmui_init(void) {
    ImGuiIO &io = ImGui::GetIO();
    for (int i = 0; i < 5; i++) {
        g_fontBody[i] = io.Fonts->AddFontFromFileTTF(font_regular(i), 17.0f);
        g_fontBold[i] = io.Fonts->AddFontFromFileTTF(font_bold_file(i), 17.0f);
    }
    if (!g_fontBody[0]) g_fontBody[0] = io.Fonts->AddFontDefaultVector();
    for (int i = 1; i < 5; i++)
        if (!g_fontBody[i]) g_fontBody[i] = g_fontBody[0];
    for (int i = 0; i < 5; i++)
        if (!g_fontBold[i]) g_fontBold[i] = g_fontBody[i];
    use_fonts(g_config.ui_font);
    g_s = 1.0f;
    apply_style();
    g_page = 0;
}

// ------------------------------------------------------------------ small typography helpers
static void PushText(float size, bool bold = false) { ImGui::PushFont(bold ? g_bold : g_body, size); }
static void PopText() { ImGui::PopFont(); }
static void Txt(ImU32 color, const char *s) {
    ImGui::PushStyleColor(ImGuiCol_Text, V4(color));
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
}
static void SmallTxt(const char *s) {
    PushText(12.0f);
    Txt(T().dim, s);
    PopText();
}
static void Label(const char *s) {
    PushText(9.5f, true);
    Txt(T().dim, s);
    PopText();
}
static void CardTitle(const char *title, const char *sub = nullptr) {
    PushText(15.5f, true);
    Txt(T().text, title);
    PopText();
    if (sub) {
        PushText(10.5f);
        Txt(T().dim, sub);
        PopText();
    }
    ImGui::Dummy(V(0, 2));
}
static void Spacer(float h = 8) { ImGui::Dummy(V(0, h)); }

// ------------------------------------------------------------------ custom widgets

static void ChipAt(ImDrawList *dl, float &x, float y, const char *text, bool on) {
    const float fs = 10.5f * g_s;
    ImVec2 ts = g_body->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);
    ImVec2 a(x, y), b(x + ts.x + 20 * g_s, y + 25 * g_s);
    ImU32 col = on ? T().accent : T().dim;
    dl->AddRectFilled(a, b, A(col, on ? 30 : 12), R(6));
    dl->AddRect(a, b, A(col, on ? 130 : 55), R(6));
    if (on) dl->AddCircleFilled(ImVec2(a.x + 9 * g_s, a.y + 12.5f * g_s), 3 * g_s, T().accent);
    dl->AddText(g_body, fs, ImVec2(a.x + (on ? 16 : 10) * g_s, a.y + (25 * g_s - ts.y) * 0.5f), col, text);
    x = b.x + 8 * g_s;
}

static bool ToggleRow(const char *id, const char *label, const char *desc, bool *value) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 42 * g_s;
    ImGui::InvisibleButton("##row", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    bool changed = false;
    if (ImGui::IsItemClicked()) {
        *value = !*value;
        changed = true;
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hovered) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), A(T().card2, 150), R(6));
    dl->AddText(g_bold, 13 * g_s, ImVec2(p.x + 10 * g_s, p.y + 6 * g_s), T().text, label);
    if (desc) dl->AddText(g_body, 10.5f * g_s, ImVec2(p.x + 10 * g_s, p.y + 23 * g_s), T().dim, desc);
    float sw = 38 * g_s, sh = 20 * g_s;
    ImVec2 a(p.x + w - sw - 8 * g_s, p.y + (h - sh) * 0.5f), b(a.x + sw, a.y + sh);
    dl->AddRectFilled(ImVec2(a.x + 1 * g_s, a.y + 2 * g_s), ImVec2(b.x + 1 * g_s, b.y + 2 * g_s),
                      A(IM_COL32(0, 0, 0, 255), 90), sh * 0.5f);
    // [AllGenU] rounded fill: AddRectFilledMultiColor has no corner rounding, so the switch used to read as a square
    // block under its pill-shaped outline (the "botones con esquinas cuadradas" of the UI report)
    dl->AddRectFilled(a, b, *value ? T().accent2 : Mix(T().card2, T().text, 0.08f), sh * 0.5f);
    if (*value) dl->AddRectFilled(a, ImVec2(b.x, a.y + sh * 0.55f), A(T().accent2, 150), sh * 0.5f, ImDrawFlags_RoundCornersTop);
    dl->AddRect(a, b, *value ? T().accent2 : T().border, sh * 0.5f);
    float kx = *value ? b.x - sh * 0.5f - 2 * g_s : a.x + sh * 0.5f + 2 * g_s;
    dl->AddCircleFilled(ImVec2(kx + 0.8f * g_s, a.y + sh * 0.5f + 1.5f * g_s), sh * 0.5f - 3 * g_s,
                        A(IM_COL32(0, 0, 0, 255), 110));
    dl->AddCircleFilled(ImVec2(kx, a.y + sh * 0.5f), sh * 0.5f - 3 * g_s, *value ? IM_COL32(255, 255, 255, 255) : T().dim);
    ImGui::PopID();
    return changed;
}

static bool Segmented(const char *id, const char *const *items, int n, int *value, float height = 30) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = height * g_s;
    ImGui::InvisibleButton("##seg", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered(), changed = false;
    int hover = -1;
    if (hovered && n > 0) {
        float cell = w / n;
        hover = (int)((ImGui::GetIO().MousePos.x - p.x) / cell);
        if (hover < 0) hover = 0;
        if (hover >= n) hover = n - 1;
        if (ImGui::IsItemClicked() && *value != hover) {
            *value = hover;
            changed = true;
            fprintf(stderr, "[kzmui] %s -> %s\n", id, items[hover]);
        }
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), T().card2, R(7));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), T().border, R(7));
    float cell = w / n;
    for (int i = 0; i < n; i++) {
        ImVec2 a(p.x + cell * i + 2 * g_s, p.y + 2 * g_s), b(p.x + cell * (i + 1) - 2 * g_s, p.y + h - 2 * g_s);
        bool sel = i == *value;
        if (sel) {
            dl->AddRectFilled(ImVec2(a.x + 1 * g_s, a.y + 2 * g_s), ImVec2(b.x + 1 * g_s, b.y + 2 * g_s),
                              A(IM_COL32(0, 0, 0, 255), 100), R(5));
            // [AllGenU] rounded selection (a gradient fill here squared off the segment's corners)
            dl->AddRectFilled(a, b, T().accent, R(5));
            dl->AddRectFilled(a, ImVec2(b.x, a.y + (b.y - a.y) * 0.55f), T().accent2, R(5), ImDrawFlags_RoundCornersTop);
            dl->AddRect(a, b, T().accent2, R(5));
            dl->AddLine(ImVec2(a.x + 4 * g_s, a.y + 1.5f), ImVec2(b.x - 4 * g_s, a.y + 1.5f),
                        A(IM_COL32(255, 255, 255, 255), 70), 1.0f);
        } else if (hover == i) {
            dl->AddRectFilled(a, b, A(T().accent, 26), R(5));
        }
        ImVec2 ts = g_body->CalcTextSizeA(10.5f * g_s, FLT_MAX, 0.0f, items[i]);
        dl->AddText(g_body, 10.5f * g_s,
                    ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f),
                    sel ? IM_COL32(23, 14, 14, 255) : (hover == i ? T().text : T().dim), items[i]);
    }
    ImGui::PopID();
    return changed;
}

static bool KButton(const char *label, bool primary = false) {
    PushText(11.0f, true);
    ImGui::PushStyleColor(ImGuiCol_Button, V4(primary ? T().accent : T().card2));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, V4(primary ? T().accent2 : Mix(T().card2, T().accent, 0.28f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, V4(primary ? T().accent2 : Mix(T().card2, T().accent, 0.42f)));
    ImGui::PushStyleColor(ImGuiCol_Text, V4(primary ? IM_COL32(23, 14, 14, 255) : T().text));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(R(13), R(7)));
    bool r = ImGui::Button(label);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    PopText();
    return r;
}

static bool ActionRow(const char *id, const char *title, const char *desc) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 58 * g_s;
    ImGui::InvisibleButton("##action", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered(), clicked = ImGui::IsItemClicked();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hovered ? Mix(T().card, T().accent, 0.10f) : T().card, R(8));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), hovered ? A(T().accent, 140) : T().border, R(8));
    dl->AddText(g_bold, 13 * g_s, ImVec2(p.x + 14 * g_s, p.y + 11 * g_s), T().text, title);
    dl->AddText(g_body, 10.5f * g_s, ImVec2(p.x + 14 * g_s, p.y + 31 * g_s), T().dim, desc);
    float ax = p.x + w - 23 * g_s, ay = p.y + h * 0.5f;
    dl->AddLine(ImVec2(ax - 3 * g_s, ay - 5 * g_s), ImVec2(ax + 3 * g_s, ay), T().accent, 2 * g_s);
    dl->AddLine(ImVec2(ax + 3 * g_s, ay), ImVec2(ax - 3 * g_s, ay + 5 * g_s), T().accent, 2 * g_s);
    ImGui::PopID();
    return clicked;
}

static void BeginTwoCols(const char *id, float left = 1.35f, float right = 1.0f) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(R(6), 0));
    ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings);
    ImGui::TableSetupColumn("##left", ImGuiTableColumnFlags_WidthStretch, left);
    ImGui::TableSetupColumn("##right", ImGuiTableColumnFlags_WidthStretch, right);
}
static void EndTwoCols() {
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

static void CardCorners(ImVec2 p, ImVec2 s);
static void CardArt(ImVec2 p, ImVec2 s);

static bool BeginCard(const char *id, float height, const char *title = nullptr, const char *sub = nullptr) {
    const Theme &t = T();
    float rad = (g_config.ui_sharp ? 2.0f : 9.0f) * g_s;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, V4(A(t.card, 208)));
    ImGui::PushStyleColor(ImGuiCol_Border, V4(A(t.border, 185)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, rad);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(R(13), R(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(R(7), R(5)));
    ImGuiChildFlags flags = ImGuiChildFlags_Borders | (height > 0 ? 0 : ImGuiChildFlags_AutoResizeY);
    bool open = ImGui::BeginChild(id, ImVec2(0, height > 0 ? height * g_s : 0), flags);
    if (open) {
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        ImVec2 e(wp.x + ws.x, wp.y + ws.y);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        // glass: a themed glow on top, a shadow pooling at the bottom, so the card reads as a lit pane, not a box.
        // [AllGenU] rounded here on purpose: the old AddRectFilledMultiColor overlays had square corners and painted the
        // card's rounded ones square again (the UI bug); these use the same radius as the child.
        dl->AddRectFilled(wp, ImVec2(e.x, wp.y + ws.y * 0.55f), A(t.accent, 20), rad, ImDrawFlags_RoundCornersTop);
        dl->AddRectFilled(ImVec2(wp.x, wp.y + ws.y * 0.45f), e, A(IM_COL32(0, 0, 0, 255), 40), rad,
                          ImDrawFlags_RoundCornersBottom);
        // bevel: a light top edge, an inner hairline, and the shadowed lower lip
        dl->AddLine(ImVec2(wp.x + rad, wp.y + 1.5f), ImVec2(e.x - rad, wp.y + 1.5f), A(t.text, 34), 1.0f);
        dl->AddRect(ImVec2(wp.x + 2.0f, wp.y + 2.0f), ImVec2(e.x - 2.0f, e.y - 2.0f), A(t.text, 15), rad, 0, 1.0f);
        dl->AddLine(ImVec2(wp.x + rad, e.y - 2.0f), ImVec2(e.x - rad, e.y - 2.0f), A(IM_COL32(0, 0, 0, 255), 90), 1.0f);
        // a small accent tooth on the top-left edge, as the reference's cards have
        dl->AddRectFilled(ImVec2(wp.x + 12 * g_s, wp.y + 1), ImVec2(wp.x + 40 * g_s, wp.y + 2.5f), A(t.accent, 150),
                          R(1));
        dl->AddCircleFilled(ImVec2(e.x - 8 * g_s, e.y - 8 * g_s), 1.6f * g_s, A(t.text, 60));
        CardCorners(wp, ws);
        if (height == 0 || height >= 96) CardArt(wp, ws);
        if (title) CardTitle(title, sub);
    }
    return open;
}
static void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
}

static void DrawIcon(ImDrawList *dl, int kind, ImVec2 c, float r, ImU32 col) {
    const float th = std::max(1.3f * g_s, 1.0f);
    switch (kind) {
    case 0:  // monitor
        dl->AddRect(ImVec2(c.x - r, c.y - r * 0.72f), ImVec2(c.x + r, c.y + r * 0.44f), col, R(2), 0, th);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.44f), ImVec2(c.x, c.y + r * 0.76f), col, th);
        dl->AddLine(ImVec2(c.x - r * 0.45f, c.y + r * 0.78f), ImVec2(c.x + r * 0.45f, c.y + r * 0.78f), col, th);
        break;
    case 1: {  // cube
        ImVec2 pts[6] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.86f, c.y - r * 0.48f),
                         ImVec2(c.x + r * 0.86f, c.y + r * 0.48f), ImVec2(c.x, c.y + r),
                         ImVec2(c.x - r * 0.86f, c.y + r * 0.48f), ImVec2(c.x - r * 0.86f, c.y - r * 0.48f)};
        for (int i = 0; i < 6; i++) dl->AddLine(pts[i], pts[(i + 1) % 6], col, th);
        dl->AddLine(pts[0], c, col, th);
        dl->AddLine(pts[2], c, col, th);
        dl->AddLine(pts[4], c, col, th);
        break;
    }
    case 2:  // sliders
        for (int i = -1; i <= 1; i++) {
            float y = c.y + i * r * 0.62f;
            dl->AddLine(ImVec2(c.x - r, y), ImVec2(c.x + r, y), col, th);
            dl->AddCircleFilled(ImVec2(c.x + i * r * 0.45f, y), 2.5f * g_s, col);
        }
        break;
    case 3:  // gamepad
        dl->AddRect(ImVec2(c.x - r, c.y - r * 0.55f), ImVec2(c.x + r, c.y + r * 0.55f), col, r * 0.42f, 0, th);
        dl->AddLine(ImVec2(c.x - r * 0.58f, c.y - r * 0.18f), ImVec2(c.x - r * 0.58f, c.y + r * 0.22f), col, th);
        dl->AddLine(ImVec2(c.x - r * 0.78f, c.y + r * 0.02f), ImVec2(c.x - r * 0.38f, c.y + r * 0.02f), col, th);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.55f, c.y - r * 0.14f), 2.3f * g_s, col);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.74f, c.y + r * 0.08f), 2.3f * g_s, col);
        break;
    case 4:  // star
        dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col, th);
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, th);
        dl->AddLine(ImVec2(c.x - r * 0.65f, c.y - r * 0.65f), ImVec2(c.x + r * 0.65f, c.y + r * 0.65f), col, th);
        dl->AddLine(ImVec2(c.x - r * 0.65f, c.y + r * 0.65f), ImVec2(c.x + r * 0.65f, c.y - r * 0.65f), col, th);
        break;
    case 5: {  // shield
        ImVec2 pts[6] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.76f, c.y - r * 0.55f),
                         ImVec2(c.x + r * 0.62f, c.y + r * 0.28f), ImVec2(c.x, c.y + r),
                         ImVec2(c.x - r * 0.62f, c.y + r * 0.28f), ImVec2(c.x - r * 0.76f, c.y - r * 0.55f)};
        dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, th);
        break;
    }
    case 6:  // folder
        dl->AddRect(ImVec2(c.x - r, c.y - r * 0.55f), ImVec2(c.x + r, c.y + r * 0.65f), col, R(2), 0, th);
        dl->AddLine(ImVec2(c.x - r, c.y - r * 0.2f), ImVec2(c.x + r, c.y - r * 0.2f), col, th);
        break;
    case 7:  // save disk
        dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, R(2), 0, th);
        dl->AddRect(ImVec2(c.x - r * 0.5f, c.y - r), ImVec2(c.x + r * 0.5f, c.y - r * 0.15f), col, 0, 0, th);
        break;
    default:  // info circle
        dl->AddCircle(c, r, col, 0, th);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.35f), ImVec2(c.x, c.y + r * 0.55f), col, th);
        dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.62f), 1.8f * g_s, col);
        break;
    }
}

// ------------------------------------------------------------------ theme art (vector, no textures)
static void Pumpkin(ImDrawList *dl, ImVec2 c, float r, int a) {
    ImU32 body = A(IM_COL32(255, 126, 28, 255), a), rib = A(IM_COL32(173, 63, 12, 255), a);
    ImU32 leaf = A(IM_COL32(116, 178, 68, 255), a), ink = A(IM_COL32(28, 12, 8, 255), a);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.55f, c.y), r * 0.70f, rib, 20);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.55f, c.y), r * 0.70f, rib, 20);
    dl->AddCircleFilled(c, r * 0.82f, body, 24);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.42f, c.y - r * 0.05f), r * 0.45f, body, 18);
    dl->AddCircleFilled(ImVec2(c.x + r * 0.42f, c.y - r * 0.05f), r * 0.45f, body, 18);
    dl->AddLine(ImVec2(c.x, c.y - r * 0.72f), ImVec2(c.x + r * 0.16f, c.y - r * 1.12f), ink, std::max(1.0f, r * 0.16f));
    dl->AddTriangleFilled(ImVec2(c.x + r * 0.16f, c.y - r * 1.02f), ImVec2(c.x + r * 0.65f, c.y - r * 0.88f),
                          ImVec2(c.x + r * 0.20f, c.y - r * 0.68f), leaf);
    if (r > 4 * g_s) {
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.38f, c.y - r * 0.18f), ImVec2(c.x - r * 0.10f, c.y - r * 0.32f),
                              ImVec2(c.x - r * 0.12f, c.y + r * 0.02f), ink);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.38f, c.y - r * 0.18f), ImVec2(c.x + r * 0.10f, c.y - r * 0.32f),
                              ImVec2(c.x + r * 0.12f, c.y + r * 0.02f), ink);
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.28f, c.y + r * 0.42f), ImVec2(c.x + r * 0.28f, c.y + r * 0.42f),
                              ImVec2(c.x, c.y + r * 0.18f), ink);
    }
}

static void SpiderWeb(ImDrawList *dl, ImVec2 origin, float size, int a) {
    ImU32 col = A(T().text, a);
    const float pi = 3.14159265f;
    ImVec2 ends[6];
    for (int i = 0; i < 6; i++) {
        float ang = pi * 0.5f + (pi * 0.5f) * (float)i / 5.0f;
        ends[i] = ImVec2(origin.x + std::cos(ang) * size, origin.y + std::sin(ang) * size);
        dl->AddLine(origin, ends[i], col, 1.0f);
    }
    for (int ring = 1; ring <= 3; ring++) {
        ImVec2 pts[6];
        float k = 0.30f + ring * 0.24f;
        for (int i = 0; i < 6; i++) pts[i] = ImVec2(origin.x + (ends[i].x - origin.x) * k, origin.y + (ends[i].y - origin.y) * k);
        dl->AddPolyline(pts, 6, col, 1.0f, 0);
    }
}

static void Snowflake(ImDrawList *dl, ImVec2 c, float r, int a) {
    ImU32 col = A(T().text, a);
    const float pi = 3.14159265f;
    for (int i = 0; i < 3; i++) {
        float ang = pi * (float)i / 3.0f;
        ImVec2 d(std::cos(ang) * r, std::sin(ang) * r);
        dl->AddLine(ImVec2(c.x - d.x, c.y - d.y), ImVec2(c.x + d.x, c.y + d.y), col, 1.2f);
        dl->AddLine(ImVec2(c.x + d.x * 0.55f, c.y + d.y * 0.55f),
                    ImVec2(c.x + d.x * 0.80f + d.y * 0.22f, c.y + d.y * 0.80f - d.x * 0.22f), col, 1.0f);
        dl->AddLine(ImVec2(c.x + d.x * 0.55f, c.y + d.y * 0.55f),
                    ImVec2(c.x + d.x * 0.80f - d.y * 0.22f, c.y + d.y * 0.80f + d.x * 0.22f), col, 1.0f);
    }
}

static void Flame(ImDrawList *dl, ImVec2 c, float r, int a) {
    ImU32 col = A(T().accent, a), hot = A(IM_COL32(255, 205, 120, 255), a);
    dl->AddBezierCubic(ImVec2(c.x, c.y + r), ImVec2(c.x - r * 0.95f, c.y + r * 0.15f),
                       ImVec2(c.x - r * 0.20f, c.y - r * 0.25f), ImVec2(c.x, c.y - r), col, 1.8f);
    dl->AddBezierCubic(ImVec2(c.x, c.y + r), ImVec2(c.x + r * 0.95f, c.y + r * 0.15f),
                       ImVec2(c.x + r * 0.20f, c.y - r * 0.25f), ImVec2(c.x, c.y - r), col, 1.8f);
    dl->AddBezierCubic(ImVec2(c.x, c.y + r * 0.6f), ImVec2(c.x - r * 0.35f, c.y + r * 0.05f),
                       ImVec2(c.x + r * 0.10f, c.y - r * 0.20f), ImVec2(c.x, c.y - r * 0.45f), hot, 1.2f);
}

static void Sun(ImDrawList *dl, ImVec2 c, float r, int a) {
    ImU32 col = A(T().accent2, a);
    dl->AddCircle(c, r * 0.55f, col, 24, 1.4f);
    for (int i = 0; i < 8; i++) {
        float ang = (float)i * 3.14159265f / 4.0f;
        ImVec2 d(std::cos(ang), std::sin(ang));
        dl->AddLine(ImVec2(c.x + d.x * r * 0.72f, c.y + d.y * r * 0.72f),
                    ImVec2(c.x + d.x * r, c.y + d.y * r), col, 1.2f);
    }
}

static void Crystal(ImDrawList *dl, ImVec2 c, float r, int a) {
    ImU32 col = A(T().accent, a), light = A(T().accent2, a);
    ImVec2 p[5] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.62f, c.y - r * 0.15f),
                   ImVec2(c.x + r * 0.32f, c.y + r), ImVec2(c.x - r * 0.32f, c.y + r),
                   ImVec2(c.x - r * 0.62f, c.y - r * 0.15f)};
    dl->AddPolyline(p, 5, col, 1.4f, ImDrawFlags_Closed);
    dl->AddLine(p[0], ImVec2(c.x, c.y + r * 0.25f), light, 1.0f);
    dl->AddLine(p[4], ImVec2(c.x, c.y + r * 0.25f), light, 1.0f);
}

static void Motif(ImDrawList *dl, int idx, ImVec2 c, float s, int a) {
    const Theme &t = T();
    ImU32 ac = A(t.accent, a), ac2 = A(t.accent2, a);
    switch (idx) {
    case 0:  // Bherna: bronze leaf
        dl->AddEllipse(c, ImVec2(s * 0.55f, s), ac, 0, 20, 1.4f);
        dl->AddLine(ImVec2(c.x, c.y + s), ImVec2(c.x, c.y + s * 1.35f), ac2, 1.2f);
        break;
    case 1:  // Kokoto: embers
    case 3:  // Rathalos: flame
        Flame(dl, c, s, a);
        break;
    case 2:  // Yukumo: steam
        dl->AddBezierCubic(ImVec2(c.x - s * 0.45f, c.y + s), ImVec2(c.x - s * 0.95f, c.y + s * 0.2f),
                           ImVec2(c.x + s * 0.25f, c.y - s * 0.2f), ImVec2(c.x - s * 0.10f, c.y - s), ac, 1.4f);
        dl->AddBezierCubic(ImVec2(c.x + s * 0.45f, c.y + s), ImVec2(c.x + s * 0.95f, c.y + s * 0.2f),
                           ImVec2(c.x - s * 0.25f, c.y - s * 0.2f), ImVec2(c.x + s * 0.10f, c.y - s), ac2, 1.0f);
        break;
    case 4:  // Pokke: snow
        Snowflake(dl, c, s, a);
        break;
    case 5:  // Soaratorium: cloud and arc
        dl->AddCircleFilled(ImVec2(c.x - s * 0.35f, c.y), s * 0.42f, ac, 16);
        dl->AddCircleFilled(ImVec2(c.x + s * 0.25f, c.y - s * 0.16f), s * 0.52f, ac, 16);
        dl->AddCircleFilled(ImVec2(c.x + s * 0.62f, c.y + s * 0.10f), s * 0.32f, ac, 16);
        dl->AddLine(ImVec2(c.x - s, c.y + s * 0.55f), ImVec2(c.x + s, c.y + s * 0.55f), ac2, 1.2f);
        break;
    case 6: {  // Wycademy: lightning
        ImVec2 bolt[4] = {ImVec2(c.x - s * 0.15f, c.y - s), ImVec2(c.x + s * 0.35f, c.y - s * 0.15f),
                          ImVec2(c.x + s * 0.02f, c.y - s * 0.05f), ImVec2(c.x + s * 0.25f, c.y + s)};
        dl->AddPolyline(bolt, 4, ac, 1.8f, 0);
        break;
    }
    case 7:  // Ancestral: aurora and pine
        for (int i = 0; i < 3; i++)
            dl->AddBezierCubic(ImVec2(c.x - s, c.y - s * 0.55f + i * s * 0.30f), ImVec2(c.x - s * 0.2f, c.y - s),
                               ImVec2(c.x + s * 0.2f, c.y + s * 0.10f), ImVec2(c.x + s, c.y - s * 0.35f + i * s * 0.30f),
                               A(t.accent2, a - i * 12), 1.1f);
        dl->AddTriangleFilled(ImVec2(c.x - s * 0.28f, c.y + s), ImVec2(c.x + s * 0.28f, c.y + s),
                              ImVec2(c.x, c.y + s * 0.10f), ac);
        break;
    case 8:  // Deviljho: claw marks
        for (int i = -1; i <= 1; i++)
            dl->AddLine(ImVec2(c.x - s * 0.6f + i * s * 0.38f, c.y - s), ImVec2(c.x + s * 0.1f + i * s * 0.38f, c.y + s),
                        i ? A(t.accent, a - 20) : ac2, std::max(1.0f, s * 0.12f));
        break;
    case 9:  // Dunas: sun over dunes
        Sun(dl, ImVec2(c.x, c.y - s * 0.25f), s * 0.65f, a);
        dl->AddBezierCubic(ImVec2(c.x - s, c.y + s * 0.65f), ImVec2(c.x - s * 0.3f, c.y),
                           ImVec2(c.x + s * 0.3f, c.y + s * 1.1f), ImVec2(c.x + s, c.y + s * 0.45f), ac, 1.4f);
        break;
    case 10:  // Obsidiana: crystal
        Crystal(dl, c, s, a);
        break;
    default:  // Halloween: pumpkin and bats
        Pumpkin(dl, ImVec2(c.x, c.y + s * 0.18f), s * 0.55f, a + 40 > 255 ? 255 : a + 40);
        dl->AddLine(ImVec2(c.x - s * 0.95f, c.y - s * 0.72f), ImVec2(c.x - s * 0.58f, c.y - s * 0.92f), ac2, 1.2f);
        dl->AddLine(ImVec2(c.x - s * 0.58f, c.y - s * 0.92f), ImVec2(c.x - s * 0.22f, c.y - s * 0.72f), ac2, 1.2f);
        dl->AddTriangleFilled(ImVec2(c.x - s * 0.95f, c.y - s * 0.72f), ImVec2(c.x - s * 0.80f, c.y - s * 0.92f),
                              ImVec2(c.x - s * 0.66f, c.y - s * 0.70f), ac2);
        dl->AddTriangleFilled(ImVec2(c.x - s * 0.22f, c.y - s * 0.72f), ImVec2(c.x - s * 0.38f, c.y - s * 0.92f),
                              ImVec2(c.x - s * 0.52f, c.y - s * 0.70f), ac2);
        break;
    }
}

// A hanging decoration at the top-right corner of a card: pumpkins for Halloween, the theme's own mark otherwise.
static void CardArt(ImVec2 p, ImVec2 s) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = 9.0f * g_s;
    ImVec2 c(p.x + s.x - 18 * g_s, p.y + 19 * g_s);
    if (g_config.ui_theme == 11) {
        // pumpkin on a sagging thread, with a cobweb tucked into the corner behind it
        SpiderWeb(dl, ImVec2(p.x + s.x - 2 * g_s, p.y + 2 * g_s), 30 * g_s, 46);
        dl->AddBezierCubic(ImVec2(c.x, p.y), ImVec2(c.x + 1, p.y + r * 0.55f), ImVec2(c.x - 1, c.y - r * 1.5f),
                           ImVec2(c.x, c.y - r * 1.15f), A(T().border, 190), 1.1f);
        dl->AddCircleFilled(ImVec2(c.x, c.y - r * 1.15f), 1.7f * g_s, A(T().border, 220));
        Pumpkin(dl, c, r, 240);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.55f, c.y - r * 0.28f), 1.2f * g_s, A(IM_COL32(255, 236, 180, 255), 200));
    } else {
        dl->AddCircleFilled(c, 11 * g_s, A(T().accent, 16), 18);
        dl->AddCircle(c, 11 * g_s, A(T().accent, 70), 18, 1.0f);
        Motif(dl, g_config.ui_theme, c, 7.5f * g_s, 185);
    }
}

// Thin L brackets in the card corners, as the reference interface has.
static void CardCorners(ImVec2 p, ImVec2 s) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 col = A(T().accent, 155);
    float k = 10 * g_s, t = 1.3f;
    dl->AddLine(ImVec2(p.x + 1, p.y + k), ImVec2(p.x + 1, p.y + 1), col, t);
    dl->AddLine(ImVec2(p.x + 1, p.y + 1), ImVec2(p.x + k, p.y + 1), col, t);
    dl->AddLine(ImVec2(p.x + s.x - k, p.y + 1), ImVec2(p.x + s.x - 1, p.y + 1), col, t);
    dl->AddLine(ImVec2(p.x + s.x - 1, p.y + 1), ImVec2(p.x + s.x - 1, p.y + k), col, t);
    dl->AddLine(ImVec2(p.x + 1, p.y + s.y - k), ImVec2(p.x + 1, p.y + s.y - 1), col, t);
    dl->AddLine(ImVec2(p.x + 1, p.y + s.y - 1), ImVec2(p.x + k, p.y + s.y - 1), col, t);
    dl->AddLine(ImVec2(p.x + s.x - k, p.y + s.y - 1), ImVec2(p.x + s.x - 1, p.y + s.y - 1), col, t);
    dl->AddLine(ImVec2(p.x + s.x - 1, p.y + s.y - k), ImVec2(p.x + s.x - 1, p.y + s.y - 1), col, t);
}

static void PanelChrome(ImDrawList *dl, ImVec2 p, ImVec2 e) {
    const Theme &t = T();
    float r = 15 * g_s;
    for (int i = 10; i >= 1; i--)  // soft lift from the game behind
        dl->AddRect(ImVec2(p.x - i * 2.2f * g_s, p.y - i * 1.6f * g_s), ImVec2(e.x + i * 2.2f * g_s, e.y + i * 2.8f * g_s),
                    A(IM_COL32(0, 0, 0, 255), 9), r + i * 2.2f * g_s, 0, 2.2f);
    // an outer halo, then the glass itself: brighter at the top, deeper at the bottom
    dl->AddRect(ImVec2(p.x - 2 * g_s, p.y - 2 * g_s), ImVec2(e.x + 2 * g_s, e.y + 2 * g_s), A(t.accent, 46), r + 2 * g_s, 0, 2.0f);
    dl->AddRectFilled(p, e, A(t.panel, 226), r);
    // [AllGenU] rounded glass fills: AddRectFilledMultiColor has no rounding and squared the panel's corners
    float ph = e.y - p.y;
    dl->AddRectFilled(p, ImVec2(e.x, p.y + ph * 0.55f), A(t.accent, 26), r, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(p.x, p.y + ph * 0.45f), e, A(t.bg, 120), r, ImDrawFlags_RoundCornersBottom);
    // inner glass sheet, inset: gives the panel a rim with depth
    ImVec2 ip(p.x + 4 * g_s, p.y + 4 * g_s), ie(e.x - 4 * g_s, e.y - 4 * g_s);
    dl->AddRectFilled(ip, ie, A(t.text, 6), r - 4 * g_s);
    dl->AddRect(p, e, A(t.accent, 196), r, 0, 1.7f);
    dl->AddRect(ImVec2(p.x + 1.5f, p.y + 1.5f), ImVec2(e.x - 1.5f, e.y - 1.5f), A(t.text, 34), r - 1.0f, 0, 1.0f);
    dl->AddRect(ip, ie, A(t.border, 110), r - 4 * g_s, 0, 1.0f);
    dl->AddLine(ImVec2(p.x + r, p.y + 1.5f), ImVec2(e.x - r, p.y + 1.5f), A(t.accent2, 150), 1.2f);
    dl->AddLine(ImVec2(p.x + r, e.y - 1.5f), ImVec2(e.x - r, e.y - 1.5f), A(IM_COL32(0, 0, 0, 255), 170), 1.2f);
    // ornamental diamonds along the top and bottom edges
    float mid = (p.x + e.x) * 0.5f;
    for (int k = -1; k <= 1; k += 2) {
        ImVec2 d(mid + k * 92 * g_s, p.y + 4 * g_s);
        dl->AddQuadFilled(ImVec2(d.x, d.y - 3.2f * g_s), ImVec2(d.x + 3.2f * g_s, d.y), ImVec2(d.x, d.y + 3.2f * g_s),
                          ImVec2(d.x - 3.2f * g_s, d.y), A(t.accent2, 170));
    }
    dl->AddQuadFilled(ImVec2(mid, e.y - 4 * g_s - 3.2f * g_s), ImVec2(mid + 3.2f * g_s, e.y - 4 * g_s),
                      ImVec2(mid, e.y - 4 * g_s + 3.2f * g_s), ImVec2(mid - 3.2f * g_s, e.y - 4 * g_s), A(t.accent2, 140));
    if (g_config.ui_theme == 11) {
        SpiderWeb(dl, ImVec2(e.x - 9 * g_s, p.y + 9 * g_s), 92 * g_s, 74);
        Pumpkin(dl, ImVec2(p.x + 24 * g_s, e.y - 20 * g_s), 8.5f * g_s, 170);
    }
    Motif(dl, g_config.ui_theme, ImVec2(e.x - 46 * g_s, p.y + 42 * g_s), 20 * g_s, 42);
    // corner brackets with a small screw dot: the reference's mechanical frame
    ImU32 corner = A(t.accent, 170), dot = A(t.accent2, 120);
    float k = 20 * g_s;
    dl->AddLine(ImVec2(p.x, p.y + r + k), ImVec2(p.x, p.y + r), corner, 1.6f);
    dl->AddLine(ImVec2(p.x + r, p.y), ImVec2(p.x + r + k, p.y), corner, 1.6f);
    dl->AddLine(ImVec2(e.x, e.y - r - k), ImVec2(e.x, e.y - r), corner, 1.6f);
    dl->AddLine(ImVec2(e.x - r - k, e.y), ImVec2(e.x - r, e.y), corner, 1.6f);
    dl->AddLine(ImVec2(e.x, p.y + r + k), ImVec2(e.x, p.y + r), corner, 1.6f);
    dl->AddLine(ImVec2(e.x - r, p.y), ImVec2(e.x - r - k, p.y), corner, 1.6f);
    dl->AddLine(ImVec2(p.x, e.y - r - k), ImVec2(p.x, e.y - r), corner, 1.6f);
    dl->AddLine(ImVec2(p.x + r, e.y), ImVec2(p.x + r + k, e.y), corner, 1.6f);
    dl->AddCircleFilled(ImVec2(p.x + r + 7 * g_s, p.y + r * 0.62f), 2.0f * g_s, dot);
    dl->AddCircleFilled(ImVec2(e.x - r - 7 * g_s, e.y - r * 0.62f), 2.0f * g_s, dot);
}

static void DrawBackdrop(ImDrawList *dl, ImVec2 size) {
    const Theme &t = T();
    dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(3, 2, 4, 148));  // the game stays visible behind the glass
    dl->AddRectFilledMultiColor(ImVec2(0, 0), size, A(t.accent, 28), A(t.bg, 12), IM_COL32(0, 0, 0, 184),
                                IM_COL32(0, 0, 0, 214));
    for (int i = 10; i >= 1; i--)
        dl->AddCircleFilled(ImVec2(size.x * 0.80f, size.y * 0.12f), (size.x * 0.045f + i * 18 * g_s),
                            A(t.accent, 3), 80);
    for (float x = -size.y; x < size.x; x += 72 * g_s)
        dl->AddLine(ImVec2(x, 0), ImVec2(x + size.y, size.y), A(t.accent, 5), 1.0f);
    // vignette: the edges fall away, so the floating panel sits in the scene
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, size.y * 0.22f), IM_COL32(0, 0, 0, 150),
                                IM_COL32(0, 0, 0, 150), 0, 0);
    dl->AddRectFilledMultiColor(ImVec2(0, size.y * 0.80f), size, 0, 0, IM_COL32(0, 0, 0, 170),
                                IM_COL32(0, 0, 0, 170));
    // drifting embers / dust, fixed in place (no animation cost)
    for (int i = 0; i < 26; i++) {
        float fx = (float)((i * 73) % 100) / 100.0f, fy = (float)((i * 37) % 100) / 100.0f;
        dl->AddCircleFilled(ImVec2(fx * size.x, fy * size.y), (i % 3 ? 1.0f : 1.7f) * g_s,
                            A(i % 4 ? t.accent2 : t.accent, 22 + (i % 3) * 12), 6);
    }
    Motif(dl, g_config.ui_theme, ImVec2(size.x * 0.86f, size.y * 0.16f), 96 * g_s, 30);
    Motif(dl, g_config.ui_theme, ImVec2(size.x * 0.10f, size.y * 0.86f), 58 * g_s, 20);
    if (g_config.ui_theme == 11) {
        SpiderWeb(dl, ImVec2(size.x - 8 * g_s, 8 * g_s), 120 * g_s, 38);
        Pumpkin(dl, ImVec2(size.x * 0.055f, size.y * 0.55f), 14 * g_s, 60);
    }
}

// [AllGenU] (Acerca de) the credits page is appended as index 6: the original six pages keep their indices, so every
// existing shortcut, badge and page switch (g_page == 4 from the theme card...) still points where it did.
static const char *kNav[7] = {"Gráficos", "Mods", "Sistema", "Controles", "Aspecto", "Guardados", "Acerca de"};
static const char *kKicker[7] = {"CONFIGURACIÓN DE GRÁFICOS", "GESTOR DE MODIFICACIONES", "AJUSTES DE SISTEMA",
                                 "MAPEO DE CONTROLES", "PERSONALIZACIÓN DE ASPECTO", "GESTOR DE PARTIDAS GUARDADAS",
                                 "INFORMACIÓN DEL PROYECTO"};
static const char *kTitle[7] = {"Motor Gráfico y Pantalla", "Gestor de Mods", "Sistema y Carpetas",
                                "Controles y Periféricos", "Estudio de Aspecto y Temas", "Bóveda de Guardados",
                                "Acerca de AllGenU Recomp"};
static const char *kSub[7] = {
    "Vulkan nativo, cadencia y escalado interno del port.",
    "Modificaciones RomFS y parches, cargadas desde la carpeta mods/.",
    "Reinicio, accesos directos y valores de fábrica de AllGenU.",
    "Asignación del mando, teclado, ratón y disposición de botones.",
    "Atmósferas de Monster Hunter Generations Ultimate para KZMUI.",
    "Copias de seguridad, importación y exportación de partidas.",
    "Créditos, enlaces oficiales y aviso de distribución.",
};

static void DrawSidebar(ImDrawList *dl, ImVec2 p, ImVec2 size, bool *keep_open) {
    const Theme &t = T();
    ImVec2 e(p.x + size.x, p.y + size.y);
    float rr = R(g_config.ui_sharp ? 2 : 12);
    dl->AddRectFilled(ImVec2(p.x + 1, p.y + 3), ImVec2(e.x + 1, e.y + 3), A(IM_COL32(0, 0, 0, 255), 90), rr);
    // [AllGenU] rounded: the sidebar plate used a square gradient fill under a rounded outline
    dl->AddRectFilled(p, e, A(t.panel, 205), rr);
    dl->AddRectFilled(ImVec2(p.x, p.y + (e.y - p.y) * 0.55f), e, A(t.bg, 150), rr, ImDrawFlags_RoundCornersBottom);
    dl->AddRect(p, e, A(t.border, 210), rr);
    dl->AddLine(ImVec2(p.x + 12 * g_s, p.y + 1), ImVec2(e.x - 12 * g_s, p.y + 1), A(t.text, 26), 1.0f);

    // brand: AllGenU / KZMUI, on a raised plaque with a lit rim
    ImVec2 lp(p.x + 16 * g_s, p.y + 16 * g_s);
    float ls = 40 * g_s;
    ImVec2 lc(lp.x + ls * 0.5f, lp.y + ls * 0.5f);
    for (int i = 4; i >= 1; i--)
        dl->AddRect(ImVec2(lp.x - i * 3 * g_s, lp.y - i * 3 * g_s), ImVec2(lp.x + ls + i * 3 * g_s, lp.y + ls + i * 3 * g_s),
                    A(t.accent, 22), R(10 + i * 3), 0, 1.5f);
    dl->AddCircleFilled(lc, ls * 0.95f, A(t.accent, 16), 28);
    dl->AddRectFilled(ImVec2(lp.x, lp.y + 2 * g_s), ImVec2(lp.x + ls, lp.y + ls + 2 * g_s), A(IM_COL32(0, 0, 0, 255), 110),
                      R(g_config.ui_sharp ? 2 : 10));
    // [AllGenU] (UI bugfix) the logo plaque keeps its radius: a rounded gradient instead of the square-filled one
    GradV(dl, lp, ImVec2(lp.x + ls, lp.y + ls), t.accent2, Mix(t.accent, IM_COL32(0, 0, 0, 255), 0.30f),
          R(g_config.ui_sharp ? 2 : 10));
    dl->AddRectFilled(ImVec2(lp.x + 3 * g_s, lp.y + 3 * g_s), ImVec2(lp.x + ls - 3 * g_s, lp.y + ls - 3 * g_s),
                      A(IM_COL32(20, 12, 12, 255), 175), R(g_config.ui_sharp ? 1 : 7));
    dl->AddLine(ImVec2(lp.x + 7 * g_s, lp.y + 3.5f * g_s), ImVec2(lp.x + ls - 7 * g_s, lp.y + 3.5f * g_s),
                A(t.text, 70), 1.0f);
    ImVec2 at = g_bold->CalcTextSizeA(23 * g_s, FLT_MAX, 0.0f, "A");
    dl->AddText(g_bold, 23 * g_s, ImVec2(lp.x + (ls - at.x) * 0.5f + 1, lp.y + (ls - at.y) * 0.5f),
                A(IM_COL32(0, 0, 0, 255), 150), "A");
    dl->AddText(g_bold, 23 * g_s, ImVec2(lp.x + (ls - at.x) * 0.5f, lp.y + (ls - at.y) * 0.5f - 1 * g_s),
                t.accent2, "A");
    // plaque corner ticks
    ImU32 tick = A(t.accent2, 150);
    for (int k = 0; k < 4; k++) {
        float sx = k % 2 ? -1.0f : 1.0f, sy = k / 2 ? -1.0f : 1.0f;
        ImVec2 q(lc.x + sx * (ls * 0.5f + 9 * g_s), lc.y + sy * (ls * 0.5f + 9 * g_s));
        dl->AddLine(ImVec2(q.x - sx * 6 * g_s, q.y), ImVec2(q.x, q.y), tick, 1.3f);
        dl->AddLine(ImVec2(q.x, q.y), ImVec2(q.x, q.y - sy * 6 * g_s), tick, 1.3f);
    }
    float bx = lp.x + ls + 11 * g_s;
    dl->AddText(g_bold, 9.0f * g_s, ImVec2(bx, p.y + 17 * g_s), t.accent, "ALLGENU");
    dl->AddText(g_bold, 23 * g_s, ImVec2(bx, p.y + 29 * g_s), t.text, "KZMUI");
    dl->AddText(g_body, 7.8f * g_s, ImVec2(bx, p.y + 56 * g_s), t.dim, "INTERFAZ DE ALLGENU");
    float sy2 = p.y + 88 * g_s;
    dl->AddLine(ImVec2(p.x + 14 * g_s, sy2), ImVec2(e.x * 0.5f - 8 * g_s, sy2), A(t.border, 190), 1.0f);
    dl->AddLine(ImVec2(e.x * 0.5f + 8 * g_s, sy2), ImVec2(e.x - 14 * g_s, sy2), A(t.border, 190), 1.0f);
    dl->AddQuadFilled(ImVec2(e.x * 0.5f, sy2 - 3 * g_s), ImVec2(e.x * 0.5f + 3 * g_s, sy2),
                      ImVec2(e.x * 0.5f, sy2 + 3 * g_s), ImVec2(e.x * 0.5f - 3 * g_s, sy2), A(t.accent2, 170));

    // navigation
    float y = p.y + 104 * g_s;
    float navW = size.x - 22 * g_s;
    // [AllGenU] (Acerca de) seven rows now share the column with the telemetry card: the row height is sized from the
    // room above that card, so the seventh entry can never overlap it on a short panel (it compresses to 26 px first).
    const int navCount = (int)(sizeof kNav / sizeof kNav[0]);
    float navGap = 5 * g_s;
    float navRoom = (e.y - 168 * g_s) - y - navGap * (navCount - 1);
    float navH = std::clamp(navRoom / navCount, 26 * g_s, 40 * g_s);
    char badge[20];
    for (int i = 0; i < navCount; i++) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + 11 * g_s, y));
        ImGui::PushID(i);
        ImGui::InvisibleButton("##nav", ImVec2(navW, navH));
        bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) g_page = i;
        bool active = g_page == i;
        ImVec2 a(p.x + 11 * g_s, y), b(a.x + navW, y + navH);
        float nr = R(g_config.ui_sharp ? 2 : 7);
        // every row rests on a glass plate with a shadowed lip: the sidebar reads as stacked keys
        dl->AddRectFilled(ImVec2(a.x + 1, a.y + 2), ImVec2(b.x + 1, b.y + 2), A(IM_COL32(0, 0, 0, 255), 70), nr);
        // [AllGenU] rounded plate (the gradient fill used to square off this button's corners)
        dl->AddRectFilled(a, b, A(t.card2, active ? 235 : 120), nr);
        dl->AddRectFilled(ImVec2(a.x, a.y + navH * 0.55f), b, A(t.card, active ? 170 : 70), nr, ImDrawFlags_RoundCornersBottom);
        dl->AddRect(a, b, active ? A(t.accent, 190) : A(t.border, 120), nr, 0, active ? 1.4f : 1.0f);
        dl->AddLine(ImVec2(a.x + nr, a.y + 1.2f), ImVec2(b.x - nr, a.y + 1.2f), A(t.text, active ? 40 : 18), 1.0f);
        if (active) {
            // [AllGenU] (UI bugfix) rounded highlight: the gradient fill used to square the selected row's corners
            GradV(dl, a, b, A(t.accent, 62), A(t.bg, 40), nr);
            dl->AddRectFilled(ImVec2(a.x, a.y + 6 * g_s), ImVec2(a.x + 2.5f * g_s, b.y - 6 * g_s), t.accent2, R(2));
            dl->AddCircleFilled(ImVec2(a.x + 2.5f * g_s, (a.y + b.y) * 0.5f), 4.0f * g_s, A(t.accent, 60), 12);
            dl->AddCircleFilled(ImVec2(a.x + 2.5f * g_s, (a.y + b.y) * 0.5f), 1.8f * g_s, t.accent2, 10);
        } else if (hovered) {
            dl->AddRectFilled(a, b, A(t.card2, 90), nr);
            dl->AddRect(a, b, A(t.accent, 80), nr);
        }
        DrawIcon(dl, i, ImVec2(a.x + 21 * g_s, y + navH * 0.5f), 8.5f * g_s,
                 active ? t.accent : (hovered ? t.text : t.dim));
        dl->AddText(g_bold, 12.0f * g_s, ImVec2(a.x + 36 * g_s, y + 12.5f * g_s), active ? t.text : t.dim, kNav[i]);
        if (i == 0) snprintf(badge, sizeof badge, "%d Hz", g_config.fps);
        else if (i == 1) snprintf(badge, sizeof badge, "%s", g_config.mods_enabled ? "ON" : "OFF");
        else if (i == 2) snprintf(badge, sizeof badge, "SYS");
        else if (i == 3) snprintf(badge, sizeof badge, "PAD");
        else if (i == 4) snprintf(badge, sizeof badge, "T%d", g_config.ui_theme + 1);
        else if (i == 5) snprintf(badge, sizeof badge, "SAVE");
        else snprintf(badge, sizeof badge, "INFO");
        ImVec2 bs = g_body->CalcTextSizeA(8.0f * g_s, FLT_MAX, 0.0f, badge);
        float bw = bs.x + 11 * g_s;
        ImVec2 ba(b.x - bw - 7 * g_s, y + 12 * g_s), bb(ba.x + bw, ba.y + 16 * g_s);
        dl->AddRectFilled(ImVec2(ba.x + 1, ba.y + 1.5f), ImVec2(bb.x + 1, bb.y + 1.5f), A(IM_COL32(0, 0, 0, 255), 60),
                          R(4));
        dl->AddRectFilled(ba, bb, A(active ? t.accent : t.card, active ? 52 : 150), R(4));
        dl->AddRect(ba, bb, A(active ? t.accent2 : t.border, 160), R(4), 0, 1.0f);
        dl->AddText(g_body, 8.0f * g_s, ImVec2(ba.x + 5.5f * g_s, ba.y + (16 * g_s - bs.y) * 0.5f),
                    active ? t.accent2 : t.dim, badge);
        ImGui::PopID();
        y += navH + navGap;
    }

    // live telemetry: frame rate, the real window size, and a sparkline of the last seconds
    float ty = e.y - 160 * g_s;
    ImVec2 ta(p.x + 11 * g_s, ty), tb(e.x - 11 * g_s, ty + 96 * g_s);
    float tr = R(g_config.ui_sharp ? 2 : 8);
    dl->AddRectFilled(ImVec2(ta.x + 1, ta.y + 2), ImVec2(tb.x + 1, tb.y + 2), A(IM_COL32(0, 0, 0, 255), 80), tr);
    // [AllGenU] (UI bugfix) rounded telemetry glass (was a square gradient inside the rounded plate)
    GradV(dl, ta, tb, A(t.card2, 225), A(t.card, 190), tr);
    dl->AddRect(ta, tb, A(t.border, 200), tr);
    dl->AddLine(ImVec2(ta.x + 8 * g_s, ta.y + 1.5f), ImVec2(tb.x - 8 * g_s, ta.y + 1.5f), A(t.text, 34), 1.0f);
    dl->AddRect(ImVec2(ta.x + 2, ta.y + 2), ImVec2(tb.x - 2, tb.y - 2), A(t.text, 14), tr, 0, 1.0f);
    float pulse = 0.55f + 0.45f * (float)sin(ImGui::GetTime() * 2.2);
    dl->AddCircleFilled(ImVec2(ta.x + 12 * g_s, ta.y + 13 * g_s), 2.6f * g_s, A(t.accent2, (int)(120 + 120 * pulse)), 10);
    dl->AddText(g_body, 8.0f * g_s, ImVec2(ta.x + 19 * g_s, ta.y + 8 * g_s), t.dim, "TELEMETRÍA EN VIVO");
    int fps = (int)(overlay_fps() + 0.5f);  // [AllGenU] presented rate (60 with V-Sync, not the engine's 62)
    if (fps < 1 || fps > 999) fps = g_config.fps;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    char fbuf[16], rbuf[64];
    snprintf(fbuf, sizeof fbuf, "%d", fps);
    snprintf(rbuf, sizeof rbuf, "%s · %dx%d", g_config.display_mode == DISPLAY_WINDOWED ? "VENTANA" :
             g_config.display_mode == DISPLAY_FULLSCREEN ? "FULLSCREEN" : "SIN BORDES", (int)ds.x, (int)ds.y);
    dl->AddText(g_bold, 20 * g_s, ImVec2(ta.x + 11 * g_s, ta.y + 21 * g_s), t.accent, fbuf);
    ImVec2 fs = g_bold->CalcTextSizeA(20 * g_s, FLT_MAX, 0.0f, fbuf);
    dl->AddText(g_body, 8.0f * g_s, ImVec2(ta.x + 16 * g_s + fs.x, ta.y + 27 * g_s), t.dim, "FPS");
    dl->AddText(g_body, 7.6f * g_s, ImVec2(ta.x + 11 * g_s, ta.y + 44 * g_s), t.dim, rbuf);
    // sparkline (the history is filled in kzmui_draw)
    float gx0 = ta.x + 11 * g_s, gx1 = tb.x - 11 * g_s, gy0 = ta.y + 59 * g_s, gy1 = ta.y + 79 * g_s;
    dl->AddRectFilled(ImVec2(gx0, gy0), ImVec2(gx1, gy1), A(IM_COL32(0, 0, 0, 255), 60), R(3));
    dl->AddLine(ImVec2(gx0, gy1), ImVec2(gx1, gy1), A(t.border, 130), 1.0f);
    if (g_fpsCount) {
        ImVec2 pts[41], area[43];
        float top = (float)std::max(g_config.fps, 60) * 1.15f;
        int n = g_fpsCount < 40 ? g_fpsCount : 40, start = g_fpsCount < 40 ? 0 : g_fpsIdx;
        for (int i = 0; i < n; i++) {
            float v = g_fpsHist[(start + i) % 40];
            float fx = gx0 + (gx1 - gx0) * (n > 1 ? (float)i / (n - 1) : 1.0f);
            float fy = gy1 - (gy1 - gy0) * std::clamp(v / top, 0.0f, 1.0f);
            pts[i] = ImVec2(fx, fy);
        }
        area[0] = ImVec2(pts[0].x, gy1);
        for (int i = 0; i < n; i++) area[i + 1] = pts[i];
        area[n + 1] = ImVec2(pts[n - 1].x, gy1);
        dl->AddConvexPolyFilled(area, n + 2, A(t.accent, 40));
        dl->AddPolyline(pts, n, A(t.accent2, 210), 0, 1.2f);
        dl->AddCircleFilled(pts[n - 1], 1.8f * g_s, t.accent2, 8);
    }
    dl->AddText(g_body, 7.2f * g_s, ImVec2(ta.x + 11 * g_s, ta.y + 82 * g_s), A(t.dim, 210),
                g_config.vsync ? "V-Sync activo · paso asíncrono" : "V-Sync off · paso inmediato");

    // close
    float ch2 = 32 * g_s;
    ImGui::SetCursorScreenPos(ImVec2(p.x + 11 * g_s, e.y - 46 * g_s));
    ImGui::PushID("close");
    ImGui::InvisibleButton("##close", ImVec2(navW, ch2));
    bool ch = ImGui::IsItemHovered();
    ImVec2 ca(p.x + 11 * g_s, e.y - 46 * g_s), cb(e.x - 11 * g_s, e.y - 14 * g_s);
    dl->AddRectFilled(ImVec2(ca.x + 1, ca.y + 2), ImVec2(cb.x + 1, cb.y + 2), A(IM_COL32(0, 0, 0, 255), 90), R(7));
    // [AllGenU] (UI bugfix) the "volver al juego" button keeps its radius (square gradient before)
    GradV(dl, ca, cb, ch ? Mix(t.card2, t.accent, 0.32f) : Mix(t.card2, t.text, 0.06f), t.card2, R(7));
    dl->AddRect(ca, cb, ch ? A(t.accent, 190) : A(t.border, 190), R(7));
    dl->AddLine(ImVec2(ca.x + 8 * g_s, ca.y + 1.2f), ImVec2(cb.x - 8 * g_s, ca.y + 1.2f), A(t.text, 40), 1.0f);
    const char *cl = "VOLVER AL JUEGO  [ESC]";
    ImVec2 cs = g_bold->CalcTextSizeA(9.0f * g_s, FLT_MAX, 0.0f, cl);
    dl->AddText(g_bold, 9.0f * g_s, ImVec2((ca.x + cb.x - cs.x) * 0.5f, (ca.y + cb.y - cs.y) * 0.5f),
                ch ? t.accent2 : t.text, cl);
    if (ImGui::IsItemClicked()) *keep_open = false;
    ImGui::PopID();
    dl->AddText(g_body, 6.6f * g_s, ImVec2(p.x + 14 * g_s, e.y - 11 * g_s), A(t.dim, 200), "KZMUI 1.0 · ALLGENU");
}

static void DrawHeader(ImDrawList *dl, ImVec2 p, ImVec2 size) {
    const Theme &t = T();
    ImVec2 e(p.x + size.x, p.y + size.y);
    float hr = R(g_config.ui_sharp ? 2 : 11);
    dl->AddRectFilled(ImVec2(p.x + 1, p.y + 3), ImVec2(e.x + 1, e.y + 3), A(IM_COL32(0, 0, 0, 255), 95), hr);
    // [AllGenU] rounded header (gradient fills squared its corners)
    dl->AddRectFilled(p, e, A(t.panel, 214), hr);
    dl->AddRectFilled(p, ImVec2(e.x, p.y + size.y * 0.55f), A(t.bg, 140), hr, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(p.x, p.y + size.y * 0.45f), e, A(t.bg, 120), hr, ImDrawFlags_RoundCornersBottom);
    dl->AddRect(p, e, A(t.accent, 130), hr);
    dl->AddRect(ImVec2(p.x + 1.5f, p.y + 1.5f), ImVec2(e.x - 1.5f, e.y - 1.5f), A(t.border, 150), hr - 1, 0, 1.0f);
    dl->AddLine(ImVec2(p.x + 14 * g_s, p.y + 1.5f), ImVec2(e.x - 14 * g_s, p.y + 1.5f), A(t.text, 36), 1.0f);
    // a faint diagonal hatch anchors the header to the theme
    for (float x = p.x + 240 * g_s; x < e.x; x += 26 * g_s)
        dl->AddLine(ImVec2(x, p.y + size.y), ImVec2(x + size.y * 0.7f, p.y), A(t.accent, 7), 1.0f);
    if (g_config.ui_theme == 11) SpiderWeb(dl, ImVec2(p.x + 2 * g_s, p.y + 2 * g_s), 46 * g_s, 44);

    // page medallion: double ring, icon, and four ticks (the page's own little crest)
    ImVec2 ic(p.x + 29 * g_s, p.y + size.y * 0.5f);
    float ir = 19.0f * g_s;
    dl->AddCircleFilled(ic, ir + 6 * g_s, A(t.accent, 14), 32);
    dl->AddCircleFilled(ic, ir, A(t.bg, 190), 28);
    dl->AddCircle(ic, ir, A(t.accent, 130), 28, 1.2f);
    dl->AddCircle(ic, ir + 4 * g_s, A(t.accent2, 80), 28, 1.0f);
    for (int k = 0; k < 4; k++) {
        float ang = (float)k * 3.14159265f * 0.5f + 0.785f;
        ImVec2 a(ic.x + std::cos(ang) * (ir + 2 * g_s), ic.y + std::sin(ang) * (ir + 2 * g_s));
        ImVec2 b(ic.x + std::cos(ang) * (ir + 7 * g_s), ic.y + std::sin(ang) * (ir + 7 * g_s));
        dl->AddLine(a, b, A(t.accent2, 170), 1.4f);
    }
    DrawIcon(dl, g_page, ic, 8.5f * g_s, t.accent);
    if (g_config.ui_theme == 11) Pumpkin(dl, ImVec2(ic.x + 14 * g_s, ic.y + 15 * g_s), 4.6f * g_s, 235);

    dl->AddLine(ImVec2(p.x + 56 * g_s, p.y + 15 * g_s), ImVec2(p.x + 68 * g_s, p.y + 15 * g_s), t.accent, 1.6f);
    dl->AddText(g_bold, 8.2f * g_s, ImVec2(p.x + 73 * g_s, p.y + 11 * g_s), t.accent, kKicker[g_page]);
    dl->AddText(g_bold, 19 * g_s, ImVec2(p.x + 72 * g_s, p.y + 23 * g_s), t.text, kTitle[g_page]);
    dl->AddText(g_body, 9.0f * g_s, ImVec2(p.x + 73 * g_s, p.y + 49 * g_s), t.dim, kSub[g_page]);
    dl->AddLine(ImVec2(p.x + 73 * g_s, p.y + 64 * g_s), ImVec2(p.x + 210 * g_s, p.y + 64 * g_s), A(t.accent, 90), 1.0f);
    dl->AddCircleFilled(ImVec2(p.x + 214 * g_s, p.y + 64 * g_s), 1.8f * g_s, A(t.accent2, 150));

    // active theme card (click: go to Appearance)
    ImVec2 ta(e.x - 176 * g_s, p.y + 12 * g_s), tb(e.x - 12 * g_s, p.y + 60 * g_s);
    ImGui::SetCursorScreenPos(ta);
    ImGui::PushID("themeCard");
    ImGui::InvisibleButton("##theme", ImVec2(tb.x - ta.x, tb.y - ta.y));
    bool th = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) g_page = 4;
    ImGui::PopID();
    float tr2 = R(g_config.ui_sharp ? 2 : 7);
    dl->AddRectFilled(ImVec2(ta.x + 1, ta.y + 2), ImVec2(tb.x + 1, tb.y + 2), A(IM_COL32(0, 0, 0, 255), 85), tr2);
    // [AllGenU] (UI bugfix) rounded theme card (the gradient used to square its corners on hover)
    GradV(dl, ta, tb, th ? Mix(t.card2, t.accent, 0.22f) : A(t.card2, 220), A(t.card, 195), tr2);
    dl->AddRect(ta, tb, th ? A(t.accent, 180) : A(t.border, 200), tr2);
    dl->AddLine(ImVec2(ta.x + tr2, ta.y + 1.2f), ImVec2(tb.x - tr2, ta.y + 1.2f), A(t.text, 32), 1.0f);
    dl->AddCircleFilled(ImVec2(ta.x + 11 * g_s, ta.y + 14 * g_s), 2.6f * g_s, t.accent);
    dl->AddText(g_body, 7.2f * g_s, ImVec2(ta.x + 18 * g_s, ta.y + 8.5f * g_s), t.dim, "TEMA ACTIVO");
    dl->AddText(g_bold, 10.5f * g_s, ImVec2(ta.x + 10 * g_s, ta.y + 23 * g_s), t.text, T().name);
    char pg[16];
    snprintf(pg, sizeof pg, "%02d/%02d", g_page + 1, (int)(sizeof kNav / sizeof kNav[0]));
    ImVec2 pgs = g_body->CalcTextSizeA(7.2f * g_s, FLT_MAX, 0.0f, pg);
    dl->AddText(g_body, 7.2f * g_s, ImVec2(tb.x - pgs.x - 9 * g_s, tb.y - 12 * g_s), A(t.dim, 180), pg);
    Motif(dl, g_config.ui_theme, ImVec2(tb.x - 18 * g_s, tb.y - 22 * g_s), 7.5f * g_s, 170);
    if (g_restart_needed)
        dl->AddCircleFilled(ImVec2(tb.x - 8 * g_s, ta.y + 8 * g_s), 2.2f * g_s, t.accent2);
}

// ------------------------------------------------------------------ filesystem: mods and saves
struct ModEntry {
    fs::path path;
    std::string name;
    bool active;
    std::string kind;
};
static std::vector<ModEntry> g_mods;
static bool g_mods_dirty = true;
static std::string g_mod_status;

static void RefreshMods(void) {
    g_mods.clear();
    std::error_code ec;
    fs::path dir = fs::u8path(mods_dir());
    if (!fs::exists(dir)) mods_make_dir();
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        std::string n = it->path().filename().u8string();
        if (n.empty() || n[0] == '.') continue;
        bool active = n[0] != '_';
        std::string display = active ? n : n.substr(1);
        std::string kind = "Contenido";
        if (fs::exists(it->path() / "romfs") || fs::exists(it->path() / "nativeNX")) kind = "RomFS";
        else if (fs::exists(it->path() / "exefs")) kind = "ExeFS";
        g_mods.push_back({it->path(), display, active, kind});
    }
    std::sort(g_mods.begin(), g_mods.end(), [](const ModEntry &a, const ModEntry &b) {
        if (a.active != b.active) return a.active > b.active;
        return a.name < b.name;
    });
    g_mods_dirty = false;
}

static void ToggleMod(size_t i) {
    if (i >= g_mods.size()) return;
    ModEntry &m = g_mods[i];
    std::string fname = m.path.filename().u8string();
    std::string want = m.active ? "_" + fname : (fname.size() > 1 ? fname.substr(1) : fname);
    fs::path to = m.path.parent_path() / fs::u8path(want);
    std::error_code ec;
    fs::rename(m.path, to, ec);
    if (ec) {
        g_mod_status = "No se pudo cambiar el mod: " + ec.message();
    } else {
        g_mod_status = "Mod " + std::string(m.active ? "desactivado" : "activado") + ". Reinicia para aplicarlo.";
        g_restart_needed = true;
        g_mods_dirty = true;
    }
}

static bool PickFolder(std::string &out) {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    BROWSEINFOA bi = {};
    bi.lpszTitle = "Selecciona una carpeta";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST id = SHBrowseForFolderA(&bi);
    if (!id) return false;
    bool ok = SHGetPathFromIDListA(id, buf) != FALSE;
    CoTaskMemFree(id);
    if (ok) out = buf;
    return ok;
#else
    (void)out;
    return false;
#endif
}

static bool PickZip(std::string &out) {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = "Paquetes ZIP (*.zip)\0*.zip\0Todos los archivos (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = "Importar un mod de AllGenU";
    if (!GetOpenFileNameA(&ofn)) return false;
    out = buf;
    return true;
#else
    (void)out;
    return false;
#endif
}

static void OpenPath(const std::string &path) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#endif
}

// [AllGenU] (Acerca de) open a web link in the player's browser: SDL_OpenURL hands http(s) to the desktop's default
// handler on Windows, macOS and Linux alike, so the credits page needs no platform code of its own.
static void OpenUrl(const char *url) {
    if (SDL_OpenURL(url)) {
        SetStatus("Enlace abierto en el navegador");
    } else {
        SetStatus("No se pudo abrir el enlace");
        fprintf(stderr, "[kzmui] SDL_OpenURL failed for %s: %s\n", url, SDL_GetError());
    }
}

static void ImportModZip(void) {
    std::string zip;
    if (!PickZip(zip)) return;
    std::string cmd = "tar.exe -xf \"" + zip + "\" -C \"" + fs::absolute(fs::u8path(mods_dir())).u8string() + "\"";
    int rc = std::system(cmd.c_str());
    if (rc == 0) {
        g_mod_status = "Paquete importado en mods/. Reinicia para cargarlo.";
        g_mods_dirty = true;
        g_restart_needed = true;
    } else {
        g_mod_status = "No se pudo extraer el ZIP (código " + std::to_string(rc) + ").";
    }
}

static fs::path ActiveSave(void) { return fs::absolute(fs::u8path(g_save_dir ? g_save_dir : "save")) / "save_0"; }
static fs::path BackupRoot(void) {
    return fs::absolute(fs::u8path(g_save_dir ? g_save_dir : "save")).parent_path() / "save_backups";
}
static std::string Stamp(void) {
    time_t now = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char s[32];
    strftime(s, sizeof s, "%Y%m%d_%H%M%S", &tmv);
    return s;
}
static std::string FormatTime(std::time_t t) {
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char s[64];
    strftime(s, sizeof s, "%d/%m/%Y %H:%M", &tmv);
    return s;
}
struct BackupEntry {
    fs::path path;
    std::string name;
    std::string date;
    uintmax_t bytes;
};
static std::vector<BackupEntry> g_backups;
static bool g_backups_dirty = true;
static std::string g_save_status;

static bool CopyDir(const fs::path &src, const fs::path &dst, std::string &err) {
    std::error_code ec;
    if (!fs::exists(src, ec)) {
        err = "la carpeta de origen no existe";
        return false;
    }
    fs::remove_all(dst, ec);
    ec.clear();
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err = ec.message();
        return false;
    }
    return true;
}
static void RefreshBackups(void) {
    g_backups.clear();
    std::error_code ec;
    fs::path root = BackupRoot();
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        uintmax_t bytes = 0;
        for (fs::recursive_directory_iterator fi(it->path(), ec), fe; !ec && fi != fe; fi.increment(ec))
            if (fi->is_regular_file(ec)) bytes += fi->file_size(ec);
        std::string n = it->path().filename().u8string();
        std::time_t t = 0;
        fs::file_time_type ft = fs::last_write_time(it->path(), ec);
        if (!ec) {
            auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            t = std::chrono::system_clock::to_time_t(sys);
        }
        g_backups.push_back({it->path(), n, t ? FormatTime(t) : "—", bytes});
    }
    std::sort(g_backups.begin(), g_backups.end(),
              [](const BackupEntry &a, const BackupEntry &b) { return a.name > b.name; });
    g_backups_dirty = false;
}
static void MakeBackup(const char *prefix) {
    std::string err;
    fs::path root = BackupRoot();
    std::error_code ec;
    fs::create_directories(root, ec);
    fs::path dst = root / (std::string(prefix) + "_" + Stamp());
    if (CopyDir(ActiveSave(), dst, err)) {
        g_save_status = "Copia creada en " + dst.u8string();
        g_backups_dirty = true;
    } else {
        g_save_status = "No se pudo crear la copia: " + err;
    }
}
static void RestoreBackup(const fs::path &src) {
    std::string err;
    MakeBackup("pre_restore");
    if (CopyDir(src, ActiveSave(), err)) {
        g_save_status = "Partida restaurada. Vuelve a cargarla desde el juego.";
    } else {
        g_save_status = "No se pudo restaurar: " + err;
    }
}
static void ExportSave(void) {
    std::string folder;
    if (!PickFolder(folder)) return;
    std::string err;
    fs::path dst = fs::u8path(folder) / ("AllGenU_save_" + Stamp());
    if (CopyDir(ActiveSave(), dst, err)) g_save_status = "Partida exportada a " + dst.u8string();
    else g_save_status = "No se pudo exportar: " + err;
}
static void ImportSave(void) {
    std::string folder;
    if (!PickFolder(folder)) return;
    fs::path p = fs::u8path(folder), src;
    std::error_code ec;
    if (fs::exists(p / "save_0", ec)) src = p / "save_0";
    else if (fs::exists(p / "system", ec)) src = p;
    else {
        for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec) && it->path().filename() == "save_0") { src = it->path(); break; }
    }
    if (src.empty()) {
        g_save_status = "La carpeta elegida no contiene una partida (save_0).";
        return;
    }
    std::string err;
    MakeBackup("pre_import");
    if (CopyDir(src, ActiveSave(), err)) g_save_status = "Partida importada. Reinicia el juego para cargarla.";
    else g_save_status = "No se pudo importar: " + err;
}
static void RestartGame(void) {
#ifdef _WIN32
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return;
    std::string cmd = GetCommandLineA();
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        fflush(stderr);
        ExitProcess(0);
    }
    g_save_status = "No se pudo reiniciar el port.";
#endif
}

static void ResetDefaults(void) {
    g_config.display_mode = DISPLAY_BORDERLESS;
    g_config.width = 1920;
    g_config.height = 1080;
    g_config.render_auto = 1;
    g_config.render_w = 1920;
    g_config.render_h = 1080;
    g_config.fps = 60;
    g_config.vsync = 1;
    g_config.msaa = 1;
    g_config.fxaa = 0;  // [AllGenU] anti-aliasing off by default
    g_config.show_fps = 0;  // [AllGenU] the counter starts off
    g_config.fsr = 0;
    g_config.fsr_sharp = 55;  // [AllGenU] 0..100 now
    g_config.fsr_scale = 100;
    g_config.hdr = 0;
    g_config.false_dlss = 0;
    config_keybinds_default(g_config.keybinds);
    g_config.walk_key = SDL_SCANCODE_LALT;
    g_config.dock_mode = 0;  // [AllGenU] the FSR / Dock additions reset with the rest
    g_config.face_buttons = FACE_POSITION;
    g_config.mouse_sensitivity = 1.0f;
    g_config.ui_theme = 3;
    g_config.ui_sharp = 0;
    g_config.ui_font = 0;
    g_config.mods_enabled = 1;
    vk_set_msaa(1);
    vi_set_framerate(60);
    vk_set_vsync(1);
    // (the 3D size is the guest's render-target size: it changes at the next start, not under a running scene)
    ui_apply_display();
    SetStatus("Valores de fábrica aplicados");
    config_set("Display", "Mode", "Borderless");
    config_set("Display", "Resolution", "1920x1080");
    config_set("Display", "RenderResolution", "Auto");
    config_set("Performance", "FPS", "60");
    config_set("Performance", "VSync", "On");
    config_set("Performance", "ShowFPS", "Off");  // [AllGenU]
    config_set("Anti-aliasing", "MSAA", "Off");
    config_set("Anti-aliasing", "FXAA", "Off");  // [AllGenU]
    config_set("Upscaling", "FSR", "Off");
    config_set("Upscaling", "Sharpness", "55");
    config_set("Upscaling", "FsrScale", "100");
    config_set("Image", "HDR", "Off");
    config_set("Image", "FalseDLSS", "Off");
    host_keymap_save();
    config_set("Display", "DockMode", "Off");  // [AllGenU]
    config_set("Controls", "FaceButtons", "Position");
    config_set("Mouse", "MouseSensitivity", "1.0");
    config_set("Interface", "Theme", kThemeKeys[3]);
    config_set("Interface", "Geometry", "Rounded");
    config_set("Interface", "Font", "Segoe UI");
    config_set("Mods", "ModsEnabled", "On");
    g_restart_needed = true;
    g_save_status = "Valores de fábrica restaurados.";
}

// ------------------------------------------------------------------ pages
static void SaveBool(const char *section, const char *key, bool v) {
    config_set(section, key, v ? "On" : "Off");
}

// Auto 3D resolution: as large as fits the window at 16:9 (the same rule config_load uses at start).
static void AutoRenderSize(int *w, int *h) {
    int sw = g_config.width, sh = g_config.height;
    if (!sw || !sh) {
        const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (dm) sw = dm->w, sh = dm->h;
    }
    if (!sw || !sh) sw = 1920, sh = 1080;
    int rw = sw, rh = sw * 9 / 16;
    if (rh > sh) rh = sh, rw = sh * 16 / 9;
    *w = rw & ~1, *h = rh & ~1;  // (even sizes)
}

static void PageGraphics(void) {
    const float rowH = 188;
    BeginTwoCols("##gfxrow1", 1.30f, 1.0f);
    ImGui::TableNextColumn();
    if (BeginCard("##motor", rowH, "MOTOR GRÁFICO Y CADENCIA", "Vulkan, límite de frames y sincronía")) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float x = ImGui::GetCursorScreenPos().x, y = ImGui::GetCursorScreenPos().y;
        ChipAt(dl, x, y, "Vulkan 1.2", true);
        ChipAt(dl, x, y, "OpenGL ES", false);
        ImGui::Dummy(V(0, 27));
        // --- [AllGenU] added the "∞" (uncapped) entry: fps 0 in the settings means no limit ---
        static const char *const fpsL[8] = {"30", "45", "60", "90", "120", "144", "240", "∞"};
        static const int fpsV[8] = {30, 45, 60, 90, 120, 144, 240, 0};
        int fi = 2;
        for (int i = 0; i < 8; i++) if (fpsV[i] == g_config.fps) fi = i;
        if (Segmented("##fps", fpsL, 8, &fi, 26)) {
            g_config.fps = fpsV[fi];
            vi_set_framerate(g_config.fps);
            config_set("Performance", "FPS", std::to_string(g_config.fps).c_str());
            char b[56];
            if (g_config.fps == 0) snprintf(b, sizeof b, "Sin límite de FPS: el GPU marca el ritmo");
            else snprintf(b, sizeof b, "Límite de %d FPS aplicado al juego", g_config.fps);
            SetStatus(b);
        }
        Spacer(4);
        bool vsync = g_config.vsync != 0;
        if (ToggleRow("##vsync", "V-Sync", "Sin tearing · cambia en el siguiente frame", &vsync)) {
            g_config.vsync = vsync ? 1 : 0;
            SaveBool("Performance", "VSync", vsync);
            vk_set_vsync(g_config.vsync);
            SetStatus(vsync ? "V-Sync activado" : "V-Sync desactivado");
        }
    }
    EndCard();
    ImGui::TableNextColumn();
    if (BeginCard("##window", rowH, "VENTANA Y PRESENTACIÓN", "Tamaño real de la ventana del juego")) {
        // The list's order is the interface's, not the DISPLAY_* values': map both ways (Borderless is 2, not 1).
        static const char *const modeL[3] = {"Ventana", "Sin bordes", "Fullscreen"};
        static const int modeV[3] = {DISPLAY_WINDOWED, DISPLAY_BORDERLESS, DISPLAY_FULLSCREEN};
        int mode = 0;
        for (int i = 0; i < 3; i++) if (modeV[i] == g_config.display_mode) mode = i;
        if (Segmented("##mode", modeL, 3, &mode, 26)) {
            g_config.display_mode = modeV[mode];
            int dm = g_config.display_mode;
            const char *v = dm == DISPLAY_WINDOWED ? "Windowed" : dm == DISPLAY_FULLSCREEN ? "Fullscreen" : "Borderless";
            config_set("Display", "Mode", v);
            ui_apply_display();
            SetStatus(dm == DISPLAY_WINDOWED ? "Modo ventana aplicado" : dm == DISPLAY_FULLSCREEN ? "Pantalla completa aplicada" : "Modo sin bordes aplicado");
        }
        Spacer(4);
        static const char *const resL[5] = {"720p", "900p", "1080p", "1440p", "Nativa"};
        static const int resV[5][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {0, 0}};
        int ri = 2;
        for (int i = 0; i < 5; i++) if (resV[i][0] == g_config.width && resV[i][1] == g_config.height) ri = i;
        if (Segmented("##res", resL, 5, &ri, 26)) {
            g_config.width = resV[ri][0];
            g_config.height = resV[ri][1];
            char buf[32];
            if (g_config.width) snprintf(buf, sizeof buf, "%dx%d", g_config.width, g_config.height);
            else snprintf(buf, sizeof buf, "Desktop");
            config_set("Display", "Resolution", buf);
            if (g_config.render_auto) {  // (the 3D size follows the new screen size at the next start, not mid-game)
                int aw, ah;
                AutoRenderSize(&aw, &ah);
                g_config.render_w = aw, g_config.render_h = ah;
            }
            ui_apply_display();
            SetStatus(g_config.width ? "Tamaño de ventana aplicado" : "Resolución nativa aplicada");
        }
        Spacer(6);
        const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        char cur[128];
        if (g_config.display_mode == DISPLAY_FULLSCREEN && dm)
            snprintf(cur, sizeof cur, "Pantalla completa · monitor %dx%d", dm->w, dm->h);
        else
            snprintf(cur, sizeof cur, "Ventana ahora · %dx%d", (int)ds.x, (int)ds.y);
        SmallTxt(cur);
        if (g_config.display_mode == DISPLAY_BORDERLESS)
            SmallTxt("Sin bordes respeta el tamaño elegido; «Nativa» cubre el monitor.");
    }
    EndCard();
    EndTwoCols();

    Spacer(0);
    char rsub[128];
    if (g_config.render_auto)
        snprintf(rsub, sizeof rsub, "Resolución del 3D del juego · Auto %dx%d · se aplica al reiniciar", g_config.render_w,
                 g_config.render_h);
    else
        snprintf(rsub, sizeof rsub, "Resolución del 3D del juego · %dx%d fija · se aplica al reiniciar", g_config.render_w,
                 g_config.render_h);
    if (BeginCard("##renderres", 116, "ESCALADO DE RESOLUCIÓN INTERNA", rsub)) {
        static const char *const rrL[7] = {"Auto", "540p", "720p", "900p", "1080p", "1440p", "4K"};
        static const int rrV[6][2] = {{960, 540}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
        int r = 0;
        if (!g_config.render_auto)
            for (int i = 0; i < 6; i++) if (rrV[i][0] == g_config.render_w && rrV[i][1] == g_config.render_h) r = i + 1;
        if (Segmented("##rr", rrL, 7, &r, 26)) {
            if (r == 0) {
                g_config.render_auto = 1;
                AutoRenderSize(&g_config.render_w, &g_config.render_h);
                config_set("Display", "RenderResolution", "Auto");
            } else {
                g_config.render_auto = 0;
                g_config.render_w = rrV[r - 1][0];
                g_config.render_h = rrV[r - 1][1];
                char buf[32];
                snprintf(buf, sizeof buf, "%dx%d", g_config.render_w, g_config.render_h);
                config_set("Display", "RenderResolution", buf);
            }
            g_restart_needed = true;  // (the guest creates its render targets once, at start)
            SetStatus(g_config.render_auto ? "Resolución 3D en Auto · se aplica al reiniciar"
                                            : "Resolución 3D guardada · se aplica al reiniciar");
        }
    }
    EndCard();

    // --- [AllGenU] two new cards: Dock mode and FSR 1.0 (added below the existing ESCALADO card) ---
    Spacer(0);
    if (BeginCard("##dockcard", 148, "MODO DOCK", "Perfil fijo 720p/1080p nativo · HUD 1:1 · se aplica al reiniciar")) {
        bool dock = g_config.dock_mode != 0;
        if (ToggleRow("##dockmode", "Dock mode", "Resolución 3D fija nativa + FSR al encender; fuentes nítidas", &dock)) {
            g_config.dock_mode = dock ? 1 : 0;
            SaveBool("Display", "DockMode", dock);
            if (dock) {
                // the profile: 1080p when the window is at least that big, else 720p (both 16:9, the HUD at 1:1)
                int rw, rh;
                AutoRenderSize(&rw, &rh);  // the window's size at 16:9 (the same helper the Auto setting uses)
                g_config.render_auto = 0;
                if (rw >= 1920 && rh >= 1080) rw = 1920, rh = 1080;
                else rw = 1280, rh = 720;
                g_config.render_w = rw;
                g_config.render_h = rh;
                char buf[32];
                snprintf(buf, sizeof buf, "%dx%d", rw, rh);
                config_set("Display", "RenderResolution", buf);
                if (!g_config.fsr) {  // sharp upscale to the window: FSR turns on with the profile (instant)
                    g_config.fsr = 1;
                    config_set("Upscaling", "FSR", "On");
                }
                g_restart_needed = true;
                char b[64];
                snprintf(b, sizeof b, "Dock mode: perfil %dx%d + FSR (al reiniciar)", rw, rh);
                SetStatus(b);
            } else {
                g_config.render_auto = 1;
                AutoRenderSize(&g_config.render_w, &g_config.render_h);
                config_set("Display", "RenderResolution", "Auto");
                g_restart_needed = true;
                SetStatus("Dock mode desactivado · resolución 3D en Auto");
            }
        }
        SmallTxt("HUD en su lienzo de 720 líneas; si FSR tiene escala propia, esa manda al arrancar.");  // [AllGenU]
    }
    EndCard();

    Spacer(0);
    // [AllGenU] FSR 1.0: switch, the internal render scale (where the upscale comes from), a real sharpening slider and
    // the four presets asked for. The switch and sharpness apply on the next frame (exec.c reads g_config per draw);
    // the scale is the guest's render target size, which it creates once at start, so it is applied at the next start.
    if (BeginCard("##fsrcard", 300, "ESCALADO FSR 1.0", "EASU + RCAS · nitidez en caliente · la escala, al reiniciar")) {
        bool fsr = g_config.fsr != 0;
        if (ToggleRow("##fsrtoggle", "FSR 1.0", "Escalado adaptivo + nitidez; actúa ya al cambiar de frame", &fsr)) {
            g_config.fsr = fsr ? 1 : 0;
            SaveBool("Upscaling", "FSR", fsr);
            SetStatus(fsr ? "FSR 1.0 activado (en caliente)" : "FSR 1.0 desactivado (bilineal)");
        }
        Spacer(2);
        // the upscale source: 100% leaves EASU with nothing to resample (RCAS still sharpens), 75% is a real 1.33x
        Label("ESCALA INTERNA DEL 3D (FSR)");
        int fsc = g_config.fsr_scale;
        if (fsc < 50 || fsc > 100) fsc = 100;
        ImGui::PushItemWidth(-1);
        if (ImGui::SliderInt("##fsrscale", &fsc, 50, 100, "%d%%")) g_config.fsr_scale = fsc;
        ImGui::PopItemWidth();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            char v[8];
            snprintf(v, sizeof v, "%d", g_config.fsr_scale);
            config_set("Upscaling", "FsrScale", v);
            g_restart_needed = true;
            SetStatus("Escala FSR guardada · se aplica al reiniciar");
        }
        {
            char line[96];
            ImVec2 ds = ImGui::GetIO().DisplaySize;  // [AllGenU] the real window (Fullscreen picks the closest mode)
            int pw = (int)(ds.x * g_config.fsr_scale / 100.0f + 0.5f) & ~1;
            int ph = (int)(ds.y * g_config.fsr_scale / 100.0f + 0.5f) & ~1;
            snprintf(line, sizeof line, "3D a %d%% de la ventana (%dx%d) y EASU lo sube al tamaño real%s", g_config.fsr_scale, pw, ph,
                     g_restart_needed ? " · al reiniciar" : "");
            SmallTxt(line);
        }
        Spacer(3);
        Label("NITIDEZ RCAS");
        int sh = g_config.fsr_sharp;
        if (sh < 0) sh = 0;
        if (sh > 100) sh = 100;
        ImGui::PushItemWidth(-1);
        if (ImGui::SliderInt("##fsrsharpslider", &sh, 0, 100, "%d / 100")) g_config.fsr_sharp = sh;
        ImGui::PopItemWidth();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            char v[8];
            snprintf(v, sizeof v, "%d", g_config.fsr_sharp);
            config_set("Upscaling", "Sharpness", v);
            SetStatus("Nitidez FSR aplicada al instante");
        }
        Spacer(2);
        static const char *const shL[4] = {"Nítido", "Muy nítido", "Ultra nítido", "Extremo"};
        static const int shV[4] = {55, 70, 85, 100};
        int si = 0, best = 1000;
        for (int i = 0; i < 4; i++) {
            int d = shV[i] - g_config.fsr_sharp;
            if (d < 0) d = -d;
            if (d < best) best = d, si = i;
        }
        if (Segmented("##fsrsharp", shL, 4, &si, 26)) {
            g_config.fsr_sharp = shV[si];
            char v[8];
            snprintf(v, sizeof v, "%d", g_config.fsr_sharp);
            config_set("Upscaling", "Sharpness", v);
            SetStatus(si == 3 ? "Nitidez FSR: extrema (puede anillar en texto)" : "Nitidez FSR aplicada al instante");
        }
        SmallTxt("Nítido es el ajuste seguro para el texto del HUD; Extremo realza mucho y puede anillar.");
    }
    EndCard();

    // [AllGenU] the image look of the present pass (shaders.c): HDR and the "False DLSS" look, both live.
    Spacer(0);
    if (BeginCard("##imagecard", 178, "CALIDAD DE IMAGEN", "Grading sobre la imagen final · se aplica al instante")) {
        bool hdr = g_config.hdr != 0;
        if (ToggleRow("##hdr", "HDR (por software)", "Más saturación y contraste suave: colores más vivos", &hdr)) {
            g_config.hdr = hdr ? 1 : 0;
            if (hdr) g_config.false_dlss = 0;  // (the two looks are exclusive: one grading at a time)
            SaveBool("Image", "HDR", hdr);
            if (hdr) SaveBool("Image", "FalseDLSS", false);
            SetStatus(hdr ? "HDR activado (en caliente)" : "HDR desactivado");
        }
        Spacer(1);
        bool fdl = g_config.false_dlss != 0;
        if (ToggleRow("##fdlss", "False DLSS", "Desaturado, más luminoso y sin resplandor: imagen limpia", &fdl)) {
            g_config.false_dlss = fdl ? 1 : 0;
            if (fdl) g_config.hdr = 0;
            SaveBool("Image", "FalseDLSS", fdl);
            if (fdl) SaveBool("Image", "HDR", false);
            SetStatus(fdl ? "False DLSS activado: look limpio (en caliente)" : "False DLSS desactivado");
        }
        SmallTxt(g_config.false_dlss ? "Comprime los altos brillos: el glow del juego deja de florecer sobre la imagen."
                                     : "El brillo (bloom) es un pase del propio juego; este look lo aplana sin quitarlo.");
    }
    EndCard();

    Spacer(0);
    if (BeginCard("##fpscard", 128, "CONTADOR DE FPS", "Arriba a la derecha de la pantalla · en caliente")) {
        bool fpson = g_config.show_fps != 0;
        if (ToggleRow("##showfps", "Mostrar FPS", "Velocidad y tiempo de frame encima del juego", &fpson)) {
            g_config.show_fps = fpson ? 1 : 0;
            SaveBool("Performance", "ShowFPS", fpson);
            SetStatus(fpson ? "Contador de FPS activado" : "Contador de FPS desactivado");
        }
    }
    EndCard();

    // [AllGenU] the ANTI-ALIASING card (MSAA + FXAA) is gone from the menu, as asked: both stay off by default, and the
    // [Anti-aliasing] keys of allgenurecomp.ini still work for anyone who wants them back.
    Spacer(0);
    if (BeginCard("##state", 148, "ESTADO DEL RENDERIZADOR", "Configuración activa de KZMUI")) {
        char line[110];
        snprintf(line, sizeof line, "API   ·  Vulkan 1.2 nativo");
        SmallTxt(line);
        snprintf(line, sizeof line, "3D    ·  %dx%d%s", g_config.render_w, g_config.render_h,
                 g_restart_needed ? " (al reiniciar)" : "");
        SmallTxt(line);
        if (g_config.fps)  // [AllGenU] 0 = uncapped
            snprintf(line, sizeof line, "FPS   ·  %d objetivo  ·  %s", g_config.fps, g_config.fsr ? "FSR on" : "FSR off");
        else
            snprintf(line, sizeof line, "FPS   ·  sin límite  ·  %s", g_config.fsr ? "FSR on" : "FSR off");
        SmallTxt(line);
        snprintf(line, sizeof line, "Salida ·  Dock %s  ·  V-Sync %s", g_config.dock_mode ? "on" : "off",
                 g_config.vsync ? "on" : "off");
        SmallTxt(line);
        Spacer(2);
        if (g_config.vsync && g_config.fps > 60)
            SmallTxt("V-Sync on limita al monitor; desactívalo para más FPS.");
        else
            SmallTxt("FPS, V-Sync y FSR cambian en caliente; Dock mode pide reinicio.");
    }
    EndCard();
    if (g_restart_needed) {
        Spacer(6);
        if (KButton("Reiniciar para aplicar la resolución 3D", true)) RestartGame();
        ImGui::SameLine();
        SmallTxt("La configuración ya está guardada en allgenurecomp.ini.");
    }
}

static void PageMods(void) {
    if (BeginCard("##modsenable", 118, "SISTEMA DE MODS", "Capas RomFS, parches ExeFS y plugins de Forge")) {
        bool on = g_config.mods_enabled != 0;
        if (ToggleRow("##mods", "Mods activos", "Desactivado: mods/ se ignora por completo", &on)) {
            g_config.mods_enabled = on ? 1 : 0;
            SaveBool("Mods", "ModsEnabled", on);
            g_restart_needed = true;
            SetStatus(on ? "Mods activados: se cargan al reiniciar" : "Mods desactivados: se quitan al reiniciar");
        }
    }
    EndCard();
    Spacer(8);
    if (KButton("Importar ZIP...", true)) ImportModZip();
    ImGui::SameLine();
    if (KButton("Abrir carpeta mods/")) {
        mods_make_dir();
        OpenPath(fs::absolute(fs::u8path(mods_dir())).u8string());
    }
    ImGui::SameLine();
    if (KButton("Recargar")) {
        g_mods_dirty = true;
        g_mod_status = "Lista de mods actualizada.";
    }
    if (!g_mod_status.empty()) {
        Spacer(4);
        SmallTxt(g_mod_status.c_str());
    }
    Spacer(8);
    if (g_mods_dirty) RefreshMods();
    int active = 0;
    for (const ModEntry &m : g_mods) active += m.active ? 1 : 0;
    char head[128];
    snprintf(head, sizeof head, "%d mod(s) activo(s) de %d instalado(s)", active, (int)g_mods.size());
    if (BeginCard("##modlist", 286, "MODS INSTALADOS", head)) {
        if (g_mods.empty()) {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            DrawIcon(dl, 1, ImVec2(p.x + w * 0.5f, p.y + 54 * g_s), 17 * g_s, A(T().accent, 190));
            const char *l1 = "No hay mods instalados en la carpeta mods/";
            ImVec2 s1 = g_bold->CalcTextSizeA(13 * g_s, FLT_MAX, 0.0f, l1);
            dl->AddText(g_bold, 13 * g_s, ImVec2(p.x + (w - s1.x) * 0.5f, p.y + 82 * g_s), T().text, l1);
            const char *l2 = "Usa «Importar ZIP...» o copia una carpeta de mod en mods/.";
            ImVec2 s2 = g_body->CalcTextSizeA(10.5f * g_s, FLT_MAX, 0.0f, l2);
            dl->AddText(g_body, 10.5f * g_s, ImVec2(p.x + (w - s2.x) * 0.5f, p.y + 104 * g_s), T().dim, l2);
            ImGui::Dummy(V(0, 150));
        } else {
            ImGui::BeginChild("##modscroll", ImVec2(0, 204 * g_s), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground);
            for (size_t i = 0; i < g_mods.size(); i++) {
                ImGui::PushID((int)i);
                ImVec2 p = ImGui::GetCursorScreenPos();
                float w = ImGui::GetContentRegionAvail().x, h = 48 * g_s;
                ImGui::InvisibleButton("##mod", ImVec2(w, h));
                bool hov = ImGui::IsItemHovered();
                ImDrawList *dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov ? Mix(T().card2, T().accent, 0.10f) : A(T().card2, 110),
                                  R(6));
                dl->AddRect(p, ImVec2(p.x + w, p.y + h), T().border, R(6));
                DrawIcon(dl, 1, ImVec2(p.x + 18 * g_s, p.y + h * 0.5f), 8 * g_s, g_mods[i].active ? T().accent : T().dim);
                dl->AddText(g_bold, 11.5f * g_s, ImVec2(p.x + 34 * g_s, p.y + 8 * g_s), T().text,
                            g_mods[i].name.c_str());
                std::string meta = g_mods[i].kind + (g_mods[i].active ? " · activo" : " · desactivado");
                dl->AddText(g_body, 9.5f * g_s, ImVec2(p.x + 34 * g_s, p.y + 26 * g_s),
                            g_mods[i].active ? T().accent2 : T().dim, meta.c_str());
                float sw = 34 * g_s, sh = 18 * g_s;
                ImVec2 a(p.x + w - sw - 10 * g_s, p.y + (h - sh) * 0.5f), b(a.x + sw, a.y + sh);
                dl->AddRectFilled(a, b, g_mods[i].active ? T().accent : T().card, sh * 0.5f);
                dl->AddRect(a, b, g_mods[i].active ? T().accent2 : T().border, sh * 0.5f);
                float kx = g_mods[i].active ? b.x - sh * 0.5f - 2 * g_s : a.x + sh * 0.5f + 2 * g_s;
                dl->AddCircleFilled(ImVec2(kx, a.y + sh * 0.5f), sh * 0.5f - 3 * g_s,
                                    g_mods[i].active ? IM_COL32(255, 255, 255, 255) : T().dim);
                if (ImGui::IsItemClicked()) ToggleMod(i);
                ImGui::PopID();
                ImGui::Dummy(V(0, 2));
            }
            ImGui::EndChild();
        }
    }
    EndCard();
}

static void PageSystem(void *cpu) {
    if (BeginCard("##restart", 102, "REINICIO RÁPIDO DEL PORT", "Guarda los cambios y relanza AllGenU automáticamente")) {
        if (KButton("Reiniciar ahora", true)) RestartGame();
        ImGui::SameLine();
        SmallTxt("Configuración, mods y temas se guardan antes de reiniciar.");
    }
    EndCard();
    Spacer(10);
    BeginTwoCols("##sysdirs", 1.0f, 1.0f);
    ImGui::TableNextColumn();
    if (BeginCard("##savedir", 126, "DIRECTORIO DE GUARDADOS", "Partidas y copias de seguridad")) {
        std::string p = fs::absolute(fs::u8path(g_save_dir ? g_save_dir : "save")).u8string();
        PushText(9.5f);
        Txt(T().dim, p.c_str());
        PopText();
        Spacer(7);
        if (KButton("Abrir carpeta")) OpenPath(p);
    }
    EndCard();
    ImGui::TableNextColumn();
    if (BeginCard("##rootdir", 126, "DIRECTORIO RAÍZ DEL PORT", "mods/, cheats/, datos y ejecutable")) {
        std::string p = fs::current_path().u8string();
        PushText(9.5f);
        Txt(T().dim, p.c_str());
        PopText();
        Spacer(7);
        if (KButton("Abrir ubicación")) OpenPath(p);
    }
    EndCard();
    EndTwoCols();
    Spacer(10);
    if (BeginCard("##factory", 96, "RESTABLECER VALORES DE FÁBRICA",
                  "Devuelve gráficos, ventana, controles, tema y guardado a su estado original")) {
        if (KButton("Restablecer todo")) ImGui::OpenPopup("Restablecer valores");
        ImGui::SameLine();
        SmallTxt("No borra partidas, mods ni capturas.");
    }
    EndCard();

    const char *names[16];
    int n = forge_plugin_titles(names, 16);
    if (n > 0) {
        Spacer(10);
        if (BeginCard("##plugins", 0, "PLUGINS DE FORGE", "Interfaz propia de los plugins cargados")) {
            for (int i = 0; i < n; i++) SmallTxt(names[i]);
            Spacer(4);
            forge_imgui_run(cpu, FORGE_IMGUI_RENDER);
        }
        EndCard();
    }

    if (ImGui::BeginPopupModal("Restablecer valores", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        PushText(13);
        Txt(T().text, "¿Restablecer los valores de fábrica?");
        PopText();
        SmallTxt("Se guardarán los valores por defecto. Reinicia para aplicarlos.");
        Spacer(8);
        if (KButton("Sí, restablecer", true)) {
            ResetDefaults();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (KButton("Cancelar")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static void MappingRow(const char *action, const char *value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    PushText(11.5f, true);
    Txt(T().text, action);
    PopText();
    ImGui::TableNextColumn();
    PushText(10.5f);
    Txt(T().dim, value);
    PopText();
}

// [AllGenU] one rebindable key row: click it, then the next key pressed becomes the binding (runtime/host_input.c
// captures it before the menu sees it, so Escape cancels instead of closing KZMUI).
static void BindKeyRow(int slot, const char *label, int code, bool active, bool cells = true) {
    if (cells) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
    }
    PushText(11.0f);
    Txt(T().text, label);
    PopText();
    if (cells) ImGui::TableNextColumn();
    ImGui::PushID(slot);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 23 * g_s;
    ImGui::InvisibleButton("##bind", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) host_bind_start(slot);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), active ? A(T().accent, 46) : (hov ? T().card2 : A(T().card2, 120)), R(5));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), active ? A(T().accent, 210) : T().border, R(5));
    const char *name = active ? "Pulsa una tecla..."
                       : (code > 0 && code < SDL_SCANCODE_COUNT ? SDL_GetScancodeName((SDL_Scancode)code) : "--");
    ImVec2 ts = g_body->CalcTextSizeA(10.0f * g_s, FLT_MAX, 0.0f, name);
    dl->AddText(g_body, 10.0f * g_s, ImVec2(p.x + 8 * g_s, p.y + (h - ts.y) * 0.5f), active ? T().accent2 : T().text, name);
    ImGui::PopID();
}

static void PageControls(void) {
    static int tab = 0;
    if (BeginCard("##ctrltabs", 60, nullptr)) {
        static const char *const tabs[2] = {"MANDO (GAMEPAD)", "TECLADO Y RATÓN"};
        Segmented("##ctrltab", tabs, 2, &tab, 32);
    }
    EndCard();
    Spacer(6);
    if (tab == 0) {
        if (BeginCard("##padmap", 392, "MAPEO DEL MANDO", "Disposición activa de AllGenU")) {
            Label("DISPOSICIÓN DE BOTONES");
            static const char *const layout[2] = {"Posición (Switch)", "Carácter (Xbox)"};
            int li = g_config.face_buttons == FACE_LABEL ? 1 : 0;
            if (Segmented("##layout", layout, 2, &li)) {
                g_config.face_buttons = li ? FACE_LABEL : FACE_POSITION;
                config_set("Controls", "FaceButtons", li ? "Label" : "Position");
                SetStatus("Disposición del mando aplicada");
            }
            Spacer(5);
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(R(8), R(2)));
            ImGui::BeginTable("##padrows", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                                ImGuiTableFlags_NoSavedSettings);
            ImGui::TableSetupColumn("##a", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("##b", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            if (g_config.face_buttons == FACE_POSITION) {
                MappingRow("A (Confirmar)", "Este · A de Switch");
                MappingRow("B (Cancelar)", "Sur · B de Switch");
                MappingRow("X (Objeto)", "Norte · X de Switch");
                MappingRow("Y (Arma)", "Oeste · Y de Switch");
            } else {
                MappingRow("A (Confirmar)", "Mando · carácter A");
                MappingRow("B (Cancelar)", "Mando · carácter B");
                MappingRow("X (Objeto)", "Mando · carácter X");
                MappingRow("Y (Arma)", "Mando · carácter Y");
            }
            MappingRow("L (Cámara)", "LB / L");
            MappingRow("R (Correr)", "RB / R");
            MappingRow("ZL", "LT / ZL");
            MappingRow("ZR", "RT / ZR");
            ImGui::EndTable();
            ImGui::PopStyleVar();
            Spacer(5);
            SmallTxt("STICK IZQ · movimiento   ·   STICK DER · cámara   ·   ESC · abre y cierra KZMUI");
            Spacer(4);
            float sens = g_config.mouse_sensitivity;
            PushText(10.5f);
            Txt(T().dim, "SENSIBILIDAD DEL RATÓN");
            PopText();
            if (ImGui::SliderFloat("##sens", &sens, 0.1f, 10.0f, "%.2f x")) g_config.mouse_sensitivity = sens;
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                char b[32];
                snprintf(b, sizeof b, "%.2f", g_config.mouse_sensitivity);
                config_set("Mouse", "MouseSensitivity", b);
            }
            Spacer(4);
            if (KButton("Restaurar mando a valores originales")) {
                g_config.face_buttons = FACE_POSITION;
                g_config.mouse_sensitivity = 1.0f;
                config_set("Controls", "FaceButtons", "Position");
                config_set("Mouse", "MouseSensitivity", "1.0");
                SetStatus("Mando restaurado a los valores originales");
            }
        }
        EndCard();
    } else {
        if (BeginCard("##keymap", 0, "TECLADO Y RATÓN", "Clic en una fila y pulsa la tecla nueva · se guarda al instante")) {
            static const char *const names[16] = {"A (confirmar)", "B (cancelar)", "X (objeto)", "Y (arma)",
                                                  "Stick izq. (clic)", "Stick der. (clic)", "L (cámara)",
                                                  "R (correr / bumper)", "ZL", "ZR", "Plus", "Minus",
                                                  "D-Pad izquierda", "D-Pad arriba", "D-Pad derecha", "D-Pad abajo"};
            int active = host_bind_active();
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(R(8), R(2)));
            ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                               ImGuiTableFlags_NoSavedSettings);
            ImGui::TableSetupColumn("##a", ImGuiTableColumnFlags_WidthStretch, 1.15f);
            ImGui::TableSetupColumn("##b", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            for (int i = 0; i < 16; i++) BindKeyRow(i, names[i], g_config.keybinds[i], active == i);
            ImGui::EndTable();
            ImGui::PopStyleVar();
            Spacer(5);
            BindKeyRow(16, "Caminar (medio stick)", g_config.walk_key, active == 16, false);
            Spacer(5);
            SmallTxt("Ratón: clic derecho = A, izquierdo = X, movimiento = cámara; WASD mueve.");
            SmallTxt("Por defecto: R = Shift, ZL = Tab; la función que estaba en Shift (caminar) pasó a Alt.");
            Spacer(4);
            if (KButton("Restaurar teclas por defecto")) {
                config_keybinds_default(g_config.keybinds);
                g_config.walk_key = SDL_SCANCODE_LALT;
                host_keymap_save();
                SetStatus("Teclas restauradas: R = Shift, ZL = Tab, caminar = Alt");
            }
        }
        EndCard();
    }
}

static void ThemeCard(int i, float width) {
    ImGui::PushID(i);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = 64 * g_s;
    ImGui::InvisibleButton("##theme", ImVec2(width, h));
    bool hov = ImGui::IsItemHovered(), active = g_config.ui_theme == i;        if (ImGui::IsItemClicked() && !active) {
            g_config.ui_theme = i;
            config_set("Interface", "Theme", kThemeKeys[i]);
            SetStatus("Tema aplicado en caliente");
        }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), active ? Mix(T().card2, kThemes[i].accent, 0.18f)
                                                               : (hov ? T().card2 : A(T().card2, 145)),
                      R(g_config.ui_sharp ? 2 : 7));
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), active ? A(kThemes[i].accent, 210) : T().border,
                R(g_config.ui_sharp ? 2 : 7), 0, active ? 1.6f : 1.0f);
    for (int k = 0; k < 3; k++)
        dl->AddCircleFilled(ImVec2(p.x + (17 + k * 13) * g_s, p.y + 18 * g_s), 3.5f * g_s, kThemes[i].swatch[k]);
    if (active) {
        dl->AddCircleFilled(ImVec2(p.x + width - 16 * g_s, p.y + 16 * g_s), 6 * g_s, kThemes[i].accent);
        dl->AddCircleFilled(ImVec2(p.x + width - 16 * g_s, p.y + 16 * g_s), 2.5f * g_s, IM_COL32(20, 12, 12, 255));
    }
    dl->AddText(g_bold, 9.5f * g_s, ImVec2(p.x + 10 * g_s, p.y + 27 * g_s), T().text, kThemes[i].name);
    dl->AddText(g_body, 7.5f * g_s, ImVec2(p.x + 10 * g_s, p.y + 43 * g_s), T().dim, kThemes[i].desc);
    ImGui::PopID();
}

static void PageAppearance(void) {
    if (BeginCard("##themes", 0, "ATMÓSFERAS DE ALLGENU", "12 paletas de GU; Halloween se conserva intacto")) {
        float gap = 6 * g_s;
        float w = ImGui::GetContentRegionAvail().x;
        float cw = (w - gap * 5) / 6;
        for (int i = 0; i < 12; i++) {
            if (i % 6) ImGui::SameLine(0, gap);
            ThemeCard(i, cw);
            if (i % 6 == 5 && i < 11) Spacer(2);
        }
    }
    EndCard();
    Spacer(0);
    if (BeginCard("##geometry", 112, "GEOMETRÍA DE PANELES Y TARJETAS", "Curvatura suave o corte recto industrial")) {
        static const char *const geo[2] = {"Redondeado suave", "Corte recto"};
        int g = g_config.ui_sharp ? 1 : 0;
        if (Segmented("##geo", geo, 2, &g)) {
            g_config.ui_sharp = g;
            config_set("Interface", "Geometry", g ? "Sharp" : "Rounded");
            SetStatus(g ? "Geometría de corte recto" : "Geometría redondeada");
        }
    }
    EndCard();
    Spacer(0);
    if (BeginCard("##font", 142, "FAMILIA TIPOGRÁFICA DEL ESTUDIO", "Cambia sin reiniciar")) {
        static const char *const fonts[5] = {"Segoe UI", "Consolas", "Bahnschrift", "Arial", "Verdana"};
        int f = g_config.ui_font;
        if (Segmented("##fontsel", fonts, 5, &f)) {
            g_config.ui_font = f;
            use_fonts(f);
            config_set("Interface", "Font", fonts[f]);
            SetStatus("Tipografía de KZMUI cambiada");
        }
        Spacer(5);
        SmallTxt("Tipografía limpia para menús y telemetría. Sin cambios en el texto interno del juego.");
    }
    EndCard();
}

static void PageSaves(void) {
    if (ActionRow("##backup", "Crear copia de seguridad (Backup)",
                  "Copia fechada en save_backups/; no modifica la partida activa."))
        MakeBackup("backup");
    Spacer(5);
    if (ActionRow("##import", "Importar partida externa",
                  "Elige una carpeta con save_0; se crea una copia de seguridad antes de importar."))
        ImportSave();
    Spacer(5);
    if (ActionRow("##export", "Exportar partida actual",
                  "Copia save_0 a la carpeta que elijas, dentro de AllGenU_save_<fecha>."))
        ExportSave();
    if (!g_save_status.empty()) {
        Spacer(5);
        SmallTxt(g_save_status.c_str());
    }
    Spacer(6);
    if (g_backups_dirty) RefreshBackups();
    char head[128];
    snprintf(head, sizeof head, "%d copia(s) en %s", (int)g_backups.size(), BackupRoot().u8string().c_str());
    if (BeginCard("##backuplist", 244, "BÓVEDA DE GUARDADOS", head)) {
        if (KButton("Abrir carpeta de copias")) {
            std::error_code ec;
            fs::create_directories(BackupRoot(), ec);
            OpenPath(BackupRoot().u8string());
        }
        ImGui::SameLine();
        if (KButton("Actualizar lista")) g_backups_dirty = true;
        Spacer(6);
        ImGui::BeginChild("##backscroll", ImVec2(0, 146 * g_s), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        if (g_backups.empty()) {
            SmallTxt("Todavía no hay copias. Crea una con «Crear copia de seguridad».");
        } else {
            for (size_t i = 0; i < g_backups.size(); i++) {
                ImGui::PushID((int)i);
                ImVec2 p = ImGui::GetCursorScreenPos();
                float w = ImGui::GetContentRegionAvail().x, h = 46 * g_s;
                ImGui::InvisibleButton("##b", ImVec2(w, h));
                bool hov = ImGui::IsItemHovered();
                ImDrawList *dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov ? Mix(T().card2, T().accent, 0.08f) : A(T().card2, 100),
                                  R(6));
                dl->AddRect(p, ImVec2(p.x + w, p.y + h), T().border, R(6));
                DrawIcon(dl, 7, ImVec2(p.x + 17 * g_s, p.y + h * 0.5f), 7 * g_s, T().accent);
                dl->AddText(g_bold, 10.5f * g_s, ImVec2(p.x + 32 * g_s, p.y + 7 * g_s), T().text,
                            g_backups[i].name.c_str());
                char meta[128];
                snprintf(meta, sizeof meta, "%s · %.1f MB", g_backups[i].date.c_str(),
                         g_backups[i].bytes / (1024.0 * 1024.0));
                dl->AddText(g_body, 8.8f * g_s, ImVec2(p.x + 32 * g_s, p.y + 25 * g_s), T().dim, meta);
                ImGui::SetCursorScreenPos(ImVec2(p.x + w - 150 * g_s, p.y + 9 * g_s));
                if (KButton("Restaurar")) RestoreBackup(g_backups[i].path);
                ImGui::SameLine();
                if (KButton("Borrar")) {
                    std::error_code ec;
                    fs::remove_all(g_backups[i].path, ec);
                    g_backups_dirty = true;
                }
                ImGui::PopID();
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 4 * g_s));
            }
        }
        ImGui::EndChild();
    }
    EndCard();
}

// ------------------------------------------------------------------ Acerca de
// [AllGenU] (Acerca de) the credits page, last entry of the sidebar. The links are the project's single official
// channels; a click opens them in the desktop browser (OpenUrl). Appended as its own page: no existing page, button
// or setting is touched by it.
static const char *const kAboutGithub = "https://github.com/krisztsa/AllGenURecomp";
static const char *const kAboutFork = "https://github.com/krisztsa/AllGenURecomp";
static const char *const kAboutYoutube = "https://youtube.com/@kriszts?si=la18fuis0frX-LGA";
static const char *const kAboutTiktok = "https://vm.tiktok.com/ZS9yNSKMqe2h9-2gYhm/";
static const char *const kAboutTelegram = "https://t.me/MHNativeProject";

// One link: what it is on the left, the address under it, and the button that opens it. Laid out by hand (as the
// backup rows of PageSaves) so the long URLs never collide with the button at any panel width.
static void LinkRow(const char *label, const char *url, const char *button) {
    ImGui::PushID(url);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 48 * g_s;
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(ImVec2(p.x + 10 * g_s, p.y + 6 * g_s));
    PushText(11.5f, true);
    Txt(T().text, label);
    PopText();
    float fs = 9.0f * g_s;
    ImVec2 up(p.x + 10 * g_s, p.y + 26 * g_s);
    dl->AddText(g_body, fs, up, T().accent2, url);
    ImVec2 us = g_body->CalcTextSizeA(fs, FLT_MAX, 0.0f, url);
    dl->AddLine(ImVec2(up.x, up.y + us.y + 1.0f), ImVec2(up.x + us.x, up.y + us.y + 1.0f), A(T().accent2, 80), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - 92 * g_s, p.y + 12 * g_s));
    if (KButton(button)) OpenUrl(url);
    // The row reserved its space with the Dummy above, so putting the cursor back is enough; the trailing Dummy(0,0)
    // clears ImGui's "cursor was moved by hand" flag. Without it, a row that happens to be the last thing in the card
    // aborts the menu with ImGui's "SetCursorPos used to extend boundaries" assert.
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    dl->AddLine(ImVec2(p.x + 8 * g_s, p.y + h - 1.0f), ImVec2(p.x + w - 8 * g_s, p.y + h - 1.0f), A(T().border, 110), 1.0f);
    ImGui::PopID();
}

static void PageAbout(void) {
    if (BeginCard("##aboutBrand", 0, "ALLGENU RECOMP", "Recompilación estática de Monster Hunter Generations Ultimate (NS)")) {
        PushText(22.0f, true);
        Txt(T().accent, "Creado por Kris´ZtS");
        PopText();
        Spacer(2);
        SmallTxt("Creado en 2026.");
    }
    EndCard();
    Spacer(6);
    if (BeginCard("##aboutLinks", 0, "GITHUB Y COMUNIDAD", "Los canales oficiales del proyecto y del equipo")) {
        LinkRow("GitHub del proyecto", kAboutGithub, "GitHub");
        LinkRow("Fork original · creador de las herramientas", kAboutFork, "Fork");
        LinkRow("Mi canal de YT", kAboutYoutube, "YouTube");
        LinkRow("TikTok de mi contribuidor Alex Hunter", kAboutTiktok, "TikTok");
        LinkRow("Canal de Telegram del equipo", kAboutTelegram, "Telegram");
    }
    EndCard();
    Spacer(6);
    if (BeginCard("##aboutNotice", 0, "AVISO IMPORTANTE", "Léelo antes de compartir el port")) {
        PushText(12.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, V4(T().text));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("AllGenURecomp es una recompilación estática de Monster Hunter Generations Ultimate "
                               "(NS), es completamente gratuita y si obtuviste este port de pago has sido estafado, "
                               "la única vía oficial es nuestro GitHub.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        PopText();
    }
    EndCard();
}

static void DrawPage(void *cpu) {
    switch (g_page) {
    case 0: PageGraphics(); break;
    case 1: PageMods(); break;
    case 2: PageSystem(cpu); break;
    case 3: PageControls(); break;
    case 4: PageAppearance(); break;
    case 5: PageSaves(); break;
    // [AllGenU] (Acerca de) the credits page
    case 6: PageAbout(); break;
    default: PageGraphics(); break;
    }
}

// ------------------------------------------------------------------ the menu
void kzmui_draw(bool *keep_open, void *cpu, float scale) {
    g_s = std::clamp(scale, 0.75f, 2.5f);
    ImGuiIO &io = ImGui::GetIO();
    if (overlay_fps() > 0.5f) {  // [AllGenU] telemetry history: the presented rate
        g_fpsHist[g_fpsIdx] = overlay_fps();
        g_fpsIdx = (g_fpsIdx + 1) % 40;
        if (g_fpsCount < 40) g_fpsCount++;
    }
    if (g_fontUsed != g_config.ui_font) use_fonts(g_config.ui_font);
    apply_style();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##KZMUI", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    DrawBackdrop(dl, io.DisplaySize);

    // A floating panel, as the reference: smaller than the screen, centred, with the game visible around it.
    float panelW = std::min(900.0f * g_s, io.DisplaySize.x - 24 * g_s);
    float panelH = std::min(604.0f * g_s, io.DisplaySize.y - 20 * g_s);
    ImVec2 pp((io.DisplaySize.x - panelW) * 0.5f, (io.DisplaySize.y - panelH) * 0.5f);
    ImVec2 pe(pp.x + panelW, pp.y + panelH);
    PanelChrome(dl, pp, pe);

    float margin = 16 * g_s, gap = 8 * g_s;
    float sideW = 212 * g_s;
    ImVec2 sideP(pp.x + margin, pp.y + margin), sideS(sideW, panelH - 2 * margin);
    DrawSidebar(dl, sideP, sideS, keep_open);

    float cx = sideP.x + sideW + gap;
    float cw = pe.x - margin - cx;
    ImVec2 hp(cx, pp.y + margin), hs(cw, 74 * g_s);
    DrawHeader(dl, hp, hs);

    float content_top = hp.y + hs.y + gap, content_bottom = pe.y - margin;
    ImGui::SetCursorScreenPos(ImVec2(cx, content_top));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##kzcontent", ImVec2(cw, content_bottom - content_top), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    static int shown_page = -1;
    if (shown_page != g_page) {
        shown_page = g_page;
        ImGui::SetScrollY(0);  // (each page starts at its top)
    }
    float page_top = ImGui::GetCursorPosY();
    ImGui::PushClipRect(ImVec2(cx, content_top), ImVec2(cx + cw, content_bottom), true);
    DrawPage(cpu);
    ImGui::PopClipRect();
    if (getenv("MHGU_KZMUI_DEBUG")) {
        static int last_page = -1;
        if (last_page != g_page) {
            last_page = g_page;
            fprintf(stderr, "[kzmui] page %d layout: page %.1f px, view %.1f px, scroll %.1f px, scale %.3f\n", g_page,
                    ImGui::GetCursorPosY() - page_top, ImGui::GetWindowHeight(), ImGui::GetScrollMaxY(), g_s);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // Feedback that the setting reached the game (a small glass pill above the panel's lower edge).
    if (g_status[0] && SDL_GetTicks() < g_status_until) {
        const char *s = g_status;
        ImVec2 ts = g_body->CalcTextSizeA(10.5f * g_s, FLT_MAX, 0.0f, s);
        float w = ts.x + 34 * g_s, h = 26 * g_s;
        ImVec2 a((pp.x + pe.x - w) * 0.5f, pe.y - h - 12 * g_s), b(a.x + w, a.y + h);
        dl->AddRectFilled(ImVec2(a.x + 1, a.y + 3), ImVec2(b.x + 1, b.y + 3), A(IM_COL32(0, 0, 0, 255), 120), h * 0.5f);
        dl->AddRectFilled(a, b, A(T().panel, 245), h * 0.5f);
        dl->AddRect(a, b, A(T().accent, 190), h * 0.5f);
        dl->AddCircleFilled(ImVec2(a.x + 13 * g_s, a.y + h * 0.5f), 3.5f * g_s, T().accent2);
        dl->AddText(g_body, 10.5f * g_s, ImVec2(a.x + 22 * g_s, a.y + (h - ts.y) * 0.5f), T().text, s);
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}
