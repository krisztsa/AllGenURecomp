// Event camera pans skippable with A. Entering a village or hub area when something is new (first visit, a new
// facility or NPC) plays an event: a scheduler (rScheduler, arc/ev/*.arc: event\village\event_vNNN ...) that moves
// the camera past the NPCs, with no way to skip it in the original game. The scheduler unit (uScheduler) advances
// its frame each update by speed x frame time; pressing A raises that speed to 30, so the rest of the event plays out
// in a moment and everything it does at its end (the NPC talking to you, ...) still happens in order.
// Hooked at uScheduler's update (tools/gen.py PATCHES, 0xb66164); only event schedulers are touched (the resource
// path has "event\"), not the looping stage ones (birds, smoke...), and not one the game has paused.
#include <string.h>

#include "../hle.h"
#include "input.h"

// uScheduler (main v1.4.0): +0x34 frame (float), +0x3c speed, +0x40 paused (u8), +0x41 loop (u8), +0x54 rScheduler
// (a cResource: path at +0x08, e.g. "event\village\event_v002\event_v002")
#define SC_SPEED  0x3cu
#define SC_PAUSED 0x40u
#define SC_LOOP   0x41u
#define SC_RES    0x54u
#define RES_PATH  0x08u
#define SKIP_SPEED 30.0f
#define PRESS_NS  150000000ull  // an A press counts this long (every scheduler updates once a frame)

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

static bool is_event(uint32_t res) {
    char path[64];
    memcpy(path, G2H(res + RES_PATH), sizeof path);
    path[sizeof path - 1] = 0;
    return strncmp(path, "event\\", 6) == 0;
}

// The patched entry of uScheduler's update (r0 = the scheduler).
void evskip_update(Cpu *c) {
    uint32_t s = c->r[0];
    uint64_t pressed = g_a_pressed_ns;
    if (!pressed || plat_now_ns() - pressed > PRESS_NS || !sane(s)) return;
    if (LD8(s + SC_LOOP) || LD8(s + SC_PAUSED)) return;
    uint32_t res = LD32(s + SC_RES);
    if (!sane(res) || !is_event(res)) return;
    float speed;
    uint32_t w = LD32(s + SC_SPEED);
    memcpy(&speed, &w, 4);
    if (speed <= 0.0f || speed >= SKIP_SPEED) return;
    speed = SKIP_SPEED;
    memcpy(&w, &speed, 4);
    ST32(s + SC_SPEED, w);
    LOG("[evskip] skipping %s\n", (const char *)G2H(res + RES_PATH));
}
