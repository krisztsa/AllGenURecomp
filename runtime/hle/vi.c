// nn::vi (display / layer) and nv:: (graphics driver init), implemented on the host.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../config.h"
#include "../kernel.h"

static uint32_t vsync_r, vsync_w;
static KEvent *vsync_event;
uint32_t vi_vsync_handle(void) { return vsync_r; }
volatile uint64_t g_vsync_count;
uint32_t g_native_window;

// Frame rate. The engine paces itself (frame limiter at main 0xda5d00: sleeps until a frame period has passed, then
// derives its frame delta, which game logic scales by): the target is a float at +0x3c of its main system object
// (pointer in the global at 0x2326764, the same field theboy181's 60fps patch sets). The game puts 30.0 there at
// start and resets it at some transitions, so every presented frame writes the chosen rate back (the 90 / 120 fps
// cheat codes write this same field). The frame delta is in 60 fps frames, measured: walking speed is the same at 30,
// 60 and ~135 fps. The settings' FPS (allgenurecomp.ini: 30..240, default 60); MHGU_FRAMERATE overrides it. Above the
// monitor's refresh rate the settings' VSync must be Off (immediate present): FIFO / mailbox wait for the display.
#define FPS_OBJ_GLOBAL 0x2326764u
// [AllGenU] fps 0 in the settings = no cap ("∞" in the KZMUI menu): the engine is given a target high enough that its
// own frame limiter never sleeps; the frame delta stays real time, so game speed does not change. The value lives
// outside the function so vi_set_framerate (the menu) can reset it at run time.
#define FPS_UNCAPPED 1000.0f
static float g_framerate = -1;
static float framerate(void) {
    if (g_framerate < 0) {
        const char *e = getenv("MHGU_FRAMERATE");
        float f = e ? (float)atof(e) : (float)g_config.fps;
        if (f == 0.0f) f = FPS_UNCAPPED;
        if (!(f >= 30.0f)) f = 30.0f;
        if (f > FPS_UNCAPPED) f = FPS_UNCAPPED;
        g_framerate = f;
        LOG("[vi] frame rate target %.0f%s\n", f, f >= FPS_UNCAPPED ? " (no cap)" : "");
    }
    return g_framerate;
}

// [AllGenU] the rate the engine is actually handed. With V-Sync on and a target at or above the panel, the engine is
// paced a hair above the panel (hz + 2) so a frame is always ready when the retrace comes: FIFO then shows exactly the
// panel's rate. That is the fix for "60 selected, 55-57 shown": at exactly 16.7 ms the queue missed a retrace every
// few frames. A target below the panel keeps its own rate (30 / 45 stay exact; the swapchain uses mailbox then, which
// does not hold a frame back). Without V-Sync the player's target stands (0 = uncapped).
static float pace_rate(void) {
    float t = framerate();
    if (!g_config.vsync) return t;
    int hz = ui_display_hz();
    if (t >= (float)hz - 0.5f) return (float)hz + 2.0f;
    return t;
}

// [AllGenU] the menu's FPS limiter: g_config.fps (0 = no cap) is already the new value. The cached target is reset
// and the engine's field written at once; the vsync thread reads the new period on its next iteration.
void vi_apply_framerate(void);  // (defined just below)
void vi_set_framerate(int fps) {
    (void)fps;
    g_framerate = -1;
    (void)framerate();
    vi_apply_framerate();
}
void vi_apply_framerate(void) {  // (also called per presented frame: runtime/nvn/vulkan/device.c)
    float want = pace_rate();
    uint32_t obj = LD32(FPS_OBJ_GLOBAL);
    if (obj < HEAP_BASE || obj >= HEAP_BASE + HEAP_MAX) return;  // (not created yet)
    float cur = LDF(obj + 0x3c);
    static float written = -1.0f;  // [AllGenU] what this side last put there (the menu changes it at run time)
    if (cur == want) { written = want; return; }
    // the game writes 30.0 at start / some transitions: put the target back. It is also rewritten when the menu picks
    // another FPS (the old check only ever replaced 30, so 120 -> 60 left the engine at 120 while the display waited
    // at 60: the mismatch behind the uneven frame times).
    if (cur == 30.0f || written < 0.0f || fabsf(cur - written) < 0.01f) {
        STF(obj + 0x3c, want);
        written = want;
    }
}

// The display's vsync event: 60 Hz, or the frame rate target when that is higher (the game waits for it per frame).
static void *vsync_thread(void *arg) {
    (void)arg;
    uint64_t next = plat_now_ns();
    for (;;) {
        // [AllGenU] the period follows the settings' cap at run time (the FPS limiter in the menu) and the V-Sync
        // pacing above (a hair over the panel, so FIFO sees a frame ready at every retrace)
        float fr = pace_rate();
        const uint64_t period = (uint64_t)(1e9 / (fr > 60.0f ? fr : 60.0f));
        next += period;
        plat_sleep_until_ns(next);
        uint64_t now = plat_now_ns();
        if (now > next + period * 3) next = now;  // don't burst-signal to catch up
        vi_apply_framerate();
        __atomic_add_fetch(&g_vsync_count, 1, __ATOMIC_RELAXED);
        pthread_mutex_lock(&g_klock);
        kernel_signal(vsync_event);
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}
HLE_FUNC("_ZN2nn2vi10InitializeEv", vi_initialize) { (void)c; }
HLE_FUNC("_ZN2nn2vi8FinalizeEv", vi_finalize) { (void)c; }

HLE_FUNC("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_default_display) {
    ST32(ARG(0), hle_alloc(0x100));
    RET(0);
}

HLE_FUNC("_ZN2nn2vi12ListDisplaysEPNS0_11DisplayInfoEi", vi_list_displays) {  // DisplayInfo: name[64], ...
    if (ARG(0) && (int32_t)ARG(1) > 0) {
        memset(ARGP(0), 0, 0x60);
        strcpy((char *)ARGP(0), "Default");
        RET(1);
    } else {
        RET(0);
    }
}

HLE_FUNC("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", vi_close_display) { (void)c; }

HLE_FUNC("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer) {
    ST32(ARG(0), hle_alloc(0x100));
    RET(0);
}

HLE_FUNC("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", vi_destroy_layer) { (void)c; }
HLE_FUNC("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", vi_set_layer_crop) { RET(0); }
HLE_FUNC("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", vi_set_layer_scaling) { RET(0); }

HLE_FUNC("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window) {
    if (!g_native_window) g_native_window = hle_alloc(0x100);
    ST32(ARG(0), g_native_window);
    RET(0);
}

HLE_FUNC("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_get_vsync_event) {
    if (!vsync_event) {
        vsync_event = kernel_create_event_pair(&vsync_w, &vsync_r);
        pthread_t t;
        pthread_create(&t, NULL, vsync_thread, NULL);
    }
    // nn::os::AttachReadableHandleToSystemEvent(event, handle, managed=false, clear_mode=AutoClear)
    c->r[1] = vsync_r;
    c->r[2] = 0;
    c->r[3] = 1;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    RET(0);
}

HLE_FUNC("_ZN2nv18InitializeGraphicsEPvj", nv_initialize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv16FinalizeGraphicsEv", nv_finalize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv20SetGraphicsAllocatorEPFPvjjS0_EPFvS0_S0_EPFS0_S0_jS0_ES0_", nv_set_allocator) { (void)c; }
HLE_FUNC("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvjjS0_EPFvS0_S0_EPFS0_S0_jS0_ES0_", nv_set_dt_allocator) { (void)c; }
