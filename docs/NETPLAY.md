# Netplay: current state and the plan for version 6

## What's there (versions 1-5)
- Delay-based lockstep over UDP (`emu/netplay.c`). The host is player 1. Each packet repeats the last 12 local inputs, so a lost packet costs nothing.
- Handshake: HELLO (protocol version + ROM CRC) → WELCOME (input delay, **session rules**, host SRAM). Both sides power-cycle with the host's SRAM, so they start identical.
- WRAM hash every 60 frames for desync detection.
- **Version 5:**
  - Protocol version 2. The WELCOME carries one byte of session rules (`rules_pack()`: bit 0 = 200cc, bit 1 = unlock everything). The client uses the host's rules, and the menu shows them as locked.
  - Remapping, turbo and auto-gas are applied before the local input is sent. The network only ever carries the final pad state, so both sides (and, later, a rollback resimulation) replay exactly what the game used.
  - Photo mode and every renderer option are render-only and stay local.

## Version 6, part 1: easier online setup
Goal: no command line and no port forwarding for most players.
1. **Menu-driven:** *Online* page in the Enhancements menu with Host / Join / Delay / Cancel. The join address is typed on an on-screen keyboard and remembered in `smkplay.cfg`. Shown while waiting: the host's LAN address and a status line.
2. **Room codes instead of IPs:** the host shows a short code. The code encodes the public endpoint learned from a STUN server, and both sides UDP hole-punch (simultaneous HELLOs). If hole-punching fails (symmetric NAT), fall back to an optional relay, or to the manual IP + port forwarding we have now.
3. **Connection quality:** measure RTT during the handshake and pick the input delay automatically (ceil(RTT/2 / 16.7 ms) + 1). Show ping and the rollback depth in a small overlay.
4. **Robustness:** timeouts with a clear message, reconnect within a few seconds, and a version/ROM/rules mismatch message on the client instead of a silent refusal.

## Version 6, part 2: rollback
The lockstep core already has what rollback needs: a deterministic emulator, inputs keyed by frame, and an exact state hash. The missing pieces:
1. **Fast savestates in memory:** `snes_saveState` into a ring of N=8..10 buffers (about 270 KB each). One per frame is cheap compared with emulating a frame.
2. **Prediction:** when the remote input for frame f hasn't arrived, repeat its last known input. Remember which frames were predicted.
3. **Resimulation:** when a real input differs from the prediction, load the state from before that frame and re-run up to the present with rendering and audio off. Needs a "headless frame" mode in the PPU/APU: skip pixel output and the DSP sample mix, but keep every register side effect and the timing.
4. **Rendering only the final frame:** HD-2D captures (`hd2d_capture`) and the DSP-1 camera taps run only on the frame that's shown. They're render-only, so skipping them during resimulation can't desync.
5. **Audio:** keep the last presented frame's samples. A rollback replaces at most a few frames of audio, so either cross-fade or accept a tiny glitch, as most rollback emulators do.
6. **Frame budget:** the recompiled build runs a frame in a fraction of 16 ms. 8 frames of resimulation plus one rendered frame has to fit in one display frame. Measure with `smktrace prof`.
7. **Checking:** the existing hash exchange becomes the rollback self-test. Hash only confirmed (non-predicted) frames. The automated test runs two clients over a lossy loopback (random drop and delay) and compares checkpoint hashes, like the current `SMKPLAY_TESTINPUT` test.

Keep the input delay at 1-2 frames even with rollback: it halves the rollbacks for a frame or two of latency.

## Rules for anything new that changes the game
Anything that alters the simulation (another `emu/rules.c` hook, a C override, a cheat) has to go into the session-rules byte (or a bigger rules block in the WELCOME) and be locked in the menu during netplay. Anything purely visual can stay local.
