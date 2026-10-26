// Forge plugins (Forge: an MHGU mod loader for code mods, github.com/Fexty12573/forge). A plugin is an AArch32 .nro in
// the RomFS folder nativeNX/plugins/ (so it comes with a mod's romfs, runtime/mods.c); on the console Forge's own
// module (exefs subsdk) loads it and gives it an API: function hooks, code patches, the game's singletons, save data,
// logging, input, an ImGui overlay. The port does not need Forge's module (its hooks rewrite the console's code): it
// loads the plugins itself, runs their code with the interpreter (runtime/interp.c, the plugins call into the
// recompiled game and back) and implements Forge's API on top of the recompiled game, at the points Forge uses:
//   sApp::run          singletons resolved, plugins loaded: forge_onLoad, forge_onInit
//   the frame update   forge_onUpdate(dt) after it, then the ImGui overlay's frame (runtime/overlay.cpp:
//                      forge_onImGuiInit once, forge_onImGuiRender in Forge's window, forge_onImGuiFreeRender)
//   cSystem ctor/dtor  the singletons (forge_singleton_*)
// Hooks run through FN_ENTER (cpu.h, dispatch.c). Patches over data apply as written; a patch over code can only
// redirect a function entry (Forge's own hook form, `ldr pc, [pc, #-4]` + address, or a `b`): others are logged.
// The plugins' ImGui calls (cimgui's ig* / Im*): runtime/forge_imgui.c. Not provided: forge_socket_*.
// Plugins that leave out a required Forge version newer than this one (FORGE_MAJOR.FORGE_MINOR) are not started.
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL3/SDL.h>

#include "forge.h"
#include "hle.h"
#include "hle/input.h"
#include "interp.h"
#include "mods.h"
#include "overlay.h"

#define FORGE_MAJOR 1
#define FORGE_MINOR 5
#define FORGE_PATCH 1

// Main module offsets (from its text) Forge uses, MHGU 1.4.0
#define OFF_APP_RUN     0xB8692Cu  // sApp::run
#define OFF_UPDATE      0x3DA8FCu  // the frame update (this: dt at +0x68)
#define OFF_SYSTEM_CTOR 0x887810u  // cSystem::cSystem / ~cSystem: every singleton
#define OFF_SYSTEM_DTOR 0x88784Cu
#define OFF_MOUNT_SAVE  0x878DA0u  // sSavedata's mount
#define OFF_JOB_SAFE    0x212041Cu

#define MAX_PLUGINS 8
typedef struct {
    char name[256];
    Module *m;
    uint32_t on_load, on_init, on_update;
    uint32_t on_imgui[3];  // forge_onImGuiInit, forge_onImGuiRender, forge_onImGuiFreeRender (FORGE_IMGUI_*)
    uint32_t param;  // PluginInitParam: u16 major, minor, patch; char name[64]
    bool started;
} Plugin;
static Plugin plugins[MAX_PLUGINS];
static int nplugins;
static uint32_t main_text;
static int log_level = 1;  // Forge LogLevel: 0 debug, 1 info, 2 warn, 3 error (MHGU_FORGE_LOG)

static uint32_t text_addr(uint32_t off) { return main_text + off; }

// a guest call from host code with up to four word arguments (r0-r3 are scratch here: callers are hooks or HLE
// functions, at a call boundary of the guest)
static uint32_t gcall(Cpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    c->r[0] = a0, c->r[1] = a1, c->r[2] = a2, c->r[3] = a3;
    guest_call(c, fn);
    return c->r[0];
}

static uint32_t gstring(const char *s) {  // a constant string in guest memory
    size_t n = strlen(s) + 1;
    uint32_t a = hle_alloc((uint32_t)n);
    memcpy(G2H(a), s, n);
    return a;
}

// ------------------------------------------------------------------ singletons
#define MAX_SINGLETONS 1024
static uint32_t pending[MAX_SINGLETONS], npending;
static struct { uint32_t id, obj; } singletons[MAX_SINGLETONS];
static uint32_t nsingletons;
static pthread_mutex_t singleton_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t mt_id(const char *name) {  // MtDti::makeId: CRC-32 without the final inversion, 31 bits
    uint32_t crc = 0xFFFFFFFFu;
    for (; *name; name++) crc = rt_crc32_tab[(crc ^ (uint8_t)*name) & 0xFF] ^ (crc >> 8);
    return crc & 0x7FFFFFFFu;
}

static void system_ctor(Cpu *c) {
    pthread_mutex_lock(&singleton_lock);
    if (npending < MAX_SINGLETONS) pending[npending++] = c->r[0];
    pthread_mutex_unlock(&singleton_lock);
    fnhook_call_original(c, text_addr(OFF_SYSTEM_CTOR));
}

