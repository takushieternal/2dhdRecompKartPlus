# Reverse-engineering notes

Everything here was observed with `smktrace` on the USA ROM. Addresses are CPU addresses.

## Boot and main loop
| Address | Name | Notes |
|---|---|---|
| `$80FF70` | Vector_Reset | native mode, stack `$1FFF`, enables FastROM (`$420D=1`), `JML Boot` |
| `$80FF85` | Vector_NMI | `JSL $808000 : RTI` |
| `$80FF8A` | Vector_IRQ | `JSL $80801F : RTI` |
| `$80803A` | Boot | |
| `$808056` | MainLoop | `JSL $81E067`, clear `$44`, wait for NMI to set `$44`, then `JSR (GameModeTable,X)` with `X = $36` |
| `$808197` | GameModeTable | 15 entries |

Code runs from the FastROM mirror (`$80-$87`). Data in the low halves of the banks is reached through `$C0-$C7`.

## WRAM
| Address | Meaning |
|---|---|
| `$36` (word) | game mode × 2 (index into GameModeTable). `$02` = race, `$06` = character select, `$0A` = standings, `$08` = cup select |
| `$3A` (word) | race sub-state: `$0A` = intro, `$04` = countdown (lights), `$06` = racing (stays `$06` through the results screen) |
| `$44` | set by NMI; the main loop waits on it |
| `$1000 + n*$100` | kart object n (0 = player 1, 1–7 = others) |
| `$10C1 + n*$100` | lap counter: `$7F` before the line, `$80` = lap 1 … `$84` = final lap, `$85` = finished (`$10F9` mirrors it) |

Debug trick used by the coverage campaign: poke `$10C1`/`$10F9` = `$84` (final lap) during the countdown. The kart crosses the start line about 2.8 s into the race, the counter becomes `$85`, and the race is won. That's how the campaign plays through whole cups and unlocks content.

2-player: player 2 is kart 1, so its lap counter is `$11C1`/`$11F9`. Poking it is enough, but player 2 also has to **hold gas** to reach the line (the `p2hold B` script command). A 2P race ends once both counters reach `$85`.

## DSP-1 usage
| Cmd | Name | Where |
|---|---|---|
| `$02` | Parameter (camera) | title screen and races, `$81F9E4` |
| `$0A` | Raster (Mode 7 matrix per line) | title screen, `$81F98C`; the game reads 4 words per line, then writes 4 words to end raster mode |
| `$00` | Multiply | races |
| `$04` | Triangle (sin/cos × r) | races |
| `$06` | Project (3D → screen) | races: places every kart/object sprite |
| `$0C` | Rotate | races |
| `$28` | Distance | races |

The game resyncs the DSP by writing `$80` bytes at boot (`$81E3F0`). Bytes ≥ `$40` written in command mode are no-ops.

## SRAM
2 KiB at `$30:6000` (banks `$20-$3F`/`$A0-$BF`, `$6000-$7FFF`): records, cup trophies, unlocks.

Cup trophies (decoded in version 5 by saving SRAM after every cup of a campaign):
- `$7F2`, `$7F3`, `$7F4`, `$7F5` = Mushroom, Flower, Star, Special Cup. Each byte has a 2-bit trophy per class: bits 2-3 = 50cc, bits 0-1 = 100cc, bits 4-5 = 150cc (0 none, 1 gold, 2 silver, 3 bronze). The masks are the tables at `$84FCB7` / `$84FCBA`, indexed by `$30`/2.
- `$7F0` (16-bit) = checksum: the sum of the 16-bit words at `$7F2` and `$7F4` (`$84FC94`). `$84FCF1` clears all trophies if it doesn't match.
- Writer: `$84FC6F` keeps the better trophy (`$19FC` holds the new one, already shifted into place).

Unlock checks (each is `LDA.l $3067Fx` followed by `AND #3` / `CMP #1`, i.e. "100cc gold"):
- Special Cup on the cup menu: `$84F668`/`74`/`80`, `$84F6CF`/`DB`/`E7`, `$84FAA2`/`AE`/`BA`, `$84FB13`/`1F`/`2B` (gold in the first three cups at 100cc; checked only when `$30` != 0).
- 150cc on the class menu: `$85822C`, `$858661`, `$8587E0`, `$858806` (gold in the Special Cup at 100cc).
- `$81C7A0` loops over the first three cups too (not hooked: it isn't a menu unlock).

## Engine class (`$30` = class × 2)
- `$81F01F` sets up the player karts. Top speed = `DATA_818000[character]`, then −`$80` (50cc), +0 (100cc), +`$A0` (150cc), stored by `STA $00B4,X` at `$81F040`.
- `$81EFFF` scales the acceleration curve (`$81F00A`: 150cc = A + A/2), `$81F011` adds `$30` to a second stat block at 150cc (`$81F01B`).
- `$81FEB6` copies the CPU drivers' 64-entry speed profile for the class (pointers at `$81FED5`) ×16 into `$0690` (`STA $0690,X` at `$81FEC9`).
- 200cc (version 5, `emu/rules.c`) hooks those four instructions: top speed +`$140`, acceleration ×2, stat bonus `$60`, CPU profile ×1.3 (capped).

## Unlocks (verified by the `gpfull` campaign)
- Gold in Mushroom, Flower and Star at 100cc unlocks the **Special Cup** (100cc list).
- With golds in all four 100cc cups (Special included), **150cc** appears on the class menu.
- The campaign saves the result as `trace/out/unlocked.srm`. Copy it next to your ROM as `smk.srm` to start smkplay with everything unlocked.

## Open questions
- Lap-limit check: `CMP #$84` at `$809BE4` reads the lap byte but isn't the finish test. The finish logic is still to be located.

## Recompiled build: how it hooks in
- `cpu_runOpcode` (emu/snes/cpu.c) calls `cpu_recompHook` first. `recomp.c` picks the bank function by K (any mirror: 00-07, 80-87 or C0-C7) and runs compiled code until:
  - an interrupt is pending,
  - `snes_runFrame` has to re-check its loop (`snes_yieldCounter`), or
  - control reaches an address that isn't compiled (RAM, unanalysed code). The interpreter takes over there.
- Instructions whose length depends on M/X carry a guard: if the runtime flags disagree with the analysis, the interpreter executes that one instruction.
- Debug: smktrace `ilog <file>` writes PC and master cycle for every instruction in both modes, so `diff` finds the first divergence.
