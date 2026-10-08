// rules.c: see rules.h. Addresses come from the disassembly (asm/bank_81.asm); docs/NOTES.md
// explains what each routine does.
#include <string.h>
#include "rules.h"
#include "snes.h"

Rules rules;

static inline uint16_t ramw(Cpu* c, uint32_t a) { Snes* s = c->mem; return s->ram[a & 0x1ffff] | s->ram[(a + 1) & 0x1ffff] << 8; }
static inline void busw(Cpu* c, uint32_t a, uint16_t v) { c->write(c->mem, a & 0xffffff, v & 0xff); c->write(c->mem, (a + 1) & 0xffffff, v >> 8); }
static inline void zn16(Cpu* c, uint16_t v) { c->z = v == 0; c->n = v & 0x8000; }
// Rules stand in for one original instruction. They perform the same bus reads the instruction
// would (opcode + operands) so a hooked frame costs the same cycles in both engines.
static inline void fetchIns(Cpu* c, int len) { for(int i = 0; i < len; i++) c->read(c->mem, (c->k << 16) | (uint16_t)(c->pc + i)); }
static inline uint16_t ccIndex(Cpu* c) { return ramw(c, 0x30); }      // $30: engine class * 2 (0/2/4)

// ------------------------------------------------------------------ 200cc (rides on 150cc)
// $81F040  STA $00B4,X : player kart top speed. Original: base-$80 (50cc), base (100cc), base+$A0 (150cc).
SMK_RULE(0x81F040, rule_200_topSpeed)
bool rule_200_topSpeed(Cpu* c) {
  if(!rules.mode200 || ccIndex(c) != 4 || ramw(c, 0x2C) != 0) return false;
  fetchIns(c, 3);
  busw(c, (c->db << 16) + 0x00B4 + c->x, c->a + 0xA0);              // 150cc + another $A0 step
  c->pc = 0xF043;
  return true;
}

// $81F058  STA [$04],Y : one entry of the player kart's acceleration curve (16 speed bands of $40,
// read by $80A7E1). 150cc stores base*16*1.5. The curve falls off at high speed, so only raising
// the top speed doesn't make a human kart faster: it never gets near it. 200cc stretches the curve
// over 15% more speed (band b takes base band b/1.15) and doubles it, so the kart accelerates
// harder and keeps accelerating up to a higher speed.
SMK_RULE(0x81F058, rule_200_accelCurve)
bool rule_200_accelCurve(Cpu* c) {
  if(!rules.mode200 || ccIndex(c) != 4) return false;
  fetchIns(c, 2);
  int idx = c->y >> 1;                                  // band 0..15
  int src = idx * 100 / 115;
  uint32_t a = (c->db << 16) | (uint16_t)(c->x - idx + src);
  uint32_t v = (uint32_t)c->read(c->mem, a) * 16 * 2;
  // The top bands (speed $280 and up) are where 150cc runs out of push: the kart crawls towards its
  // top speed at about 1 unit per frame and never gets there on a real straight, while the computer
  // drivers run at theirs. Give the top bands a floor so the kart actually reaches 200cc speed.
  if(idx >= 10 && v < 0x600) v = 0x600;
  if(v > 0xffff) v = 0xffff;
  uint16_t dp = c->dp;
  uint32_t ptr = c->read(c->mem, (uint16_t)(dp + 4)) | c->read(c->mem, (uint16_t)(dp + 5)) << 8 | c->read(c->mem, (uint16_t)(dp + 6)) << 16;
  busw(c, ptr + c->y, (uint16_t)v);
  c->pc = 0xF05A;
  return true;
}

// $81F01B  ADC #$0030 (150cc bonus on the second stat block). 200cc: twice the bonus.
SMK_RULE(0x81F01B, rule_200_bonus)
bool rule_200_bonus(Cpu* c) {
  if(!rules.mode200) return false;
  fetchIns(c, 3);
  uint32_t v = (uint32_t)c->a + 0x60 + (c->c ? 1 : 0);
  c->v = (~(c->a ^ 0x60) & (c->a ^ v) & 0x8000) != 0;
  c->c = v > 0xffff;
  c->a = (uint16_t)v; zn16(c, c->a);
  c->pc = 0xF01E;
  return true;
}

// $81FEC9  STA $0690,X : the CPU drivers' per-class speed profile (64 entries, value*16).
// 200cc: 150cc profile * 1.15: the same step up as the player karts get (top speed +$A0 over 150cc).
SMK_RULE(0x81FEC9, rule_200_cpuProfile)
bool rule_200_cpuProfile(Cpu* c) {
  if(!rules.mode200 || ccIndex(c) != 4) return false;
  fetchIns(c, 3);
  uint32_t v = (uint32_t)c->a * 23 / 20;
  if(v > 0x0ff0) v = 0x0ff0;
  busw(c, (c->db << 16) + 0x0690 + c->x, (uint16_t)v);
  c->pc = 0xFECC;
  return true;
}