static void system_dtor(Cpu *c) {
    uint32_t obj = c->r[0];
    pthread_mutex_lock(&singleton_lock);
    for (uint32_t i = 0; i < npending; i++)
        if (pending[i] == obj) pending[i--] = pending[--npending];
    for (uint32_t i = 0; i < nsingletons; i++)
        if (singletons[i].obj == obj) singletons[i--] = singletons[--nsingletons];
    pthread_mutex_unlock(&singleton_lock);
    fnhook_call_original(c, text_addr(OFF_SYSTEM_DTOR));
}

static void singletons_resolve(Cpu *c) {  // (the type of an object is known once it is constructed)
    pthread_mutex_lock(&singleton_lock);
    uint32_t list[MAX_SINGLETONS], n = npending;
    memcpy(list, pending, n * 4);
    npending = 0;
    pthread_mutex_unlock(&singleton_lock);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t dti = gcall(c, LD32(LD32(list[i]) + 0x14), list[i], 0, 0, 0);  // MtObject::getDti
        if (!dti) continue;
        uint32_t id = LD32(dti + 0x1C);
        pthread_mutex_lock(&singleton_lock);
        uint32_t k = 0;
        while (k < nsingletons && singletons[k].id != id) k++;
        if (k < MAX_SINGLETONS) {
            singletons[k].id = id;
            singletons[k].obj = list[i];
            if (k == nsingletons) nsingletons++;
        }
        pthread_mutex_unlock(&singleton_lock);
        if (log_level == 0) LOG("[forge] singleton %s at %08x\n", LD32(dti + 4) ? gstr(LD32(dti + 4)) : "?", list[i]);
    }
}

static uint32_t singleton_by_id(uint32_t id) {
    pthread_mutex_lock(&singleton_lock);
    uint32_t r = 0;
    for (uint32_t i = 0; i < nsingletons; i++)
        if (singletons[i].id == id) r = singletons[i].obj;
    pthread_mutex_unlock(&singleton_lock);
    return r;
}

static uint32_t singleton(Cpu *c, uint32_t id) {  // (singletons made since sApp::run are resolved here)
    uint32_t r = singleton_by_id(id);
    if (!r && __atomic_load_n(&npending, __ATOMIC_RELAXED)) {
        singletons_resolve(c);
        r = singleton_by_id(id);
    }
    return r;
}
HLE_FUNC("forge_singleton_getInstanceByName", f_singleton_by_name) { RET(singleton(c, mt_id(gstr(ARG(0))))); }
HLE_FUNC("forge_singleton_getInstanceById", f_singleton_by_id) { RET(singleton(c, ARG(0))); }
HLE_FUNC("forge_singleton_getAllInstances", f_singleton_all) {
    uint32_t out = ARG(0), max = ARG(1), n = 0;
    pthread_mutex_lock(&singleton_lock);
    if (!out) n = nsingletons;
    else
        for (; n < nsingletons && n < max; n++) ST32(out + 4 * n, singletons[n].obj);
    pthread_mutex_unlock(&singleton_lock);
    RET(n);
}

// ------------------------------------------------------------------ hooks and patches
// A hook makes the target's first two words Forge's `ldr pc, [pc, #-4]; .word detour` (plugins check that) and
// installs the detour as a function hook; the original is a trampoline holding the two original words and a jump
// back, as Forge builds it (plugins check that too), which dispatch.c runs as the unhooked function.
static uint32_t tramp_next = FORGE_TRAMP, stub_next = FORGE_STUBS;
static pthread_mutex_t forge_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread uint32_t t_hook_context;

static bool in_code(uint32_t a) {
    for (int i = 0; i < 2 && i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        if (a >= m->base + m->text_off && a < m->base + m->text_off + m->text_size) return true;
    }
    return false;
}

// Code patched in guest memory: re-derive the hooks at the function entries in [a, a + n). orig: the bytes before
// (enabling a patch), to report a change that is not a hook.
static void code_changed(uint32_t a, uint32_t n, uint32_t orig) {
    bool hook = false;
    for (uint32_t x = a & ~3u; x < a + n; x += 4) {
        if (!in_code(x) || !dispatch_lookup(x)) continue;
        uint32_t w = LD32(x);
        if (orig && (w == 0xE51FF004u || (w & 0xFF000000u) == 0xEA000000u))  // (the words it replaced)
            fnhook_keep_entry(x, LD32(orig + (x - a)), x + 4 < a + n ? LD32(orig + (x + 4 - a)) : LD32(x + 4));
        if (w == 0xE51FF004u) hook = fnhook_set(x, LD32(x + 4));
        else if ((w & 0xFF000000u) == 0xEA000000u) hook = fnhook_set(x, x + 8 + (uint32_t)((int32_t)(w << 8) >> 6));
        else if (fnhook_get(x)) fnhook_set(x, 0);
    }
    if (!hook && orig && memcmp(G2H(a), G2H(orig), n))
        LOG("[forge] code patch at main+%x (%u bytes) cannot apply to recompiled code: ignored\n", a - main_text, n);
}

