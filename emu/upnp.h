// upnp: ask the home router (UPnP IGD) to forward a UDP port to this PC, so the other player can
// reach a host without manual port forwarding. Blocking (a few seconds at most): call it on a thread.
#ifndef UPNP_H
#define UPNP_H
#include <stdint.h>
#include <stdbool.h>

// Forward UDP `port` (external = internal) to this machine. On success returns true and the
// router's external IPv4 address (host order, 0 if the router didn't say). err gets a reason otherwise.
bool upnp_mapUdp(int port, uint32_t* externalIp, char* err, int errLen);
// Remove the mapping made by upnp_mapUdp (best effort).
void upnp_unmapUdp(int port);

// test hook: SMK_UPNP_SSDP=<ip:port> sends the discovery request there instead of the multicast group

#endif
