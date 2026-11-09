#pragma once
#include "x64rt.h"
struct UnitTest {
  void (*fn)(Ctx*);
  uint8_t code[16];
  int len, base, index, scale, disp, msz;
  uint32_t flagmask;
  const char* text;
};
extern const UnitTest g_tests[];
extern const unsigned g_ntests;