static uint32_t hook_install(uint32_t target, uint32_t detour, uint32_t original_out) {
    target &= ~1u;
    if (!in_code(target) || !dispatch_lookup(target)) {
        LOG("[forge] hook target %08x is not a function of the game: not hooked\n", target);
        return 0;
    }
    pthread_mutex_lock(&forge_lock);
    uint32_t tramp = 0, prev = fnhook_get(target);  // (a hook already there: the original runs it)
    if (original_out && tramp_next + 16 <= FORGE_STUBS) {
        tramp = tramp_next;
        tramp_next += 16;
        ST32(tramp, LD32(target));
        ST32(tramp + 4, LD32(target + 4));
        ST32(tramp + 8, 0xE51FF004u);  // ldr pc, [pc, #-4]
        ST32(tramp + 12, target + 8);
        fnhook_add_trampoline(tramp, prev ? prev : target, !prev);
    }
    fnhook_keep_entry(target, LD32(target), LD32(target + 4));  // (interpreted, the function runs its own words)
    ST32(target, 0xE51FF004u);
    ST32(target + 4, detour);
    pthread_mutex_unlock(&forge_lock);
    fnhook_set(target, detour);
    if (original_out) ST32(original_out, tramp);
    if (log_level == 0) LOG("[forge] hook main+%x -> %08x\n", target - main_text, detour);
    return tramp;
}

// (debug) A hook made the way a plugin makes one: MHGU_FORGE_TESTHOOK=<runtime hex address> hooks that function at
// start-up, before the cheats are applied; the control file's "testhook <address>" while the game runs, after them
// (as plugins hook, from forge_onLoad). The detour logs its first calls and runs the original through its trampoline.
static uint32_t test_target, test_tramp;
static int test_calls;

static void test_detour(Cpu *c) {
    if (test_calls++ < 5) LOG("[forge] test hook %08x: call %d\n", test_target, test_calls);
    guest_call(c, test_tramp);
}

void forge_test_hook(uint32_t target) {
    if (test_target || !target) return;
    test_target = target;
    kernel_map_region(FORGE_TRAMP, FORGE_STUBS - FORGE_TRAMP, 3, 5);  // (no plugins: not mapped yet)
    test_tramp = hook_install(target, hle_register("forge:test hook", test_detour), hle_alloc(4));
    LOG("[forge] test hook %08x (trampoline %08x)\n", target, test_tramp);
}

static void setcontext(Cpu *c) { t_hook_context = ARG(0); }

// Hook forge_hook_create(void *target, void *detour, void **original): returned in memory (r0)
HLE_FUNC("forge_hook_create", f_hook_create) {
    uint32_t out = ARG(0), target = ARG(1), detour = ARG(2), orig = ARG(3);
    if (orig) ST32(orig, 0);
    uint32_t tramp = hook_install(target, detour, orig);
    memset(G2H(out), 0, 24);
    ST32(out, target), ST32(out + 4, detour), ST32(out + 8, tramp);
}

// The context stub of forge_hook_createWithContext: Forge's own ARM code (interpreted), which stores the context
// for forge_hook_getContext and goes on to the detour.
HLE_FUNC("forge_hook_createWithContext", f_hook_create_ctx) {
    uint32_t out = ARG(0), target = ARG(1), detour = ARG(2), orig = ARG(3), context = STK(0);
    if (orig) ST32(orig, 0);
    memset(G2H(out), 0, 24);
    ST32(out, target), ST32(out + 4, detour), ST32(out + 12, context), ST8(out + 20, 1);
    pthread_mutex_lock(&forge_lock);
    uint32_t stub = stub_next;
    stub_next += 48;
    pthread_mutex_unlock(&forge_lock);
    if (stub + 48 > FORGE_END) { LOG("[forge] too many hooks with context\n"); return; }
    static const uint32_t code[7] = {0xE92D500F, 0xE59F0010, 0xE5900000, 0xE59FC00C, 0xE12FFF3C, 0xE8BD500F, 0xE59FF004};
    for (int i = 0; i < 7; i++) ST32(stub + 4 * i, code[i]);
    ST32(stub + 28, stub + 40);
    ST32(stub + 32, hle_register("forge_hook_setContextInternal", setcontext));
    ST32(stub + 36, detour);
    ST32(stub + 40, context);
    ST32(out + 16, stub + 40);
    ST32(out + 8, hook_install(target, stub, orig));
}

HLE_FUNC("forge_hook_updateContext", f_hook_update_ctx) {
    uint32_t h = ARG(0);
    if (!LD8(h + 20) || !LD32(h + 16)) { RET(MAKE_RESULT(345, 1)); return; }  // (libnx: bad input)
    __atomic_store_n((uint32_t *)G2H(LD32(h + 16)), ARG(1), __ATOMIC_RELEASE);
    ST32(h + 12, ARG(1));
    RET(0);
}

HLE_FUNC("forge_hook_getContext", f_hook_get_ctx) { RET(t_hook_context); }

