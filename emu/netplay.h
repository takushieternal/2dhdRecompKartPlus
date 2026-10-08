// netplay: 2-player online play over UDP, with rollback (or plain lockstep).
//
// Connection (non-blocking, driven by net_poll from the UI loop):
//   - Both sides open a UDP socket. The host binds the game port (default 7845), the client any port.
//   - STUN (net_stunStart) finds the socket's public address -> a room code (net_codeEncode).
//     The host can also open the port on the router with UPnP (upnp.c).
//   - The client sends HELLO to the host's address (from a room code, an IP, or LAN discovery).
//     If the host's router drops it, the host types the client's code and sends PUNCH packets back,
//     which opens its NAT for the client (UDP hole punching).
//   - The host measures the round trip, picks the input delay, and sends WELCOME: delay, rollback
//     on/off, session rules (200cc etc.) and its SRAM. Both sides power-cycle with that SRAM.
//
// Running (net_tick, once per displayed frame): inputs are exchanged with redundancy and acks.
// With rollback, a missing remote input is predicted (= its last known input); when the real
// input arrives and differs, the emulator rewinds to that frame (callbacks) and replays it
// without rendering. Without rollback, a frame only runs when both inputs are known (lockstep).
// Every 60 frames a hash of a confirmed frame's state is compared (desync detection).
#ifndef NETPLAY_H
#define NETPLAY_H
#include <stdint.h>
#include <stdbool.h>

typedef struct Netplay Netplay;

enum { NET_IDLE, NET_HOSTING, NET_JOINING, NET_RUNNING, NET_FAILED, NET_CLOSED };

#define NET_DEFAULT_PORT 7845
#define NET_MAX_ROLLBACK 8
#define NET_SLOTS (NET_MAX_ROLLBACK + 2)

// emulator hooks for rollback (slot = 0..NET_SLOTS-1)
typedef struct {
  void* ctx;
  void (*save)(void* ctx, int slot);
  void (*load)(void* ctx, int slot);
  void (*run)(void* ctx, uint16_t p1, uint16_t p2, bool render);
  uint64_t (*hashSlot)(void* ctx, int slot);    // hash of a saved state (desync check)
} NetEmu;

typedef struct {
  uint32_t ip; uint16_t port;    // host order
  char name[40];
} NetLanGame;

typedef struct {
  int rttMs;                      // smoothed round trip
  int delay;                      // input delay in frames
  bool rollback;
  int lastRollback, maxRollback;  // frames replayed by the latest / worst rollback
  uint32_t rollbacks, stalls;     // counts
  int advantage;                  // our frame minus the peer's (estimated), in frames
  bool desync;
} NetStats;

// ---- setup
// port 0 = any (client). Returns NULL with err set if the socket can't be opened.
Netplay* net_open(int port, char* err, int errLen);
void net_close(Netplay* n);                         // sends BYE if a peer is known
int net_localPort(Netplay* n);
// STUN: server addresses resolved by the caller (DNS may block: do it on a thread); IPv4, host order
void net_stunStart(Netplay* n, const uint32_t* ips, const uint16_t* ports, int count);
bool net_publicAddr(Netplay* n, uint32_t* ip, uint16_t* port);   // true once STUN answered
bool net_stunFailed(Netplay* n);                                 // no answer after the timeout
// local IPv4 address used for the internet (best guess), host order; 0 if unknown
uint32_t net_lanAddr(void);
// resolve "host" (name or dotted IPv4) to IPv4, host order; blocking
bool net_resolve(const char* host, uint32_t* ip);
// host: wait for a client. delay 0 = automatic. sram is copied.
void net_host(Netplay* n, uint32_t romCrc, const uint8_t* sram, int sramLen, uint8_t rules, int delay, bool rollback);
void net_punch(Netplay* n, uint32_t ip, uint16_t port);          // host: open the NAT towards a client
// client
void net_join(Netplay* n, uint32_t ip, uint16_t port, uint32_t romCrc);
void net_lanSearch(Netplay* n, int port);                         // broadcast "who's hosting?"
int net_lanGames(Netplay* n, NetLanGame* out, int max);
// drive the connection; returns the state (NET_*)
int net_poll(Netplay* n);
const char* net_status(Netplay* n);                               // human-readable state / error
// after NET_RUNNING: what the host decided
bool net_isHost(Netplay* n);
int net_sram(Netplay* n, uint8_t* out, int cap);                 // host's SRAM (both sides)
uint8_t net_rules(Netplay* n);
int net_delay(Netplay* n);
bool net_rollback(Netplay* n);

// ---- running
// Once per displayed frame with the local (already transformed) pad. Runs zero or more emulator
// frames through emu (at most one rendered). Returns 1 if a new frame was shown, 0 if waiting
// for the other player, -1 if the connection is gone.
int net_tick(Netplay* n, uint16_t localPad, const NetEmu* emu);
uint32_t net_frameNo(Netplay* n);              // next frame to run
uint32_t net_confirmedFrame(Netplay* n);       // all frames below this have both real inputs
// hash of a confirmed frame (frame % 60 == 0) once known: returns false if not available
bool net_checkpoint(Netplay* n, uint32_t frame, uint64_t* hash);
void net_stats(Netplay* n, NetStats* s);
// keep exchanging packets for a while without advancing (lets the peer receive our last inputs)
void net_linger(Netplay* n, int ms);

// ---- room codes: IPv4 + port + check bits, 12 characters of Crockford base32 ("ABCD-EFGH-JKMN")
void net_codeEncode(uint32_t ip, uint16_t port, char* out, int cap);
// accepts codes (any case, dashes/spaces ignored, O->0, I/L->1) and "a.b.c.d[:port]";
// host names only with resolve = true (blocking DNS)
bool net_codeDecode(const char* s, uint32_t* ip, uint16_t* port, int defPort, bool resolve);

// test hooks (environment): SMKNET_LAG=<ms one way>, SMKNET_JITTER=<ms>, SMKNET_LOSS=<percent>

#endif
