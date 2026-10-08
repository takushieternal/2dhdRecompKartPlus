// netplay.c: see netplay.h
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "netplay.h"

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
typedef int socklen_t_;
#define CLOSESOCK closesocket
static uint64_t nowMs(void) { return GetTickCount64(); }
static void sleepMs(int ms) { Sleep(ms); }
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
static uint64_t nowMs(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void sleepMs(int ms) { usleep(ms * 1000); }
#endif

#define MAGIC 0x4E4B4D53u   // "SMKN"
#define VERSION 3           // 3: rollback, room codes, automatic delay
enum { P_HELLO = 1, P_WELCOME = 2, P_INPUT = 3, P_BYE = 4, P_PING = 5, P_PONG = 6, P_PUNCH = 7,
       P_DISCOVER = 8, P_HOSTINFO = 9, P_REJECT = 10 };
#define RING 256             // frames of input history
#define MAXSEND 64           // inputs per packet at most
#define TIMEOUT_MS 8000
#define STUN_COOKIE 0x2112A442u
#define HASH_EVERY 60
#define HASH_HIST 64
#define MAXPUNCH 4
#define MAXLAN 8
#define QMAX 512

typedef struct { uint64_t at; int len; struct sockaddr_in to; uint8_t data[2200]; } Queued;

struct Netplay {
  sock_t s;
  int port;
  int state;
  char status[128];
  bool host;
  uint32_t romCrc;
  // peer
  struct sockaddr_in peer; bool havePeer;
  uint32_t target; uint16_t targetPort;      // client: host address
  uint64_t lastHello, startMs, lastRecv, lastSend;
  // stun
  struct sockaddr_in stun[4]; int nstun; uint8_t stunTx[12]; uint64_t stunSent; int stunTries;
  bool stunOk, stunFail; uint32_t pubIp; uint16_t pubPort;
  // host handshake
  int phase;                                 // 0 waiting for HELLO, 1 measuring, 2 running
  int rttSamples[8], nrtt; uint64_t lastPing;
  uint32_t punchIp[MAXPUNCH]; uint16_t punchPort[MAXPUNCH]; int npunch; uint64_t lastPunch;
  uint8_t sram[0x800]; int sramLen; uint8_t rules;
  int wantDelay; bool wantRollback;
  uint8_t welcome[32 + 0x800]; int welcomeLen;
  // lan
  NetLanGame lan[MAXLAN]; int nlan;
  // session
  int delay; bool rollback; int maxPred;
  uint32_t cur;                              // next frame to simulate
  uint16_t local[RING], rem[RING], used[RING];
  uint32_t localCount, remCount;             // frames [0, count) known
  uint32_t peerAck, peerCur;
  bool rewind; uint32_t rewindFrom;          // earliest frame whose remote input differs from what was used
  // timing
  double rtt; uint32_t echoStamp; uint64_t echoAt; uint64_t lastSyncStall;
  // desync check
  uint32_t nextHash;
  uint32_t hashFrame[HASH_HIST]; uint64_t hashVal[HASH_HIST]; int nhash;
  uint32_t peerHashFrame; uint64_t peerHash; bool desync;
  NetStats st;
  // lag simulation (tests)
  int lagMs, jitterMs, lossPct; uint32_t rng;
  Queued* q; int nq;
};

static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static uint16_t get16(const uint8_t* p) { return p[0] | p[1] << 8; }
static void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint64_t get64(const uint8_t* p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

static bool sockInit(void) {
#ifdef _WIN32
  static bool done = false;
  if(!done) { WSADATA w; if(WSAStartup(MAKEWORD(2, 2), &w)) return false; done = true; }
#endif
  return true;
}

static void setNonBlocking(sock_t s) {
#ifdef _WIN32
  u_long one = 1; ioctlsocket(s, FIONBIO, &one);
#else
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static struct sockaddr_in mkaddr(uint32_t ip, uint16_t port) {
  struct sockaddr_in a; memset(&a, 0, sizeof a);
  a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(ip); a.sin_port = htons(port);
  return a;
}
static bool sameAddr(const struct sockaddr_in* a, const struct sockaddr_in* b) {
  return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}
static void setStatus(Netplay* n, const char* s) { snprintf(n->status, sizeof n->status, "%s", s); }

// ------------------------------------------------------------------ sending (with optional lag simulation)
static void rawSend(Netplay* n, const uint8_t* d, int len, const struct sockaddr_in* to) {
  sendto(n->s, (const char*)d, len, 0, (const struct sockaddr*)to, sizeof *to);
}
static uint32_t rnd(Netplay* n) { n->rng = n->rng * 1103515245u + 12345u; return n->rng >> 8; }
static void flushQueue(Netplay* n) {
  if(!n->nq) return;
  uint64_t t = nowMs();
  int k = 0;
  for(int i = 0; i < n->nq; i++) {
    if(n->q[i].at <= t) rawSend(n, n->q[i].data, n->q[i].len, &n->q[i].to);
    else n->q[k++] = n->q[i];
  }
  n->nq = k;
}
static void sendTo(Netplay* n, const uint8_t* d, int len, const struct sockaddr_in* to) {
  if(!n->lagMs && !n->lossPct && !n->jitterMs) { rawSend(n, d, len, to); return; }
  if(n->lossPct && (int)(rnd(n) % 100) < n->lossPct) return;
  if(n->nq >= QMAX || len > (int)sizeof n->q[0].data) return;
  Queued* e = &n->q[n->nq++];
  int j = n->jitterMs ? (int)(rnd(n) % (n->jitterMs + 1)) : 0;
  uint64_t at = nowMs() + n->lagMs + j;
  // UDP may reorder, but keep it mostly in order like a real path: never earlier than the last one
  if(n->nq > 1 && at < n->q[n->nq - 2].at && !n->jitterMs) at = n->q[n->nq - 2].at;
  e->at = at; e->len = len; e->to = *to; memcpy(e->data, d, len);
  flushQueue(n);
}
static void sendPeer(Netplay* n, const uint8_t* d, int len) { if(n->havePeer) sendTo(n, d, len, &n->peer); }
static int hdr(uint8_t* p, int type) { put32(p, MAGIC); p[4] = type; return 5; }

// ------------------------------------------------------------------ setup
Netplay* net_open(int port, char* err, int errLen) {
  if(!sockInit()) { snprintf(err, errLen, "socket init failed"); return NULL; }
  sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
  if(s == INVALID_SOCKET) { snprintf(err, errLen, "cannot create a UDP socket"); return NULL; }
  struct sockaddr_in a = mkaddr(INADDR_ANY, (uint16_t)port);
  if(bind(s, (struct sockaddr*)&a, sizeof a) != 0) {
    snprintf(err, errLen, "UDP port %d is in use (another copy running?)", port);
    CLOSESOCK(s); return NULL;
  }
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof one);
  setNonBlocking(s);
  Netplay* n = calloc(1, sizeof(Netplay));
  n->s = s;
  struct sockaddr_in b; socklen_t bl = sizeof b;
  getsockname(s, (struct sockaddr*)&b, &bl);
  n->port = ntohs(b.sin_port);
  n->state = NET_IDLE;
  n->lastRecv = n->startMs = nowMs();
  n->peerHashFrame = 0xffffffff;
  n->rng = (uint32_t)nowMs() ^ (uint32_t)(uintptr_t)n;
  if(getenv("SMKNET_LAG")) n->lagMs = atoi(getenv("SMKNET_LAG"));
  if(getenv("SMKNET_JITTER")) n->jitterMs = atoi(getenv("SMKNET_JITTER"));
  if(getenv("SMKNET_LOSS")) n->lossPct = atoi(getenv("SMKNET_LOSS"));
  if(n->lagMs || n->jitterMs || n->lossPct) n->q = calloc(QMAX, sizeof(Queued));
  setStatus(n, "ready");
  return n;
}

int net_localPort(Netplay* n) { return n->port; }

void net_close(Netplay* n) {
  if(!n) return;
  uint8_t b[8]; int l = hdr(b, P_BYE);
  if(n->havePeer) { for(int i = 0; i < 3; i++) rawSend(n, b, l, &n->peer); }
  CLOSESOCK(n->s);
  free(n->q);
  free(n);
}

uint32_t net_lanAddr(void) {
  if(!sockInit()) return 0;
  sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
  if(s == INVALID_SOCKET) return 0;
  struct sockaddr_in a = mkaddr(0x08080808, 53);      // no packet is sent: only picks the route
  uint32_t ip = 0;
  if(connect(s, (struct sockaddr*)&a, sizeof a) == 0) {
    struct sockaddr_in b; socklen_t bl = sizeof b;
    if(getsockname(s, (struct sockaddr*)&b, &bl) == 0) ip = ntohl(b.sin_addr.s_addr);
  }
  CLOSESOCK(s);
  return ip;
}

bool net_resolve(const char* host, uint32_t* ip) {
  if(!sockInit()) return false;
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
  if(getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;
  *ip = ntohl(((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
  freeaddrinfo(res);
  return true;
}

// ------------------------------------------------------------------ STUN (RFC 5389 binding request)
static void stunSend(Netplay* n) {
  uint8_t p[20];
  p[0] = 0; p[1] = 1; p[2] = 0; p[3] = 0;               // Binding Request, no attributes
  p[4] = 0x21; p[5] = 0x12; p[6] = 0xA4; p[7] = 0x42;
  memcpy(p + 8, n->stunTx, 12);
  for(int i = 0; i < n->nstun; i++) rawSend(n, p, 20, &n->stun[i]);
  n->stunSent = nowMs(); n->stunTries++;
}
void net_stunStart(Netplay* n, const uint32_t* ips, const uint16_t* ports, int count) {
  n->nstun = 0; n->stunOk = n->stunFail = false; n->stunTries = 0;
  for(int i = 0; i < count && n->nstun < 4; i++) n->stun[n->nstun++] = mkaddr(ips[i], ports[i]);
  for(int i = 0; i < 12; i++) n->stunTx[i] = (uint8_t)rnd(n);
  if(!n->nstun) { n->stunFail = true; return; }
  stunSend(n);
}
static void stunPoll(Netplay* n) {
  if(!n->nstun || n->stunOk || n->stunFail) return;
  if(nowMs() - n->stunSent > 700) {
    if(n->stunTries >= 4) n->stunFail = true;
    else stunSend(n);
  }
}
static bool stunParse(Netplay* n, const uint8_t* p, int len) {
  if(len < 20 || p[0] != 0x01 || p[1] != 0x01) return false;
  if((uint32_t)(p[4] << 24 | p[5] << 16 | p[6] << 8 | p[7]) != STUN_COOKIE) return false;
  if(memcmp(p + 8, n->stunTx, 12)) return true;          // someone else's transaction: drop
  int alen = p[2] << 8 | p[3], o = 20;
  while(o + 4 <= 20 + alen && o + 4 <= len) {
    int t = p[o] << 8 | p[o + 1], l = p[o + 2] << 8 | p[o + 3];
    const uint8_t* v = p + o + 4;
    if(o + 4 + l > len) break;
    if((t == 0x0020 || t == 0x8020) && l >= 8 && v[1] == 1) {        // XOR-MAPPED-ADDRESS, IPv4
      n->pubPort = (uint16_t)((v[2] << 8 | v[3]) ^ (STUN_COOKIE >> 16));
      n->pubIp = (uint32_t)(v[4] << 24 | v[5] << 16 | v[6] << 8 | v[7]) ^ STUN_COOKIE;
      n->stunOk = true; return true;
    }
    if(t == 0x0001 && l >= 8 && v[1] == 1 && !n->stunOk) {             // MAPPED-ADDRESS (old servers)
      n->pubPort = (uint16_t)(v[2] << 8 | v[3]);
      n->pubIp = (uint32_t)(v[4] << 24 | v[5] << 16 | v[6] << 8 | v[7]);
      n->stunOk = true;
    }
    o += 4 + ((l + 3) & ~3);
  }
  return true;
}
bool net_publicAddr(Netplay* n, uint32_t* ip, uint16_t* port) {
  if(!n->stunOk) return false;
  *ip = n->pubIp; *port = n->pubPort;
  return true;
}
bool net_stunFailed(Netplay* n) { return n->stunFail; }

// ------------------------------------------------------------------ host / client setup
void net_host(Netplay* n, uint32_t romCrc, const uint8_t* sram, int sramLen, uint8_t rules, int delay, bool rollback) {
  n->host = true; n->romCrc = romCrc; n->state = NET_HOSTING; n->phase = 0;
  n->sramLen = sramLen > 0x800 ? 0x800 : sramLen; memcpy(n->sram, sram, n->sramLen);
  n->rules = rules; n->wantDelay = delay; n->wantRollback = rollback;
  setStatus(n, "waiting for player 2");
}
void net_punch(Netplay* n, uint32_t ip, uint16_t port) {
  if(n->npunch < MAXPUNCH) { n->punchIp[n->npunch] = ip; n->punchPort[n->npunch] = port; n->npunch++; }
  n->lastPunch = 0;
}
void net_join(Netplay* n, uint32_t ip, uint16_t port, uint32_t romCrc) {
  n->host = false; n->romCrc = romCrc; n->state = NET_JOINING;
  n->target = ip; n->targetPort = port; n->peer = mkaddr(ip, port); n->havePeer = true;
  n->lastHello = 0; n->startMs = nowMs();
  setStatus(n, "connecting");
}
void net_lanSearch(Netplay* n, int port) {
  uint8_t p[16]; int l = hdr(p, P_DISCOVER); p[l++] = VERSION;
  struct sockaddr_in b = mkaddr(0xffffffffu, (uint16_t)port), lo = mkaddr(0x7f000001u, (uint16_t)port);
  rawSend(n, p, l, &b);
  rawSend(n, p, l, &lo);
}
int net_lanGames(Netplay* n, NetLanGame* out, int max) {
  int k = n->nlan < max ? n->nlan : max;
  memcpy(out, n->lan, k * sizeof(NetLanGame));
  return k;
}

static void startSession(Netplay* n, int delay, bool rollback) {
  n->delay = delay < 1 ? 1 : delay > 15 ? 15 : delay;
  n->rollback = rollback;
  n->maxPred = rollback ? NET_MAX_ROLLBACK : 0;
  n->cur = 0;
  memset(n->local, 0, sizeof n->local); memset(n->rem, 0, sizeof n->rem); memset(n->used, 0, sizeof n->used);
  n->localCount = n->remCount = (uint32_t)n->delay;   // the first `delay` frames are neutral on both sides
  n->peerAck = n->peerCur = 0;
  n->nextHash = HASH_EVERY; n->nhash = 0; n->peerHashFrame = 0xffffffff; n->desync = false;
  n->state = NET_RUNNING;
  n->lastRecv = nowMs();
  memset(&n->st, 0, sizeof n->st);
  setStatus(n, "connected");
}

static int autoDelay(Netplay* n, double rttMs) {
  if(n->wantDelay > 0) return n->wantDelay;
  double oneWay = rttMs / 2.0 / 16.67;             // frames
  if(n->wantRollback) return oneWay < 1.5 ? 1 : oneWay < 4 ? 2 : 3;
  int d = (int)ceil(oneWay + 0.5) + 1;
  return d < 2 ? 2 : d > 12 ? 12 : d;
}

static void sendWelcome(Netplay* n) {
  uint8_t* w = n->welcome;
  int l = hdr(w, P_WELCOME);
  w[l++] = VERSION; w[l++] = (uint8_t)n->delay; w[l++] = n->rules; w[l++] = n->rollback;
  put16(w + l, (uint16_t)n->sramLen); l += 2;
  memcpy(w + l, n->sram, n->sramLen); l += n->sramLen;
  n->welcomeLen = l;
  sendPeer(n, w, l);
}

// ------------------------------------------------------------------ packets
static void sendInputs(Netplay* n) {
  uint8_t p[32 + MAXSEND * 2 + 64];
  int l = hdr(p, P_INPUT);
  uint32_t first = n->peerAck;
  if(n->localCount > MAXSEND && first < n->localCount - MAXSEND) first = n->localCount - MAXSEND;
  if(first > n->localCount) first = n->localCount;
  int cnt = (int)(n->localCount - first);
  put32(p + l, n->cur); l += 4;
  put32(p + l, n->remCount); l += 4;
  put32(p + l, first); l += 4;
  p[l++] = (uint8_t)cnt;
  for(int i = 0; i < cnt; i++) { put16(p + l, n->local[(first + i) % RING]); l += 2; }
  uint32_t hf = 0xffffffff; uint64_t hv = 0;
  if(n->nhash) { hf = n->hashFrame[(n->nhash - 1) % HASH_HIST]; hv = n->hashVal[(n->nhash - 1) % HASH_HIST]; }
  put32(p + l, hf); l += 4; put64(p + l, hv); l += 8;
  put32(p + l, (uint32_t)nowMs()); l += 4;
  put32(p + l, n->echoStamp); l += 4;
  uint64_t age = n->echoAt ? nowMs() - n->echoAt : 0;
  put16(p + l, (uint16_t)(age > 60000 ? 60000 : age)); l += 2;
  sendPeer(n, p, l);
  n->lastSend = nowMs();
}

static void handleInput(Netplay* n, const uint8_t* p, int len) {
  if(len < 18) return;
  uint32_t peerCur = get32(p + 5), ack = get32(p + 9), first = get32(p + 13);
  int cnt = p[17];
  if(len < 18 + cnt * 2 + 22) return;
  if(peerCur > n->peerCur) n->peerCur = peerCur;
  if(ack > n->peerAck && ack <= n->localCount) n->peerAck = ack;
  for(int i = 0; i < cnt; i++) {
    uint32_t f = first + i;
    if(f < n->remCount) continue;
    if(f != n->remCount) break;                 // only contiguous (redundancy fills gaps next time)
    if(f >= n->cur + RING / 2) break;
    uint16_t v = get16(p + 18 + i * 2);
    n->rem[f % RING] = v;
    n->remCount = f + 1;
    if(f < n->cur && v != n->used[f % RING] && (!n->rewind || f < n->rewindFrom)) { n->rewind = true; n->rewindFrom = f; }
  }
  int o = 18 + cnt * 2;
  uint32_t hf = get32(p + o); uint64_t hv = get64(p + o + 4);
  if(hf != 0xffffffff) { n->peerHashFrame = hf; n->peerHash = hv; }
  uint32_t stamp = get32(p + o + 12), echo = get32(p + o + 16); uint16_t age = get16(p + o + 20);
  n->echoStamp = stamp; n->echoAt = nowMs();
  if(echo) {
    double r = (double)((uint32_t)nowMs() - echo) - age;
    if(r >= 0 && r < 5000) n->rtt = n->rtt ? n->rtt * 0.9 + r * 0.1 : r;
  }
}

static void addLan(Netplay* n, const struct sockaddr_in* from, const uint8_t* p, int len) {
  uint32_t ip = ntohl(from->sin_addr.s_addr); uint16_t port = ntohs(from->sin_port);
  for(int i = 0; i < n->nlan; i++) if(n->lan[i].ip == ip && n->lan[i].port == port) return;
  if(n->nlan >= MAXLAN) return;
  NetLanGame* g = &n->lan[n->nlan++];
  g->ip = ip; g->port = port;
  int nl = len - 6; if(nl < 0) nl = 0; if(nl > 39) nl = 39;
  memcpy(g->name, p + 6, nl); g->name[nl] = 0;
}

static void receive(Netplay* n) {
  uint8_t buf[4096];
  for(;;) {
    struct sockaddr_in from; socklen_t fl = sizeof from;
    int r = recvfrom(n->s, (char*)buf, sizeof buf, 0, (struct sockaddr*)&from, &fl);
    if(r <= 0) break;
    if(stunParse(n, buf, r)) continue;
    if(r < 5 || get32(buf) != MAGIC) continue;
    int t = buf[4];
    bool fromPeer = n->havePeer && sameAddr(&from, &n->peer);
    if(t == P_DISCOVER) {                                  // LAN search: answer if we're a waiting host
      if(n->host && n->state == NET_HOSTING) {
        uint8_t p[64]; int l = hdr(p, P_HOSTINFO); p[l++] = VERSION;
        char name[40] = "smkplay";
        gethostname(name, sizeof name - 1);
        int nl = (int)strlen(name); memcpy(p + l, name, nl); l += nl;
        rawSend(n, p, l, &from);
      }
      continue;
    }
    if(t == P_HOSTINFO) { if(r >= 6 && buf[5] == VERSION) addLan(n, &from, buf, r); continue; }
    if(n->host) {
      if(t == P_HELLO) {
        if(r < 14) continue;
        if(buf[5] != VERSION || get32(buf + 6) != n->romCrc) {
          uint8_t p[8]; int l = hdr(p, P_REJECT); p[l++] = buf[5] != VERSION ? 1 : 2;
          rawSend(n, p, l, &from); continue;
        }
        if(n->state == NET_HOSTING && n->phase == 0) {
          n->peer = from; n->havePeer = true; n->phase = 1; n->nrtt = 0; n->lastPing = 0; n->startMs = nowMs();
          setStatus(n, "player 2 found, measuring the connection");
        } else if(fromPeer && n->state == NET_RUNNING && n->welcomeLen) {
          sendPeer(n, n->welcome, n->welcomeLen);          // our WELCOME got lost
        } else if(!fromPeer) {
          uint8_t p[8]; int l = hdr(p, P_REJECT); p[l++] = 3; rawSend(n, p, l, &from);   // busy
        }
        continue;
      }
      if(!fromPeer) continue;
      n->lastRecv = nowMs();
      if(t == P_PONG && n->phase == 1 && r >= 9 && n->nrtt < 8) {
        int rt = (int)((uint32_t)nowMs() - get32(buf + 5));
        if(rt >= 0 && rt < 5000) n->rttSamples[n->nrtt++] = rt;
      }
      else if(t == P_INPUT && n->state == NET_RUNNING) handleInput(n, buf, r);
      else if(t == P_BYE) { n->state = NET_CLOSED; setStatus(n, "the other player left"); }
      continue;
    }
    // client
    bool fromHost = from.sin_addr.s_addr == htonl(n->target);
    if(t == P_PUNCH && fromHost && n->state == NET_JOINING) {      // host opened its NAT: answer from there
      n->peer = from; n->lastHello = 0; continue;
    }
    if(!fromPeer && !(fromHost && (t == P_WELCOME || t == P_PING || t == P_REJECT))) continue;
    n->lastRecv = nowMs();
    if(t == P_PING && r >= 9) {
      if(!fromPeer) n->peer = from;
      uint8_t p[12]; int l = hdr(p, P_PONG); memcpy(p + l, buf + 5, 4); l += 4;
      sendPeer(n, p, l);
      if(n->state == NET_JOINING) setStatus(n, "host found, measuring the connection");
    } else if(t == P_REJECT) {
      n->state = NET_FAILED;
      setStatus(n, r > 5 && buf[5] == 1 ? "the host runs a different version" : r > 5 && buf[5] == 2 ? "the host has a different ROM" : "the host already has a player 2");
    } else if(t == P_WELCOME && n->state == NET_JOINING && r >= 12 && buf[5] == VERSION) {
      n->peer = from;
      int sl = get16(buf + 9);
      if(sl > 0x800 || r < 11 + sl) continue;
      n->rules = buf[7]; n->sramLen = sl; memcpy(n->sram, buf + 11, sl);
      startSession(n, buf[6], buf[8] != 0);
    } else if(t == P_INPUT && n->state == NET_RUNNING) handleInput(n, buf, r);
    else if(t == P_BYE) { n->state = NET_CLOSED; setStatus(n, "the other player left"); }
  }
}

int net_poll(Netplay* n) {
  flushQueue(n);
  receive(n);
  stunPoll(n);
  uint64_t t = nowMs();
  if(n->state == NET_HOSTING) {
    if(n->npunch && t - n->lastPunch > 250) {
      uint8_t p[8]; int l = hdr(p, P_PUNCH);
      for(int i = 0; i < n->npunch; i++) { struct sockaddr_in a = mkaddr(n->punchIp[i], n->punchPort[i]); sendTo(n, p, l, &a); }
      n->lastPunch = t;
    }
    if(n->phase == 1) {
      if(t - n->lastPing > 80 && n->nrtt < 6) {
        uint8_t p[12]; int l = hdr(p, P_PING); put32(p + l, (uint32_t)t); l += 4;
        sendPeer(n, p, l); n->lastPing = t;
      }
      if(n->nrtt >= 5 || t - n->startMs > 2000) {
        double rtt = 100;
        if(n->nrtt) {           // median
          int s[8]; memcpy(s, n->rttSamples, sizeof s);
          for(int i = 0; i < n->nrtt; i++) for(int j = i + 1; j < n->nrtt; j++) if(s[j] < s[i]) { int x = s[i]; s[i] = s[j]; s[j] = x; }
          rtt = s[n->nrtt / 2];
        }
        n->rtt = rtt;
        n->delay = autoDelay(n, rtt); n->rollback = n->wantRollback;
        startSession(n, n->delay, n->rollback);
        sendWelcome(n);
      }
    }
  } else if(n->state == NET_JOINING) {
    if(t - n->lastHello > 250) {
      uint8_t p[16]; int l = hdr(p, P_HELLO); p[l++] = VERSION; put32(p + l, n->romCrc); l += 4; put32(p + l, (uint32_t)t); l += 4;
      sendPeer(n, p, l);
      n->lastHello = t;
    }
    if(t - n->startMs > 20000) {
      n->state = NET_FAILED;
      setStatus(n, "no answer from the host");
    }
  } else if(n->state == NET_RUNNING) {
    if(t - n->lastRecv > TIMEOUT_MS) { n->state = NET_CLOSED; setStatus(n, "connection lost"); }
  }
  return n->state;
}

const char* net_status(Netplay* n) { return n->status; }
bool net_isHost(Netplay* n) { return n->host; }
int net_sram(Netplay* n, uint8_t* out, int cap) { int l = n->sramLen < cap ? n->sramLen : cap; memcpy(out, n->sram, l); return l; }
uint8_t net_rules(Netplay* n) { return n->rules; }
int net_delay(Netplay* n) { return n->delay; }
bool net_rollback(Netplay* n) { return n->rollback; }
uint32_t net_frameNo(Netplay* n) { return n->cur; }
uint32_t net_confirmedFrame(Netplay* n) { return n->remCount < n->cur ? n->remCount : n->cur; }

// ------------------------------------------------------------------ running
static uint16_t remoteFor(Netplay* n, uint32_t f) {
  if(f < n->remCount) return n->rem[f % RING];
  return n->remCount ? n->rem[(n->remCount - 1) % RING] : 0;      // prediction: repeat the last known
}
static void runOne(Netplay* n, const NetEmu* e, uint32_t f, bool render) {
  e->save(e->ctx, (int)(f % NET_SLOTS));                 // state before frame f
  uint16_t me = n->local[f % RING], them = remoteFor(n, f);
  n->used[f % RING] = them;
  e->run(e->ctx, n->host ? me : them, n->host ? them : me, render);
}

static void checkHashes(Netplay* n, const NetEmu* e) {
  // frame h is final when all its inputs are known; the state after it is slot (h+1)
  while(n->nextHash < n->remCount && n->nextHash + 1 < n->cur && n->nextHash + NET_SLOTS > n->cur) {
    uint32_t h = n->nextHash;
    uint64_t v = e->hashSlot(e->ctx, (int)((h + 1) % NET_SLOTS));
    n->hashFrame[n->nhash % HASH_HIST] = h; n->hashVal[n->nhash % HASH_HIST] = v; n->nhash++;
    n->nextHash += HASH_EVERY;
  }
  if(n->nextHash + NET_SLOTS <= n->cur) n->nextHash = (n->cur / HASH_EVERY + 1) * HASH_EVERY;   // fell behind (shouldn't happen)
  if(n->peerHashFrame != 0xffffffff) {
    for(int i = 0; i < n->nhash && i < HASH_HIST; i++) {
      int k = (n->nhash - 1 - i) % HASH_HIST;
      if(n->hashFrame[k] == n->peerHashFrame) { if(n->hashVal[k] != n->peerHash) n->desync = true; break; }
    }
  }
}

int net_tick(Netplay* n, uint16_t localPad, const NetEmu* e) {
  net_poll(n);
  if(n->state != NET_RUNNING) return -1;
  // 1. our input for frame cur+delay (once per advanced frame)
  if(n->localCount == n->cur + n->delay) { n->local[n->localCount % RING] = localPad; n->localCount++; }
  // 2. late remote inputs that differ from the prediction: rewind and replay without rendering
  if(n->rewind) {
    n->rewind = false;
    uint32_t from = n->rewindFrom;
    if(from + NET_SLOTS - 1 < n->cur) from = n->cur - (NET_SLOTS - 1);      // can't happen with maxPred < slots
    e->load(e->ctx, (int)(from % NET_SLOTS));
    for(uint32_t f = from; f < n->cur; f++) runOne(n, e, f, false);
    int depth = (int)(n->cur - from);
    n->st.lastRollback = depth; if(depth > n->st.maxRollback) n->st.maxRollback = depth;
    n->st.rollbacks++;
  }
  checkHashes(n, e);
  // 3. advance?
  bool can = n->cur < n->remCount + (uint32_t)n->maxPred;
  // time sync: if we're running ahead of the other side, give it a frame now and then
  double ahead = 0;
  if(n->rollback && n->peerCur) {
    double est = n->peerCur + (n->rtt / 2.0) / 16.67;
    ahead = (double)n->cur - est;
    n->st.advantage = (int)lround(ahead);
    if(can && ahead >= 1.5 && nowMs() - n->lastSyncStall > 200) { can = false; n->lastSyncStall = nowMs(); n->st.stalls++; }
  }
  int shown = 0;
  if(can) {
    runOne(n, e, n->cur, true);
    n->cur++;
    shown = 1;
  }
  if(shown || nowMs() - n->lastSend > 15) sendInputs(n);
  return shown;
}

bool net_checkpoint(Netplay* n, uint32_t frame, uint64_t* hash) {
  for(int i = 0; i < n->nhash && i < HASH_HIST; i++) {
    int k = (n->nhash - 1 - i) % HASH_HIST;
    if(n->hashFrame[k] == frame) { *hash = n->hashVal[k]; return true; }
  }
  return false;
}

void net_stats(Netplay* n, NetStats* s) {
  *s = n->st;
  s->rttMs = (int)lround(n->rtt); s->delay = n->delay; s->rollback = n->rollback; s->desync = n->desync;
}

void net_linger(Netplay* n, int ms) {
  uint64_t end = nowMs() + ms;
  while(nowMs() < end) {
    net_poll(n);
    if(n->state == NET_RUNNING) sendInputs(n);
    sleepMs(10);
  }
  // let queued (lagged) packets out
  while(n->nq && nowMs() < end + 2000) { flushQueue(n); sleepMs(5); }
}

// ------------------------------------------------------------------ room codes
static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
static uint32_t codeCheck(uint64_t v) {
  uint32_t h = 2166136261u;
  for(int i = 0; i < 6; i++) { h ^= (uint8_t)(v >> (8 * i)); h *= 16777619u; }
  return (h ^ (h >> 12) ^ (h >> 24)) & 0xfff;
}
void net_codeEncode(uint32_t ip, uint16_t port, char* out, int cap) {
  uint64_t v = (uint64_t)ip << 16 | port;
  uint64_t all = v << 12 | codeCheck(v);                 // 60 bits
  char s[16]; int k = 0;
  for(int i = 11; i >= 0; i--) {
    s[k++] = B32[(all >> (i * 5)) & 31];
    if(i == 8 || i == 4) s[k++] = '-';
  }
  s[k] = 0;
  snprintf(out, cap, "%s", s);
}
bool net_codeDecode(const char* s, uint32_t* ip, uint16_t* port, int defPort, bool resolve) {
  char t[128]; int k = 0;
  for(const char* p = s; *p && k < 127; p++) if(*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') t[k++] = *p;
  t[k] = 0;
  if(!k) return false;
  bool addr = strchr(t, '.') || strchr(t, ':');
  if(!addr) {
    uint64_t all = 0; int nd = 0;
    for(int i = 0; t[i]; i++) {
      char c = t[i];
      if(c == '-') continue;
      if(c >= 'a' && c <= 'z') c -= 32;
      if(c == 'O') c = '0';
      if(c == 'I' || c == 'L') c = '1';
      const char* q = strchr(B32, c);
      if(!q || !c) { addr = true; break; }
      all = all << 5 | (uint64_t)(q - B32); nd++;
    }
    if(!addr) {
      if(nd != 12) return false;
      uint64_t v = all >> 12;
      if(codeCheck(v) != (all & 0xfff)) return false;
      *ip = (uint32_t)(v >> 16); *port = (uint16_t)v;
      return *ip != 0 && *port != 0;
    }
  }
  // "a.b.c.d[:port]" or "name[:port]"
  char h[128]; snprintf(h, sizeof h, "%s", t);
  int pt = defPort;
  char* c = strrchr(h, ':');
  if(c) { *c = 0; pt = atoi(c + 1); if(pt <= 0 || pt > 65535) return false; }
  unsigned a, b, cc, d; char extra;
  if(sscanf(h, "%u.%u.%u.%u%c", &a, &b, &cc, &d, &extra) == 4 && a < 256 && b < 256 && cc < 256 && d < 256) {
    *ip = a << 24 | b << 16 | cc << 8 | d; *port = (uint16_t)pt; return true;
  }
  if(!resolve || !net_resolve(h, ip)) return false;
  *port = (uint16_t)pt;
  return true;
}