// Patch: u32 address, size, void *patch_bytes, *original_bytes; bool enabled
static void patch_apply(uint32_t p, bool on) {
    uint32_t addr = LD32(p), size = LD32(p + 4), src = LD32(p + (on ? 8 : 12));
    if (!addr || LD8(p + 16) == on) return;
    memmove(G2H(addr), G2H(src), size);
    ST8(p + 16, on);
    if (in_code(addr) || in_code(addr + size - 1)) code_changed(addr, size, on ? LD32(p + 12) : 0);
}

HLE_FUNC("forge_patch_create", f_patch_create) {  // (Patch *out, u32 address, const void *bytes, u32 length, bool enable)
    uint32_t out = ARG(0), addr = ARG(1), bytes = ARG(2), len = ARG(3), enable = STK(0) & 0xFF;
    memset(G2H(out), 0, 20);
    if (!len) return;
    uint32_t pb = hle_alloc(len), ob = hle_alloc(len);
    memcpy(G2H(pb), G2H(bytes), len);
    memcpy(G2H(ob), G2H(addr), len);
    ST32(out, addr), ST32(out + 4, len), ST32(out + 8, pb), ST32(out + 12, ob);
    if (enable) patch_apply(out, true);
}
HLE_FUNC("forge_patch_enable", f_patch_enable) { patch_apply(ARG(0), true); }
HLE_FUNC("forge_patch_disable", f_patch_disable) { patch_apply(ARG(0), false); }
HLE_FUNC("forge_patch_destroy", f_patch_destroy) {
    uint32_t p = ARG(0);
    if (!LD32(p)) return;
    patch_apply(p, false);
    memset(G2H(p), 0, 20);
}

// ------------------------------------------------------------------ memory
HLE_FUNC("forge_mem_getMainTextAddr", f_text) { RET(g_modules[0].base + g_modules[0].text_off); }
HLE_FUNC("forge_mem_getMainRodataAddr", f_rodata) { RET(g_modules[0].base + g_modules[0].ro_off); }
HLE_FUNC("forge_mem_getMainDataAddr", f_data) { RET(g_modules[0].base + g_modules[0].data_off); }
HLE_FUNC("forge_mem_getMainBssAddr", f_bss) {
    Module *m = &g_modules[0];
    RET(m->base + m->data_off + m->data_size - m->bss_size);
}
HLE_FUNC("forge_mem_getMainHeapAddr", f_heap) { RET(HEAP_BASE); }

// ------------------------------------------------------------------ log
// printf from the guest: variadic arguments in r2.. (after level and format) and on the stack, 64-bit values in an
// even register pair or an 8-byte aligned stack slot (doubles too: variadic arguments do not use VFP registers)
typedef struct { Cpu *c; int reg; uint32_t stk; } GArgs;
static uint32_t ga32(GArgs *a) {
    if (a->reg < 4) return a->c->r[a->reg++];
    uint32_t v = LD32(a->stk);
    a->stk += 4;
    return v;
}
static uint64_t ga64(GArgs *a) {
    if (a->reg & 1) a->reg++;
    if (a->reg < 4) {
        uint64_t v = a->c->r[a->reg] | ((uint64_t)a->c->r[a->reg + 1] << 32);
        a->reg += 2;
        return v;
    }
    a->reg = 4;
    a->stk = (a->stk + 7) & ~7u;
    uint64_t v = LD64(a->stk);
    a->stk += 8;
    return v;
}

static void gformat(char *out, size_t n, const char *f, GArgs *a) {  // (forge_format)
    size_t o = 0;
    while (*f && o + 1 < n) {
        if (*f != '%') { out[o++] = *f++; continue; }
        char spec[32];
        size_t k = 0;
        spec[k++] = *f++;
        while (*f && strchr("-+ #0", *f) && k < 20) spec[k++] = *f++;
        if (*f == '*') { k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)ga32(a)); f++; }
        else while (isdigit((unsigned char)*f) && k < 24) spec[k++] = *f++;
        if (*f == '.') {
            spec[k++] = *f++;
            if (*f == '*') { k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)ga32(a)); f++; }
            else while (isdigit((unsigned char)*f) && k < 28) spec[k++] = *f++;
        }
        int ll = 0;
        while (*f && strchr("hlzjtL", *f)) { if (*f == 'l' || *f == 'j' || *f == 'L') ll++; f++; }
        char conv = *f ? *f++ : 0;
        char tmp[512];
        if (conv == '%') { out[o++] = '%'; continue; }
        if (strchr("diuxXoc", conv) && conv) {
            uint64_t v = ll >= 2 ? ga64(a) : ga32(a);
            spec[k++] = 'l', spec[k++] = 'l', spec[k++] = conv, spec[k] = 0;
            if (conv == 'd' || conv == 'i') snprintf(tmp, sizeof tmp, spec, ll >= 2 ? (long long)v : (long long)(int32_t)v);
            else if (conv == 'c') snprintf(tmp, sizeof tmp, "%c", (int)v);
            else snprintf(tmp, sizeof tmp, spec, (unsigned long long)v);
        } else if (conv == 's') {
            spec[k++] = 's', spec[k] = 0;
            uint32_t p = ga32(a);
            snprintf(tmp, sizeof tmp, spec, p ? gstr(p) : "(null)");
        } else if (conv == 'p') {
            snprintf(tmp, sizeof tmp, "0x%x", ga32(a));
        } else if (conv && strchr("fFeEgGaA", conv)) {
            uint64_t bits = ga64(a);
            double d;
            memcpy(&d, &bits, 8);
            spec[k++] = conv, spec[k] = 0;
            snprintf(tmp, sizeof tmp, spec, d);
        } else {
            break;
        }
        for (char *t = tmp; *t && o + 1 < n; t++) out[o++] = *t;
    }
    out[o] = 0;
}

