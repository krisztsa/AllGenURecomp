# MHGU Recomp: status and working notes

Static recompilation of *Monster Hunter Generations Ultimate* (Switch, v1.4.0, build id `FB08F1D2...`) to a native
Windows executable, modeled on LAS Recomp (`../LASRecomp`). Goal: easy modding of game data (weapon / monster
values etc.). No game files or generated code are ever committed (see `.gitignore`).

## Key facts

- The game is **32-bit ARM** (AArch32, ARM mode only, `NX-NXFP2-a32`, NintendoSDK 4.4.2), not AArch64 like LAS.
  Modules: `main` (~4.9M reachable instructions), `sdk` (nnSdk, fully symbolized), `subsdk0` (Android Stagefright
  movie player: **not recompiled**; `movie::` API to be implemented on the host with FFmpeg, as LAS does).
- Guest memory: a 4 GB window at `GUEST_BASE` (0x10000000000); guest address `a` = host `GUEST_BASE + a`
  (`G2H` / `H2G` in `runtime/cpu.h`; the name `GPTR` clashes with windows.h). Pages are committed on first touch.
- Layout (`runtime/rt.h`): main @0x200000, sdk @0x2400000, HLE fake functions @0x3000000, TLS @0x3100000,
  HLE heap @0x4000000, heap @0x8000000 (SetHeapSize gets 2.5 GB: GetInfo total memory is reported so),
  alias @0xA8000000, stacks @0xD0000000.
- 32-bit SVC ABI derived from the SDK's svc stubs (see `runtime/kernel.c` comments): 64-bit values in register
  pairs, 5th argument in r4.
- Target hardware of the owner: i7-4770K, 8 GB RAM, AMD R9 200 series (GCN, driver 2020, **Vulkan 1.2.133**,
  1.8 GB VRAM). `build/vkprobe.exe` (tools/vkprobe.c) output, 2026-10-20:
  - missing: dynamic_rendering, synchronization2, extended_dynamic_state 1/2/3, push_descriptor,
    custom_border_color, maintenance4, index_type_uint8, robustness2, provoking_vertex, ASTC, ETC2, shaderFloat16;
  - present: imageless_framebuffer, create_renderpass2, separate_depth_stencil_layouts, timeline semaphores,
    shader_demote_to_helper_invocation, scalarBlockLayout, shaderInt8 + 8/16-bit storage, samplerMirrorClampToEdge,
    vertex_attribute_divisor, external_memory_host, line_rasterization, depth_clip_enable, BC textures,
    descriptor_indexing, inline_uniform_block, shaderInt64, robustBufferAccess, dualSrcBlend, logicOp.
  The renderer must be Vulkan 1.2 baseline (works on GTX 1080 too). LAS's Vulkan backend is 1.3: reuse its pieces,
  not wholesale.

## Recompiler (tools/)

- `nso.py` ELF32/REL loader; `lifter.py` A32 + VFP; `neon.py` Advanced SIMD (+ AES, CRC32 via helpers);
  `gen.py` function discovery (.ARM.exidx, symbols, relocs, call targets, fixpoint), recursive descent (literal pools
  never lifted; pc-relative literal loads folded to constants), jump tables (`add rB, pc, #imm / ldr rT, [rI, rB] /
  add pc, rT, rB`, with scheduled instructions in between), computed gotos ("all labels" switch fallback).
- `test_lift.py` + `test_harness.c`: differential test against Unicorn (UC_CPU_ARM_MAX). 0 mismatches over tens of
  thousands of sampled instructions (incl. all reachable VFP / NEON). Run after every lifter change:
  `make test-lift`.
- `hle_list.py` writes `runtime/hle/hle_symbols.txt` from `HLE_SDK(...)` uses (SDK functions replaced on the host:
  division helpers, memcpy & co).
- `mtarc.py`: MT Framework ARC v17 list / extract / pack / index.

## Runtime (runtime/)

Ported from LAS, adapted to 32-bit: `kernel.c` (SVCs), `ipc.c`, `loader.c` (REL relocations), `dispatch.c`,
`romfs.c` (**virtual RomFS** built in memory from the loose `romfs/` folder; mod folders `mods/<name>/romfs/`
layer on top: `mods.c`), services (`fs`, `am`, `misc` (acc, set, pctl, aoc, hid service), `time`, `audio` (LAS's
audren mixer; REV4 output layout fixed)), HLE: `vi.c`, `nvn_boot.c`, `ngc.c` (no profanity filter), `input.c`
(nn::hid at API level), `offline.c` (bcat / friends / applets as an offline console), `libc.c`.
NVN layer: `nvn/nvn.h` (argument adapter `nvn_args.c`: signature strings, 'l' = 64-bit, 'f' = float),
`objects.c`, `cmdbuf.c` ported from LAS. Renderer: `nvn/vulkan/` (Vulkan 1.2, see `vk.h`): render pass objects +
framebuffers cached by view, every image in GENERAL layout with full barriers between passes, all fixed-function
state in the pipeline key, descriptor sets from per-submission pools, guest vertex / index / uniform ranges copied
per submission (the 2.5 GB heap is not imported), textures change-detected with page write tracking
(MEM_WRITE_WATCH). `NULL_GPU=1 make` builds the null renderer instead. Shader global memory (LDG/STG) reads zeros
(logged if a shader uses it). Window and controls: `ui.c`, `host_input.c`.

## Progress (2026-10-20)

Plays: health & safety -> opening movie (FFmpeg) -> title -> character creation (name typed: see "Text entry") ->
style / arts -> save slot (save written to run/save/) -> Bherna village intro cutscene -> village with player
control, ~30 fps on the R9 (the game's own rate). Everything seen so far renders correctly.

Fixed on the way (2026-10-20):
- jump tables: the longer of bound-based and heuristic lengths; backward entries; scheduled instructions (VFP
  `vmov`, up to ~20 constant loads) between `add rB, pc` and `add pc`. `generated/*/undetected_tables.txt` lists
  `add pc, rX, rY` jumps not understood as tables (currently none).
- `movie::` HLE (`runtime/hle/movie.c`): deep decoded-frame queues (video held until its time on the audio clock),
  the output-format-changed event before the first frame, and GetOutputBuffer copies into the game's own Buffer.
- InlineKeyboard: DecidedEnterArgUtf8 = text inline at +0, length at +0x7d4; Calc returns the state (0 none,
  1 ready / hidden, 3 shown) - the game waits for 1 after a text entry.
- NVN fixed-function alpha test (ColorState alpha func + SetAlphaRef) in the translator (foliage, impostors).
- Vulkan loader: System32\vulkan-1.dll of older drivers has no 1.3 entry points (VMA_VULKAN_VERSION 1.2).
- Audio: MHGU sends every voice through a splitter (now implemented, plus the BufferMixer effect) - silence before.
- Movies: tight NV12 (the game copies Y then UV right after it) into the game's own Buffer memory.
- Performance: device-local vertex cache refreshed per written page (page write tracking): guest copies went from
  ~880 MB/s to ~8 MB/s; mailbox presentation (FIFO with 2 images held the game at ~26 fps).
- Settings: mhgurecomp.ini in the game folder (runtime/config.c; written with the defaults when missing): Mode = Fullscreen /
  Windowed / Borderless, Resolution = WxH or Desktop, RenderResolution = Auto or WxH, FPS = 30..240, VSync. Alt+Enter
  toggles windowed / full screen. The 3D resolution is the game's own docked render size (main 0xdbcd34 / 0xdbcd38,
  `mov` 1920 / 1080, the constants the 4K-2K mod patches): tools/gen.py PATCHES makes those two instructions read
  g_render_size. Overrides for test runs: MHGU_CONFIG=<file>, MHGU_WINDOW=WxH (windowed), MHGU_RENDER=WxH,
  MHGU_MSAA=1|2|4|8, MHGU_FXAA=0|1, MHGU_PAD_POSITIONAL=0|1. [Controls] FaceButtons = Position (default: the Switch
  layout, right button A) or Label (the letter on the button, via SDL_GetGamepadButtonLabel).
