# marioKartRecomp, version 5

Version 5 reorganises the Enhancements menu into **Video / Gameplay / Controls** pages and adds:

- **Button remapping** (Controls page): every SNES button can be bound to any key and any gamepad button (triggers included). Pick a row, press A, then press the new key or button. Taking a key that's already used swaps the two. The left stick also steers. The menu always also answers to the default keys, so a bad mapping can't lock you out.
- **Auto-gas** (off by default): holds B while racing. Hold Y (brake) to let go of the gas.
- **Turbo** (off by default): rapid-fire on chosen buttons (A, B, A+B, Y, X or all four face buttons) at 30, 15, 10 or 7.5 Hz.
- **Unlock everything** (off by default): Special Cup, 150cc and 200cc are available without the trophies. Your save file is never changed: the game's unlock checks just see a gold trophy while this is on.
- **200cc**: replaces 150cc on the class menu (it still says 150cc in the game's own text). Karts get one more top-speed step, sharper acceleration, and the computer drivers are 30% faster. It unlocks with a **gold trophy in the 150cc Special Cup** (the same rule the game uses to open 150cc, one class up) or with *Unlock everything*.
- **HD-2D photo mode** (F8, or *Photo mode* in the menu during a race with HD-2D on): the game freezes and you can fly a camera around your kart. Karts and items stand in the 3D world as upright billboards, and the HUD is left out. D-pad orbits, L/R zoom, X/Y raise/lower the camera, A saves a PNG to `photos/` next to the executable, Select hides the help, B exits.

200cc and *Unlock everything* change the game, so in netplay **the host's settings apply to both players**: they're sent in the connection handshake (protocol version 2, so a version 4 client can't join a version 5 host by accident). Remapping, turbo and auto-gas are applied to your own pad before it's sent, so the other side always runs exactly the inputs your game used. That's also what rollback needs. See `docs/NETPLAY.md` for the plan for the next version (easier online setup, rollback).

How the game rules are implemented: `emu/rules.c` hooks single instructions found in the disassembly (top speed, acceleration and CPU speed tables for 200cc; the trophy loads for the unlocks). Each hook runs only when its option is on, and otherwise the original instruction runs untouched. The hooks work in both engines because the recompiler emits a call to them. With all options off, the frame hashes are identical to version 4; with them on, the interpreter and the recompiled build still match frame for frame.


Version 4 added the **Enhancements menu**. Press **SELECT on the title screen** (a hint is shown there), **SELECT+START** anywhere, or Esc. You can switch these with the controller:

- widescreen
- HD-2D 3D world
- 3D walls
- tilt-shift/bloom
- HD Mode 7
- engine (recompiled or interpreter)
- fullscreen
- window size

Settings are saved to `smkplay.cfg`. The game pauses while the menu is open, except in netplay. `smkplay` now uses OpenGL when available, so HD-2D can be switched on at any time, and falls back to the classic renderer (HD-2D greyed out) otherwise or with `--nogl`. Also fixed: in HD-2D the race-position number was missing. SMK draws it as a colour-math window, not a sprite, and it's now captured.


Version 3 adds the **HD-2D renderer**: `smkplay --hd2d`. Every Mode 7 view is re-rendered as a real 3D scene from the game's own camera, at full resolution and 16:9. Walls are extruded from the track's surface data, and tilt-shift, bloom and a vignette go on top. Details and status are in `docs/HD2D.md`.
Version 2 added widescreen (`--wide` / F2). Version 1 was the recompiled build, netplay and HD Mode 7.

A from-scratch reverse-engineering project for **Super Mario Kart (USA, SNES)**: a native PC port you can mod.

You need to supply the ROM yourself. Nothing from the game is stored in this repository. Every ROM-derived file (the disassembly, the generated C, rebuilt ROMs, traces, save states) is built locally from your own copy and is git-ignored.

Expected ROM: `Super Mario Kart (USA)`, 512 KiB, no copier header, CRC32 `CD80DB86`, SHA-1 `47e103d8398cf5b7cbb42b95df3a3c270691163b`.

## Status

| Phase | What | State |
|---|---|---|
| 0 | Emulated hardware (PPU/APU/DMA) + HLE DSP-1, tracer, SDL2 player | done |
| 1 | Labeled, byte-identical, rebuildable disassembly | done: rebuilds to CRC32 `CD80DB86`, ~29.7k instructions |
| 2 | Static recompilation to C | **running**: the game code executes as recompiled C and is verified frame-for-frame against the interpreter. C overrides work. Lifting into readable C is the ongoing step |
| 3 | Mods | **netplay (2P over UDP)**, **HD Mode 7**, **widescreen 16:9**, **remapping / turbo / auto-gas**, **200cc**, **unlock everything** done; online setup + rollback next |
| 4 | HD-2D: 3D environment on top of the original logic | **phase 1 running** (`--hd2d`): 3D floors and walls, post effects, photo mode with sprite billboards |

Details: `docs/ROADMAP.md`.

## Layout

```
emu/            hardware emulation (LakeSnes, MIT) + DSP-1 HLE + programs
  snes/cpu_core.inc  65816 core split per opcode (shared by interpreter and recompiled code)
  snes/dsp1.c        DSP-1 math coprocessor (clean-room HLE)
  smkplay.c          the PC game: SDL2, gamepads, savestates, netplay, HD Mode 7, photo mode
  options.c          Enhancements menu, settings file, remapping, turbo / auto-gas
  rules.c            gameplay options as instruction hooks (200cc, unlock everything)
  hd2d.c             HD-2D renderer (OpenGL 3.3)
  netplay.c          2-player lockstep netplay over UDP
  smktrace.c         headless runner: scripts, coverage, hashes, watchpoints, screenshots
recomp/
  gen/               GENERATED C from your ROM (git-ignored)
  overrides/         your hand-written C replacing recompiled code (see README there)
  smk.h              helpers for override code
  recomp.c           dispatcher between interpreter and recompiled banks
tools/
  disasm.py          coverage-guided disassembler -> asm/ (asar)
  build.py           assemble asm/ and verify against the original ROM
  recomp.py          static recompiler -> recomp/gen/
  verify_recomp.py   lockstep check: interpreter vs recompiled, every campaign, every frame
  apply_mods.py      rebuild + apply mods/*.asm -> build/smk_mod.sfc
  peek.py, op65816.py
trace/
  bootstrap.txt      creates the starting savestates
  run_coverage.py    all coverage campaigns, then regenerates asm/ and verifies it
mods/            asar patches (short_races.asm = example)
symbols.txt      routine names used by the disassembler and recompiler
docs/            roadmap, RE notes (RAM map, DSP-1 usage, unlocks), modding guide
bin/win/         Windows binaries: smkplay.exe (recompiled build), smktrace.exe, SDL2.dll, asar.exe
```

## Quick start (Windows)

1. Put your ROM in the project root as `smk.sfc` (and in `bin\win\` for the player).
2. **Play:** run `bin\win\smkplay.exe`.
   - Default keys (remappable in Enhancements > Controls): arrows = D-pad, Z = B (gas), X = A (item), A = Y (brake), S = X (rear view), Q/W = L/R (hop), Enter = Start, Right Shift = Select. Gamepads work for players 1 and 2.
   - Esc or Select on the title screen = Enhancements menu. F2 = widescreen 16:9, F3 = HD-2D, F8 = photo mode, H = HD Mode 7, F5/F9 = save/load state, hold Tab = fast-forward, P = pause, F11 = fullscreen, 1–6 = window scale.
   - `smkplay.exe --hd2d` starts the HD-2D renderer (needs OpenGL 3.3; F3 = 3D on/off, F6 = post effects).
   - `smkplay.exe --wide --hd` starts in widescreen with HD Mode 7. `--interp` runs the original code on the interpreter instead of the recompiled C.
3. **Netplay (2 players, 2 PCs):**
   - Player 1: `smkplay.exe --host` (UDP port 7845; open it in the firewall, or use a LAN/VPN such as WireGuard).
   - Player 2: `smkplay.exe --join <host-ip>`.
   - Options: `--host 9000`, `--join 10.0.0.5:9000`, `--delay 2` (host picks the input delay; raise it on bad connections).
   - Both sides need the same ROM. Player 2 gets the host's save data for the session, and their own save is left untouched.
   - Pick **2P GAME** on the title screen: player 1 is the host, player 2 the client.
4. **Disassembly / recompilation pipeline** (Python 3):
   ```
   python trace\run_coverage.py smk.sfc          # campaigns -> asm\ -> byte-identical rebuild check
   python tools\recomp.py smk.sfc trace\out recomp\gen --symbols symbols.txt
   python tools\verify_recomp.py smk.sfc        # needs a build of smktrace with recomp\gen linked in
   ```
5. **Modding:**
   - Assembly patches: `mods\*.asm`, then `python tools\apply_mods.py`.
   - C: `recomp\overrides\*.c`, then regenerate and rebuild.
   - See `docs\MODDING.md`.

## Building from source

Linux/WSL/MSYS2: `make -C emu` (gcc + SDL2 dev). When `recomp/gen/` exists, the recompiled code is linked in automatically. Windows binaries: `make -C emu win` (mingw-w64 + SDL2 mingw devel package, see `SDL2WIN` in `emu/Makefile`).

## Licensing

- Project code: MIT.
- `emu/snes/*` derives from [LakeSnes](https://github.com/elzo-d/LakeSnes) (MIT, `emu/LICENSE-LakeSnes.txt`).
- `bin/win/asar.exe` is [asar](https://github.com/RPGHacker/asar) (licenses included). `SDL2.dll` is zlib-licensed.
- Super Mario Kart is © Nintendo. Do not commit or distribute the ROM, `asm/`, `recomp/gen/`, rebuilt ROMs, or the Windows `smkplay.exe`/`smktrace.exe` built with recompiled code: they contain translated game code.
