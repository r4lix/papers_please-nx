// Differential unit tester: runs every instruction form natively and via lifted C++ and compares the results.
#include "unit_test.h"
#include "x64rt_sse.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

uint64_t g_delta = 0, g_gs = 0;
void rt_call(Ctx*, uint64_t) { abort(); }
void rt_unimpl(uint64_t a, const char* w) { fprintf(stderr, "unimpl %llx %s\n", (unsigned long long)a, w); abort(); }
void rt_badjump(uint64_t, uint64_t) { abort(); }
void rt_fastfail(Ctx*, uint64_t) { abort(); }
void rt_cpuid(uint32_t*, uint32_t*, uint32_t*, uint32_t*) {}
uint64_t rt_rdtsc() { return 0; }
bool rt_cmpxchg16(uint64_t, uint64_t*, uint64_t*, uint64_t, uint64_t) { return false; }

extern "C" {
uint64_t u_saved_rsp, u_ctx, u_code, u_tmp, u_flags;
volatile int u_fault, u_in_native;
void native_exec(Ctx* c, uint64_t code);
void native_after();
void native_recover();
}

asm(R"(
.intel_syntax noprefix
.text
.globl native_exec
native_exec:
    push rbp
    push rbx
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 168
    movups [rsp+0], xmm6
    movups [rsp+16], xmm7
    movups [rsp+32], xmm8
    movups [rsp+48], xmm9
    movups [rsp+64], xmm10
    movups [rsp+80], xmm11
    movups [rsp+96], xmm12
    movups [rsp+112], xmm13
    movups [rsp+128], xmm14
    movups [rsp+144], xmm15
    mov [rip+u_saved_rsp], rsp
    mov [rip+u_ctx], rcx
    mov [rip+u_code], rdx
    mov dword ptr [rip+u_fault], 0
    mov dword ptr [rip+u_in_native], 1
    mov r11, rcx
    push qword ptr [r11+384]
    popfq
    movups xmm0, [r11+128]
    movups xmm1, [r11+144]
    movups xmm2, [r11+160]
    movups xmm3, [r11+176]
    movups xmm4, [r11+192]
    movups xmm5, [r11+208]
    movups xmm6, [r11+224]
    movups xmm7, [r11+240]
    movups xmm8, [r11+256]
    movups xmm9, [r11+272]
    movups xmm10, [r11+288]
    movups xmm11, [r11+304]
    movups xmm12, [r11+320]
    movups xmm13, [r11+336]
    movups xmm14, [r11+352]
    movups xmm15, [r11+368]
    mov rbx, [r11+24]
    mov rbp, [r11+40]
    mov rsi, [r11+48]
    mov rdi, [r11+56]
    mov r8, [r11+64]
    mov r9, [r11+72]
    mov r10, [r11+80]
    mov r12, [r11+96]
    mov r13, [r11+104]
    mov r14, [r11+112]
    mov r15, [r11+120]
    mov rcx, [r11+8]
    mov rdx, [r11+16]
    mov rax, [r11]
    mov r11, [r11+88]
    jmp qword ptr [rip+u_code]
.globl native_after
native_after:
    mov [rip+u_tmp], rax
    pushfq
    pop rax
    mov [rip+u_flags], rax
    mov rax, [rip+u_ctx]
    mov [rax+8], rcx
    mov [rax+16], rdx
    mov [rax+24], rbx
    mov [rax+40], rbp
    mov [rax+48], rsi
    mov [rax+56], rdi
    mov [rax+64], r8
    mov [rax+72], r9
    mov [rax+80], r10
    mov [rax+88], r11
    mov [rax+96], r12
    mov [rax+104], r13
    mov [rax+112], r14
    mov [rax+120], r15
    mov rcx, [rip+u_tmp]
    mov [rax], rcx
    mov rcx, [rip+u_flags]
    mov [rax+384], rcx
    movups [rax+128], xmm0
    movups [rax+144], xmm1
    movups [rax+160], xmm2
    movups [rax+176], xmm3
    movups [rax+192], xmm4
    movups [rax+208], xmm5
    movups [rax+224], xmm6
    movups [rax+240], xmm7
    movups [rax+256], xmm8
    movups [rax+272], xmm9
    movups [rax+288], xmm10
    movups [rax+304], xmm11
    movups [rax+320], xmm12
    movups [rax+336], xmm13
    movups [rax+352], xmm14
    movups [rax+368], xmm15
    jmp 1f
.globl native_recover
native_recover:
    mov dword ptr [rip+u_fault], 1
1:
    mov dword ptr [rip+u_in_native], 0
    mov rsp, [rip+u_saved_rsp]
    movups xmm6, [rsp+0]
    movups xmm7, [rsp+16]
    movups xmm8, [rsp+32]
    movups xmm9, [rsp+48]
    movups xmm10, [rsp+64]
    movups xmm11, [rsp+80]
    movups xmm12, [rsp+96]
    movups xmm13, [rsp+112]
    movups xmm14, [rsp+128]
    movups xmm15, [rsp+144]
    add rsp, 168
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbx
    pop rbp
    ret
.att_syntax
)");

