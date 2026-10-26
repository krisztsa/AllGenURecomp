# Valor Sheathe damage reduction (2026-10-24)

Goal: undo one change of MHGU Rebalanced (GU Rebalanced) without editing the mod: the damage taken in a Valor
Sheathe evade (Valor style, hold sheathe, the next hit is auto-evaded with reduced damage). Rebalanced lowers the
reduction from 70% to 50% (Blademaster) and from 85% to 65% (Gunner). Done as a cheat:
`extras/cheats/Valor Sheathe original damage reduction.txt`.

## Where the values are

- `player\param\pl_com` (type 0x496F8F22, a "FUP" parameter file) in `nativeNX/arc/player/quest/com.arc`, the only
  archive that has it. Rebalanced replaces the whole archive (also `player\alchemyData`), so another mod replacing
  it would have to carry a copy of all of it.
- Damage multipliers (the share of the damage that still gets through), file offsets:
  +0x224 Blademaster 0.30 (Rebalanced 0.50), +0x250 Gunner 0.15 (Rebalanced 0.35). Rebalanced's other pl_com
  changes: +0x44/+0x48/+0x4c (ints 2/5/12 -> 3/8/16), +0x1e8/+0x1ec (240/480 -> 1.0), +0x248 (26 -> 12).
- Loaded, the file's 122 floats (from file +0x6c, 0x1e8 bytes) are a heap block of their own: Blademaster at
  block +0x1b8, Gunner at +0x1e4 (the last float). (main holds a copy of the original defaults at 0xf04e8c; not used
  for this.)

## The pointer chain (found in a quest, Find the Ferns, with GU Rebalanced)

    sPlayer *            = [0x1a85b58]          (Atmosphere: main+0x1885B58)
    player (slot 0)      = [sPlayer + 0x1c]
    pl_com resource      = [player + 0x2638]     (+0x8 its path "player\param\pl_com", +0x5c type 0x496F8F22)
    float block          = [resource + 0x74]     (+0x70 the int block, +0x78 a third block)

The resource is one shared object (every player slot points at the same one), so slot 0 covers everyone's damage on
that game. The cheat checks each pointer for 0 and the resource's type before writing, and writes every frame (the
archive is loaded again for every quest). Checked in the port: with Rebalanced and the cheat, the block holds
0.3 / 0.15 and Rebalanced's +0x248 change is still there. The damage itself was not measured in a hunt.

## Other values

The last two store lines of the cheat write the multipliers as float bits (`64000000 00000000 <float>`), first
Blademaster, then Gunner. Damage reduction -> value:

| Reduction | Multiplier | Float bits |
|---|---|---|
| 90% | 0.10 | 3DCCCCCD |
| 85% | 0.15 | 3E19999A |
| 80% | 0.20 | 3E4CCCCD |
| 75% | 0.25 | 3E800000 |
| 70% | 0.30 | 3E99999A |
| 65% | 0.35 | 3EB33333 |
| 60% | 0.40 | 3ECCCCCD |
| 50% | 0.50 | 3F000000 |
