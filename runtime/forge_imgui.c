// Forge plugins' Dear ImGui calls. Forge exports cimgui's C API (ig* / Im*) to its plugins; here those imports run
// the host's Dear ImGui (the overlay's context, runtime/overlay.cpp). The calls themselves are generated
// (tools/imgui_thunks.py -> forge_imgui_thunks.inc, included below); this file has what they rely on.
//
// Mirrors: a plugin sees ImGui's objects (ImGuiIO, ImGuiStyle, ImDrawList...) in its own 32-bit layout, at guest
// addresses. Every host object a plugin gets hold of has a mirror: a block of guest memory in that layout, and a
// shadow of it (the values last copied in). m_out (an object handed to the plugin): its fields are copied in. m_in
// (an object from the plugin): the fields the plugin changed since (guest differs from the shadow) are copied to the
// host object first. Around every call the objects passed or returned are brought up to date, and ImGuiIO /
// ImGuiStyle / ImGuiPlatformIO (read and written at any time) both ways. Fields: bytes as they are, size_t, strings
// and other data pointers (guest memory: its host address and back; a host string the plugin reads: a copy in guest
// memory), pointers to other objects (their mirrors), function pointers (not carried over). Internal structs
// (imgui_internal.h) are opaque: their address is all that means something. An object in the plugin's own memory
// (a struct of its own passed by address) gets a host object of its own, kept in step the same way.
#include <stdarg.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#include "forge.h"
#include "hle.h"

// ------------------------------------------------------------------ argument helpers (used by the thunks)
#define GP(a) ((a) ? G2H(a) : NULL)  // guest pointer -> host pointer (NULL stays NULL)

static inline float ig_f(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }
static inline double ig_d(uint32_t lo, uint32_t hi) {
    uint64_t v = (uint64_t)hi << 32 | lo;
    double d;
    memcpy(&d, &v, 8);
    return d;
}

static bool in_guest(const void *p) {
    return (uintptr_t)p >= GUEST_BASE && (uintptr_t)p < GUEST_BASE + 0x100000000ull;
}

// a host pointer the plugin gets: guest memory's own address; host memory has none (0, logged once per call site)
static uint32_t h2g_quiet(const void *p) { return in_guest(p) ? (uint32_t)((uintptr_t)p - GUEST_BASE) : 0; }
#define h2g(p) ({                                                                            \
    const void *p_ = (const void *)(p);                                                      \
    static bool told_;                                                                       \
    if (p_ && !in_guest(p_) && !told_) {                                                     \
        told_ = true;                                                                        \
        LOG("[forge] %s returns a pointer to ImGui's memory: the plugin gets NULL\n", __func__); \
    }                                                                                        \
    h2g_quiet(p_);                                                                           \
})

static __thread Cpu *t_cpu;  // the plugin's thread, during its ImGui calls (callbacks run on it)
static __thread uint32_t t_cb;  // the guest function behind the callback trampoline being passed

// printf-style text from the guest's arguments
static __thread char t_fmt[16384];
static const char *ig_fmt(Cpu *c, uint32_t fmt, int reg, uint32_t stk_off) {
    forge_format(t_fmt, sizeof t_fmt, fmt ? gstr(fmt) : "", c, reg, c->r[13] + stk_off);
    return t_fmt;
}
static const char *ig_vfmt(Cpu *c, uint32_t fmt, uint32_t ap) {  // (AAPCS va_list: the address of the next argument)
    forge_format(t_fmt, sizeof t_fmt, fmt ? gstr(fmt) : "", c, 4, ap);
    return t_fmt;
}

// const char *const items[]: the guest's array of 32-bit pointers as a host one
static const char **ig_strarr(uint32_t a, uint32_t n) {
    static __thread const char **buf;
    static __thread uint32_t cap;
    if (!a) return NULL;
    if ((int32_t)n < 0) n = 0;
    if (n > cap) {
        cap = n + 16;
        buf = realloc(buf, cap * sizeof *buf);
    }
    for (uint32_t i = 0; i < n; i++) buf[i] = GP(LD32(a + 4 * i));
    return buf;
}