void forge_format(char *out, size_t n, const char *fmt, Cpu *c, int reg, uint32_t stk) {
    GArgs a = {c, reg, stk};
    gformat(out, n, fmt, &a);
}

static void forge_print(int level, const char *fmt, GArgs *a) {
    if (level < log_level) return;
    static const char *names[] = {"debug", "info", "warn", "error"};
    char buf[1024];
    gformat(buf, sizeof buf, fmt, a);
    size_t n = strlen(buf);
    while (n && buf[n - 1] == '\n') buf[--n] = 0;
    LOG("[forge] [%s] %s\n", names[level & 3], buf);
}

HLE_FUNC("forge_log", f_log) {
    GArgs a = {c, 2, c->r[13]};
    forge_print((int)ARG(0), gstr(ARG(1)), &a);
}
HLE_FUNC("forge_log_trace", f_log_trace) {
    GArgs a = {c, 1, c->r[13]};
    forge_print(0, gstr(ARG(0)), &a);
}
HLE_FUNC("forge_log_getLevel", f_log_level) { RET(log_level); }

// ------------------------------------------------------------------ save data
static uint32_t str_savedata;

HLE_FUNC("forge_fs_unmountSaveData", f_unmount_save) {
    bool job_safe = LD8(text_addr(OFF_JOB_SAFE));
    uint32_t save = singleton_by_id(mt_id("sSavedata"));
    if (!save || !LD32(save + 0xF1C)) return;  // not mounted
    gcall(c, guest_sym("_ZN2nn2fs14CommitSaveDataEPKc"), str_savedata, 0, 0, 0);
    bool thread_safe = job_safe || LD8(save + 0x18);
    if (thread_safe) gcall(c, guest_sym("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE"), save + 4, 0, 0, 0);
    uint32_t n = LD32(save + 0xF1C) - 1;
    ST32(save + 0xF1C, n);
    if (!n) gcall(c, guest_sym("_ZN2nn2fs7UnmountEPKc"), str_savedata, 0, 0, 0);
    if (thread_safe) gcall(c, guest_sym("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE"), save + 4, 0, 0, 0);
}

HLE_FUNC("forge_fs_mountSaveData", f_mount_save) {
    uint32_t save = singleton(c, mt_id("sSavedata"));
    if (!save) { RET(0); return; }
    if (gcall(c, text_addr(OFF_MOUNT_SAVE), save, 0, 0, 0) != 0) {
        f_unmount_save(c);
        RET(0);
        return;
    }
    RET(1);
}

HLE_FUNC("forge_fs_getSaveDataMountPoint", f_save_mount_point) { RET(str_savedata); }
HLE_FUNC("forge_fs_getRomFsMountPoint", f_romfs_mount_point) {
    static uint32_t s;
    if (!s) s = gstring("app");
    RET(s);
}
HLE_FUNC("forge_fs_fileExists", f_file_exists) {
    uint32_t path = ARG(0), sp = c->r[13];
    c->r[13] = (sp - 16) & ~7u;
    uint32_t handle = c->r[13];
    uint32_t r = gcall(c, guest_sym("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci"), handle, path, 1, 0);
    if (!r) gcall(c, guest_sym("_ZN2nn2fs9CloseFileENS0_10FileHandleE"), LD32(handle), 0, 0, 0);
    c->r[13] = sp;
    RET(r == 0);
}

// ------------------------------------------------------------------ nn::os wrappers (same arguments as the SDK's)
#define FORWARD(name, sym, fn)                         \
    HLE_FUNC(name, fn) {                               \
        c->pc = guest_sym(sym);                        \
        rt_dispatch(c);                                \
    }
