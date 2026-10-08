// options.c: see options.h
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "options.h"
#include "font5x7.h"

// ------------------------------------------------------------------ settings file
static const char* const btnKey[SB_COUNT] = {"b", "y", "select", "start", "up", "down", "left", "right", "a", "x", "l", "r"};
static const char* const btnName[SB_COUNT] = {"B", "Y", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT", "A", "X", "L", "R"};

void opt_defaultBindings(Options* o) {
  // SDL scancodes: Z, A, RShift, Return, arrows, X, S, Q, W (same keys as earlier versions)
  static const int keys[SB_COUNT] = {29, 4, 229, 40, 82, 81, 80, 79, 27, 22, 20, 26};
  // SDL game controller: A(bottom)=B, X(left)=Y, Back, Start, D-pad, B(right)=A, Y(top)=X, shoulders
  static const int pads[SB_COUNT] = {0, 2, 4, 6, 11, 12, 13, 14, 1, 3, 9, 10};
  memcpy(o->keyBind, keys, sizeof keys);
  memcpy(o->padBind, pads, sizeof pads);
}

void opt_defaults(Options* o) {
  memset(o, 0, sizeof(*o));
  o->hd2dWalls = true;
  o->hd2dPost = true;
  o->recompiled = true;
  o->scale = 3;
  o->turboMask = 0x0100;      // A (items)
  o->turboRate = 4;           // 15 Hz
  opt_defaultBindings(o);
}

bool opt_load(Options* o, const char* path) {
  FILE* f = fopen(path, "r");
  if(!f) return false;
  char line[256];
  while(fgets(line, sizeof line, f)) {
    char key[64]; int v;
    if(sscanf(line, " %63[a-zA-Z0-9_] = %i", key, &v) != 2) continue;
    if(!strcmp(key, "widescreen")) o->widescreen = v;
    else if(!strcmp(key, "hd2d")) o->hd2d = v;
    else if(!strcmp(key, "hd2d_walls")) o->hd2dWalls = v;
    else if(!strcmp(key, "hd2d_post")) o->hd2dPost = v;
    else if(!strcmp(key, "hd_mode7")) o->hdMode7 = v;
    else if(!strcmp(key, "recompiled")) o->recompiled = v;
    else if(!strcmp(key, "fullscreen")) o->fullscreen = v;
    else if(!strcmp(key, "scale")) o->scale = v < 2 ? 2 : v > 6 ? 6 : v;
    else if(!strcmp(key, "auto_gas")) o->autoGas = v;
    else if(!strcmp(key, "turbo")) o->turbo = v;
    else if(!strcmp(key, "turbo_buttons")) o->turboMask = (uint16_t)(v & 0x0fff);
    else if(!strcmp(key, "turbo_rate")) o->turboRate = v < 2 ? 2 : v > 8 ? 8 : (v & ~1);
    else if(!strcmp(key, "unlock_everything")) o->unlockAll = v;
    else if(!strcmp(key, "mode_200cc")) o->mode200 = v;
    else {
      for(int i = 0; i < SB_COUNT; i++) {
        char k[32];
        snprintf(k, sizeof k, "key_%s", btnKey[i]); if(!strcmp(key, k)) o->keyBind[i] = v;
        snprintf(k, sizeof k, "pad_%s", btnKey[i]); if(!strcmp(key, k)) o->padBind[i] = v;
      }
    }
  }
  fclose(f);
  return true;
}

bool opt_save(const Options* o, const char* path) {
  FILE* f = fopen(path, "w");
  if(!f) return false;
  fprintf(f, "# smkplay settings (edited by the in-game Enhancements menu)\n");
  fprintf(f, "widescreen = %d\nhd2d = %d\nhd2d_walls = %d\nhd2d_post = %d\nhd_mode7 = %d\nrecompiled = %d\nfullscreen = %d\nscale = %d\n",
          o->widescreen, o->hd2d, o->hd2dWalls, o->hd2dPost, o->hdMode7, o->recompiled, o->fullscreen, o->scale);
  fprintf(f, "auto_gas = %d\nturbo = %d\nturbo_buttons = 0x%03x\nturbo_rate = %d\nunlock_everything = %d\nmode_200cc = %d\n",
          o->autoGas, o->turbo, o->turboMask, o->turboRate, o->unlockAll, o->mode200);
  fprintf(f, "# key_*: SDL scancodes, pad_*: SDL game controller buttons (100/101 = left/right trigger, -1 = none)\n");
  for(int i = 0; i < SB_COUNT; i++) fprintf(f, "key_%s = %d\n", btnKey[i], o->keyBind[i]);
  for(int i = 0; i < SB_COUNT; i++) fprintf(f, "pad_%s = %d\n", btnKey[i], o->padBind[i]);
  fclose(f);
  return true;
}

// ------------------------------------------------------------------ pad transforms
uint16_t opt_transformPad(const Options* o, PadState* st, uint16_t raw, bool racing) {
  uint16_t out = raw;
  if(o->turbo) {
    int rate = o->turboRate < 2 ? 2 : o->turboRate, half = rate / 2;
    for(int i = 0; i < SB_COUNT; i++) {
      uint16_t bit = 1 << i;
      if(!(o->turboMask & bit)) continue;
      if(raw & bit) {
        if(st->held[i] < 60000) st->held[i]++;
        if((st->held[i] - 1) % rate >= half) out &= ~bit;     // first frame of a press is always "on"
      } else st->held[i] = 0;
    }
  }
  if(o->autoGas && racing && !(raw & (1 << SB_Y))) out |= 1 << SB_B;
  return out;
}

// ------------------------------------------------------------------ menu model
enum {
  // main page
  IT_VIDEO, IT_GAME, IT_CONTROLS, IT_PHOTO, IT_RESUME, IT_QUIT,
  // video
  IT_WIDE, IT_HD2D, IT_WALLS, IT_POST, IT_HDM7, IT_ENGINE, IT_FULL, IT_SCALE, IT_BACKV,
  // gameplay
  IT_AUTOGAS, IT_TURBO, IT_TURBOBTN, IT_TURBORATE, IT_UNLOCK, IT_200, IT_BACKG,
  // controls
  IT_BIND0, IT_BINDLAST = IT_BIND0 + SB_COUNT - 1, IT_RESETBIND, IT_BACKC,
  IT_COUNT
};
enum { PG_MAIN, PG_VIDEO, PG_GAME, PG_CONTROLS, PG_COUNT };
static const int pageFirst[PG_COUNT] = {IT_VIDEO, IT_WIDE, IT_AUTOGAS, IT_BIND0};
static const int pageLast[PG_COUNT] = {IT_QUIT, IT_BACKV, IT_BACKG, IT_BACKC};
static const char* const pageTitle[PG_COUNT] = {"ENHANCEMENTS", "VIDEO", "GAMEPLAY", "CONTROLS"};

struct Menu {
  bool open;
  int page, sel;        // sel = item id
  int ret[PG_COUNT];    // remembered selection per page
  uint16_t prev;
  int repeat;           // auto-repeat counter for held Up/Down
  int capture;          // SNES button being rebound, or -1
  int captureFrames;
};

Menu* menu_create(void) { Menu* m = calloc(1, sizeof(Menu)); m->capture = -1; return m; }
void menu_free(Menu* m) { free(m); }
bool menu_isOpen(const Menu* m) { return m->open; }
void menu_open(Menu* m) {
  m->open = true; m->page = PG_MAIN; m->sel = IT_VIDEO; m->prev = 0xffff; m->repeat = 0; m->capture = -1;   // prev: ignore the opening press
  for(int p = 0; p < PG_COUNT; p++) m->ret[p] = pageFirst[p];
}
void menu_close(Menu* m) { m->open = false; m->capture = -1; }
bool menu_capturing(const Menu* m) { return m->open && m->capture >= 0; }
void menu_captureCancel(Menu* m) { m->capture = -1; m->prev = 0xffff; }

void menu_captureKey(Menu* m, Options* o, int sc) {
  if(m->capture < 0) return;
  int b = m->capture;
  for(int i = 0; i < SB_COUNT; i++) if(i != b && o->keyBind[i] == sc) o->keyBind[i] = o->keyBind[b];   // swap duplicates
  o->keyBind[b] = sc;
  menu_captureCancel(m);
}
void menu_capturePad(Menu* m, Options* o, int btn) {
  if(m->capture < 0) return;
  int b = m->capture;
  for(int i = 0; i < SB_COUNT; i++) if(i != b && o->padBind[i] == btn) o->padBind[i] = o->padBind[b];
  o->padBind[b] = btn;
  menu_captureCancel(m);
}

static bool itemEnabled(int it, const Options* o, const MenuCaps* c) {
  switch(it) {
    case IT_PHOTO: return c->glAvailable && o->hd2d && c->photoReady && !c->netplay;
    case IT_HD2D: return c->glAvailable;
    case IT_WALLS: case IT_POST: return c->glAvailable && o->hd2d;
    case IT_HDM7: return !o->hd2d;
    case IT_ENGINE: return c->recompAvailable && !c->netplay;
    case IT_TURBOBTN: case IT_TURBORATE: return o->turbo;
    case IT_UNLOCK: return !c->netplay;
    case IT_200: return !c->netplay && c->unlocked200;
  }
  return true;
}

static const uint16_t turboPresets[] = {0x0100, 0x0001, 0x0101, 0x0002, 0x0200, 0x0303};
static const char* const turboPresetNames[] = {"A", "B", "A+B", "Y", "X", "A+B+X+Y"};
#define NTURBO (int)(sizeof turboPresets / sizeof turboPresets[0])

#define B_B 0x0001
#define B_Y 0x0002
#define B_SELECT 0x0004
#define B_START 0x0008
#define B_UP 0x0010
#define B_DOWN 0x0020
#define B_LEFT 0x0040
#define B_RIGHT 0x0080
#define B_A 0x0100
#define B_X 0x0200

static void gotoPage(Menu* m, int page) {
  m->ret[m->page] = m->sel;
  m->page = page;
  m->sel = m->ret[page];
}

int menu_update(Menu* m, uint16_t pad, Options* o, const MenuCaps* c) {
  if(!m->open) return MENU_NONE;
  if(m->capture >= 0) {                       // waiting for a key / button (see menu_capture*)
    if(++m->captureFrames > 6 * 60) menu_captureCancel(m);
    m->prev = pad;
    return MENU_NONE;
  }
  uint16_t pressed = pad & ~m->prev;
  m->prev = pad;
  // held up/down repeat after ~1/3 s
  if(pad & (B_UP | B_DOWN)) { if(++m->repeat > 20 && (m->repeat % 6) == 0) pressed |= pad & (B_UP | B_DOWN); }
  else m->repeat = 0;
  if(pressed & (B_START | B_SELECT)) { menu_close(m); return MENU_CLOSED; }
  if(pressed & B_X) {                         // X: back one page (closes from the main page)
    if(m->page == PG_MAIN) { menu_close(m); return MENU_CLOSED; }
    gotoPage(m, PG_MAIN);
    return MENU_NONE;
  }
  int first = pageFirst[m->page], n = pageLast[m->page] - first + 1;
  if(!itemEnabled(m->sel, o, c)) {            // e.g. photo mode became unavailable
    for(int k = 0; k < n; k++) if(itemEnabled(first + k, o, c)) { m->sel = first + k; break; }
  }
  if(pressed & B_UP) { int s = m->sel - first; for(int k = 0; k < n; k++) { s = (s + n - 1) % n; if(itemEnabled(first + s, o, c)) break; } m->sel = first + s; }
  if(pressed & B_DOWN) { int s = m->sel - first; for(int k = 0; k < n; k++) { s = (s + 1) % n; if(itemEnabled(first + s, o, c)) break; } m->sel = first + s; }
  bool act = pressed & (B_A | B_B | B_LEFT | B_RIGHT | B_Y);
  bool confirm = pressed & (B_A | B_B);
  if(!act || !itemEnabled(m->sel, o, c)) return MENU_NONE;
  int it = m->sel;
  if(it >= IT_BIND0 && it <= IT_BINDLAST) {
    if(confirm) { m->capture = it - IT_BIND0; m->captureFrames = 0; }
    return MENU_NONE;
  }
  switch(it) {
    case IT_VIDEO: if(confirm) gotoPage(m, PG_VIDEO); return MENU_NONE;
    case IT_GAME: if(confirm) gotoPage(m, PG_GAME); return MENU_NONE;
    case IT_CONTROLS: if(confirm) gotoPage(m, PG_CONTROLS); return MENU_NONE;
    case IT_PHOTO: if(confirm) { menu_close(m); return MENU_PHOTO; } return MENU_NONE;
    case IT_RESUME: if(confirm) { menu_close(m); return MENU_CLOSED; } return MENU_NONE;
    case IT_QUIT: if(confirm) return MENU_QUIT; return MENU_NONE;
    case IT_BACKV: case IT_BACKG: case IT_BACKC: if(confirm) gotoPage(m, PG_MAIN); return MENU_NONE;
    case IT_WIDE: o->widescreen = !o->widescreen; if(!o->widescreen) o->hd2d = false; break;
    case IT_HD2D: o->hd2d = !o->hd2d; if(o->hd2d) o->widescreen = true; break;
    case IT_WALLS: o->hd2dWalls = !o->hd2dWalls; break;
    case IT_POST: o->hd2dPost = !o->hd2dPost; break;
    case IT_HDM7: o->hdMode7 = !o->hdMode7; break;
    case IT_ENGINE: o->recompiled = !o->recompiled; break;
    case IT_FULL: o->fullscreen = !o->fullscreen; break;
    case IT_SCALE:
      if(pressed & B_LEFT) o->scale = o->scale <= 2 ? 6 : o->scale - 1;
      else o->scale = o->scale >= 6 ? 2 : o->scale + 1;
      break;
    case IT_AUTOGAS: o->autoGas = !o->autoGas; break;
    case IT_TURBO: o->turbo = !o->turbo; break;
    case IT_TURBOBTN: {
      int cur = -1;
      for(int i = 0; i < NTURBO; i++) if(turboPresets[i] == o->turboMask) cur = i;
      cur = (pressed & B_LEFT) ? (cur <= 0 ? NTURBO - 1 : cur - 1) : (cur + 1) % NTURBO;
      o->turboMask = turboPresets[cur];
      break;
    }
    case IT_TURBORATE:
      if(pressed & B_LEFT) o->turboRate = o->turboRate <= 2 ? 8 : o->turboRate - 2;
      else o->turboRate = o->turboRate >= 8 ? 2 : o->turboRate + 2;
      break;
    case IT_UNLOCK: o->unlockAll = !o->unlockAll; break;
    case IT_200: o->mode200 = !o->mode200; break;
    case IT_RESETBIND: if(confirm) opt_defaultBindings(o); break;
  }
  return MENU_CHANGED;
}

// ------------------------------------------------------------------ drawing
#define RGBA(r, g, b, a) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | (uint32_t)(a) << 24)
#define GOLD RGBA(255, 210, 60, 255)

static void fill(uint32_t* p, int x, int y, int w, int h, uint32_t c) {
  for(int j = y; j < y + h; j++) for(int i = x; i < x + w; i++)
    if(i >= 0 && j >= 0 && i < MENU_W && j < MENU_H) p[j * MENU_W + i] = c;
}

static int text(uint32_t* p, int x, int y, const char* s, uint32_t c, bool shadow) {
  for(; *s; s++, x += 6) {
    unsigned ch = (unsigned char)toupper((unsigned char)*s);
    if(ch < 32 || ch > 127) ch = '?';
    const uint8_t* g = font5x7[ch - 32];
    for(int r = 0; r < 7; r++) for(int b = 0; b < 5; b++) if(g[r] & (0x10 >> b)) {
      if(shadow) fill(p, x + b + 1, y + r + 1, 1, 1, RGBA(0, 0, 0, 255));
      fill(p, x + b, y + r, 1, 1, c);
    }
  }
  return x;
}
static void textRight(uint32_t* p, int xr, int y, const char* s, uint32_t c) { text(p, xr - (int)strlen(s) * 6, y, s, c, true); }
static void textCenter(uint32_t* p, int y, const char* s, uint32_t c) { text(p, (MENU_W - (int)strlen(s) * 6) / 2, y, s, c, true); }

const char* opt_padName(int b) {
  static const char* const names[] = {"A", "B", "X", "Y", "BACK", "GUIDE", "START", "LSTICK", "RSTICK", "LB", "RB",
                                      "UP", "DOWN", "LEFT", "RIGHT", "MISC", "PADDLE1", "PADDLE2", "PADDLE3", "PADDLE4", "TOUCH"};
  if(b == PADB_LT) return "LT";
  if(b == PADB_RT) return "RT";
  if(b >= 0 && b < (int)(sizeof names / sizeof names[0])) return names[b];
  return "-";
}

static void panel(uint32_t* p, int px, int py, int pw, int ph) {
  fill(p, px, py, pw, ph, RGBA(12, 16, 40, 225));
  fill(p, px, py, pw, 1, GOLD); fill(p, px, py + ph - 1, pw, 1, GOLD);
  fill(p, px, py, 1, ph, GOLD); fill(p, px + pw - 1, py, 1, ph, GOLD);
}

static const char* onOff(bool v) { return v ? "ON" : "OFF"; }

void menu_draw(const Menu* m, const Options* o, const MenuCaps* c, uint32_t* p) {
  memset(p, 0, MENU_W * MENU_H * 4);
  if(!m->open) return;
  const int px = 10, py = 8, pw = MENU_W - 20, ph = 208;
  panel(p, px, py, pw, ph);
  textCenter(p, py + 6, pageTitle[m->page], GOLD);
  int first = pageFirst[m->page], last = pageLast[m->page];
  bool ctl = m->page == PG_CONTROLS;
  int y0 = py + 24, step = m->page == PG_MAIN ? 18 : ctl ? 11 : 15;
  if(ctl) {
    text(p, px + 16, y0, "BUTTON", RGBA(150, 160, 190, 255), true);
    text(p, px + 76, y0, "KEYBOARD", RGBA(150, 160, 190, 255), true);
    text(p, px + 166, y0, "GAMEPAD", RGBA(150, 160, 190, 255), true);
    y0 += 13;
  }
  static const char* const names[IT_COUNT] = {
    [IT_VIDEO] = "VIDEO", [IT_GAME] = "GAMEPLAY", [IT_CONTROLS] = "CONTROLS", [IT_PHOTO] = "PHOTO MODE",
    [IT_RESUME] = "RESUME", [IT_QUIT] = "QUIT GAME",
    [IT_WIDE] = "WIDESCREEN 16:9", [IT_HD2D] = "HD-2D 3D WORLD", [IT_WALLS] = "  3D WALLS", [IT_POST] = "  TILT-SHIFT + BLOOM",
    [IT_HDM7] = "HD MODE 7 (2D)", [IT_ENGINE] = "ENGINE", [IT_FULL] = "FULLSCREEN", [IT_SCALE] = "WINDOW SIZE", [IT_BACKV] = "BACK",
    [IT_AUTOGAS] = "AUTO-GAS", [IT_TURBO] = "TURBO", [IT_TURBOBTN] = "  TURBO BUTTONS", [IT_TURBORATE] = "  TURBO SPEED",
    [IT_UNLOCK] = "UNLOCK EVERYTHING", [IT_200] = "200CC (REPLACES 150CC)", [IT_BACKG] = "BACK",
    [IT_RESETBIND] = "RESET TO DEFAULTS", [IT_BACKC] = "BACK",
  };
  for(int it = first; it <= last; it++) {
    int i = it - first;
    bool gap = (m->page == PG_MAIN && it >= IT_RESUME) || (it == IT_BACKV || it == IT_BACKG) || (it >= IT_RESETBIND);
    int y = y0 + i * step + (gap ? (ctl ? 3 : 6) : 0);
    bool en = itemEnabled(it, o, c);
    bool sel = it == m->sel;
    if(sel) fill(p, px + 4, y - (ctl ? 2 : 3), pw - 8, ctl ? 11 : 13, RGBA(60, 80, 160, 255));
    uint32_t col = !en ? RGBA(110, 110, 120, 255) : sel ? RGBA(255, 255, 255, 255) : RGBA(200, 210, 230, 255);
    text(p, px + 8, y, sel ? ">" : " ", GOLD, true);
    if(it >= IT_BIND0 && it <= IT_BINDLAST) {
      int b = it - IT_BIND0;
      text(p, px + 16, y, btnName[b], col, true);
      if(m->capture == b) { text(p, px + 76, y, "PRESS KEY / BUTTON", GOLD, true); continue; }
      const char* kn = c->keyName ? c->keyName(o->keyBind[b]) : "?";
      char kb[16]; snprintf(kb, sizeof kb, "%.13s", kn && *kn ? kn : "-");
      text(p, px + 76, y, kb, RGBA(120, 240, 120, 255), true);
      text(p, px + 166, y, opt_padName(o->padBind[b]), RGBA(120, 200, 255, 255), true);
      continue;
    }
    text(p, px + 16, y, names[it] ? names[it] : "?", col, true);
    const char* val = NULL; bool on = false; char buf[24];
    bool netLocked = c->netplay && (it == IT_UNLOCK || it == IT_200);
    switch(it) {
      case IT_VIDEO: case IT_GAME: case IT_CONTROLS: val = ">"; on = true; break;
      case IT_PHOTO: on = en; val = !c->glAvailable || !o->hd2d ? "HD-2D ONLY" : c->netplay ? "OFFLINE ONLY" : !c->photoReady ? "IN A RACE" : ">"; break;
      case IT_WIDE: on = o->widescreen; val = onOff(on); break;
      case IT_HD2D: on = o->hd2d; val = !c->glAvailable ? "N/A" : onOff(on); break;
      case IT_WALLS: on = o->hd2dWalls; val = onOff(on); break;
      case IT_POST: on = o->hd2dPost; val = onOff(on); break;
      case IT_HDM7: on = o->hdMode7; val = onOff(on); break;
      case IT_ENGINE: on = true; val = !c->recompAvailable ? "INTERP" : o->recompiled ? "RECOMP" : "INTERP"; break;
      case IT_FULL: on = o->fullscreen; val = onOff(on); break;
      case IT_SCALE: on = true; snprintf(buf, sizeof buf, "< X%d >", o->scale); val = buf; break;
      case IT_AUTOGAS: on = o->autoGas; val = onOff(on); break;
      case IT_TURBO: on = o->turbo; val = onOff(on); break;
      case IT_TURBOBTN: {
        on = true; val = "CUSTOM";
        for(int k = 0; k < NTURBO; k++) if(turboPresets[k] == o->turboMask) val = turboPresetNames[k];
        snprintf(buf, sizeof buf, "< %s >", val); val = buf;
        break;
      }
      case IT_TURBORATE: on = true; snprintf(buf, sizeof buf, "< %s HZ >", o->turboRate == 2 ? "30" : o->turboRate == 4 ? "15" : o->turboRate == 6 ? "10" : "7.5"); val = buf; break;
      case IT_UNLOCK: on = netLocked ? (c->netRules & 2) : o->unlockAll; val = onOff(on); break;
      case IT_200:
        if(netLocked) { on = c->netRules & 1; val = onOff(on); }
        else if(!c->unlocked200) { on = false; val = "LOCKED"; }
        else { on = o->mode200; val = onOff(on); }
        break;
    }
    if(val) {
      uint32_t vc = !en && !netLocked ? RGBA(110, 110, 120, 255) : on ? RGBA(120, 240, 120, 255) : RGBA(240, 120, 110, 255);
      if(netLocked) vc = on ? RGBA(90, 170, 90, 255) : RGBA(170, 90, 80, 255);
      textRight(p, px + pw - 8, y, val, vc);
    }
  }
  const char* help;
  if(m->capture >= 0) help = "ESC: CANCEL";
  else if(ctl) help = "A: REBIND   X: BACK   START: CLOSE";
  else if(m->page == PG_GAME && c->netplay) help = "NETPLAY: RULES ARE SET BY THE HOST";
  else if(m->page == PG_GAME && !c->unlocked200 && m->sel == IT_200) help = "WIN 150CC SPECIAL CUP GOLD FOR 200CC";
  else if(m->page != PG_MAIN) help = "A: CHANGE   X: BACK   START: CLOSE";
  else help = c->netplay ? "NETPLAY: GAME KEEPS RUNNING" : "A: SELECT   START: CLOSE";
  textCenter(p, py + ph - 12, help, RGBA(170, 180, 210, 255));
}

void menu_drawHint(uint32_t* p, int frame) {
  memset(p, 0, MENU_W * MENU_H * 4);
  if((frame / 40) % 4 == 3) return;               // gentle blink
  const char* s = "SELECT: ENHANCEMENTS";
  int w = (int)strlen(s) * 6 + 8, x = (MENU_W - w) / 2, y = 210;
  fill(p, x, y - 2, w, 11, RGBA(12, 16, 40, 170));
  text(p, x + 4, y, s, RGBA(255, 230, 120, 255), true);
}

void menu_drawPhotoHint(uint32_t* p, const char* msg, bool showHelp) {
  memset(p, 0, MENU_W * MENU_H * 4);
  if(showHelp) {
    fill(p, 0, 199, MENU_W, 25, RGBA(12, 16, 40, 150));
    textCenter(p, 202, "D-PAD: ORBIT  L/R: ZOOM  X/Y: HEIGHT", RGBA(230, 235, 255, 255));
    textCenter(p, 213, "A: SAVE PHOTO  B: EXIT  SELECT: HIDE", RGBA(230, 235, 255, 255));
    textCenter(p, 4, "PHOTO MODE", GOLD);
  }
  if(msg) {
    int w = (int)strlen(msg) * 6 + 8;
    fill(p, (MENU_W - w) / 2, 16, w, 11, RGBA(12, 16, 40, 200));
    textCenter(p, 18, msg, RGBA(120, 240, 120, 255));
  }
}
