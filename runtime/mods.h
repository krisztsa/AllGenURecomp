// Mods: folders under mods/ (beside allgenurecomp.exe in the standalone build, else in the current folder;
// MHGU_MODS_DIR overrides). Dropping a mod's folder in installs it, removing the folder uninstalls it: everything is
// layered over the game's own files at start, nothing is changed on disk. A mod folder may hold:
//   romfs/   loose files layered over the game's RomFS (same paths as in the game's romfs/ folder; a bare nativeNX/
//            folder counts as romfs/nativeNX/)
//   exefs/   IPSwitch patches (*.pchtxt) and IPS / IPS32 patches (<build id>.ips) for the game's code modules.
//            Only patches to data are applied: the port runs code compiled ahead of time (tools/gen.py), so a
//            patched instruction would change nothing; those are listed in the log as not applied.
// These folders are found up to three levels down, so a mod unpacked as Mod/Mod/romfs or with an emulator or
// Atmosphere layout (atmosphere/contents/0100770008DD8000/romfs) works as it is.
// Mods load in name order; a file in a later mod wins. mods/load_order.txt (one folder name per line, '#'
// comments) picks and orders them instead; a folder whose name starts with '_' is skipped.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {  // (runtime/kzmui.cpp, C++: the same symbols the C files define)
#endif

struct Module;

const char *mods_dir(void);
void mods_make_dir(void);  // creates an empty mods/ (standalone: shows players where mods go)
int mods_romfs_layers(const char **out, int max);  // host folders to layer over the RomFS, in order
void mods_patch_module(struct Module *m, const uint8_t build_id[32]);  // exefs patches for a loaded module
void forge_init(void);  // Forge plugins from the mods' RomFS (runtime/forge.c): before the game starts
void hpscale_init(void);  // large monster HP by the number of hunters (runtime/hle/hpscale.c)
void villagehub_init(void);  // Village quests at the Hub / Pub counter (runtime/hle/villagehub.c)
void movelog_init(void);  // (debug) MHGU_LOG_MOVES: the player's command table changes (runtime/hle/movelog.c)
void quickhunt_init(void);  // (debug) MHGU_QUICKHUNT: start-up straight into a quest (runtime/hle/quickhunt.c)

// Cheat codes (Atmosphere's format) from the .txt files in cheats/, beside mods/ (runtime/cheats.c)
const char *cheats_dir(void);
void cheats_make_dir(void);  // creates an empty cheats/ (standalone: shows players where cheats go)
void cheats_init(void);      // loads them and runs them once: before the game starts
void cheats_frame(void);     // runs them for a frame the game presented (the window loop, each pass)

#ifdef __cplusplus
}
#endif