- MSAA (settings MSAA = Off / 2x / 4x / 8x, F7 or MHGU_CONTROL "msaa <n>" switches it live; exec.c "MSAA"): passes
  whose depth target has the 3D resolution render into multisampled companions (TexBackend.ms) while those hold the
  target's contents or are cleared at the pass start (a look-ahead over the command list: leading_clears); a target
  is resolved only when something reads it (lazy: vk_ms_resolve). Colour: vkCmdResolveImage; depth / stencil: a copy
  into a sampleable multisampled image + a shader writing sample 0 (stencil via VK_EXT_shader_stencil_export). NVN
  alpha tests (> >= < <=) become sample coverage (gl_SampleMask, ShaderKey.alpha_coverage). Cut-outs the game does
  with its own discard stay hard-edged. R9 270X, village, 1080p: 56 fps off, 35 at 2x, 28 at 4x, 18 at 8x. Measured
  with MHGU_CONTROL "gpuprof" (GPU timestamps per pass): the multisampled passes cost ~3 ms more at 4x, a colour
  resolve ~1.6 ms, the depth / stencil resolve ~9 ms (the driver decompressing it; Vulkan 1.2 render pass resolves
  were as slow, and a sampleable multisampled image loses its compression: passes several times slower).
- FXAA (settings FXAA = On / Off): the game's FXAA pass is recognised by its fragment shader's code hash
  (FXAA_FS_HASH, exec.c) and becomes a copy of its input when off.
- Frame rate: the settings' FPS or MHGU_FRAMERATE (default 60, 30..240) writes the engine's target fps (float at +0x3c of the object in
  global 0x2326764, the field theboy181's 60fps patch sets) each frame. Above the monitor's refresh rate it needs
  VSync = Off: see "Frame rates above 60". MHGU_VSYNC=0/1 overrides the setting: no vsync / classic vsync.
- Open: a white flashing bar the user saw in pause menu > Status (not found yet: need a screenshot); lens flare
  blocks when first entering an area (draws skipped while shaders compile: much reduced, see "Fidelity audit").

## Fixed (2026-10-20): shimmer / light-shaft "triangle" / blocky bloom (pending the user's in-game check)

Symptom: flickering 16x8 blocks in bright areas, a wedge from the sun in hunts (light shafts), sparkle on sunlit stone.
Cause: AMD's 2020 Windows driver for GCN1 (R9 200) does not flush the colour-attachment cache for a barrier that only
says ALL_COMMANDS / MEMORY_WRITE (what vk_full_barrier and the render-pass dependencies used). The next pass sampling
the image just rendered read stale 8x8 tiles, mostly of the last-drawn triangle of a full-screen quad; lockstep dumps
hid it. Fix: barriers and dependencies name every stage and access type (vk.h VK_STAGES_ALL / VK_ACCESS_ALL*).
How it was found: F10 dumps in the hunt (light-shaft chain: pass 023 extract 960x540 -> 240x135, 024-026 radial blur
fs 7a142300 with sun position in cb4[0].xy and length cb4[2].z, offsets / weights in cb5, 027 upscale, 029 composite);
a CPU emulation of the blur matched the GPU except in those tiles; a standalone Vulkan replay (tools/vkreplay.c:
shaders from run/build/glsl, dumped input, the copy + blur ping-pong) reproduced the tiles on this GPU and
showed explicit barrier flags remove them. Ruled out on the way: clears (MHGU_CLEAR_DRAW / F9), depth / stencil,
alpha test / alpha-to-coverage, primitive restart, vertex buffer size, mip data, samplers, the 60 fps patch.
- The user's Game Bar videos are much brighter than the real image (capture encoding); on-screen pixels == "shot".
- Debug aids: F10 / "dump [n]" (all passes of n frames, depth-only passes as grey + raw float32), run-debug.bat,
  MHGU_DUMP_VERBOSE=1 (targets, pipeline state, sampler, 16 floats of the first fragment uniform buffers per pass),
  MHGU_CB_WATCH=<fs addr>, MHGU_SAMPLER_LOG=1, MHGU_TEX_LOD=n, MHGU_CLEAR_DRAW=1 / F9 (clears as draws).

## Fixed (2026-10-20): holes in dynamic shadows (pending the user's in-game check)

Hard shadows had straight-edged, camera-dependent gaps next to the sharp near cascade (a dinosaur in a hunt; villagers
in Bherna). Cause: the translator ignored condition codes. The receivers' shadow lookup (e.g. fs 7abfef00, the
ground, in the screen-space shadow-mask pass) picks a cascade with `FSET.CC RZ, |x - 0.5| < c[1][1]` then
`@P1 BRA NEU`: branch when cascade 0's y range (P1) AND its x range (CC) hold. Without the CC test, pixels inside
cascade 0's y range but outside its x range jumped with an unset depth reference -> no shadow. Now FSET .CC sets the
zero / sign flags, and BRA / EXIT / KIL / SYNC / BRK / CONT test them (translate.c cc_test; 26 village shaders use it).
Also fixed on the way: nvnCommandBufferSetDepthClamp was a no-op; the game enables depth clamp for the shadow-map pass
(now PipeKey.depth_clamp -> depthClampEnable).
Debug aids added: MHGU_GLSL_OVERRIDE=1 (run/glsl_override/<addr>_fs.glsl replaces a translated fragment shader),
MHGU_FORCE_WMASK=<fs addr> (all channels written), MHGU_DUMP_DRAWS=1 (every colour draw's program in a dump),
MHGU_SAVE_MSL=1 (instruction words as comments in saved GLSL), F8 (shadow-pass scissor: as set / none / mirrored).

## Local Play over IP (2026-10-20)

The game's Local Play (Switch local wireless: Multiplayer -> Play with People Nearby) works between PCs. The game
uses nn::ldn for the wireless network and plain nn::socket for its own traffic (one UDP socket, port 12345, with
broadcasts); MT Framework's nNetwork session layer sits on top (NEX is linked too, for online play only). Both
APIs are HLE'd at the function level, so no Switch service (ldn:u, bsd:u) is involved:
- runtime/net/ldn.c: nn::ldn. Create Hub -> the host listens on ini [Multiplayer] Port (default 39500, TCP) and
  answers LAN search probes on the same UDP port. Search Hubs -> Scan asks the LAN (broadcast + this computer) and
  every ini Join address (host[:port], comma separated) for the host's NetworkInfo. Joining keeps one TCP connection
  to the host: it assigns node numbers / addresses (169.254.x.n, the host n = 1), pushes NetworkInfo on every change
  (joins, leaves, advertise data) with the state change event signalled, and relays packets between players. The
  game's packets themselves go by UDP to/from the same port when that gets through ("MHGD" datagrams with a per-
  station token; station hello every 2 s keeps NAT mappings open), else - and after 15 s without UDP - through the
  TCP link. Only the host needs a reachable port (forwarded TCP + UDP; TCP alone works). Host gone -> the stations go to state Error (reason SignalLost / DestroyedByUser):
  the game says "Connection lost." and returns to offline.