FORWARD("forge_nnosInitializeLightEvent", "_ZN2nn2os20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE", f_le_init)
FORWARD("forge_nnosFinalizeLightEvent", "_ZN2nn2os18FinalizeLightEventEPNS0_14LightEventTypeE", f_le_fini)
FORWARD("forge_nnosSignalLightEvent", "_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE", f_le_signal)
FORWARD("forge_nnosWaitLightEvent", "_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE", f_le_wait)
FORWARD("forge_nnosTryWaitLightEvent", "_ZN2nn2os17TryWaitLightEventEPNS0_14LightEventTypeE", f_le_trywait)
FORWARD("forge_nnosTimedWaitLightEvent", "_ZN2nn2os19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE", f_le_timedwait)
FORWARD("forge_nnosClearLightEvent", "_ZN2nn2os15ClearLightEventEPNS0_14LightEventTypeE", f_le_clear)
FORWARD("forge_nnosLockMutex", "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE", f_mutex_lock)
FORWARD("forge_nnosUnlockMutex", "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE", f_mutex_unlock)
HLE_FUNC("forge_nnosInitializeMutex", f_mutex_init) {  // (mutex, recursive): lock level 0
    c->r[2] = 0;
    c->pc = guest_sym("nnosInitializeMutex");
    rt_dispatch(c);
}

// ------------------------------------------------------------------ input (the controller the game sees, the keyboard)
static uint64_t in_now, in_prev;
static uint8_t keys_now[512], keys_prev[512];
static int32_t in_sticks[4];

static void input_frame(void) {
    in_prev = in_now;
    input_get(&in_now, in_sticks);
    memcpy(keys_prev, keys_now, sizeof keys_now);
    int n = 0;
    const bool *k = SDL_GetKeyboardState(&n);
    for (int i = 0; i < 512; i++) keys_now[i] = k && i < n ? k[i] : 0;
}

static bool key(const uint8_t *s, uint32_t k) { return k < 512 && s[k]; }
HLE_FUNC("forge_input_isDown", f_in_down) { RET(ARG(0) < 64 && (in_now >> ARG(0)) & 1); }
HLE_FUNC("forge_input_isPressed", f_in_pressed) { RET(ARG(0) < 64 && ((in_now & ~in_prev) >> ARG(0)) & 1); }
HLE_FUNC("forge_input_isReleased", f_in_released) { RET(ARG(0) < 64 && ((~in_now & in_prev) >> ARG(0)) & 1); }
HLE_FUNC("forge_input_isConnected", f_in_connected) { RET(1); }
HLE_FUNC("forge_input_getTouch", f_in_touch) { RET(0); }
static void stick(Cpu *c, int i) {
    if (ARG(0)) STF(ARG(0), in_sticks[i] / 32767.0f);
    if (ARG(1)) STF(ARG(1), in_sticks[i + 1] / 32767.0f);
}
HLE_FUNC("forge_input_getStickL", f_in_stick_l) { stick(c, 0); }
HLE_FUNC("forge_input_getStickR", f_in_stick_r) { stick(c, 2); }
HLE_FUNC("forge_input_isKeyDown", f_key_down) { RET(key(keys_now, ARG(0))); }
HLE_FUNC("forge_input_isKeyPressed", f_key_pressed) { RET(key(keys_now, ARG(0)) && !key(keys_prev, ARG(0))); }
HLE_FUNC("forge_input_isKeyReleased", f_key_released) { RET(!key(keys_now, ARG(0)) && key(keys_prev, ARG(0))); }
HLE_FUNC("forge_input_isShiftDown", f_shift) { RET(key(keys_now, SDL_SCANCODE_LSHIFT) || key(keys_now, SDL_SCANCODE_RSHIFT)); }
HLE_FUNC("forge_input_isCtrlDown", f_ctrl) { RET(key(keys_now, SDL_SCANCODE_LCTRL) || key(keys_now, SDL_SCANCODE_RCTRL)); }
HLE_FUNC("forge_input_isAltDown", f_alt) { RET(key(keys_now, SDL_SCANCODE_LALT) || key(keys_now, SDL_SCANCODE_RALT)); }
HLE_FUNC("forge_graphics_isInitialized", f_gfx_init) { RET(overlay_enabled()); }
HLE_FUNC("forge_graphics_isMenuOpen", f_gfx_menu) { RET(overlay_menu_open()); }
HLE_FUNC("forge_graphics_isMenuVisible", f_gfx_menu_visible) { RET(overlay_menu_open()); }

// ------------------------------------------------------------------ plugins
void forge_plugins_imgui(Cpu *c, int which, uint32_t a0, uint32_t a1, uint32_t a2) {
    for (int i = 0; i < nplugins; i++)
        if (plugins[i].started && plugins[i].on_imgui[which]) gcall(c, plugins[i].on_imgui[which], a0, a1, a2, 0);
}

bool forge_plugins_have(int which) {
    for (int i = 0; i < nplugins; i++)
        if (plugins[i].started && plugins[i].on_imgui[which]) return true;
    return false;
}

int forge_plugin_titles(const char **out, int max) {
    int n = 0;
    for (int i = 0; i < nplugins && n < max; i++)
        if (plugins[i].started) out[n++] = LD8(plugins[i].param + 6) ? gstr(plugins[i].param + 6) : plugins[i].name;
    return n;
}