// ------------------------------------------------------------------ guest memory for mirrors and string copies
static struct { uint32_t addr, size; } g_free[256];
static int g_nfree;

static uint32_t gmem_alloc(uint32_t size) {
    size = (size + 15) & ~15u;
    for (int i = 0; i < g_nfree; i++)
        if (g_free[i].size == size) {
            uint32_t a = g_free[i].addr;
            g_free[i] = g_free[--g_nfree];
            memset(G2H(a), 0, size);
            return a;
        }
    return hle_alloc(size);
}

static void gmem_free(uint32_t addr, uint32_t size) {
    if (g_nfree < 256) g_free[g_nfree].addr = addr, g_free[g_nfree++].size = (size + 15) & ~15u;
}

// Host strings the plugin reads, copied into guest memory (by their host address; again if the text changed)
#define NSTR 1024
static struct { const char *host; uint32_t guest, cap; } strs[NSTR];
static int nstrs;

static uint32_t ig_str_out(const char *s) {
    if (!s) return 0;
    if (in_guest(s)) return h2g_quiet(s);
    size_t n = strlen(s) + 1;
    int i = 0;
    while (i < nstrs && strs[i].host != s) i++;
    if (i < nstrs && !strcmp(gstr(strs[i].guest), s)) return strs[i].guest;
    if (i == nstrs) {
        if (nstrs == NSTR) i = (int)((uintptr_t)s >> 4) % NSTR;  // (full: reuse a slot)
        else nstrs++;
        strs[i].host = s, strs[i].guest = 0, strs[i].cap = 0;
    }
    if (n > strs[i].cap) {
        if (strs[i].guest) gmem_free(strs[i].guest, strs[i].cap);
        strs[i].cap = (uint32_t)((n + 15) & ~15u);
        strs[i].guest = gmem_alloc(strs[i].cap);
    }
    memcpy(G2H(strs[i].guest), s, n);
    return strs[i].guest;
}

// ------------------------------------------------------------------ mirrors
enum { MF_RAW, MF_SIZE, MF_STR, MF_OBJ, MF_DATA, MF_FN };
typedef struct { uint32_t g, h, n; uint8_t kind; uint16_t type; } MField;  // offsets in the guest / host layout
typedef struct { const char *name; uint32_t gsize, hsize; const MField *f; uint32_t nf; bool hot; } MType;

#define FORGE_IMGUI_TABLES
#include "forge_imgui_thunks.inc"
#undef FORGE_IMGUI_TABLES

enum { M_OWN_GUEST = 1, M_OWN_HOST = 2 };  // the guest block is ours / the host object is ours (plugin's object)
typedef struct { void *host; uint32_t guest; uint16_t type; uint8_t flags; uint8_t *shadow; } Mirror;

#define MAXM 8192   // mirrors (slots of the two hash tables: twice that)
static Mirror mir[MAXM];
static int nmir, mfree_list[MAXM], nmfree;
static int hidx[2 * MAXM], gidx[2 * MAXM];  // open addressing: index + 1, 0 empty, -1 deleted
static int hot[64], nhot;

static uint32_t hash_ptr(uintptr_t p) { return (uint32_t)((p >> 3) * 0x9E3779B1u ^ (p >> 35)); }

static Mirror *find_host(const void *h, int type) {
    for (uint32_t k = hash_ptr((uintptr_t)h + (uintptr_t)type * 0x10001), n = 0; n < 2 * MAXM; n++, k++) {
        int i = hidx[k % (2 * MAXM)];
        if (!i) return NULL;
        if (i > 0 && mir[i - 1].host == h && mir[i - 1].type == type) return &mir[i - 1];
    }
    return NULL;
}

static Mirror *find_guest(uint32_t g) {
    for (uint32_t k = hash_ptr(g), n = 0; n < 2 * MAXM; n++, k++) {
        int i = gidx[k % (2 * MAXM)];
        if (!i) return NULL;
        if (i > 0 && mir[i - 1].guest == g) return &mir[i - 1];
    }
    return NULL;
}

