# Online play (version 6)

## Using it
- **Host:** Enhancements menu → **Online → Host a game**. The page shows the room code, how it was found, and the LAN address. *Copy room code* puts it on the clipboard.
- **Join:** **Online → Join a game**. Type or paste the code, then **Connect**. Games on the same network are listed below. A code, `ip`, `ip:port` or a host name all work.
- **If player 2 can't get through:** the join page shows player 2's own code. The host types it under *Player 2's code*, and both routers then let the traffic through (UDP hole punching).
- **Settings** (Online page, saved in `smkplay.cfg`): rollback on/off, input delay (auto or 1-8 frames), host port (default 7845), ping display.
- **Command line:** `--host [port]`, `--join <code | ip[:port] | name>`, `--delay N`, `--norollback`.
- **The host decides** the input delay, rollback on/off and the gameplay rules (200cc, unlock everything). Both PCs restart with the host's save. Your own save comes back when you disconnect, and an online session never writes to it.

## How the connection is made
1. **Socket:** the host binds UDP 7845 (or the chosen port), the client any port.
2. **Public address (STUN):** a Binding Request to `stun.l.google.com:19302`, `stun1.l.google.com:19302` and `stun.cloudflare.com:3478` returns the address the internet sees for that socket. DNS runs on a thread.
3. **Port forward (UPnP, host only, on a thread):** SSDP discovery → device description → `AddPortMapping` (UDP, permanent lease; falls back to 2 hours if the router only accepts timed leases) → `GetExternalIPAddress`. The mapping is removed when hosting stops.
4. **Room code:** IPv4 + port + a 12-bit check value, as 12 characters of Crockford base32 (`ABCD-EFGH-JKMN`). Typing is forgiving: any case, dashes optional, O reads as 0, I/L as 1. A typo fails the check instead of connecting somewhere random. With UPnP the code uses the router's address and the forwarded port, otherwise the STUN address. With neither, it uses the LAN address (LAN/VPN only).
5. **LAN discovery:** the join page broadcasts DISCOVER on the port. Waiting hosts answer with their name.
6. **Hole punching:** the client sends HELLO every 250 ms. If the host enters the client's code, it sends PUNCH packets there every 250 ms, which opens its NAT towards the client. When the client gets a PUNCH, it answers from wherever it came. This doesn't work when both sides are behind symmetric NAT (some mobile and carrier-grade NAT); a relay server would be needed then (not included).
7. **Handshake:** HELLO (protocol version + ROM CRC; a mismatch gets a REJECT with the reason) → the host sends 5 PINGs and takes the median round trip → WELCOME (delay, rollback, rules byte, host SRAM). The client starts on WELCOME. If the WELCOME gets lost, the next HELLO gets it again.

**Automatic delay.** *one-way* = RTT / 2 in frames.
- Rollback: delay 1 below 1.5 frames, 2 below 4 frames, otherwise 3. Rollback covers the rest, up to 8 frames.
- Lockstep: ceil(one-way + 0.5) + 1, between 2 and 12.

## Running: rollback
Every displayed frame, `net_tick`:
1. Queues our input for frame *now + delay*. It's already transformed (remap, turbo, auto-gas), so the network carries exactly what the game uses.
2. Reads packets. Each packet carries our unacknowledged inputs (up to 64, so lost packets cost nothing), an ack, the sender's frame number, the latest confirmed-state hash, and a timestamp echo for the ping.
3. If a remote input arrived for a frame we already ran with a guess, and the guess was wrong: it loads the state saved before that frame and replays up to the present with the PPU in headless mode (no pixels; everything that affects the game still runs). The guess for a missing input is "the same as their last known input".
4. Runs the next frame (drawn) if we're less than 8 frames ahead of the last confirmed remote input. Otherwise it waits.
5. **Time sync:** if we're 1.5+ frames ahead of the other side (their frame number + half the ping), it skips one frame now and then (at most every 200 ms) so neither side keeps rolling back.

**Desync check.** Every 60 frames, once a frame's inputs are confirmed on both sides, each side hashes the saved state right after it and sends the hash. The other side compares it with its own hash for the same frame. A mismatch shows "out of sync" on screen and in the title.

**Cost.** A save state is 263 KB and takes about 0.2 ms. A replayed frame takes about 2-3 ms on the build machine (a modest cloud CPU), so a worst-case 8-frame rollback fits in about one display frame.

**Engines.** The interpreter and the recompiled build produce identical frames, so the two players can use different engines.

## Testing
- `smktrace` script command `rbtest <frames> <window> [headless]`: plays random inputs and rewinds and replays every `window` frames. Every replayed frame's state hash must match the first run. Result: 0 mismatches on both engines, rendering or headless.
- Loopback with simulated network trouble: environment variables `SMKNET_LAG=<ms one way>`, `SMKNET_JITTER=<ms>`, `SMKNET_LOSS=<percent>` on both copies, plus `SMKPLAY_TESTINPUT` (random pads) and `SMKPLAY_TEST=<frames>`. Both copies print the hash of the same confirmed checkpoint frame.
  - Verified with rollback at 50 ms ±15 ms and 5% loss, and in lockstep mode.
  - Verified with mixed engines (one side `--interp`), LAN discovery, and joining by room code.
- `SMK_STUN=ip:port` points STUN at a test server, `SMK_UPNP_SSDP=ip:port` sends UPnP discovery to a test router, `SMK_NOUPNP=1` skips UPnP.

## Next
- Optional relay server for symmetric NAT (a small UDP forwarder anyone can run), with short room codes looked up there.
- Spectators (read-only input stream), and a reconnect window that doesn't drop the session after a few seconds of silence.
- Rollback audio smoothing (cross-fade the replaced samples).
