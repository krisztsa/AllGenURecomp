// (debug) MHGU_LOG_MOVES=1: logs the player's actions and command table changes, for mapping a weapon's moves to its
// tables.
// A move picks the table of follow-up commands (input -> next action, frame window) it allows; the tables are the
// per-style rPlCmdTblList in arc\player\quest\w<weapon>_m<style>.arc (and com_m<style>.arc for the common moves).
// The game sets one through two functions (player, id, start frame[, mode]): id 1000 + n is common table n, 2000 + n
// the weapon's table n, 0 and 9999 none. The player keeps the id at +0x24ac and the table at +0x24a8.
// Actions (act_st, act_no, act_param, as in the commands' rPlBaseCmd: act_st 0x0001 common, 0x0010 weapon; bit 15
// set on some) start through the player's vtable +0x2f8 (0x4ab670, "do"; it first lets the weapon class remap them,
// vtable +0x718), which the command handling calls for a matched command, or 0x4831e0 ("set", e.g. back to idle);
// both logged.
#include <stdlib.h>

#include "../hle.h"

#define FN_SET_CMDTBL   0x4e9b7cu  // (player, id, start frame)
#define FN_SET_CMDTBL2  0x4e9c50u  // (player, id, start frame, mode)
#define FN_SET_ACTION   0x4831e0u  // (player, act_st, act_no, act_param)
#define FN_DO_ACTION    0x4ab670u  // (player, act_st, act_no, act_param)

static void log_table(uint32_t pl, uint32_t id, uint32_t frame) {
    static uint32_t last_pl, last_id = ~0u;
    if (pl == last_pl && id == last_id) return;
    last_pl = pl, last_id = id;
    if (id >= 2000 && id != 9999) LOG("[moves] %llu pl %08x weapon table %u (frame %u)\n", (unsigned long long)g_present_count, pl, id - 2000, frame);
    else if (id >= 1000 && id != 9999) LOG("[moves] %llu pl %08x common table %u (frame %u)\n", (unsigned long long)g_present_count, pl, id - 1000, frame);
    else LOG("[moves] %llu pl %08x no table (%u)\n", (unsigned long long)g_present_count, pl, id);
}

static void set_cmdtbl(Cpu *c) {
    log_table(c->r[0], c->r[1], c->r[2]);
    fnhook_call_original(c, FN_SET_CMDTBL);
}

static void log_action(Cpu *c, const char *how) {
    LOG("[moves] %llu pl %08x %s %04x:%03u param %u\n", (unsigned long long)g_present_count, c->r[0], how,
        c->r[1] & 0xffff, c->r[2] & 0xffff, c->r[3] & 0xffff);
}

static void set_action(Cpu *c) {
    log_action(c, "set");
    fnhook_call_original(c, FN_SET_ACTION);
}

static void do_action(Cpu *c) {
    log_action(c, "do");
    fnhook_call_original(c, FN_DO_ACTION);
}

static void set_cmdtbl2(Cpu *c) {
    log_table(c->r[0], c->r[1], c->r[2]);
    fnhook_call_original(c, FN_SET_CMDTBL2);
}

void movelog_init(void) {
    const char *e = getenv("MHGU_LOG_MOVES");
    if (!e || !*e || *e == '0') return;
    fnhook_set(FN_SET_CMDTBL, hle_register("movelog:set_cmdtbl", set_cmdtbl));
    fnhook_set(FN_SET_CMDTBL2, hle_register("movelog:set_cmdtbl2", set_cmdtbl2));
    fnhook_set(FN_SET_ACTION, hle_register("movelog:set_action", set_action));
    fnhook_set(FN_DO_ACTION, hle_register("movelog:do_action", do_action));
    LOG("[moves] logging command table changes\n");
}
