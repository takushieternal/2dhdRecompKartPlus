// options: the Enhancements menu (opened from the title screen with SELECT, or anywhere with
// SELECT+START / Esc) and the settings it edits, saved to smkplay.cfg next to the executable.
// Also the controller pipeline that's independent of SDL: button bindings (stored as SDL
// scancodes / SDL game-controller button numbers), auto-gas and turbo.
#ifndef OPTIONS_H
#define OPTIONS_H
#include <stdint.h>
#include <stdbool.h>

// SNES buttons in controller bit order
enum { SB_B, SB_Y, SB_SELECT, SB_START, SB_UP, SB_DOWN, SB_LEFT, SB_RIGHT, SB_A, SB_X, SB_L, SB_R, SB_COUNT };

// gamepad binding codes: 0..20 = SDL_GameControllerButton, plus the analog triggers
#define PADB_LT 100
#define PADB_RT 101
#define PADB_NONE -1

typedef struct {
  // video
  bool widescreen;      // 16:9 picture
  bool hd2d;            // HD-2D: Mode 7 views re-rendered as a 3D world (needs OpenGL 3.3)
  bool hd2dWalls;       // HD-2D: extrude walls from the track's surface data
  bool hd2dPost;        // HD-2D: tilt-shift, bloom, vignette
  bool hdMode7;         // 2D renderer: 2x2 samples per Mode 7 pixel
  bool recompiled;      // run the recompiled C (off = interpreter)
  bool fullscreen;
  int scale;            // window size multiplier, 2..6
  // gameplay
  bool autoGas;         // hold B while racing (holding Y, the brake, releases it)
  bool turbo;           // rapid-fire on the buttons in turboMask while held
  uint16_t turboMask;   // SNES button bits
  int turboRate;        // frames per on/off cycle: 2 (30 Hz), 4 (15 Hz), 6 (10 Hz), 8 (7.5 Hz)
  bool unlockAll;       // Special Cup, 150cc and 200cc without the trophies (the save isn't changed)
  bool mode200;         // 200cc replaces 150cc (needs 150cc Special Cup gold or unlockAll)
  // online
  int netPort;          // UDP port for hosting (default 7845)
  int netDelay;         // input delay in frames, 0 = automatic
  bool netRollback;     // rollback netcode (off = lockstep)
  bool netStats;        // show ping / rollback in a corner while playing online
  char netLastJoin[48]; // last room code or address joined
  // controls
  int keyBind[SB_COUNT];   // SDL scancode per SNES button (player 1 keyboard)
  int padBind[SB_COUNT];   // gamepad button per SNES button (both gamepads)
} Options;

// Online page state, written by online.c, shown and partly edited by the menu
enum { NETUI_OFF, NETUI_HOSTING, NETUI_JOINING, NETUI_RUNNING };
typedef struct {
  int mode;                 // NETUI_*
  bool joinPage;            // client socket open (join page)
  char code[24];            // our room code ("" while it's being found)
  char codeNote[64];        // how the code was found / what to do if it doesn't work
  char lan[32];             // our LAN address
  char status[64];
  char joinText[48];        // room code / address being typed (join page)
  char punchText[48];       // host: player 2's code
  int nlan; char lanNames[4][48];
  int lanSel;               // LAN game picked in the menu
  bool isHost; int ping, delay; bool rollback;
} NetUi;

typedef struct {
  bool glAvailable;     // HD-2D possible
  bool recompAvailable; // build includes recompiled code
  bool netplay;         // in a netplay session (engine switch, pause and gameplay rules are locked)
  bool photoReady;      // HD-2D driving view on screen: photo mode possible
  bool unlocked200;     // 200cc earned (or unlock-everything on)
  uint8_t netRules;     // in netplay: the session's rules (rules_pack format, set by the host)
  const char* (*keyName)(int scancode);   // display names for the bindings
  NetUi* net;
} MenuCaps;

enum { MENU_NONE = 0, MENU_CHANGED, MENU_CLOSED, MENU_QUIT, MENU_PHOTO,
       MENU_NET_HOST, MENU_NET_JOINPAGE, MENU_NET_JOIN, MENU_NET_JOINLAN, MENU_NET_PUNCH, MENU_NET_CANCEL,
       MENU_NET_COPY, MENU_NET_PASTE, MENU_NET_DISCONNECT, MENU_NET_LANSEARCH };

void opt_defaults(Options* o);
void opt_defaultBindings(Options* o);
bool opt_load(Options* o, const char* path);
bool opt_save(const Options* o, const char* path);

// Turbo / auto-gas, applied to one player's pad once per emulated frame before it is used or sent
// over the network (so netplay and, later, rollback see exactly what the game got).
typedef struct { uint16_t held[SB_COUNT]; } PadState;
uint16_t opt_transformPad(const Options* o, PadState* st, uint16_t raw, bool racing);

typedef struct Menu Menu;
Menu* menu_create(void);
void menu_free(Menu* m);
bool menu_isOpen(const Menu* m);
void menu_open(Menu* m);
void menu_close(Menu* m);
// Feed the combined controller state (SNES bit layout: 0 B, 1 Y, 2 Select, 3 Start, 4 Up, 5 Down,
// 6 Left, 7 Right, 8 A, 9 X, 10 L, 11 R) once per frame while open.
int menu_update(Menu* m, uint16_t pad, Options* o, const MenuCaps* caps);
// Rebinding: while menu_capturing() the next key / gamepad button goes to menu_capture*.
bool menu_capturing(const Menu* m);
void menu_captureKey(Menu* m, Options* o, int scancode);
void menu_capturePad(Menu* m, Options* o, int button);
void menu_captureCancel(Menu* m);
// Text fields (Online page): while menu_editing(), typed text goes to menu_editText and these keys
// to menu_editKey; the gamepad drives an on-screen keyboard through menu_update.
enum { EDIT_BACKSPACE = 1, EDIT_ENTER, EDIT_CANCEL };
bool menu_editing(const Menu* m);
void menu_editText(Menu* m, NetUi* ui, const char* text);
int menu_editKey(Menu* m, NetUi* ui, int key);        // may return a MENU_NET_* action (Enter)
// open a page programmatically (e.g. after --host on the command line)
void menu_showOnline(Menu* m, int page);              // 0 online, 1 host, 2 join
// Draw into an RGBA (0xAABBGGRR) overlay of 256x224 virtual pixels. Clears it first.
void menu_draw(const Menu* m, const Options* o, const MenuCaps* caps, uint32_t* rgba);
// Title-screen hint ("SELECT: ENHANCEMENTS") into the same overlay format.
void menu_drawHint(uint32_t* rgba, int frame);
// Photo-mode help line (and a status message when msg != NULL) into the overlay format.
void menu_drawPhotoHint(uint32_t* rgba, const char* msg, bool showHelp);
const char* opt_padName(int button);
// Online status line (ping etc.) and/or a short message, into the overlay format (clears it first)
void menu_drawStatus(uint32_t* rgba, const char* hud, const char* toast);

#define MENU_W 256
#define MENU_H 224

#endif
