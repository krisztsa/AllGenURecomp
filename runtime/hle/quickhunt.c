// (debug) MHGU_QUICKHUNT=<quest number>: from start-up straight into that quest, for testing in a hunt (e.g. 101,
// Find the Ferns: Village 1 star, Jurassic Frontier, no large monster). Presses A through the title, the main menu
// (Continue), the character select (the first character) and the welcome message, then orders the quest and departs
// at the first update of the town the save is in. The save is not changed (no quest fee is paid).
//
// How the game departs: the counter's accept (0x984e94) writes the quest number to sQuestSelector +0xd68 (0 = none)
// and sets bit 0 of its flags (+0xd65); "Depart on Quest" at the departure point sets bit 3 there. The selector's
// update (vtable +0x1c, 0x5b7b94) then turns bit 3 into a request (+0x1e0 = 4) that lasts one update. In a town
// (aVillage*, aLobbyBase) the area's update switches on a mode (+0xb8); mode 1 (0x8bd0fc, the town as you walk
// around) runs the selector's update and, seeing the request and an ordered quest, calls the area's depart method
// (0x8c02a4): stage from the ordered quest (sGameControl +0x48 / +0x4a), the area's quest flag (+0xda), mode 4,
// whose states fade out and jump to the stage's area (aMap<nn>), where aQuest loads the quest from +0xd68.
#include <stdlib.h>

#include "../hle.h"

#define FN_TOWN_IDLE     0x8bd0fcu   // town area update, mode 1 (r0: the area)
#define G_SELECTOR       0x1a8ae38u  // sQuestSelector *
#define SEL_FLAGS        0xd65u      // u8: bit 0 ordered at a counter, bit 3 depart
#define SEL_ORDERED      0xd68u      // the ordered quest's number
#define FLAG_ORDERED     0x01u
#define FLAG_DEPART      0x08u

static uint32_t g_quest;          // the quest to start (0: off)
static volatile bool g_in_town;   // set by the game thread at the first town update
static bool g_done;

static void town_idle(Cpu *c) {
    if (g_quest && !g_done) {
        g_done = true;
        g_in_town = true;
        uint32_t sel = LD32(G_SELECTOR);
        if (sel) {
            ST32(sel + SEL_ORDERED, g_quest);
            ST8(sel + SEL_FLAGS, LD8(sel + SEL_FLAGS) | FLAG_ORDERED | FLAG_DEPART);
            LOG("[quickhunt] in town: ordered quest %u, departing\n", g_quest);
        } else {
            LOG("[quickhunt] in town, but there is no sQuestSelector\n");
        }
    }
    fnhook_call_original(c, FN_TOWN_IDLE);
}

// UI thread: A every 1.5 s (90 presented frames at 60 fps; more frames at higher frame rates, still fine) until the
// game reaches a town.
void quickhunt_poll(void) {
    static uint64_t next = 120;
    if (!g_quest || g_in_town || g_present_count < next) return;
    next = g_present_count + 90;
    host_input_press("A", 8);
}

void quickhunt_init(void) {
    const char *e = getenv("MHGU_QUICKHUNT");
    if (!e || !*e || !(g_quest = (uint32_t)strtoul(e, NULL, 10))) return;
    fnhook_set(FN_TOWN_IDLE, hle_register("quickhunt:town_idle", town_idle));
    LOG("[quickhunt] quest %u: pressing A to the town, then departing\n", g_quest);
}
