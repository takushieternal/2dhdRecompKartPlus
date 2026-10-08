// online.c: see online.h
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "online.h"
#include "upnp.h"

static Netplay* net;
static NetUi* ui;
static uint32_t crc;
static int hostPort;
static bool upnpMapped;

// ---- background lookups (DNS, UPnP): results published with an atomic flag
static const char* const stunHosts[] = {"stun.l.google.com", "stun1.l.google.com", "stun.cloudflare.com"};
static const uint16_t stunPorts[] = {19302, 19302, 3478};
static struct {
  SDL_atomic_t done;
  uint32_t ips[3]; uint16_t ports[3]; int n;
} stunDns;
static struct {
  SDL_atomic_t done;
  bool ok; uint32_t extIp; char err[96]; int port;
} upnpRes;
static struct {
  SDL_atomic_t done;
  bool ok; uint32_t ip; uint16_t port; char text[64]; int defPort; bool punch;
} joinDns;
static int generation;            // bumps on cancel so late thread results are ignored
static int stunGen, upnpGen, joinGen;

static int stunThread(void* arg) {
  (void)arg;
  const char* over = getenv("SMK_STUN");      // test hook: "ip:port"
  if(over) {
    uint32_t ip; uint16_t port;
    if(net_codeDecode(over, &ip, &port, 3478, false)) { stunDns.ips[0] = ip; stunDns.ports[0] = port; stunDns.n = 1; }
  } else {
    for(int i = 0; i < 3; i++) {
      uint32_t ip;
      if(net_resolve(stunHosts[i], &ip)) { stunDns.ips[stunDns.n] = ip; stunDns.ports[stunDns.n] = stunPorts[i]; stunDns.n++; }
    }
  }
  SDL_AtomicSet(&stunDns.done, 1);
  return 0;
}
static SDL_atomic_t upnpBusy;
static int upnpThread(void* arg) {
  (void)arg;
  upnpRes.ok = upnp_mapUdp(upnpRes.port, &upnpRes.extIp, upnpRes.err, sizeof upnpRes.err);
  SDL_AtomicSet(&upnpRes.done, 1);
  SDL_AtomicSet(&upnpBusy, 0);
  return 0;
}
static int joinThread(void* arg) {
  (void)arg;
  joinDns.ok = net_codeDecode(joinDns.text, &joinDns.ip, &joinDns.port, joinDns.defPort, true);
  SDL_AtomicSet(&joinDns.done, 2);
  return 0;
}
static void startStun(void) {
  stunDns.n = 0; SDL_AtomicSet(&stunDns.done, 0); stunGen = generation;
  SDL_Thread* t = SDL_CreateThread(stunThread, "stun-dns", NULL);
  if(t) SDL_DetachThread(t); else SDL_AtomicSet(&stunDns.done, 1);
}

