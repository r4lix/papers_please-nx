// aarch64 side of the differential unit tests: replays vectors recorded from the real x64 CPU (unit_main gen) against the lifted code.
#include "unit_test.h"
#include "x64rt_sse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#ifdef __SWITCH__
#include <switch.h>
#endif

uint64_t g_delta = 0;
thread_local uint64_t t_rt_gs = 0;
void rt_call(Ctx*, uint64_t) { abort(); }
void rt_unimpl(uint64_t a, const char* w) { fprintf(stderr, "unimpl %llx %s\n", (unsigned long long)a, w); abort(); }
void rt_badjump(uint64_t, uint64_t) { abort(); }
void rt_fastfail(Ctx*, uint64_t) { abort(); }
void rt_cpuid(uint32_t*, uint32_t*, uint32_t*, uint32_t*) {}
uint64_t rt_rdtsc() { return 0; }
bool rt_cmpxchg16(uint64_t, uint64_t*, uint64_t*, uint64_t, uint64_t) { return false; }

struct UnitRec { uint32_t t, pad; Ctx in, out; uint8_t memIn[256], memOut[256]; uint64_t bufx; };

int main(int argc, char** argv) {
  std::string root = ".";
#ifdef __SWITCH__
  root = "sdmc:/switch/papers";
#endif
  std::string lg = root + "/unit.log";
  { FILE* t = fopen(lg.c_str(), "w"); if (t) fclose(t); }
  FILE* log = fopen(lg.c_str(), "a");
  FILE* f = fopen((root + "/unit.vec").c_str(), "rb");
  if (!f) { fprintf(log, "no unit.vec\n"); fclose(log); return 1; }
  uint8_t* buf = (uint8_t*)aligned_alloc(4096, 8192);
  unsigned total = 0, fail = 0; int shown = 0;
  UnitRec r;
  while (fread(&r, sizeof r, 1, f) == 1) {
    const UnitTest& T = g_tests[r.t];
    uint64_t delta = (uint64_t)(uintptr_t)buf - r.bufx;
    auto rebase = [&](uint64_t v) { return (v + 65536 >= r.bufx && v < r.bufx + 8192 + 65536) ? v + delta : v; };
    Ctx c = r.in;
    for (int i = 0; i < 16; i++) c.r[i] = rebase(c.r[i]);
    memset(buf, 0, 8192); memcpy(buf + 896, r.memIn, 256);
    T.fn(&c);
    total++;
    bool ok = true; char msg[400]; int ml = 0; msg[0] = 0;
    for (int i = 0; i < 16; i++) { if (i == 4) continue; uint64_t e = rebase(r.out.r[i]); if (c.r[i] != e) { ok = false; if (ml < 300) ml += snprintf(msg + ml, sizeof msg - ml, " r%d: x64=%llx lifted=%llx;", i, (unsigned long long)e, (unsigned long long)c.r[i]); } }
    for (int i = 0; i < 16; i++) if (memcmp(&c.x[i], &r.out.x[i], 16)) { ok = false; if (ml < 300) ml += snprintf(msg + ml, sizeof msg - ml, " x%d: x64=%llx:%llx lifted=%llx:%llx;", i, (unsigned long long)r.out.x[i].q[1], (unsigned long long)r.out.x[i].q[0], (unsigned long long)c.x[i].q[1], (unsigned long long)c.x[i].q[0]); }
    uint64_t fm = 0; { uint32_t m = T.flagmask; if (m & 1) fm |= 1; if (m & 16) fm |= 4; if (m & 2) fm |= 0x40; if (m & 4) fm |= 0x80; if (m & 8) fm |= 0x800; }
    if ((c.aux ^ r.out.aux) & fm) { ok = false; if (ml < 300) ml += snprintf(msg + ml, sizeof msg - ml, " flags: x64=%llx lifted=%llx;", (unsigned long long)(r.out.aux & 0x8d5), (unsigned long long)(c.aux & 0x8d5)); }
    if (memcmp(buf + 896, r.memOut, 256)) { ok = false; if (ml < 300) ml += snprintf(msg + ml, sizeof msg - ml, " memory differs;"); }
    if (!ok) { fail++; if (shown++ < 400) { fprintf(log, "FAIL [%s]%s\n", T.text, msg); fflush(log); } }
  }
  fprintf(log, "unit vectors: %u runs, %u failing\n", total, fail);
  fclose(log);
  return fail != 0;
}
