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
  // controls
  int keyBind[SB_COUNT];   // SDL scancode per SNES button (player 1 keyboard)
  int padBind[SB_COUNT];   // gamepad button per SNES button (both gamepads)
} Options;

typedef struct {
  bool glAvailable;     // HD-2D possible
  bool recompAvailable; // build includes recompiled code
  bool netplay;         // in a netplay session (engine switch, pause and gameplay rules are locked)
  bool photoReady;      // HD-2D driving view on screen: photo mode possible
  bool unlocked200;     // 200cc earned (or unlock-everything on)
  uint8_t netRules;     // in netplay: the session's rules (rules_pack format, set by the host)
  const char* (*keyName)(int scancode);   // display names for the bindings
} MenuCaps;

enum { MENU_NONE = 0, MENU_CHANGED, MENU_CLOSED, MENU_QUIT, MENU_PHOTO };

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
// Draw into an RGBA (0xAABBGGRR) overlay of 256x224 virtual pixels. Clears it first.
void menu_draw(const Menu* m, const Options* o, const MenuCaps* caps, uint32_t* rgba);
// Title-screen hint ("SELECT: ENHANCEMENTS") into the same overlay format.
void menu_drawHint(uint32_t* rgba, int frame);
// Photo-mode help line (and a status message when msg != NULL) into the overlay format.
void menu_drawPhotoHint(uint32_t* rgba, const char* msg, bool showHelp);
const char* opt_padName(int button);

#define MENU_W 256
#define MENU_H 224

#endif