- runtime/net/vsock.c: nn::socket on that virtual network (UDP datagrams, TCP emulated with SYN/SYNACK/data/FIN/RST
  messages - the link is reliable and ordered). The Switch's values: BSD socket options, Linux errno numbers,
  O_NONBLOCK 0x800. Nothing reaches the host's real network.
- Tested on one PC with two instances (run/ hosts, run2/ joins, 640x360, MHGU_LDN_PORT / MHGU_LDN_JOIN env
  overrides): hub created, found by LAN search and by Join address, joined, both see each other walk in the hub,
  Disconnect and a killed host handled. Not tested yet: two PCs, quests together, more than 2 players.
- The game's network library is Nintendo's Pia (statically linked; RTTI names nn::pia::local / inet / session /
  transport). Local and Online play share Pia's transport (stations, ReliableSlidingWindow, RttProtocol, keep-alive /
  life check); only the session layer differs (pia::local on LDN vs pia::inet on NEX). So Local Play already adapts
  to the measured round trip like Online does - there is nothing low-ping-only to swap out. What did matter: Pia
  expects lossy datagrams, and carrying them over TCP turns a lost packet into a stall of everything behind it,
  hence the UDP path. Tested with MHGU_LDN_LAG=<ms> (received packets held back; on both ends: round trip +2x) and
  MHGU_LDN_LOSS=<percent> (received packets dropped): 300 ms, and 800 ms with 5 % loss: join, both see each other
  move, still connected after a minute idle.
