// NVN entry points: every NVN function the game asks for is looked up by name and bound to a host function
// (runtime/nvn/*.c); names without one become logging stubs.
#include <string.h>

#include "../hle.h"

static uint32_t nvn_proc(const char *name) {
    if (!name || !*name) return 0;
    return hle_register(name, hle_lookup_impl(name));
}

HLE_FUNC("nvnBootstrapLoader", nvn_bootstrap_loader) { RET(nvn_proc(gstr(ARG(0)))); }

// void *nvnDeviceGetProcAddress(const NVNdevice *, const char *)
HLE_FUNC("nvnDeviceGetProcAddress", nvn_device_get_proc_address) { RET(nvn_proc(gstr(ARG(1)))); }
