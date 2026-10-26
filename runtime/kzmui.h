// KZMUI: the AllGenU interface drawn over the game by runtime/overlay.cpp. C++.
#pragma once

// After the ImGui context exists, before the first frame (fonts and the base style).
void kzmui_init(void);
// A frame on the game's thread, inside ImGui::NewFrame()/Render(). scale: window height / 720.
// keep_open is cleared by "VOLVER AL JUEGO" (the overlay then closes the menu).
void kzmui_draw(bool *keep_open, void *cpu, float scale);