static void app_run(Cpu *c) {
    uint32_t args[4];
    memcpy(args, c->r, sizeof args);
    singletons_resolve(c);
    for (int i = 0; i < nplugins; i++) {
        Plugin *p = &plugins[i];
        if (!p->on_load) {
            LOG("[forge] %s has no forge_onLoad: not started\n", p->name);
            continue;
        }
        Module *m = p->m;  // (its initializers, as nn::ro runs them)
        if (m->init) gcall(c, m->init, 0, 0, 0, 0);
        else
            for (uint32_t k = 0; k < m->init_array_sz / 4; k++)
                if (LD32(m->init_array + 4 * k)) gcall(c, LD32(m->init_array + 4 * k), 0, 0, 0, 0);
        p->param = hle_alloc(72);
        gcall(c, p->on_load, p->param, 0, 0, 0);
        uint32_t major = LD16(p->param), minor = LD16(p->param + 2), patch = LD16(p->param + 4);
        char title[65];
        memcpy(title, G2H(p->param + 6), 64);
        title[64] = 0;
        if (major > FORGE_MAJOR || (major == FORGE_MAJOR && minor > FORGE_MINOR)) {
            LOG("[forge] %s needs Forge %u.%u.%u (this is %d.%d.%d): not started\n", p->name, major, minor, patch,
                FORGE_MAJOR, FORGE_MINOR, FORGE_PATCH);
            continue;
        }
        LOG("[forge] starting %s%s%s%s\n", p->name, title[0] ? " (" : "", title, title[0] ? ")" : "");
        if (p->on_init) gcall(c, p->on_init, 0, 0, 0, 0);
        p->started = true;
    }
    memcpy(c->r, args, sizeof args);
    fnhook_call_original(c, text_addr(OFF_APP_RUN));
}

static void frame_update(Cpu *c) {
    uint32_t self = c->r[0];
    fnhook_call_original(c, text_addr(OFF_UPDATE));
    uint32_t ret[2] = {c->r[0], c->r[1]};
    float dt = LDF(self + 0x68), s0 = c->v.f[0];
    input_frame();
    for (int i = 0; i < nplugins; i++) {
        if (!plugins[i].started || !plugins[i].on_update) continue;
        c->v.f[0] = dt;  // (plugins are softfp, as Forge: the float in r0; s0 too, for any built hard-float)
        memcpy(&c->r[0], &dt, 4);
        guest_call(c, plugins[i].on_update);
    }
    overlay_frame(c);
    c->r[0] = ret[0], c->r[1] = ret[1], c->v.f[0] = s0;
}

// a path under dir, matched without regard to case (mods come from every kind of system); false if missing
static bool ci_path(const char *dir, const char *rel, char *out, size_t n) {
    snprintf(out, n, "%s", dir);
    char part[256];
    while (*rel) {
        size_t k = strcspn(rel, "/");
        snprintf(part, sizeof part, "%.*s", (int)k, rel);
        rel += k + (rel[k] == '/');
        DIR *d = opendir(out);
        if (!d) return false;
        struct dirent *e;
        bool found = false;
        while ((e = readdir(d)))
            if (!strcasecmp(e->d_name, part)) {
                size_t len = strlen(out);
                snprintf(out + len, n - len, "/%s", e->d_name);
                found = true;
                break;
            }
        closedir(d);
        if (!found) return false;
    }
    return true;
}

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = n > 0 ? malloc((size_t)n) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    *len = (size_t)n;
    return p;
}

// The ImGui overlay, with Forge's settings (its config.ini [menu] key / font_size) and font where a mod has them
// (Forge's own files: romfs nativeNX/forge/), else Forge's defaults (Insert, 12 px) and ImGui's font.
static void overlay_start(const char **layers, int nl) {
    int key = 73;  // (a HID usage, as SDL's scancodes: Insert)
    float size = 12.0f;
    char path[1400], font[1400] = "";
    for (int l = 0; l < nl; l++) {
        if (ci_path(layers[l], "nativeNX/forge/fonts/Roboto-Medium.ttf", path, sizeof path)) snprintf(font, sizeof font, "%s", path);
        if (!ci_path(layers[l], "nativeNX/forge/config.ini", path, sizeof path)) continue;
        FILE *f = fopen(path, "r");
        char line[256];
        bool menu = false;
        while (f && fgets(line, sizeof line, f)) {
            char *s = line;
            while (isspace((unsigned char)*s)) s++;
            if (*s == '[') { menu = !strncasecmp(s, "[menu]", 6); continue; }
            if (!menu) continue;
            if (!strncasecmp(s, "key", 3) && strchr(s, '=')) key = atoi(strchr(s, '=') + 1);
            if (!strncasecmp(s, "font_size", 9) && strchr(s, '=')) size = (float)atof(strchr(s, '=') + 1);
        }
        if (f) fclose(f);
    }
    overlay_init(font[0] ? font : NULL, size, key);
}