static void ipStr(uint32_t ip, uint16_t port, char* out, int cap) {
  snprintf(out, cap, "%u.%u.%u.%u:%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port);
}
static void status(const char* s) { snprintf(ui->status, sizeof ui->status, "%s", s); }

void online_init(uint32_t romCrc, NetUi* u) { crc = romCrc; ui = u; memset(ui, 0, sizeof *ui); }

static void closeNet(void) {
  if(net) { net_close(net); net = NULL; }
  if(upnpMapped) { upnp_unmapUdp(hostPort); upnpMapped = false; }
  generation++;
  ui->code[0] = 0; ui->codeNote[0] = 0; ui->nlan = 0; ui->joinPage = false;
}

bool online_host(int port, const uint8_t* sram, int sramLen, uint8_t rules, int delay, bool rollback) {
  closeNet();
  char err[128];
  net = net_open(port, err, sizeof err);
  if(!net) { status(err); ui->mode = NETUI_OFF; return false; }
  hostPort = port;
  net_host(net, crc, sram, sramLen, rules, delay, rollback);
  ui->mode = NETUI_HOSTING; ui->isHost = true;
  uint32_t lan = net_lanAddr();
  if(lan) ipStr(lan, (uint16_t)port, ui->lan, sizeof ui->lan); else snprintf(ui->lan, sizeof ui->lan, "-");
  snprintf(ui->codeNote, sizeof ui->codeNote, "FINDING YOUR INTERNET ADDRESS...");
  status("WAITING FOR PLAYER 2");
  startStun();
  if(!getenv("SMK_NOUPNP") && SDL_AtomicCAS(&upnpBusy, 0, 1)) {
    upnpRes.port = port; upnpRes.ok = false; SDL_AtomicSet(&upnpRes.done, 0); upnpGen = generation;
    SDL_Thread* t = SDL_CreateThread(upnpThread, "upnp", NULL);
    if(t) SDL_DetachThread(t); else { SDL_AtomicSet(&upnpRes.done, 1); SDL_AtomicSet(&upnpBusy, 0); }
  } else { upnpRes.ok = false; upnpGen = generation; SDL_AtomicSet(&upnpRes.done, 1); }
  return true;
}

bool online_joinPage(int lanPort) {
  if(net && ui->joinPage) { online_lanSearch(lanPort); return true; }
  closeNet();
  char err[128];
  net = net_open(0, err, sizeof err);
  if(!net) { status(err); return false; }
  ui->joinPage = true; ui->mode = NETUI_OFF; ui->isHost = false;
  snprintf(ui->codeNote, sizeof ui->codeNote, "FINDING YOUR CODE...");
  status("ENTER THE HOST'S ROOM CODE");
  startStun();
  online_lanSearch(lanPort);
  return true;
}

void online_lanSearch(int port) {
  if(!net) return;
  net_lanSearch(net, port);
  if(port != NET_DEFAULT_PORT) net_lanSearch(net, NET_DEFAULT_PORT);
}

static void startJoin(uint32_t ip, uint16_t port) {
  char a[32]; ipStr(ip, port, a, sizeof a);
  net_join(net, ip, port, crc);
  ui->mode = NETUI_JOINING;
  char s[64]; snprintf(s, sizeof s, "CONNECTING TO %s", a); status(s);
}

void online_join(const char* text, int defPort) {
  if(!net || !ui->joinPage) online_joinPage(defPort);
  if(!net) return;
  uint32_t ip; uint16_t port;
  if(net_codeDecode(text, &ip, &port, defPort, false)) { startJoin(ip, port); return; }
  // maybe a host name: look it up on a thread
  bool name = false;
  for(const char* p = text; *p; p++) if(*p == '.') name = true;
  if(!name) { status("THAT ROOM CODE IS NOT VALID"); return; }
  snprintf(joinDns.text, sizeof joinDns.text, "%s", text); joinDns.defPort = defPort; joinDns.punch = false;
  SDL_AtomicSet(&joinDns.done, 1); joinGen = generation;
  SDL_Thread* t = SDL_CreateThread(joinThread, "join-dns", NULL);
  if(t) SDL_DetachThread(t); else SDL_AtomicSet(&joinDns.done, 0);
  status("LOOKING UP THE HOST...");
}

void online_joinLan(int index) {
  if(!net) return;
  NetLanGame g[4];
  int n = net_lanGames(net, g, 4);
  if(index < 0 || index >= n) return;
  startJoin(g[index].ip, g[index].port);
}

void online_punch(const char* text, int defPort) {
  if(!net || ui->mode != NETUI_HOSTING) return;
  uint32_t ip; uint16_t port;
  if(!net_codeDecode(text, &ip, &port, defPort, false)) { status("THAT CODE IS NOT VALID"); return; }
  net_punch(net, ip, port);
  char a[32], s[64]; ipStr(ip, port, a, sizeof a);
  snprintf(s, sizeof s, "OPENING THE WAY TO %s", a); status(s);
}

void online_cancel(void) {
  if(ui->mode == NETUI_RUNNING) return;
  closeNet();
  ui->mode = NETUI_OFF;
  status("");
}

static void refreshCode(void) {
  uint32_t ip; uint16_t port;
  if(ui->mode == NETUI_HOSTING && SDL_AtomicGet(&upnpRes.done) && upnpGen == generation && upnpRes.ok && upnpRes.extIp) {
    upnpMapped = true;
    net_codeEncode(upnpRes.extIp, (uint16_t)hostPort, ui->code, sizeof ui->code);
    snprintf(ui->codeNote, sizeof ui->codeNote, "PORT OPENED ON YOUR ROUTER (UPNP)");
    return;
  }
  if(net_publicAddr(net, &ip, &port)) {
    net_codeEncode(ip, port, ui->code, sizeof ui->code);
    if(ui->mode == NETUI_HOSTING) {
      bool upnpDone = SDL_AtomicGet(&upnpRes.done) != 0;
      snprintf(ui->codeNote, sizeof ui->codeNote, !upnpDone ? "ASKING YOUR ROUTER TO OPEN THE PORT..." :
               "NO LUCK? ENTER PLAYER 2'S CODE BELOW");
    } else snprintf(ui->codeNote, sizeof ui->codeNote, "THE HOST MAY ASK FOR THIS CODE");
    return;
  }
  if(net_stunFailed(net)) {
    uint32_t lan = net_lanAddr();
    if(lan) net_codeEncode(lan, (uint16_t)net_localPort(net), ui->code, sizeof ui->code);
    snprintf(ui->codeNote, sizeof ui->codeNote, "NO INTERNET ADDRESS FOUND: LAN / VPN ONLY");
  }
}

int online_update(void) {
  if(!net) return ONL_NONE;
  if(SDL_AtomicGet(&stunDns.done) == 1 && stunGen == generation) {
    SDL_AtomicSet(&stunDns.done, 2);
    net_stunStart(net, stunDns.ips, stunDns.ports, stunDns.n);
  }
  if(SDL_AtomicGet(&joinDns.done) == 2 && joinGen == generation) {
    SDL_AtomicSet(&joinDns.done, 0);
    if(joinDns.ok) startJoin(joinDns.ip, joinDns.port); else status("HOST NAME NOT FOUND");
  }
  int st = net_poll(net);
  if(ui->mode != NETUI_RUNNING) refreshCode();
  if(ui->joinPage) {
    NetLanGame g[4];
    ui->nlan = net_lanGames(net, g, 4);
    for(int i = 0; i < ui->nlan; i++) {
      char a[32]; ipStr(g[i].ip, g[i].port, a, sizeof a);
      snprintf(ui->lanNames[i], sizeof ui->lanNames[i], "%.20s %s", g[i].name, a);
    }
  }
  if(ui->mode == NETUI_RUNNING) {
    NetStats s; net_stats(net, &s);
    ui->ping = s.rttMs; ui->delay = s.delay; ui->rollback = s.rollback;
    if(st != NET_RUNNING) {
      char m[64]; snprintf(m, sizeof m, "%s", net_status(net));
      for(char* p = m; *p; p++) if(*p >= 'a' && *p <= 'z') *p -= 32;
      closeNet();
      ui->mode = NETUI_OFF; status(m);
      return ONL_ENDED;
    }
    return ONL_NONE;
  }
  if(st == NET_RUNNING) {
    ui->mode = NETUI_RUNNING; ui->joinPage = false;
    ui->delay = net_delay(net); ui->rollback = net_rollback(net);
    status("CONNECTED");
    return ONL_STARTED;
  }
  if(st == NET_FAILED) {
    char m[64]; snprintf(m, sizeof m, "%s", net_status(net));
    for(char* p = m; *p; p++) if(*p >= 'a' && *p <= 'z') *p -= 32;
    status(m);
    ui->mode = NETUI_OFF;
    return ONL_FAILED;
  }
  return ONL_NONE;
}

Netplay* online_session(void) { return ui && ui->mode == NETUI_RUNNING ? net : NULL; }
void online_end(void) { closeNet(); if(ui) { ui->mode = NETUI_OFF; status("DISCONNECTED"); } }
void online_shutdown(void) { closeNet(); }
