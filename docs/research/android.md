# TODO: Android build (planned 2026-10-25, not started)

Goal: the port on Android phones / tablets, played with a controller (Bluetooth or USB). No touch controls: the game
doesn't suit them, so none are planned. Players bring their own dump as on PC; the APK holds no game files.

## What carries over

- The recompiled game code and the runtime are plain C (the lifter's output, interpreter, NEON emulation): no x86
  intrinsics anywhere. Only the build flag `GENARCH ?= -march=x86-64-v3` (Makefile) is x86; arm64 needs its own
  (e.g. `-march=armv8.2-a`). The guest's NEON / VFP could even map to the host's NEON later, for speed.
- runtime/platform/linux.c is the starting point: Android is Linux with bionic (mmap, signals, threads). Check the
  4 GB guest window reservation (plat_reserve_guest at GUEST_BASE) fits the device's address space layout (39-bit
  VA kernels: 512 GB, fine; pick a GUEST_BASE the platform leaves free), and the page write tracking path (as on
  Linux: MHGU_NO_WRITE_WATCH's mechanism there).
- SDL3 supports Android (its Java activity glue, gamepads, text input with the system keyboard for names and item
  set names). Controller-only fits SDL's gamepad API as it is.
- Vulkan is Android's graphics API, and mobile GPUs decode ASTC natively (the port's ASTC decoder can be skipped
  there).

## Work

1. Build: NDK clang for arm64-v8a, the generated code as a shared library (mhgugame.so, as on Linux; it is huge,
   so build time and APK size need watching), a Gradle project around SDL3's Android template, an APK.
2. Game files: Android's scoped storage. The runtime opens files by path (romfs.c, fs service), so either copy the
   player's exefs / romfs into the app's own storage on first start (simplest; doubles the space) or read them
   through a folder the player grants (Storage Access Framework file descriptors: more work in romfs.c). Saves,
   ini, cache, mods and cheats in the app's storage, reachable for the player (Android/data/<app> or a granted folder).
3. Vulkan on mobile drivers (Adreno, Mali, Xclipse): the renderer assumes Vulkan 1.2 on desktop GPUs; check the
   features / formats it requires (device.c) against common phone drivers, shaderc on Android (it compiles the game's
   shaders at run time), and memory (phones share RAM between CPU and GPU; the game plus the port's caches must fit).
4. Movies: ffmpeg's avformat / avcodec built for Android, or MediaCodec.
5. Performance: AOT-compiled C should run well on recent arm64 cores, but measure on a mid-range phone; thermal
   throttling over a long hunt; the frame rate cap and resolution scale as the main levers. Lifecycle: the app
   paused / resumed (audio, the Vulkan surface going away), the Android back button.
6. Settings without a keyboard: the ini can't easily be edited on a phone, so an in-game settings panel (the overlay
   used for Forge's menu, opened with a controller combination) for resolution, frame rate and controls.

## Open questions

- Minimum device: arm64 only; which Android version and Vulkan level.
- Distribution: an APK on the releases page (sideloaded), like the PC zips.
- Multiplayer works the same over Wi-Fi (Local Play over IP; the internet hubs plan in internet-hubs.md).