static void table_put(int *t, uint32_t k, int v) {
    for (;; k++)
        if (t[k % (2 * MAXM)] <= 0) { t[k % (2 * MAXM)] = v; return; }
}

static void table_del(int *t, uint32_t k, int v) {
    for (uint32_t n = 0; n < 2 * MAXM; n++, k++) {
        int *e = &t[k % (2 * MAXM)];
        if (!*e) return;
        if (*e == v) { *e = -1; return; }
    }
}

static uint32_t mtype_gsize(int type) { return mtypes[type].f ? mtypes[type].gsize : 16; }

static Mirror *mirror_new(void *h, int type, uint32_t g, int flags) {
    int i;
    if (nmfree) i = mfree_list[--nmfree];
    else if (nmir < MAXM) i = nmir++;
    else {
        static bool told;
        if (!told) told = true, LOG("[forge] too many ImGui objects for plugins\n");
        return NULL;
    }
    Mirror *m = &mir[i];
    m->host = h, m->type = (uint16_t)type, m->flags = (uint8_t)flags;
    m->guest = g ? g : gmem_alloc(mtype_gsize(type));
    if (!g) m->flags |= M_OWN_GUEST;
    m->shadow = mtypes[type].f ? calloc(1, mtypes[type].gsize) : NULL;
    table_put(hidx, hash_ptr((uintptr_t)h + (uintptr_t)type * 0x10001), i + 1);
    table_put(gidx, hash_ptr(m->guest), i + 1);
    if (mtypes[type].hot && nhot < 64) hot[nhot++] = i;
    return m;
}

static void mirror_drop(Mirror *m) {
    int i = (int)(m - mir);
    table_del(hidx, hash_ptr((uintptr_t)m->host + (uintptr_t)m->type * 0x10001), i + 1);
    table_del(gidx, hash_ptr(m->guest), i + 1);
    for (int k = 0; k < nhot; k++)
        if (hot[k] == i) hot[k--] = hot[--nhot];
    if (m->flags & M_OWN_GUEST) gmem_free(m->guest, mtype_gsize(m->type));
    free(m->shadow);
    memset(m, 0, sizeof *m);
    mfree_list[nmfree++] = i;
}

static void pull(Mirror *m);
static int pull_depth;

// the mirror of a host object (made if new; its fields copied in when it is made, or when refresh)
static uint32_t mirror_of(const void *h, int type, bool refresh) {
    if (!h) return 0;
    Mirror *m = find_host(h, type);
    bool made = !m;
    if (!m && !(m = mirror_new((void *)h, type, 0, 0))) return 0;
    if (mtypes[type].f && (made || refresh) && pull_depth < 8) {
        pull_depth++;
        pull(m);
        pull_depth--;
    }
    return m->guest;
}

// host -> guest: every field
static void pull(Mirror *m) {
    const MType *t = &mtypes[m->type];
    uint8_t *g = G2H(m->guest), *h = m->host, *s = m->shadow;
    for (uint32_t i = 0; i < t->nf; i++) {
        const MField *f = &t->f[i];
        uint32_t v;
        switch (f->kind) {
        case MF_RAW:
            memcpy(g + f->g, h + f->h, f->n);
            memcpy(s + f->g, h + f->h, f->n);
            continue;
        case MF_SIZE: { size_t x; memcpy(&x, h + f->h, sizeof x); v = (uint32_t)x; break; }
        case MF_STR: { const char *p; memcpy(&p, h + f->h, sizeof p); v = ig_str_out(p); break; }
        case MF_DATA: { void *p; memcpy(&p, h + f->h, sizeof p); v = h2g_quiet(p); break; }
        case MF_OBJ: { void *p; memcpy(&p, h + f->h, sizeof p); v = mirror_of(p, f->type, false); break; }
        default: continue;  // (function pointers: not carried over)
        }
        memcpy(g + f->g, &v, 4);
        memcpy(s + f->g, &v, 4);
    }
}

