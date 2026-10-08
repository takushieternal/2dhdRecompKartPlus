// rules: built-in gameplay options (200cc, unlock everything) implemented as instruction hooks.
// Each hook replaces one original instruction when its option is on and returns true; when the
// option is off it returns false and the original instruction runs untouched, so a build with all
// options off is identical to the original game.
//
// Hooks run in both engines: the interpreter checks them before each instruction (rules_dispatch),
// and tools/recomp.py emits a call to them in front of the matching recompiled instruction.
// Options must match on both sides of a netplay session (netplay sends them with the handshake).
#ifndef RULES_H
#define RULES_H
#include <stdint.h>
#include <stdbool.h>
#include "cpu.h"

#define SMK_RULE(addr, fn)   /* picked up by tools/recomp.py */

typedef struct {
  bool mode200;      // 200cc: replaces 150cc when on
  bool unlockAll;    // Special Cup + 150cc (+ 200cc) available without the trophies
} Rules;

extern Rules rules;

// interpreter path: returns true if a rule handled the instruction at the CPU's current PC
bool rules_dispatch(Cpu* c);
void rules_install(void);
// 200cc menu availability (sram = the cartridge SRAM, 2 KiB)
bool rules_200Unlocked(const uint8_t* sram);
// rule state packed for netplay / savestate-adjacent bookkeeping
uint8_t rules_pack(void);
void rules_unpack(uint8_t v);

#endif
