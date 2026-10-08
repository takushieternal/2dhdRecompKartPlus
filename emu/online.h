// online: the Online menu's connection logic on top of netplay.c (room codes via STUN, UPnP port
// forwarding, LAN discovery, hole punching). Owns the UDP socket; slow lookups run on threads.
#ifndef ONLINE_H
#define ONLINE_H
#include <stdint.h>
#include <stdbool.h>
#include "netplay.h"
#include "options.h"

enum { ONL_NONE, ONL_STARTED, ONL_ENDED, ONL_FAILED };

void online_init(uint32_t romCrc, NetUi* ui);
// host: open `port`, find the public address, try UPnP, wait for player 2
bool online_host(int port, const uint8_t* sram, int sramLen, uint8_t rules, int delay, bool rollback);
// client: open a socket, find our own code (for punch-back) and look for LAN games
bool online_joinPage(int lanPort);
void online_join(const char* text, int defPort);     // room code, IP[:port] or host name
void online_joinLan(int index);
void online_lanSearch(int port);
void online_punch(const char* text, int defPort);    // host: player 2's code
void online_cancel(void);                            // stop hosting / joining
int online_update(void);                             // ONL_* event
Netplay* online_session(void);                       // running session, or NULL
void online_end(void);                               // leave the session
void online_shutdown(void);

#endif
