# To compare with the Switch version

Things the port now does by porting an emulator's behaviour, which only a side-by-side listen / look against the
original game can confirm. Note the result (and the date) next to each item when checked.

## Audio

- [ ] **Environmental reverb** (I3DL2 reverb, added 2026-10-20, commit f49d5de). The game sends sound into mix 1, which
  runs three mono I3DL2 reverbs in a row (room -10.45 dB, room HF -10 dB, decay 1.0 s, HF decay ratio 0.2, dry 0.24).
  The port follows Ryujinx's implementation; its delay lines use yuzu's ring size instead (Ryujinx's would click every
  400 ms). Compare: the amount and length of the reverb tail in the village, indoors (house, smithy), in the gathering
  hub and in hunting areas (caves vs open fields), on voices, footsteps, weapon hits and monster roars. Too much / too
  little / too long / too metallic?
- [ ] **The game's own final-mix effect** (Aux, added in the same commit). Every frame of the 6-channel final mix goes
  through an effect the game computes itself on the CPU and comes back to the renderer; like the emulators, this adds
  about 80 ms of delay to all sound. Compare: lip sync in cutscenes and how quickly hits / button sounds follow the
  action; any overall loudness or tone difference.
- [ ] **Stereo placement of sounds** (splitter fix, 2026-10-20). Until this fix, any splitter update containing more
  than one splitter was cut short. Stereo sounds made of two mono voices then played only their left half: the title
  "Press A" confirm, Continue, and the "Load this character?" fanfare, and probably many more in game. Reverb sends
  were sometimes lost too. Compare: sounds now centred in the menus, panning of in-game sounds (monsters, NPCs to one
  side), and how much reverb the sounds get now that their reverb sends arrive.
- A/B switches for the port: `MHGU_NO_REVERB=1` turns both reverbs off, `MHGU_NO_AUX=1` bypasses the game's final-mix
  effect, `MHGU_AUDIO_LOG=1` logs the effect settings.
