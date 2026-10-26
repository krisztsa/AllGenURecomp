# Small monster sync in multiplayer (2026-10-20/21)

Small monsters ("zako") are synced between the players in a zone like large monsters are: positions, movement and
actions. The original game syncs only their HP / deaths; each player's game runs their AI itself, so everyone sees
them elsewhere. Implemented in runtime/net/emsync.c, on by default: ini `[Multiplayer] SyncSmallMonsters = On`
(env `MHGU_SYNC_ZAKO=0/1`). Addresses below are runtime addresses (image offset + 0x200000: what
`tools/disasm.py`, `fntrace` and thread dumps use). `tools/retool.py` (strings / xref / callers / dti / vtslot)
finds all of this again.

## How the game syncs monsters

Network layer: MT Framework RPC (`cRemoteCall` messages, `cRemoteProcedure` handlers) in `nFestaNetwork`, over
Nintendo Pia. Large monsters have a *master* player whose game runs the AI; at every new action it sends
`netEmAction` (0xcc bytes: position, rotation, HP, action, area / waypoint state, targets, ailments...) and the
others replay it. Small monsters are spawned per zone from `quest\zako\emSetList\`; only their herds ("Mure":
`netMureEm*`) and HP / deaths are synced.

Small and large monsters are one class family (`uEmsNNN_00` / `uEmNNN_00` -> `uFestaEnemy` -> `uEnemy` ->
`uEnemyBase` -> `uEnemyCharacter`), with the same network code. The difference is data in the status block
(`*(monster + 0x1428)`):

| status block | meaning |
|---|---|
| +0x40 | position (3 floats) |
| +0x1a8 | flags: 8 = master (its actions are sent), 0x80 = simulated locally (all small monsters, always) |
| +0x1b6 | u16 network id: large monsters from the quest's set (e.g. 62), small monsters 10000 + a quest-wide spawn index |
| +0x1e4 | HP (float) |

- Send: `0x2cafe8(monster, kind, data)` (callers check flag 8 first) -> per-kind vtable slot (action:
  +0x42c = `0x2cb330`, fills netEmAction, sends via `0x3eb810`).
- Receive: netEmActionProcedure (`0x3f4554`) -> enemy manager (`sEnemy`, global `0x1a51fe0`) `0x265b14` ->
  find by id among its 10 live enemies (+0x1c..+0x40; small monsters included) -> per-monster receive `0x2cb120`
  (drops own messages, dispatches by kind to the vtable slots +0x430...; action: `0x2cc154` copies the state in).
- Small monsters already *receive* fine; they never send because nothing sets flag 8 on them, and their 0x80
  makes every game run them itself. Setting 8 on one player's copies and clearing 0x80 on the others' is enough:
  the others' copies then replay the master's actions (tested with poke before writing any code).

Other data used: `sPlayer` (global `0x1a85b58`): +0x1c the 4 player slots; a player's current zone is the byte at
player + 0x1054 (0..12; setter: uPlayer vtable +0xa4; other players' zones arrive by netPlAreaMove). The local
player's slot: `*(0x1a8ca48) + 0x30`. Area 0 = base camp, 255 = not in a quest.

## The port's part (emsync.c)

- Each frame (from the game's input polling, on its thread, ~30 times a second): every player's zone, debounced
  (a new value counts after 0.7 s); per zone one player runs the small monsters: the first one there. Claims go to
  the other games over the local play link (`F_AUX` frames, ldn_send_aux) on every change and twice a second.
  Two claims for one zone (arrived together): the lower slot keeps it.
- Master: flags 8 | 0x80 on the zone's small monsters. Others in the zone: 0x80 and 8 cleared. Alone in a zone:
  as the original game (0x80, no 8).
- tools/gen.py PATCHES `0x2cb120` calls `emsync_filter`: drops a small monster message on the master's game, and
  one from a player in another zone.

## Tests (two instances on one PC, Harvest Tour: Jurassic Frontier, zone 1's three Aptonoth)

- Before: same ids, positions hundreds of units apart on the two games.
- After: within 0-10 units (centimetres) while walking and grazing, for 5+ minutes; the second player's freshly
  spawned copies snap to the master's within ~4 s of arriving. Either player first in the zone works.
- Hand-over: the master leaves -> the other player's game runs them; comes back -> follows.
- Seen once before the debounce: the master role flipped between two players standing still (a passing read of
  the zone, most likely); not seen again in 6 minutes with the debounce.
- Not tested: combat (hits by a follower, deaths, carving), aggressive small monsters (Jaggi...), 3-4 players,
  two PCs.

## Debug tools made for this

- Control file (MHGU_CONTROL): `peek <hex addr> [words]`, `poke <hex addr> <hex word>`,
  `memsave <hex addr> <hex bytes> <file>`, `fntrace <lo> <hi> [n]` (log calls of guest functions in [lo, hi)),
  stick diagonals `STICK_UPLEFT` etc.
- `MHGU_DUMP_THREADS=<s>`: thread dumps with mutex owners and callers.