// ------------------------------------------------------------------ unlock everything
// SRAM $7F2..$7F5 hold one byte per cup (Mushroom, Flower, Star, Special) with a 2-bit trophy per
// class: bits 2-3 = 50cc, bits 0-1 = 100cc, bits 4-5 = 150cc (1 gold, 2 silver, 3 bronze); $7F0 is
// their checksum. The game unlocks the Special Cup with 100cc gold in the first three cups and
// 150cc with 100cc gold in the Special Cup. Every such check is an LDA.l $3067Fx followed by
// AND #3 / CMP #1; with "unlock everything" on these loads see a gold (the save is never written).
static bool unlockLoad(Cpu* c) {
  if(!rules.unlockAll) return false;
  uint32_t adr = c->read(c->mem, (c->k << 16) | (uint16_t)(c->pc + 1));
  fetchIns(c, 1);
  adr |= c->read(c->mem, (c->k << 16) | (uint16_t)(c->pc + 2)) << 8;
  adr |= c->read(c->mem, (c->k << 16) | (uint16_t)(c->pc + 3)) << 16;
  uint16_t v = c->read(c->mem, adr);
  if(!c->mf) v |= c->read(c->mem, (adr + 1) & 0xffffff) << 8;
  v = (v & ~3) | 1;
  if(c->mf) { c->a = (c->a & 0xff00) | (v & 0xff); c->z = (v & 0xff) == 0; c->n = v & 0x80; }
  else { c->a = v; zn16(c, v); }
  c->pc += 4;
  return true;
}
SMK_RULE(0x84F668, rule_unlock_84F668)
bool rule_unlock_84F668(Cpu* c) { return unlockLoad(c); }
SMK_RULE(0x84F674, rule_unlock_84F674)
bool rule_unlock_84F674(Cpu* c) { return unlockLoad(c); }
SMK_RULE(0x84F680, rule_unlock_84F680)
bool rule_unlock_84F680(Cpu* c) { return unlockLoad(c); }
SMK_RULE(0x84F6CF, rule_unlock_84F6CF)
bool rule_unlock_84F6CF(Cpu* c) { return unlockLoad(c); }
SMK_RULE(0x84F6DB, rule_unlock_84F6DB)
bool rule_unlock_84F6DB(Cpu* c) { return unlockLoad(c); }
SMK_RULE(0x84F6E7, rule_unlock_84F6E7)
bool rule_unlock_84F6E7(Cpu* c) { return unlockLoad(c); }
SMK_RULE(0x84FAA2, rule_unlock_84FAA2)
bool rule_unlock_84FAA2(Cpu* c) { return unlockLoad(c); }
SMK_RULE(0x84FAAE, rule_unlock_84FAAE)
bool rule_unlock_84FAAE(Cpu* c) { return unlockLoad(c); }
SMK_RULE(0x84FABA, rule_unlock_84FABA)
bool rule_unlock_84FABA(Cpu* c) { return unlockLoad(c); }
SMK_RULE(0x84FB13, rule_unlock_84FB13)
bool rule_unlock_84FB13(Cpu* c) { return unlockLoad(c); }
SMK_RULE(0x84FB1F, rule_unlock_84FB1F)
bool rule_unlock_84FB1F(Cpu* c) { return unlockLoad(c); }
SMK_RULE(0x84FB2B, rule_unlock_84FB2B)
bool rule_unlock_84FB2B(Cpu* c) { return unlockLoad(c); }
SMK_RULE(0x85822C, rule_unlock_85822C)
bool rule_unlock_85822C(Cpu* c) { return unlockLoad(c); }
SMK_RULE(0x858661, rule_unlock_858661)
bool rule_unlock_858661(Cpu* c) { return unlockLoad(c); }
SMK_RULE(0x8587E0, rule_unlock_8587E0)
bool rule_unlock_8587E0(Cpu* c) { return unlockLoad(c); }
SMK_RULE(0x858806, rule_unlock_858806)
bool rule_unlock_858806(Cpu* c) { return unlockLoad(c); }

// ------------------------------------------------------------------ dispatch (interpreter path)
typedef bool (*RuleFn)(Cpu*);
static const struct { uint32_t addr; RuleFn fn; } table[] = {
  {0x81F040, rule_200_topSpeed}, {0x81F058, rule_200_accelCurve}, {0x81F01B, rule_200_bonus},
  {0x81FEC9, rule_200_cpuProfile},
  {0x84F668, rule_unlock_84F668},
  {0x84F674, rule_unlock_84F674},
  {0x84F680, rule_unlock_84F680},
  {0x84F6CF, rule_unlock_84F6CF},
  {0x84F6DB, rule_unlock_84F6DB},
  {0x84F6E7, rule_unlock_84F6E7},
  {0x84FAA2, rule_unlock_84FAA2},
  {0x84FAAE, rule_unlock_84FAAE},
  {0x84FABA, rule_unlock_84FABA},
  {0x84FB13, rule_unlock_84FB13},
  {0x84FB1F, rule_unlock_84FB1F},
  {0x84FB2B, rule_unlock_84FB2B},
  {0x85822C, rule_unlock_85822C},
  {0x858661, rule_unlock_858661},
  {0x8587E0, rule_unlock_8587E0},
  {0x858806, rule_unlock_858806},
};

bool rules_dispatch(Cpu* c) {
  if(!rules.mode200 && !rules.unlockAll) return false;
  uint32_t pc = ((c->k & 0x3f) | 0x80) << 16 | c->pc;     // canonical FastROM address
  if(c->pc < 0x8000) return false;
  for(unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++)
    if(table[i].addr == pc) return table[i].fn(c);
  return false;
}

static bool hook(Cpu* c) { return rules_dispatch(c); }
void rules_install(void) { cpu_ruleHook = hook; }

// 200cc becomes available after a gold trophy in the 150cc Special Cup (the same rule the game
// uses to open 150cc, one class up), or with "unlock everything".
bool rules_200Unlocked(const uint8_t* sram) { return rules.unlockAll || (sram && (sram[0x7F5] & 0x30) == 0x10); }

uint8_t rules_pack(void) { return (rules.mode200 ? 1 : 0) | (rules.unlockAll ? 2 : 0); }
void rules_unpack(uint8_t v) { rules.mode200 = v & 1; rules.unlockAll = (v & 2) != 0; }