static LONG WINAPI veh(EXCEPTION_POINTERS* p) {
  if (u_in_native) { p->ContextRecord->Rip = (DWORD64)&native_recover; return EXCEPTION_CONTINUE_EXECUTION; }
  return EXCEPTION_CONTINUE_SEARCH;
}

static uint64_t rng_s = 88172645463325252ull;
static uint64_t rnd() { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return rng_s; }
static uint64_t rnd_val() {
  switch (rnd() % 8) {
    case 0: return 0; case 1: return 1; case 2: return ~0ull; case 3: return 0x8000000000000000ull;
    case 4: return 0x80000000ull; case 5: return rnd() & 0xff; case 6: return 0x7fffffffffffffffull;
    default: return rnd();
  }
}

struct UnitRec { uint32_t t, pad; Ctx in, out; uint8_t memIn[256], memOut[256]; uint64_t bufx; };
int main(int argc, char** argv) {
  FILE* genf = nullptr;
  if (argc > 2 && !strcmp(argv[1], "gen")) { genf = fopen(argv[2], "wb"); argc = 1; }
  AddVectoredExceptionHandler(1, veh);
  uint8_t* ex = (uint8_t*)VirtualAlloc(nullptr, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  uint8_t* buf = (uint8_t*)VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  uint8_t* buf0 = (uint8_t*)malloc(4096);
  int trials = argc > 1 ? atoi(argv[1]) : 6;
  if (genf) trials = 3;
  unsigned fail_tests = 0, skipped = 0, total = 0;
  for (unsigned t = 0; t < g_ntests; t++) {
    const UnitTest& T = g_tests[t];
    std::string mn = T.text;
    if (mn.rfind("div", 0) == 0 || mn.rfind("idiv", 0) == 0) { skipped++; continue; }
    uint8_t* code = ex + (size_t)(t % 4000) * 64;
    memcpy(code, T.code, T.len);
    code[T.len] = 0xFF; code[T.len + 1] = 0x25; *(uint32_t*)(code + T.len + 2) = 0; *(uint64_t*)(code + T.len + 6) = (uint64_t)&native_after;
    bool bad = false;
    for (int tr = 0; tr < trials && !bad; tr++) {
      Ctx c0; memset(&c0, 0, sizeof c0);
      for (int i = 0; i < 16; i++) c0.r[i] = rnd_val();
      for (int i = 0; i < 16; i++) { c0.x[i].q[0] = rnd_val(); c0.x[i].q[1] = rnd_val(); if (rnd() & 1) { c0.x[i].df[0] = (double)(int)(rnd() % 2000) / 8.0 - 100; c0.x[i].df[1] = (double)(int)(rnd() % 2000) / 4.0 - 50; } }
      uint64_t fl = 0; uint64_t r = rnd();
      if (r & 1) fl |= 1;
      if (r & 2) fl |= 4;
      if (r & 4) fl |= 0x40;
      if (r & 8) fl |= 0x80;
      if (r & 16) fl |= 0x800;
      if (r & 32) fl |= 0x10;
      c0.aux = fl;
      for (int i = 0; i < 4096; i++) buf0[i] = (uint8_t)rnd();
      if (T.base >= 0 || T.index >= 0) {
        uint64_t ea = (uint64_t)(buf + 1024);
        if (T.index >= 0) c0.r[T.index] = 0;
        if (T.base >= 0) {
          if (T.base == T.index) break;
          c0.r[T.base] = ea - (int64_t)T.disp;
        } else {
          uint64_t v = ea - (int64_t)T.disp;
          if (v % T.scale) break;
          c0.r[T.index] = v / T.scale;
        }
      }
      Ctx cn = c0, cl = c0;
      memcpy(buf, buf0, 4096);
      native_exec(&cn, (uint64_t)code);
      bool faulted = u_fault;
      uint8_t mn_[2048]; memcpy(mn_, buf + 512, 2048);
      memcpy(buf, buf0, 4096);
      if (faulted) { skipped++; break; }
      if (genf) { UnitRec r; memset(&r, 0, sizeof r); r.t = t; r.in = c0; r.out = cn; memcpy(r.memIn, buf0 + 896, 256); memcpy(r.memOut, mn_ + (896 - 512), 256); r.bufx = (uint64_t)(uintptr_t)buf; fwrite(&r, sizeof r, 1, genf); total++; continue; }
      T.fn(&cl);
      total++;
      bool ok = true;
      char msg[512]; msg[0] = 0;
      int mlen = 0;
      for (int i = 0; i < 16; i++) {
        if (i == 4) continue;
        if (cn.r[i] != cl.r[i]) { ok = false; mlen += snprintf(msg + mlen, sizeof(msg) - mlen, " r%d: native=%llx lifted=%llx;", i, (unsigned long long)cn.r[i], (unsigned long long)cl.r[i]); }
      }
      for (int i = 0; i < 16; i++) if (memcmp(&cn.x[i], &cl.x[i], 16)) { ok = false; if (mlen < 380) mlen += snprintf(msg + mlen, sizeof(msg) - mlen, " x%d: native=%llx:%llx lifted=%llx:%llx;", i, (unsigned long long)cn.x[i].q[1], (unsigned long long)cn.x[i].q[0], (unsigned long long)cl.x[i].q[1], (unsigned long long)cl.x[i].q[0]); }
      uint64_t fm = 0;
      { uint32_t m = T.flagmask; if (m & 1) fm |= 1; if (m & 16) fm |= 4; if (m & 2) fm |= 0x40; if (m & 4) fm |= 0x80; if (m & 8) fm |= 0x800; }
      if ((cn.aux ^ cl.aux) & fm) { ok = false; if (mlen < 380) mlen += snprintf(msg + mlen, sizeof(msg) - mlen, " flags: native=%llx lifted=%llx mask=%llx;", (unsigned long long)(cn.aux & 0x8d5), (unsigned long long)(cl.aux & 0x8d5), (unsigned long long)fm); }
      if (memcmp(mn_, buf + 512, 2048)) { ok = false; if (mlen < 380) mlen += snprintf(msg + mlen, sizeof(msg) - mlen, " memory differs;"); }
      if (!ok) {
        bad = true; fail_tests++;
        printf("FAIL [%s] in: ", T.text);
        for (int i = 0; i < T.len; i++) printf("%02x", T.code[i]);
        printf("%s\n", msg);
      }
    }
  }
  if (genf) fclose(genf);
  printf("unit tests: %u forms, %u runs, %u failing forms, %u skipped\n", g_ntests, total, fail_tests, skipped);
  return fail_tests != 0;
}