- Online Play entries: nn::nifm is HLE'd as "no internet connection" (runtime/hle/offline.c); Create Hub / Search
  Hubs / Search by ID go back to the menu (the system's error popup is not shown), Friend List is empty. Before,
  nifm's SDK code called a null function (crash).
- MHGU_LOG_NET=1 logs socket setup, =2 every nn::ldn / nn::socket call.
- Kernel fix found on the way: svcWaitProcessWideKeyAtomic re-took the mutex on timeout; Horizon returns without it
  and nn::os takes it itself, so every timed-out condition variable wait (TimedWaitEvent...) left the thread waiting
  on a mutex it owned - forever. Single player rarely times out (a thread or two hung unnoticed); the network
  threads do it constantly. MHGU_DUMP_THREADS dumps now show mutex owners and a heuristic list of callers.

## Event camera pans skippable with A (2026-10-21)

Entering a village / hub area when something is new plays an event (rScheduler, arc/ev/*.arc, path
event\village\event_vNNN...) that pans the camera past the NPCs; the original can't skip it. runtime/hle/evskip.c,
hooked at uScheduler's update (gen.py PATCHES 0xb66164): A sets a playing event scheduler's speed to 30 (uScheduler:
+0x34 frame, +0x3c speed, +0x40 paused, +0x41 loop, +0x54 rScheduler, path at +0x08), so it plays out in a moment
and what it does at the end still happens (log: "[evskip] skipping <path>"). Looping stage schedulers and paused
ones are left alone. Tested with a new character's arrival in Bherna (event_v002): A during the pan -> the chief's
greeting about a second later, the dialogue then advances normally. Other events (new facility / NPC pans, story
scenes with dialogue in the middle) not tested.

## Small monsters synced in multiplayer (2026-10-21)

The original game syncs small monsters' HP / deaths only; each player's game runs their AI, so players see them in
different places. runtime/net/emsync.c makes the first player in a zone their master (the game's own large-monster
mechanism: flag 8 sends netEmAction, the others replay it); ini [Multiplayer] SyncSmallMonsters (default On).
Tested with two instances: positions agree within centimetres, hand-over when the master leaves works. Combat,
3-4 players and two PCs not tested yet. Research, data layout and test details: docs/research/small-monster-sync.md.

## Text entry (2026-10-21)

Text entries use the inline keyboard (nn::swkbd::InlineKeyboard: the only one seen called; the game draws its own box
and shows the text it gets through the changed-string callback). runtime/hle/swkbd.c + runtime/textinput.c: while
it is shown, the physical keyboard types (SDL text input: any layout, accents; Backspace, Ctrl+V paste), Enter
confirms (decided-enter callback), Escape cancels; the keys don't act as controller buttons meanwhile, and the
controller can confirm with Plus or cancel with B. AppearArg +0x20 = max length (names: 10); SetInputText gives the
current text as UTF-16 inline (re-editing starts from it). Callback argument: UTF-8 text at +0, length +0x7d4, cursor
+0x7d8. Invalid UTF-8 handed to the game hangs it (seen: its decoder loops), so only valid UTF-8 goes there. Tested
on character creation: typing, Backspace over an accent, the 10-character limit, re-editing, Escape; the Palico
name entry (same calls) and chat not tested. The full-screen keyboard applet (ShowKeyboard) is not used by the game
as far as seen; it still answers "Hunter". MHGU_CONTROL "type <text>" / "textkey enter|escape|backspace" for tests.

User words (fixed 2026-10-24): the game hands the keyboard the character's earlier entries as suggestions
(InlineKeyboard::SetUserWordInfo; its keyboard manager, state machine f_a62570, state 8) and waits in state 5 for the
released-user-word-info callback before Appear, giving up after 0x38e frames (~15 s: a dimmed screen, no box, and
Edit Name dead until a restart). The callback was never called, so item set names (and any entry with user words)
failed for characters with entries typed on a Switch; characters made in the port have none and went straight to
Appear. Now Calc calls it right after Set/UnsetUserWordInfo. Red herrings on the way: the same dialog loads
nativeNX/arc/swkbd_eu.arc (not in the game's RomFS on a Switch either: "path not found", harmless), and the bug
looked Linux-only (it is per character: the Linux tester and the test save had Switch-made characters).

## Monster HP by the number of hunters (2026-10-21)

ini [Monster HP]: factors on large monsters' HP in Hub-type quests for 1-4 hunters (default 1 = original), plus
`Quest<number> = f1, f2, f3, f4` to set one quest apart. Only HP changes. A quest gives each large monster a row
of the difficulty table (resident.arc enemy\resident\em_nando_tbl, "NAN": u32 magic, version, row count, then rows
of 8 floats: HP, attack, defense...; cQuestData mVitalTblNo_N / mAttackTblNo_N / mOtherTblNo_N pick rows, and the
game moves the HP row a little per quest, e.g. 17 -> 16..19, with the size). Max HP = base HP (dtbase) x row[0], set in 0x2c04a0 /
0x2c04d8, both through the row getter 0x2baaac (monster, column 0 HP / 1 attack / 2 other), which is hooked
(runtime/hle/hpscale.c, fnhook): for column 0 it returns a copy of the row with only the HP float scaled. Village
quests are left alone by number (below 10000, Village Prowler 100000-109999); hunters = sQuestSelector's join count
(+0xd750). Multiplayer: each game computes HP itself, so the hub's host sends its settings (F_AUX, magic 0xE6) and
clients use them while in that hub. Data: sQuest *0x1a8ad90 (+0xa4 cQuestData, +8 quest number), sQuestSelector
*0x1a8ae38. Tested (two instances, Tackling the Tetsucabra, quest 10107, base HP 4000): solo x0.6 -> max HP 1392
(= 4000 x 0.58 x 0.6); two hunters, host 0.5 / client's own 3.0 -> both 1340 (= 4000 x 0.67 x 0.5). Village quest
exclusion and 3-4 hunters not tested in game. MHGU_LOG_HPSCALE=1 also logs the resulting max HP.

## Village quests in the Hub (2026-10-22)

ini [Village Quests] VillageQuestsInHub = On (default): at the Hub's or the Hunters' Pub's quest counter, L / R in
the rank menu or the star menu switch to the Village quests (the village counter's own menus: Training, Low 1-6,
High 7-10 stars, Urgent; what the save's village progress has unlocked) and back. runtime/hle/villagehub.c has the
mechanism: the counter's quests are sQuestSelector's list, built by a job from quest\quest_group through the filter
0x5b8090 with the area type (sGameControl +0x4c: 0 Hub, 1-6 villages - all Village quests are pooled -, 7 Pub) as
the counter type; categories and the counter UI's layout also go by the area type, so in Village mode those
functions run with it reading a village. A switch runs the list job again (0x5b7fd8 / 0x5b8070), builds the
categories anew (0x5ba8fc: they hold quest numbers, made from quest_group and the area type, not from the list) and
puts the UI back to its first state (+0x3c = 0). The counter UI sees a village only in its menus (states 0-2): with
the whole UI flipped, a Local Play hub's list came out empty (and the rows held the old categories' Hub quests).
Joining a posted quest looks its data up in the list (0x5ba50c): a quest from the other list is loaded on demand
(rQuestData loc\quest\questData\questData_<n>, resource manager *0x2326a64 vtable +0x30, mode 2) and appended -
before that the joiner had to switch to the same list ("You do not meet the requirements"). Quest numbers: group *
10000 + star * 100 + index (0 Village, 1 Hub, 2 Arena, 3 Village training, 4 Special Permit). HP: a Village quest
started from the Hub or Pub gets [Village Quests] HubHP (2.0) first, then the [Monster HP] factor for the number of
hunters (runtime/hle/hpscale.c; the host's values in a hub, sent with the others). Tested (2026-10-22, two
instances, the user driving): the switch both ways in the Pub and a Local Play Hub, the Village ★1 list in an online
hub, a village quest (101) posted by the host, joined by the client from the Quest Board, both departed together;
then (HR72 saves, both in the Pub) Vaulting Outlaw (206) posted from the Village list and joined by a client in Hub
mode (loaded on demand): Great Maccao max HP 860 in both games (row x0.43, x2.0 HubHP, x0.5 the host's 2-hunter
factor). Not checked yet: that clearing such a quest counts in the village (the clear is kept per quest number, so
it should), finishing a quest together, 3-4 hunters. Unlocks: a village's counter lists all Village quests of its
star ranges and only hides the locked ones in its menus (0x74af8c(quest, 0xff, 0) = unlocked in this save), and
the board's join only asks the list, so (a village never being online) nothing stopped joining a locked one. In
the Hub / Pub, locked Village quests (groups 0, 3, 10) are kept out of the list and out of the on-demand load: the
board then says "You don't meet the requirements to join this Quest" and shows it greyed. Tested (2026-10-23, host
HR72 hosting a Wycademy Hub, client a new HR1 save with only Village 1 star): Defiant Maccao (728, 7 stars) and A
Fungal Hunt (105, a 1-star quest the client hadn't unlocked) refused; Find the Ferns (101) joined. A Forge plugin
could do the same (same hooks), but the HP part is the port's own.

## Save data safety (2026-10-22)

Reported: saves in a folder symlinked to a NAS (Linux, Proton) were lost. The game saves by DeleteFile(system),
CreateFile(system, 0x4eb8bc), one 0x24 + one 0x4eb898 write, Commit, then the same for system_backup (MHGU_LOG_FILES=1
now logs every IFileSystem call). On the Switch nothing is real before Commit; the port deleted and created the
host files at once, and the SDK aborts the game on an unexpected fs result (nn::diag Abort, svcBreak: seen by making
the create fail). So any failed call between the delete and the commit (a share hiccup, a permission, a short write
that wasn't retried) killed the game with `system` already gone; at the next start the game finds no `system`,
makes a new empty save, and that also replaces system_backup. Fix (runtime/services/fs.c): save mounts are
transactional (new content in <name>.mhgu-tmp, deletes only marked; Commit renames to .mhgu-commit, then copies it
over the real file in place, so symlinked files stay symlinks; first mount finishes a .mhgu-commit and drops a
.mhgu-tmp; an unmount without commit drops the changes), reads and writes loop until complete, and failures are
logged. Tested: new save flow (4 commits, save loads again), leftover .mhgu-commit applied / .mhgu-tmp removed, a
create made to fail after the delete (game aborts as on the Switch, `system` keeps its committed content). Not
tested: a real NAS, Linux, Proton.

## Frame rates above 60 (2026-10-21)

ini [Performance] FPS = 30..240 (default 60), VSync = On / Off (default On). The engine's own limiter (main
0xda5d00) sleeps to the target in its system object (+0x3c; the 90 / 120 fps cheat codes, `[main+0x18A6210]+0x243C`,
are this same float) and gives game logic the measured frame time as a step in 60 fps frames (+0x68: 1.0 at 60, 2.0
at 30, ~0.42 at 144; capped at +0x70 = 16). There is no fixed timestep: one game update per presented frame.
The old "cap at 60 somewhere in the engine" was the presentation: the R9's driver has no mailbox mode, so VSync
falls back to FIFO and waits for the 60 Hz monitor. With VSync = Off (immediate present, tearing possible) the
village at 640x360 runs ~138 fps at a 144 target on the R9 (CPU-bound there). Walking speed checked at 30, 60 and
~133 fps (player position 2 s apart: ~355 units each time): normal game speed.

Theory, not tested: Pierce ammo hitting more often at high frame rates. Anything the game counts per update rather
than by the step behaves differently above 60 (and at 30 the other way). Two likely ways for a piercing shot: (1) a
per-target hit cooldown counted in updates: at 144 fps it would run out 2.4x sooner in real time, so the shot can
hit the same hit zones again as it passes through; (2) collision is tested once per update at the shot's new
position: smaller steps sample its path more finely, so fewer hits are skipped on thin parts. Both mean more hits per
shot, but only up to the ammo's hit count, so damage would rise towards the most a shot can do rather than without
bound. Other per-update counters (multi-hit attacks, status ticks) could act the same way. To check later: a save
with a bowgun and Pierce ammo, the same shots into a still monster at 60 and at 144 fps, and count hits (a hook on
the damage function, or the damage numbers); the shell data (shell\pl\pl_w<weapon>_<n>\..._hit<n> files,
rBowgunShellData) would show which field holds the interval. A fix would scale that counter by the step.

Monster AI and the frame rate (2026-10-25): measured, no difference. f_276154(monster, group, index, flag) starts an
enemy's new action (stores it at monster +0x73e0, sends it to the other players) for small and large monsters alike;
testsave/ai_rate.sh counts its calls per monster with MHGU_FNTRACE over 120 s of the hunter standing still. Quest 201
zone 1 (three Larinoth): 52 / 52 / 51 actions at 30 / 60 / 120 fps, 17 / 18 / 17 per monster each time (the AI's
timers run on the step, not per update). Quest 305 (two Great Maccao plus small monsters, all over the map): 340 and
311 at 30 fps, 270 and 313 at 120 fps (the Maccao 70-87 each in every run): noise, nothing rising with the frame
rate. Not measured: what happens inside an action (turning toward a target, collision, hit cooldowns per update).

## Fidelity audit (2026-10-20)

Silent-bug sweep after the shimmer and shadow fixes (the two classes found: a driver needing explicit barriers, and a
translator ignoring condition codes). Diagnostics now built in: MHGU_NVN_NOOP_LOG=1 logs the first call of every NVN
command the port ignores; the translator warns when a branch tests zero / sign flags last written by a .CC
instruction whose flags it doesn't model ("[shader] ... condition-code test"); MHGU_VK_STATS=1 counts skipped draws.
Village run + the user's hunt logs: the game calls only nvnCommandBufferAddControlMemory and SetShaderScratchMemory
(both harmless); no CC warnings, translation failures or unsupported formats. Audio effects are now complete for
MHGU: I3DL2 reverb (type 5; mix 1 chains three mono I3DL2 reverbs with buffer mixers, the environmental reverb) is a
port of Ryujinx's Reverb3d, with yuzu's max + 1 delay-line ring (Ryujinx's re-reads a stale sample every 400 ms); Aux
(type 2, 6 channels on the final mix) sends each frame into the game's guest ring and plays its return, as Ryujinx's
AuxiliaryBuffer: the game keeps up in lockstep (MHGU_AUDIO_LOG=1 logs the ring offsets; MHGU_NO_AUX=1 bypasses it).
Splitter updates skipped the 12 unused bytes after each splitter's destination ids, so everything after the first
splitter in an update was dropped: stereo sounds made of two mono voices (menu confirm, load fanfare) played left only.
docs/SWITCH_COMPARE.md lists what to check against the Switch version by ear.
Draws skipped while their shaders / pipeline build (the main remaining cause of transient glitches: a flashing status
bar, missing lens-flare pieces on entering an area). MHGU_VK_STATS=1 now also logs a "2s builds" line (skips by
cause and in how many frames, the waits, build times per stage, request -> ready latency, the slowest frame). Entering
the village needed 112 new programs and ~140 pipelines at once: translation (one thread, the translator isn't
re-entrant, ~2.3 ms each) + GLSL -> SPIR-V / modules (~3.6 ms, three workers) dominate, pipelines take ~0.2 ms (Vulkan
pipeline cache), so ~1100-1230 draws were skipped over 13 frames. Now (exec.c):
- a draw whose program / pipeline is still building waits for it, up to MHGU_PIPE_WAIT_MS (default 6) per frame in
  all, and its job moves to the front of its queue;
- cache/programs.bin keeps every translated program (GLSL + reflection, keyed by a hash of the guest code the
  translator reads + the shader keys; tied to the executable's size / time stamp, so a rebuild starts it afresh), and
  cache/pipelines.bin each pipeline's state (program by content key, render pass by formats). The next run builds them
  all on the workers at its first draw (~130 programs + ~170 pipelines: well under a second, during the logos), like
  Ryujinx's shader cache. Both are appended as they grow, so a killed run keeps them. MHGU_NO_PROGRAM_CACHE=1: off.
Village load on the R9 (warm caches): 0 skipped draws on entering (was 1064 / 1230), 6 in a whole 60 s run (the first
logo frames), slowest frame 56-62 ms (was 83, the load hitch itself), steady 57 fps unchanged. First run ever (empty
caches): 761 skipped over 9 frames (was 1230 over 13): translation throughput bound, waits cap at 6 ms a frame.
Known gaps that remain:
- A program never seen before (first run, or after a rebuild) can still be missing for a few frames when many arrive
  at once.
- R9 200 approximations: no custom border colours (nearest standard colour), D24S8 stored as D32F_S8 (polygon-offset
  units differ for decals).

## Next steps
1. Modding tools: decode resident.arc tables (weapon00..14 Base/Level data: record layouts derivable from the game
   code that reads them), monster files per `arc/enemy/emXXX_YY.arc` (`attackdata`, `bodydata`, `dtbase`,
   `dttune`), JSON <-> binary converters, and per-member ARC overrides in the virtual RomFS
   (`mods/<name>/romfs/<arc path>.arc.d/<member>`).
2. Play further (hunts work: Bherna Gal -> quest -> gate left of the balloon) and fix what breaks; shader global
   memory if some effect needs it.
3. A README with build instructions; Linux platform layer (runtime/platform/linux.c). It must also give
   plat_user_dir a home for save data: $XDG_DATA_HOME/MHGURecomp, else ~/.local/share/MHGURecomp (see "Save data").
4. Internet hubs without port forwarding (friends by code, public list) through our own relay / lobby server, and a
   security audit of the network code before strangers can join: plan in docs/research/internet-hubs.md.
5. Android build (arm64, controller only, no touch controls): plan in docs/research/android.md.
6. Configurable draw distance (ini). In Bherna Village NPCs vanish a few steps away, as on the Switch; in hunts
   monsters show across the zone (they must), so start with the towns. Find what culls them: an NPC / object
   distance or LOD constant (code, or a table in resident.arc / the stage's arcs) read per frame - fnnew around
   walking away from an NPC, then the compare against the distance. Also check gathering spots and other map
   objects (do they pop in late?). Watch the cost: more NPCs drawn, and the game may cap how many it updates.
7. A settings launcher for players who don't edit ini files: every [section] / key of runtime/config.c (it already
   knows the names, defaults, ranges and comments) in a window, saved to mhgurecomp.ini. Cheapest: inside the exe
   with the ImGui the port already ships (a `--settings` switch, or a "Settings" shortcut), so no second program or
   toolkit; the same panel can serve item 8 and Android's controller-only settings (docs/research/android.md).
8. Look into the port's options in the game's own Options menu. The menu is the game's GUI (.gui layouts, gmd text,
   per-item code), so adding entries means extending its item tables, text and handlers: find how the Options list
   is built (fnnew while opening it, the gmd its labels come from) and judge the effort. Fallback: the port's own
   overlay panel (item 7) opened in game with a key / controller combination; settings that need a restart
   (resolution, MSAA) say so.
9. Other aspect ratios. Ultrawide is in (PR #6 by dmitsuki, 2026-10-25, then fixed up): [Display] AspectRatio
   (Auto: the window's), FOV, UIAnchor (Center / Edges / 21:9 / 32:9). Wider than 16:9: the 3D cameras made for the
   screen (aspect 16:9 in sCamera / uCamera +0x38) get the picture's shape (Hor+, gen.py PATCHES at the three
   callers of the perspective matrix 0x9c617c that load it; the other 9 callers are untouched), the game's 2D
   canvas gets the HUD's width (0xdbcd48 / 0xdbcd54), and exec.c (cbuf_info, vertex stage cbuf 3 of 2D draws)
   unstretches the HUD and places its parts left / right / center by their layer depth (94 center banners, 96
   dialogue box left, 97 menus by x, 100 / 105 right, 103 status left / item bar right). From 16:10 to 16:9
   (2026-10-25): the camera keeps 16:9's width and gets more height (Vert+: config.c camera_fovy, from the
   perspective matrix's fovy argument, radians: fovy' = 2 atan(tan(fovy / 2) * (16/9) / aspect)), and exec.c
   flattens the HUD to its 16:9 shape (sy) and moves each layer to the top (status, minimap, menus), the bottom
   (dialogue, item bar, Hunter Arts prompts) or the middle (banners), one anchor per layer so a window drawn in
   pieces stays together. Narrower than 16:10 (4:3) gets 16:10 with bars. Tested 1280x720, 1280x800, 1024x768
   (bars), 1680x720 Center, 1792x504 / 1680x840 Edges: hunt HUD, pause menu, help box, equipment windows,
   nameplates. Left: the HUD's layout elsewhere (title, town, quest board, item box, cutscenes) unverified; at Edges
   the Hunter Arts prompt goes right with its layer; whether an off-screen 3D view (equipment box preview, guild
   card) uses one of the three cameras (they are left alone unless 16:9).
10. Keyboard remapping: an ini [Keyboard] section, one line per Switch button (`A = Mouse2`, `B = Space`; SDL key
   names, several keys per button, mouse buttons too), plus the left stick's four directions and the walk key. Today
   the layout is fixed: key_buttons in runtime/host_input.c, WASD / Shift handled beside it. The current layout stays
   the default, so existing ini files keep working; the settings launcher (item 7) can show it later.
## Build / run

MSYS2 UCRT64 on PATH (`C:\msys64\ucrt64\bin;C:\msys64\usr\bin`):

    make gen GAME=../mhgu/game     # recompile (needs tools/venv: capstone, unicorn, lz4)
    make -j4                       # ~8 min full, incremental (changed chunks only) is fast
    build/mhgurecomp.exe           # or double-click it; run-game.bat / run-debug.bat start it too

`make` also makes build/ the game folder: the MSYS2 DLLs the executable needs beside it (tools/bundle_dlls.py
follows the import tables; ~100 DLLs, most of them FFmpeg's; never vulkan-1.dll: the driver's loader must be used)
and the game files in build/data/exefs + build/data/romfs (target `data`, once: hard links to GAME's files, so no
second copy on disk; plain copies where hard links don't work). Started without a game folder argument, the
executable works in its own folder (runtime/main.c, platform plat_standalone): data/ for the game, cache/, mods/,
shots/, mhgurecomp.ini and log.txt beside it (save data elsewhere, see below); with a console of its own (Explorer, a shortcut) it drops the
console and logs to log.txt, and a fatal error shows a message box. `make clean` keeps all that (it removes the
compiled files only). With a game folder argument (or MHGU_GAME_DIR) it uses the current folder as before; then the
DLLs must be on PATH (C:\msys64\ucrt64\bin) unless it is the bundled build/mhgurecomp.exe.

Save data: the standalone executable keeps saves in the player's own folder, %APPDATA%\MHGURecomp\save\ (save_0/
system + system_backup, sdcard/), so deleting or rebuilding build/ can't take them along (platform plat_user_dir;
the path is in the log: "[fs] mounted host directory"). With a game folder argument (the test scripts in run/) saves
stay in save/ under the current folder, so automated runs never touch the real save. MHGU_SAVE_DIR overrides both.
Linux (not written yet): plat_user_dir should return $XDG_DATA_HOME/MHGURecomp, else ~/.local/share/MHGURecomp.
First start without a save (checked 2026-10-21 with an empty save_0): the game shows "There is no data to load." and
then the language / bonus notices; these boxes have no button prompt and only A (right click; keyboard L before 2026-10-23) closes them, B and Plus
do nothing, so it looks frozen. After that: title, New Game; a fresh system + system_backup is written by then.

Mods (runtime/mods.c, mods.h; tested 2026-10-21 with GU Rebalanced): drop a mod's folder into mods\ in the game folder (the
standalone build creates the empty folder), remove the folder to uninstall; nothing on disk is changed, all is layered
at start. romfs/ is found up to three levels down (Mod/Mod/romfs, atmosphere/contents/<title id>/romfs) or as a bare
nativeNX/ folder; names match in any case; new files are added (GU Rebalanced adds weapon/w11/163.arc). exefs/:
IPSwitch .pchtxt and IPS / IPS32 (<build id>.ips) patches, matched to main / sdk by build id; offsets are the module's
memory offsets (pchtxt with offset_shift 0x100, as all of theboy181's: 0xBBCD34 = runtime 0xdbcd34). Only data patches
(rodata / data) are applied; code patches can't be (the code is compiled ahead and literal pools are folded in), the
log lists them: "[mods] <mod>: <file> for main: ... code patch(es) NOT applied (at ...)". The three common MHGU exefs
mods are covered by the port itself: 4K-2K = ini resolution (gen.py PATCHES at the same two instructions), 60fps = ini
frame rate, Connecting-Error-Fix = nifm HLE'd as no internet. A code mod someone needs can become a gen.py PATCHES
entry (or a build option that reads .pchtxt at `make gen`). Order: folder names, or mods\load_order.txt; '_' prefix
skips a folder. Log lines: "[mods] <mod>: romfs <path>", "[romfs] mod <path>: <n> files".

Forge plugins (runtime/forge.c, interp.c; 2026-10-21): Forge (github.com/Fexty12573/forge) is a loader for code mods:
its own module (exefs subsdk9) loads .nro plugins from the RomFS folder nativeNX/plugins/ and gives them hooks,
patches, singletons, save data, log and input. The port doesn't use Forge's module (it patches the console's code);
it finds the plugins itself (game RomFS + every mod's romfs, any case), loads them at 0x03200000 (relocated against
main / sdk, imports of forge_* bound to host code) and runs their code in an AArch32 interpreter (runtime/interp.c:
the lifter's semantics instruction for instruction, same Cpu state; tools/test_lift.py --interp checks it against
Unicorn like the lifter: 0 mismatches over 20k sampled game / SDK instructions incl. NEON + VFP). Calls between the
plugin and the recompiled game go through rt_dispatch both ways. Hooks: every generated function's FN_ENTER (cpu.h)
tests a bit in g_fnhook (one bit per code word, dispatch.c fnhook_set) and runs the detour in the function's place;
the "original" is a trampoline in guest memory with Forge's layout (two original words + `ldr pc` to target+8,
plugins verify it) that dispatch.c runs as the unhooked function. forge_hook_create also writes Forge's `ldr pc,
[pc,#-4]; detour` over the entry (plugins verify that too); patches over code are re-read for that form (or a `b`),
anything else in code is logged as not applicable. Forge's own points: sApp::run (main+0xB8692C: resolve singletons,
forge_onLoad / version check / forge_onInit), the frame update (main+0x3DA8FC: forge_onUpdate(dt)), cSystem ctor /
dtor (singletons). Not provided: forge_socket_* (an "UNIMPLEMENTED" log line, 0). MHGU_FORGE_LOG=debug|info|warn|error
(default info). Install = the mod's folder in mods\ (Forge itself is not needed). Log: "[forge] plugin ...",
"[forge] starting ...", the plugin's own messages as "[forge] [info] ...". Plugins are built -mfloat-abi=softfp, as
Forge is: floats in core registers (forge_onUpdate's dt in r0; until 2026-10-23 it went in s0 only).

Forge's ImGui overlay (runtime/overlay.cpp, forge_imgui.c, tools/imgui_thunks.py; 2026-10-23): Forge's "forge"
window (About, Loaded Plugins, Plugin UI = each plugin's forge_onImGuiRender) and forge_onImGuiFreeRender, on Dear
ImGui 1.92.8 + cimgui (third_party/cimgui, Forge's versions; ImGui's Vulkan backend). The menu starts closed; Insert
opens and closes it (or Forge's config.ini [menu] key / font_size from a mod's romfs nativeNX/forge/, with its Roboto
font from there too; else ImGui's own font). Sizes follow the window: x height/720. The frame is built on the game's
thread after forge_onUpdate (plugin code runs there); texture uploads there too, under vk_lock; the render thread
draws a copy of the draw lists at the end of the window image's pass (device.c). Input: while open, keyboard / mouse
/ text go to ImGui (host_input.c asks overlay_event first; F-keys and Alt+Enter still work), the controller navigates
(A activates, B cancels) and the game gets no input. Plugins call cimgui's C API (ig* / Im*, which Forge exports):
tools/imgui_thunks.py generates runtime/forge_imgui_thunks.inc from cimgui's generator output, 1440 of 1476 functions
(the rest, listed at its end: images / textures, draw callbacks, FILE* and size_t* helpers): softfp arguments
converted, guest data pointers as their host address, printf-style ones formatted from the guest's arguments.
ImGui's objects (ImGuiIO, ImGuiStyle, ImDrawList...) reach plugins as mirrors in guest memory with the 32-bit
layout (computed per struct, checked against the compiler's with static asserts), synced both ways around each
call; internal structs are opaque. igNewFrame / igRender / igGetDrawData from a plugin are ignored; igMemAlloc is
the guest's malloc. Tested with Key Quest Markers r18 (its only ImGui is its colour settings): the menu over the
title and in a hunt, mouse (headers, tree node, buttons), typing into its hex colour fields, Save & apply (wrote
key_quest_markers.ini to the save folder), controller navigation; the game unaffected with the menu closed. Callback
trampolines (InputText callbacks, size constraints, list getters), mirrors of structs a plugin owns, and several
ImGui plugins at once are written but not exercised by that plugin. MHGU_CONTROL for tests: `sdlkey [ctrl+]<key>`,
`sdltext <text>`, `mouse <x> <y> [click]`; `mousemove <x> <y> <ms>` moves the camera as mouse look does.

Cheats (runtime/cheats.c, 2026-10-24): Atmosphere-format cheat codes (as Ryujinx and a modded Switch read them) from
every .txt in cheats\ beside mods\ (MHGU_CHEATS_DIR; '_' prefix skips a file), all on; "[Name]" starts a cheat, "{Name}"
a master code (run first), '#' / "//" lines are comments (Atmosphere itself rejects those). The VM is written from
Atmosphere's docs/features/cheats.md (every code type; Atmosphere is GPL, its dmnt code is not used): 16 registers and
16 saved, both 0 at the start of every cheat (2026-10-25: before, they carried over from the cheat before it in a frame,
so a cheat leaving r0 set shifted the next one's 0TMR writes: a player's log had the positive-only skill cheat 0x14B0
off, its jump into read-only data: FATAL no function), 128 static (shared); blocks / elses / loops matched when a cheat
loads (a cheat that does not decode is logged and left out); key codes test the buttons the game sees (input_get; stick
directions at half tilt); pause / resume (FF0 / FF1) are ignored, debug log (FFF) goes to the log. Runs once in boot()
before the main thread starts, then on the window loop once per presented frame (as dmnt runs beside the game). Regions:
main 0x200000, heap HEAP_BASE, alias ALIAS_BASE, aslr 0x200000 (GetInfo's), 4 = absolute; kernel_mapped() checks every
access (unmapped: reads 0, writes dropped). A write into main / sdk code that changes it: dispatch_interpret() makes the
function holding the word (dispatch_function_at: nearest entry below, up to the next) run in the interpreter from then
on: its range becomes interpreted and its entry gets a hook to an `ldr pc, [pc, #-4]; .word entry` stub in interpreted
memory (an existing hook stays; fnhook_call_original then interprets). Words in the padding after .text
(main+0x13F1DEC..0x13F2000, zeros) are made interpreted ranges themselves: room for a cheat's new code. Limits: a
function the lifter split (a second entry inside it) has its other part still recompiled; a call already running
finishes the old code; an interpreted function is many times slower (fine for f_2df824, called on equipment changes).
interp.c's ranges went 16 -> 256 and merge when they touch. A function a Forge plugin hooks and a cheat changes
(2026-10-25): Forge's hook writes `ldr pc, [pc, #-4]; detour` over the entry in guest memory, so interpreting it jumped
back into the detour, whose original interpreted it again: a stack overflow. hook_install (and a plugin's code patch
that makes a hook) now keeps the two words it replaced (fnhook_keep_entry) and the interpreter runs those at a hooked
entry (code_word: only where g_fnhook has a bit). Tested both orders with MHGU_FORGE_TESTHOOK=2df824 (a plugin-style
hook at start-up, before the cheats) and the control file's `testhook 2df824` (after them, as plugins hook): the detour
runs, the skill table is the cheat's; without the fix, exception c00000fd. Tested: the skill-point cheats of
docs/research/skill-points.md on the test save in a quest (exact), a VM test cheat (loops, registers, master first, C4
once per press, a bad cheat refused), the Linux build. Log: "[cheats] <file>: <n> cheat(s): ...", "[cheats] <cheat>:
changes the code at main+<x>: the function at main+<y> runs in the interpreter now".

Skill points (docs/research/skill-points.md; extras/cheats): f_2df824 totals them, rows at [obj+0x188] (12 bytes:
+4 activation, +6 tree, +8..+14 points per slot weapon/head/body/arms/waist/legs/talisman, +15 total, s8). "Skill
points x2": 0x2dfab4 `uxtbne r0, r0` -> `lsl r0, r0, #1`, plus Double Talisman's two passes adding `r0, lsl #1`
(talisman x4 with it). "positive only": 0x2dfab4 -> `b` to main+0x13F1F00 (`cmp r0, #0; addgt r0, r0, r0; b back`)
and both Double Talisman passes' `ldrsb` -> the same through two more stubs there. Test save hunter (Double Talisman
and Skill +2 active): base tree 35 (Handicraft) = 1,2,2 + talisman 8 (+2) = 15; positive only 2,4,4,16 (+2) =
28, its negatives unchanged (-5 +2 = -3); full x2 the same positives, negatives -10 +2 = -8.

Bonus content (runtime/services/bonus.c, 2026-10-21): the free downloads (event / challenge quests, item packs,
Palicos) live in the shared part of the save file, not in the game files, and Capcom's server (meteor.capcom.co.jp)
is out of reach. So they are copied from a save that has them: put that save's `system` file in bonus\ in the game folder (next
to the exe, like mods\; the current folder with a game folder argument), else %APPDATA%\MHGURecomp\bonus\ is read;
MHGU_BONUS_DIR overrides. Only with ini [Bonus Content] BonusContent = On (default On again since 2026-10-23, was Off
from 2026-10-22; with Off saves are never touched; tested both ways on an empty save). When the game opens a save whose download regions are empty (0xb2a5-0xb310 unlock flags, 0xc000 to the first character slot: item packs,
Palicos, 160 + 40 quest slots of [id][size][ARC]), the donor's are written in; a save with downloads of its own is
left alone. A brand-new save gets them from the next start (the game writes it without reading it back). Tested with
the user's save (125 event + 40 challenge quests): Low and G Rank Event lists filled (URGENT: COAL MINERS NEEDED in
G Rank), Chamberlyne > Downloadable Content > Claim Items lists the packs and claiming works (Pirate's Booty Pack:
Slickaxe x20). Palico downloads not checked. The quest ARCs equal the RomFS files byte for byte, so a donor-free
version (building the regions from the RomFS) is possible once the flag bits and the pack / Palico records are known.

Controls: see `runtime/host_input.c` (keyboard and mouse: WASD, mouse look (relative mode after a click, Esc releases; the mouse speed smoothed
over 25 ms as the right stick tilt, offset past the camera's round ~39% stick dead zone; ini [Mouse] MouseSensitivity), clicks = X / A, Space / R = B / Y, Tab / T = ZL / ZR,
arrows = d-pad, Ctrl / Alt = stick clicks, Enter = Plus; SDL gamepads, face
buttons by position (Switch layout) or label: ini FaceButtons; the controller also works with the window in the
background, ini BackgroundInput = On (default; SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, set before SDL's gamepad
subsystem starts, runtime/ui.c; MHGU_BACKGROUND_INPUT=0|1), keyboard / mouse only with the focus;
F12 screenshot, F11 8-frame burst -> shots\ in the game folder; Alt+Enter windowed / full screen; F7 cycles MSAA). Display mode,
resolution, anti-aliasing and frame rate: mhgurecomp.ini (delete it to get the defaults back).
Game folder: where the settings, caches, mods\, bonus\, shots\ and log.txt are: the exe's folder when started on its
own (build\ itself holds only what the build makes; compiler files go to obj\). MHGU_HOME=<folder> moves them there
(run-game.bat: run\, test scripts: testsave\home); the game files are still read from data\ beside the exe.

Debug env: `MHGU_TRACE_SVC`, `MHGU_TRACE_IPC`, `MHGU_LOG_FILES`, `MHGU_FNPROF` (hot functions every 5 s),
`MHGU_FNTRACE=lo-hi[:n]` (calls into a guest address range, with caller and thread), `MHGU_DUMP_THREADS=<s>`
(every guest thread's wait state after s seconds), `MHGU_HLE_SKIP`, `MHGU_AUDIO_LOG`, `MHGU_MOVIE_TRACE` (calls
through the movie vtables), `MHGU_HIDDEN=1` (no visible window), `MHGU_REGION=<n>` (GetRegionCode; default 2 Europe), `MHGU_NO_WRITE_WATCH=1` (Windows: no page write tracking, textures hashed as on Linux), `MHGU_INPUT="frame:BUTTON:frames,..."` (scripted
controller, frames = presented frames; e.g. `300:A:10`), `MHGU_NAME` (answers every text entry with that text at once),
`MHGU_CONTROL=ctl.txt` (drive a running game: append lines `A`, `DOWN 8`, `STICK_UP 60`, `shot name` -> shots/name.png;
`X 6 25` = X for 6 frames from 25 frames on, lines written together form a timed sequence; `fntrace lo hi n`;
`fnnew lo hi [learn ms]` = log only the first call of each function in [lo, hi) not seen while learning, to find what an
event runs; `peek` / `poke` / `memsave` / `find` for guest memory), `MHGU_QUICKHUNT=<quest number>` (from start-up
straight into that quest: A through the menus to the save's town, then the quest is ordered and departed on;
runtime/hle/quickhunt.c has how the game departs), `MHGU_LOG_MOVES=1` (the player's actions and command table
changes, runtime/hle/movelog.c),
`MHGU_SHOT_AT=<frames>`, `MHGU_FPS=1`, `MHGU_VK_STATS=1`, `MHGU_VK_VALIDATION=1`, `MHGU_SYNC_SHADERS=1`, `MHGU_PIPE_WAIT_MS=n`,
`MHGU_NO_PROGRAM_CACHE=1`, `MHGU_SAVE_GLSL=1`.
`python tools/disasm.py <game dir> <addr | module:symbol> [count] [before]` disassembles guest code.
