// upnp.c: see upnp.h. Minimal UPnP IGD client: SSDP discovery, device description, SOAP calls.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "upnp.h"

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#endif

static char ctlHost[64], ctlPath[256], ctlService[128];
static int ctlPort;
static uint32_t localIp;

static void setTimeout(sock_t s, int ms) {
#ifdef _WIN32
  DWORD t = ms;
#else
  struct timeval t = {ms / 1000, (ms % 1000) * 1000};
#endif
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof t);
}

static bool parseUrl(const char* url, char* host, int hostLen, int* port, char* path, int pathLen) {
  const char* p = url;
  if(!strncmp(p, "http://", 7)) p += 7; else return false;
  const char* slash = strchr(p, '/');
  int hl = slash ? (int)(slash - p) : (int)strlen(p);
  char hp[128]; if(hl >= (int)sizeof hp) return false;
  memcpy(hp, p, hl); hp[hl] = 0;
  *port = 80;
  char* c = strchr(hp, ':');
  if(c) { *c = 0; *port = atoi(c + 1); }
  snprintf(host, hostLen, "%s", hp);
  snprintf(path, pathLen, "%s", slash ? slash : "/");
  return true;
}

// one HTTP request; returns the status code (0 on failure) and the body (malloc'ed)
static int http(const char* host, int port, const char* req, char** body) {
  *body = NULL;
  struct sockaddr_in a; memset(&a, 0, sizeof a);
  a.sin_family = AF_INET; a.sin_port = htons(port);
  if(inet_pton(AF_INET, host, &a.sin_addr) != 1) {
    struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET;
    if(getaddrinfo(host, NULL, &hints, &res) || !res) return 0;
    a.sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr; freeaddrinfo(res);
  }
  sock_t s = socket(AF_INET, SOCK_STREAM, 0);
  if(s == INVALID_SOCKET) return 0;
  setTimeout(s, 3000);
  if(connect(s, (struct sockaddr*)&a, sizeof a) != 0) { CLOSESOCK(s); return 0; }
  int len = (int)strlen(req), off = 0;
  while(off < len) { int w = send(s, req + off, len - off, 0); if(w <= 0) { CLOSESOCK(s); return 0; } off += w; }
  int cap = 16384, n = 0; char* buf = malloc(cap + 1);
  for(;;) {
    if(n == cap) { if(cap >= 1 << 20) break; cap *= 2; buf = realloc(buf, cap + 1); }
    int r = recv(s, buf + n, cap - n, 0);
    if(r <= 0) break;
    n += r;
  }
  CLOSESOCK(s);
  buf[n] = 0;
  int status = 0;
  if(sscanf(buf, "HTTP/%*s %d", &status) != 1) { free(buf); return 0; }
  char* hend = strstr(buf, "\r\n\r\n");
  if(!hend) { free(buf); return status; }
  *hend = 0;
  char* b = hend + 4;
  bool chunked = false;
  for(char* p = buf; *p; p++) *p = (char)tolower((unsigned char)*p);
  if(strstr(buf, "transfer-encoding: chunked")) chunked = true;
  char* out = malloc(strlen(b) + 1); int o = 0;
  if(chunked) {
    char* p = b;
    for(;;) {
      int sz = (int)strtol(p, NULL, 16);
      char* nl = strstr(p, "\r\n"); if(!nl || sz <= 0) break;
      p = nl + 2;
      if((int)strlen(p) < sz) sz = (int)strlen(p);
      memcpy(out + o, p, sz); o += sz; p += sz;
      if(!strncmp(p, "\r\n", 2)) p += 2;
    }
    out[o] = 0;
  } else strcpy(out, b);
  free(buf);
  *body = out;
  return status;
}

