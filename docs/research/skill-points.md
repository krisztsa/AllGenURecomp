# Armor skill points: where the game totals them (2026-10-24)

Goal: double the skill points from armor, talismans and decorations (all equipment), without touching the data
tables (MHGU Rebalanced replaces `loc/arc/resident.arc`, which holds the skill tables). Done as cheats (extras/cheats,
runtime/cheats.c). Addresses are runtime addresses (image offset + 0x200000); Atmosphere cheats use the image offset
(runtime - 0x200000: the wiki's "Always Lucky" writes `mov r0, #1` at 0x3822DC over `bl hasSkill(0xfb)` at runtime
0x5822dc). Main's build id: `fb08f1d20fd1204fe7070672777f869b`.

## The calculation

- `f_2dee34` sets up an equipment-skill object and tail-calls `f_2df824`, which computes the skills. Callers of
  `f_2dee34`: 0x46f078, 0x46fc38, 0x46fcc0, 0x46fd34 (player), 0x7f1abc, 0x7f2130, 0x8ff17c, 0x8ff20c, 0x92cf84,
  0x92cfe8, 0x93ff14, 0x93ff94, 0x95cec8, 0x95cf58 (equipment box / menus, by the look of it; not checked).
- The object (`sb` in `f_2df824`) has 7 equipment slots of 0x2c bytes from +0x14: 0 weapon, 1 head, 2 body, 3 arms,
  4 waist, 5 legs, 6 talisman (body is the slot Torso Up multiplies, the talisman the one Double Talisman adds again).
  `f_2ddd48(slot)` rebuilds a slot's entries from the equipment (called for all 7 at the start of `f_2df824`, and from
  many other places); `f_2de150(slot, k)` returns entry k of 11 (`slot+0x28` + k*4: u16 skill tree, s8 points). The
  11 entries are the piece's own skills and its decorations' skills, one entry per source.
- Results at `[sb+0x188]`: 77 rows of 12 bytes. +4 u16 activated skill (skillData id), +6 u16 skill tree
  (skillTypeData id), +8..+0xe s8 points per slot, +0xf s8 total (the bytes past +0xb are the next row's unused
  first word). **Byte arithmetic**: a total over 127 wraps.
- Pass 1 (0x2dfa2c-0x2dfb9c): for each slot and entry, `ldrsb r0, [entry+2]` at 0x2dfaa8; for the body slot
  `mla r0, r0, r8, r0` (r8 = number of Torso Up entries, tree 0xcb = 203), else `uxtb r0, r0` at 0x2dfab4; then added
  with `sxtab` to the slot column and the total. Torso Up entries themselves are counted, not summed.
- Thresholds (0x2dfbd4...): total <= -20/-15/-10 or >= 10/15/20/25 picks the activation from the skillTypeData row
  (`f_6cf018`, +0xa..+0x16). Activation 0x146 (326) is Skill +2, 0x147 (327) is Double Talisman.
- Double Talisman (flag `sp+0x81e`): the talisman slot's points are added once more (0x2dfccc-0x2dfd48, and again in
  0x2e01e0-0x2e025c when Skill +2 is also active: `add r2, r2, r0` / `add r0, r2, r0` at 0x2dfd38/0x2dfd44 and
  0x2e024c/0x2e0258), then the thresholds are evaluated again. Skill +2 (flag `sp+0x81f`, 0x2dfda0...) adds 2 to the
  total of every tree with points.
- Other readers of the entries: 0x78062c, 0x780848, 0x82a854, 0x82a8c4 (probably per-piece displays; not checked).

## Doubling everything

One instruction: 0x2dfab4 `uxtbne r0, r0` -> `lsl r0, r0, #1` (E1A00080). Every entry counts twice, the body after
Torso Up's multiplier (`sxtab` only uses the low byte, so the dropped `uxtb` changes nothing). Double Talisman then
adds the talisman's plain points once more (x3); patching its adds to `add r2, r2, r0, lsl #1` (E0822080) and
`add r0, r2, r0, lsl #1` (E0820080) at all four places makes it x4. As an Atmosphere cheat:

    [Equipment skill points x2]
    04000000 000DFAB4 E1A00080

    [Double Talisman doubles again (with the above: talisman x4)]
    04000000 000DFD38 E0822080
    04000000 000DFD44 E0820080
    04000000 000E024C E0822080
    04000000 000E0258 E0820080

Shipped as extras/cheats/Skill points x2.txt (both parts in one cheat). The port runs it since cheat support
(runtime/cheats.c: the changed function f_2df824 runs in the interpreter).

## Doubling only positive points

No room in place (0x2dfaa8-0x2dfab8 is `ldrsb; cmp r4, #2; mlaeq; uxtbne; b`, and doubling only positives needs two
more instructions), so the new code goes into the zero padding after main's .text (main+0x13F1DEC..0x13F2000,
executable on the console too):

    main+0x13F1F00: cmp r0, #0; addgt r0, r0, r0; b 0x2dfaf8      (0x2dfab4 -> b main+0x13F1F00)
    main+0x13F1F10: ldrsb r0, [r0, #2]; cmp r0, #0; addgt r0, r0, r0; b 0x2dfcd0   (0x2dfccc -> b there)
    main+0x13F1F20: the same, back to 0x2e01e4                    (0x2e01e0 -> b there)

The last two are Double Talisman's passes, so its extra talisman points are doubled the same way (positive talisman
points x4, negative x2 as in the game). Plain `b` both ways: no register changes (lr is free in f_2df824 anyway: it
is set before every use). Shipped as extras/cheats/Skill points x2 (positive only).txt.

Tested on the test save's hunter (Double Talisman and Skill +2 active), table read with `peek` after quick-hunting
(skilltest.sh): tree 35 base 1,2,2 + talisman 8 (+2) = 15; positive only 2,4,4,16 (+2) = 28 (tree 70 went up a
tier, 142 -> 140), its negatives unchanged (tree 25: -1,-1,-1,-2 (+2) = -3); full x2 the same positives and tree 25
-2,-2,-2,-4 (+2) = -8.

Open: the s8 total can wrap for extreme builds (Torso Up stacks x5 on the body, doubled: x10); whether other hunters'
games compute your skills (multiplayer); whether any screen totals skills elsewhere.