// guest -> host: the fields the plugin changed
static void push(Mirror *m) {
    const MType *t = &mtypes[m->type];
    uint8_t *g = G2H(m->guest), *h = m->host, *s = m->shadow;
    for (uint32_t i = 0; i < t->nf; i++) {
        const MField *f = &t->f[i];
        if (f->kind == MF_RAW) {
            if (!memcmp(g + f->g, s + f->g, f->n)) continue;
            for (uint32_t k = 0; k < f->n; k++)  // (byte by byte: the host may have changed its neighbours since)
                if (g[f->g + k] != s[f->g + k]) h[f->h + k] = s[f->g + k] = g[f->g + k];
            continue;
        }
        if (f->kind == MF_FN || !memcmp(g + f->g, s + f->g, 4)) continue;
        uint32_t v;
        memcpy(&v, g + f->g, 4);
        memcpy(s + f->g, &v, 4);
        switch (f->kind) {
        case MF_SIZE: { size_t x = v; memcpy(h + f->h, &x, sizeof x); break; }
        case MF_STR: case MF_DATA: { void *p = GP(v); memcpy(h + f->h, &p, sizeof p); break; }
        case MF_OBJ: {
            Mirror *o = v ? find_guest(v) : NULL;
            void *p = o ? o->host : NULL;
            if (v && !o) break;  // (not an object we know: left as it is)
            memcpy(h + f->h, &p, sizeof p);
            break;
        }
        }
    }
}

static void hot_push(void) {
    for (int i = 0; i < nhot; i++) push(&mir[hot[i]]);
}
static void hot_pull(void) {
    for (int i = 0; i < nhot; i++) pull(&mir[hot[i]]);
}

#define IG_ENTER(c) (t_cpu = (c), hot_push())
#define IG_LEAVE(c) hot_pull()

// an object handed to the plugin / a new one it asked for (constructors)
static uint32_t m_out(const void *h, int type) { return mirror_of(h, type, true); }
static uint32_t m_out_new(const void *h, int type) { return mirror_of(h, type, true); }

// an object from the plugin: its host object, brought up to date
static void *m_in(uint32_t g, int type) {
    if (!g) return NULL;
    Mirror *m = find_guest(g);
    if (m) {
        if (m->shadow) push(m);
        return m->host;
    }
    if (!mtypes[type].f) {  // (an internal struct the plugin made itself: nothing to go by)
        static bool told[MTYPES];
        if (!told[type]) told[type] = true, LOG("[forge] plugin passed its own %s: not supported\n", mtypes[type].name);
        return NULL;
    }
    // the plugin's own object (in its memory, its layout): a host object for it, all fields copied over
    void *h = igMemAlloc(mtypes[type].hsize);
    memset(h, 0, mtypes[type].hsize);
    if (!(m = mirror_new(h, type, g, M_OWN_HOST))) return NULL;
    for (uint32_t i = 0; i < mtypes[type].gsize; i++) m->shadow[i] = (uint8_t)~LD8(g + i);  // (all differ)
    push(m);
    return h;
}

static void m_after(uint32_t g) {  // (after a call: the object's fields as ImGui left them)
    Mirror *m = g ? find_guest(g) : NULL;
    if (m && m->shadow) pull(m);
}

static void m_destroy(uint32_t g) {  // (the host object was deleted by the call)
    Mirror *m = g ? find_guest(g) : NULL;
    if (m) mirror_drop(m);
}

// ------------------------------------------------------------------ callbacks (trampolines: the guest's function)
static uint32_t cb_call(uint32_t fn, uint32_t a0, uint32_t a1) {
    Cpu *c = t_cpu;
    hot_pull();
    c->r[0] = a0, c->r[1] = a1;
    guest_call(c, fn);
    hot_push();
    return c->r[0];
}

static int tr_input_text(ImGuiInputTextCallbackData *d) {
    uint32_t fn = t_cb, g = m_out(d, T_ImGuiInputTextCallbackData);
    int r = (int)cb_call(fn, g, 0);
    Mirror *m = find_guest(g);
    if (m) push(m), mirror_drop(m);  // (the data lives on ImGui's stack: gone after this)
    return r;
}