static bool tagValue(const char* xml, const char* tag, char* out, int cap) {
  char open[96]; snprintf(open, sizeof open, "<%s>", tag);
  const char* p = strstr(xml, open);
  if(!p) {                                      // namespaced: <ns:tag>
    char alt[96]; snprintf(alt, sizeof alt, ":%s>", tag);
    p = strstr(xml, alt);
    if(!p) return false;
    p += strlen(alt);
  } else p += strlen(open);
  const char* e = strchr(p, '<');
  if(!e) return false;
  int l = (int)(e - p); if(l >= cap) l = cap - 1;
  while(l > 0 && isspace((unsigned char)*p)) { p++; l--; }
  memcpy(out, p, l); out[l] = 0;
  while(l > 0 && isspace((unsigned char)out[l - 1])) out[--l] = 0;
  return true;
}

static int strncasecmp_portable(const char* a, const char* b, int n) {
  for(int i = 0; i < n; i++) {
    int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
    if(x != y || !x) return x - y;
  }
  return 0;
}

static bool discover(char* location, int cap) {
  sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
  if(s == INVALID_SOCKET) return false;
  struct sockaddr_in g; memset(&g, 0, sizeof g);
  g.sin_family = AF_INET; g.sin_port = htons(1900); inet_pton(AF_INET, "239.255.255.250", &g.sin_addr);
  const char* over = getenv("SMK_UPNP_SSDP");
  if(over) {
    char h[64]; snprintf(h, sizeof h, "%s", over);
    char* c = strchr(h, ':'); if(c) { *c = 0; g.sin_port = htons(atoi(c + 1)); }
    inet_pton(AF_INET, h, &g.sin_addr);
  }
  static const char* sts[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                              "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                              "urn:schemas-upnp-org:service:WANIPConnection:1"};
  for(int round = 0; round < 2; round++) {
    for(int i = 0; i < 3; i++) {
      char m[512];
      int l = snprintf(m, sizeof m, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n", sts[i]);
      sendto(s, m, l, 0, (struct sockaddr*)&g, sizeof g);
    }
    setTimeout(s, 1200);
    for(;;) {
      char buf[2048];
      int r = recv(s, buf, sizeof buf - 1, 0);
      if(r <= 0) break;
      buf[r] = 0;
      for(char* p = buf; *p; p++) {
        if((p == buf || p[-1] == '\n') && !strncasecmp_portable(p, "location:", 9)) {
          p += 9; while(*p == ' ') p++;
          int k = 0; while(p[k] && p[k] != '\r' && p[k] != '\n' && k < cap - 1) { location[k] = p[k]; k++; }
          location[k] = 0;
          CLOSESOCK(s);
          return true;
        }
      }
    }
  }
  CLOSESOCK(s);
  return false;
}

static bool findControl(const char* location, char* err, int errLen) {
  char host[64], path[256]; int port;
  if(!parseUrl(location, host, sizeof host, &port, path, sizeof path)) { snprintf(err, errLen, "router answered with an odd address"); return false; }
  char req[512];
  snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", path, host, port);
  char* body;
  int st = http(host, port, req, &body);
  if(st != 200 || !body) { free(body); snprintf(err, errLen, "router description unavailable"); return false; }
  const char* want[] = {"WANIPConnection", "WANPPPConnection"};
  bool ok = false;
  for(int w = 0; w < 2 && !ok; w++) {
    for(const char* p = strstr(body, "<service>"); p; p = strstr(p + 9, "<service>")) {
      const char* e = strstr(p, "</service>"); if(!e) break;
      char blk[2048]; int l = (int)(e - p); if(l >= (int)sizeof blk) l = sizeof blk - 1;
      memcpy(blk, p, l); blk[l] = 0;
      char type[128], ctl[256];
      if(!tagValue(blk, "serviceType", type, sizeof type) || !strstr(type, want[w])) continue;
      if(!tagValue(blk, "controlURL", ctl, sizeof ctl)) continue;
      snprintf(ctlService, sizeof ctlService, "%s", type);
      char base[256] = "";
      tagValue(body, "URLBase", base, sizeof base);
      if(!strncmp(ctl, "http://", 7)) parseUrl(ctl, ctlHost, sizeof ctlHost, &ctlPort, ctlPath, sizeof ctlPath);
      else {
        if(base[0] && parseUrl(base, ctlHost, sizeof ctlHost, &ctlPort, ctlPath, sizeof ctlPath)) {}
        else { snprintf(ctlHost, sizeof ctlHost, "%s", host); ctlPort = port; }
        snprintf(ctlPath, sizeof ctlPath, "%s%s", ctl[0] == '/' ? "" : "/", ctl);
      }
      ok = true; break;
    }
  }
  free(body);
  if(!ok) snprintf(err, errLen, "router has no port forwarding service");
  return ok;
}

static int soap(const char* action, const char* args, char** body) {
  char env[2048];
  snprintf(env, sizeof env,
    "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
    action, ctlService, args, action);
  char req[4096];
  snprintf(req, sizeof req,
    "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"%s#%s\"\r\n"
    "Content-Length: %d\r\nConnection: close\r\n\r\n%s", ctlPath, ctlHost, ctlPort, ctlService, action, (int)strlen(env), env);
  return http(ctlHost, ctlPort, req, body);
}

static uint32_t routeIp(const char* host) {
  sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
  if(s == INVALID_SOCKET) return 0;
  struct sockaddr_in a; memset(&a, 0, sizeof a);
  a.sin_family = AF_INET; a.sin_port = htons(1900);
  if(inet_pton(AF_INET, host, &a.sin_addr) != 1) { CLOSESOCK(s); return 0; }
  uint32_t ip = 0;
  if(connect(s, (struct sockaddr*)&a, sizeof a) == 0) {
    struct sockaddr_in b; socklen_t bl = sizeof b;
    if(getsockname(s, (struct sockaddr*)&b, &bl) == 0) ip = ntohl(b.sin_addr.s_addr);
  }
  CLOSESOCK(s);
  return ip;
}

bool upnp_mapUdp(int port, uint32_t* externalIp, char* err, int errLen) {
#ifdef _WIN32
  WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
  *externalIp = 0;
  char loc[512];
  if(!discover(loc, sizeof loc)) { snprintf(err, errLen, "no UPnP router found"); return false; }
  if(!findControl(loc, err, errLen)) return false;
  localIp = routeIp(ctlHost);
  if(!localIp) { snprintf(err, errLen, "can't tell this PC's LAN address"); return false; }
  char ipStr[32];
  snprintf(ipStr, sizeof ipStr, "%u.%u.%u.%u", localIp >> 24, (localIp >> 16) & 255, (localIp >> 8) & 255, localIp & 255);
  bool ok = false;
  static const int leases[] = {0, 7200};
  for(int i = 0; i < 2 && !ok; i++) {
    char args[1024];
    snprintf(args, sizeof args,
      "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>"
      "<NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
      "<NewPortMappingDescription>Super Mario Kart netplay</NewPortMappingDescription><NewLeaseDuration>%d</NewLeaseDuration>",
      port, port, ipStr, leases[i]);
    char* body;
    int st = soap("AddPortMapping", args, &body);
    if(st == 200) ok = true;
    else {
      char code[16] = "";
      if(body) tagValue(body, "errorCode", code, sizeof code);
      snprintf(err, errLen, st ? "router refused the port forward (error %s)" : "router didn't answer", code[0] ? code : "?");
    }
    free(body);
  }
  if(!ok) return false;
  char* body;
  if(soap("GetExternalIPAddress", "", &body) == 200 && body) {
    char ext[32];
    unsigned a, b, c, d;
    if(tagValue(body, "NewExternalIPAddress", ext, sizeof ext) && sscanf(ext, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
      *externalIp = a << 24 | b << 16 | c << 8 | d;
  }
  free(body);
  return true;
}

void upnp_unmapUdp(int port) {
  if(!ctlHost[0]) return;
  char args[512];
  snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>", port);
  char* body;
  soap("DeletePortMapping", args, &body);
  free(body);
}