void forge_init(void) {
    const char *lv = getenv("MHGU_FORGE_LOG");  // debug, info, warn, error
    if (lv) log_level = !strcasecmp(lv, "debug") ? 0 : !strcasecmp(lv, "warn") ? 2 : !strcasecmp(lv, "error") ? 3 : 1;
    main_text = g_modules[0].base + g_modules[0].text_off;
    if (getenv("MHGU_FORGE_TESTHOOK")) forge_test_hook((uint32_t)strtoul(getenv("MHGU_FORGE_TESTHOOK"), NULL, 16));
    // the plugin files: the game's RomFS, then each mod's (a later one replaces one of the same name)
    char base[1024], dir[1100], found[MAX_PLUGINS][1400];
    const char *layers[65];
    snprintf(base, sizeof base, "%s/romfs", g_game_dir);
    layers[0] = base;
    int nl = 1 + mods_romfs_layers(layers + 1, 64), nf = 0;
    for (int l = 0; l < nl; l++) {
        if (!ci_path(layers[l], "nativeNX/plugins", dir, sizeof dir)) continue;
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t len = strlen(e->d_name);
            if (len < 5 || strcasecmp(e->d_name + len - 4, ".nro")) continue;
            int i = 0;
            while (i < nf && strcasecmp(strrchr(found[i], '/') + 1, e->d_name)) i++;
            if (i == MAX_PLUGINS) continue;
            snprintf(found[i], sizeof found[i], "%s/%s", dir, e->d_name);
            if (i == nf) nf++;
        }
        if (d) closedir(d);
    }
    // [AllGenU] no plugin files is no reason to stop: KZMUI (the port's own menu, Escape) is drawn by the same frame
    // hook and overlay as the plugins' UI, so the frame hook and the overlay go in either way (below).

    main_text = g_modules[0].base + g_modules[0].text_off;
    str_savedata = gstring("savedata");
    uint32_t at = PLUGIN_BASE;
    for (int i = 0; i < nf; i++) {
        size_t len;
        uint8_t *data = read_all(found[i], &len);
        const char *name = strrchr(found[i], '/') + 1;
        Module *m = data && at + len + 0x100000 <= PLUGIN_END ? loader_load_nro(name, data, len, at) : NULL;
        free(data);
        if (!m) {
            LOG("[forge] %s: not a plugin this port can load (%s)\n", name, found[i]);
            continue;
        }
        Plugin *p = &plugins[nplugins++];
        snprintf(p->name, sizeof p->name, "%s", name);
        p->m = m;
        p->on_load = loader_module_symbol(m, "forge_onLoad");
        p->on_init = loader_module_symbol(m, "forge_onInit");
        p->on_update = loader_module_symbol(m, "forge_onUpdate");
        p->on_imgui[FORGE_IMGUI_INIT] = loader_module_symbol(m, "forge_onImGuiInit");
        p->on_imgui[FORGE_IMGUI_RENDER] = loader_module_symbol(m, "forge_onImGuiRender");
        p->on_imgui[FORGE_IMGUI_FREE_RENDER] = loader_module_symbol(m, "forge_onImGuiFreeRender");
        interp_add_range(m->base + m->text_off, m->base + m->text_off + m->text_size);
        kernel_map_region(m->base + m->text_off, m->ro_off - m->text_off, 3, 5);
        kernel_map_region(m->base + m->ro_off, m->data_off - m->ro_off, 3, 1);
        kernel_map_region(m->base + m->data_off, m->size - m->data_off, 4, 3);
        LOG("[forge] plugin %s (%s)\n", name, found[i]);
        at = (at + m->size + 0xFFFF) & ~0xFFFFu;
    }
    if (!nplugins) {  // [AllGenU] no plugin loaded: only what KZMUI needs (its per-frame ImGui frame, its context)
        kernel_map_region(FORGE_TRAMP, FORGE_STUBS - FORGE_TRAMP, 3, 5);
        kernel_map_region(FORGE_STUBS, FORGE_END - FORGE_STUBS, 3, 5);
        interp_add_range(FORGE_STUBS, FORGE_END);
        fnhook_set(text_addr(OFF_UPDATE), hle_register("forge:update", frame_update));
        overlay_start(layers, nl);
        return;
    }
    kernel_map_region(FORGE_TRAMP, FORGE_STUBS - FORGE_TRAMP, 3, 5);
    kernel_map_region(FORGE_STUBS, FORGE_END - FORGE_STUBS, 3, 5);
    interp_add_range(FORGE_STUBS, FORGE_END);
    fnhook_set(text_addr(OFF_SYSTEM_CTOR), hle_register("forge:cSystem::cSystem", system_ctor));
    fnhook_set(text_addr(OFF_SYSTEM_DTOR), hle_register("forge:cSystem::~cSystem", system_dtor));
    fnhook_set(text_addr(OFF_APP_RUN), hle_register("forge:sApp::run", app_run));
    fnhook_set(text_addr(OFF_UPDATE), hle_register("forge:update", frame_update));
    overlay_start(layers, nl);
}