static void tr_size(ImGuiSizeCallbackData *d) {
    uint32_t fn = t_cb, g = m_out(d, T_ImGuiSizeCallbackData);
    cb_call(fn, g, 0);
    Mirror *m = find_guest(g);
    if (m) push(m), mirror_drop(m);
}

static const char *tr_getter_str(void *user, int idx) {
    return GP(cb_call(t_cb, h2g_quiet(user), (uint32_t)idx));
}

static float tr_getter_float(void *user, int idx) {
    uint32_t r = cb_call(t_cb, h2g_quiet(user), (uint32_t)idx);  // (softfp, as Forge's plugins: the float in r0)
    return ig_f(r);
}

// ------------------------------------------------------------------ the calls (generated)
#include "forge_imgui_thunks.inc"

// ------------------------------------------------------------------ the calls written by hand
// Allocators: plugins' ImGui memory is guest memory (the guest's malloc / free, as Forge's own alloc functions do)
static uint32_t g_alloc_fn, g_free_fn;  // what the plugins get as Forge's (forge_onImGuiInit), and set
static void imgui_alloc(Cpu *c) {  // (size, user)
    guest_call(c, guest_sym("malloc"));
}
static void imgui_free(Cpu *c) {  // (ptr, user)
    guest_call(c, guest_sym("free"));
}

HLE_FUNC("igMemAlloc", ig_igMemAlloc) { imgui_alloc(c); }
HLE_FUNC("igMemFree", ig_igMemFree) { imgui_free(c); }
HLE_FUNC("igSetAllocatorFunctions", ig_igSetAllocatorFunctions) {}  // (plugins pass on Forge's: guest malloc / free)
HLE_FUNC("igGetAllocatorFunctions", ig_igGetAllocatorFunctions) {
    if (ARG(0)) ST32(ARG(0), g_alloc_fn);
    if (ARG(1)) ST32(ARG(1), g_free_fn);
    if (ARG(2)) ST32(ARG(2), 0);
}
// One context, the overlay's: a plugin asking for another gets that one
HLE_FUNC("igCreateContext", ig_igCreateContext) {
    LOG("[forge] a plugin creates an ImGui context: it gets the overlay's\n");
    RET(m_out(igGetCurrentContext(), T_ImGuiContext));
}
HLE_FUNC("igDestroyContext", ig_igDestroyContext) {}
// The frame is the overlay's (a plugin's NewFrame / Render would break it)
static void frame_call(Cpu *c, const char *what) {
    static bool told;
    if (!told) told = true, LOG("[forge] a plugin calls ImGui %s: ignored (the overlay runs the frame)\n", what);
    RET(0);
}
HLE_FUNC("igNewFrame", ig_igNewFrame) { frame_call(c, "NewFrame"); }
HLE_FUNC("igEndFrame", ig_igEndFrame) { frame_call(c, "EndFrame"); }
HLE_FUNC("igRender", ig_igRender) { frame_call(c, "Render"); }
HLE_FUNC("igGetDrawData", ig_igGetDrawData) { frame_call(c, "GetDrawData"); }

// ------------------------------------------------------------------ the plugins' entry points (overlay.cpp)
void forge_imgui_init(void *cpu) {
    Cpu *c = cpu;
    if (!g_alloc_fn) {
        g_alloc_fn = hle_register("forge:imgui_alloc", imgui_alloc);
        g_free_fn = hle_register("forge:imgui_free", imgui_free);
    }
    t_cpu = c;
    uint32_t ctx = m_out(igGetCurrentContext(), T_ImGuiContext);
    forge_plugins_imgui(c, FORGE_IMGUI_INIT, ctx, g_alloc_fn, g_free_fn);
    hot_push();
}

void forge_imgui_run(void *cpu, int which) {
    Cpu *c = cpu;
    t_cpu = c;
    hot_pull();
    forge_plugins_imgui(c, which, 0, 0, 0);
    hot_push();
}
