// Hybrid test harness (x64 Windows only): run lifted code inside the process that also has the real, native
// GameAssembly.dll loaded. Native entries of lifted functions are patched to jump into the lifted version, and
// lifted code falls back to calling native code for anything not lifted. Lets us bisect lifter bugs per function.
//
// IMPORTANT: MSVC (LTCG) assumes callees preserve volatile registers they do not touch, so the bridge must preserve
// *every* register exactly (including r9-r11 and all xmm), not just the ABI callee-saved set.
#ifdef LIFT_HYBRID
#include "x64rt.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// per-thread state, pointed to by TEB.ArbitraryUserPointer (gs:[0x28])
struct TState { uint64_t cxx_sp; uint32_t depth, pad; uint64_t hostsp[64]; uint64_t c[64]; uint64_t cxxstk[64]; };
extern "C" {
void rt_native_call(Ctx* c, uint64_t target);
void rt_native_bridge();
void rt_bridge_dispatch(Ctx* c, unsigned idx);
void rt_hybrid_thread_init();
}

asm(R"(
.intel_syntax noprefix
.text
.globl rt_native_call
rt_native_call:
    push rbp
    push rbx
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 200
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
    mov r10, gs:[0x28]
    mov eax, dword ptr [r10+8]
    lea r11, [r10+16]
    mov [r11+rax*8], rsp
    lea r11, [r10+528]
    mov [r11+rax*8], rcx
    lea r11, [r10+1040]
    mov r9, [r10]
    mov [r11+rax*8], r9
    inc eax
    mov dword ptr [r10+8], eax
    mov [r10], rsp
    mov r11, rcx
    mov rax, rdx
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
    mov r12, [r11+96]
    mov r13, [r11+104]
    mov r14, [r11+112]
    mov r15, [r11+120]
    mov r8, [r11+64]
    mov r9, [r11+72]
    mov rdx, [r11+16]
    mov rcx, [r11+8]
    mov rsp, [r11+32]
    add rsp, 8
    mov [rsp-16], rax
    mov rax, [r11]
    mov r10, [r11+80]
    mov r11, [r11+88]
    call qword ptr [rsp-16]
    mov [rsp-16], r10
    mov [rsp-24], r11
    mov [rsp-32], r9
    mov r10, gs:[0x28]
    mov r11d, dword ptr [r10+8]
    dec r11d
    mov dword ptr [r10+8], r11d
    lea r9, [r10+528]
    mov r9, [r9+r11*8]
    xchg r9, r10
    mov [r10], rax
    mov [r10+8], rcx
    mov [r10+16], rdx
    mov [r10+24], rbx
    mov [r10+32], rsp
    mov [r10+40], rbp
    mov [r10+48], rsi
    mov [r10+56], rdi
    mov [r10+64], r8
    mov rax, [rsp-32]
    mov [r10+72], rax
    mov [r10+96], r12
    mov [r10+104], r13
    mov [r10+112], r14
    mov [r10+120], r15
    movups [r10+128], xmm0
    movups [r10+144], xmm1
    movups [r10+160], xmm2
    movups [r10+176], xmm3
    movups [r10+192], xmm4
    movups [r10+208], xmm5
    movups [r10+224], xmm6
    movups [r10+240], xmm7
    movups [r10+256], xmm8
    movups [r10+272], xmm9
    movups [r10+288], xmm10
    movups [r10+304], xmm11
    movups [r10+320], xmm12
    movups [r10+336], xmm13
    movups [r10+352], xmm14
    movups [r10+368], xmm15
    mov rax, [rsp-16]
    mov [r10+80], rax
    mov rax, [rsp-24]
    mov [r10+88], rax
    mov rsp, [r9+16+r11*8]
    mov rax, [r9+1040+r11*8]
    mov [r9], rax
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
    add rsp, 200
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbx
    pop rbp
    ret

# Entered by `jmp` from a per-function thunk that did:  mov dword ptr [rsp-8], idx
.globl rt_native_bridge
rt_native_bridge:
    mov [rsp-16], r10
    mov [rsp-24], r11
    mov r10, gs:[0x28]
    test r10, r10
    jz 8f
7:
    mov r10, rsp
    mov r11d, dword ptr [rsp-8]
    mov rsp, gs:[0x28]
    mov rsp, [rsp]
    sub rsp, 0x1c0
    mov [rsp+0x68], r9
    lea r9, [rsp+0x20]
    mov [r9], rax
    mov [r9+8], rcx
    mov [r9+16], rdx
    mov [r9+24], rbx
    mov [r9+32], r10
    mov [r9+40], rbp
    mov [r9+48], rsi
    mov [r9+56], rdi
    mov [r9+64], r8
    mov [r9+96], r12
    mov [r9+104], r13
    mov [r9+112], r14
    mov [r9+120], r15
    mov rax, [r10-16]
    mov [r9+80], rax
    mov rax, [r10-24]
    mov [r9+88], rax
    mov rax, [r10]
    mov [r9+384], rax
    movups [r9+128], xmm0
    movups [r9+144], xmm1
    movups [r9+160], xmm2
    movups [r9+176], xmm3
    movups [r9+192], xmm4
    movups [r9+208], xmm5
    movups [r9+224], xmm6
    movups [r9+240], xmm7
    movups [r9+256], xmm8
    movups [r9+272], xmm9
    movups [r9+288], xmm10
    movups [r9+304], xmm11
    movups [r9+320], xmm12
    movups [r9+336], xmm13
    movups [r9+352], xmm14
    movups [r9+368], xmm15
    mov rax, gs:[0x28]
    mov [rax], rsp
    mov rcx, r9
    mov edx, r11d
    call rt_bridge_dispatch
    lea r9, [rsp+0x20]
    mov rax, [r9]
    mov rcx, [r9+8]
    mov rdx, [r9+16]
    mov rbx, [r9+24]
    mov rbp, [r9+40]
    mov rsi, [r9+48]
    mov rdi, [r9+56]
    mov r8, [r9+64]
    mov r12, [r9+96]
    mov r13, [r9+104]
    mov r14, [r9+112]
    mov r15, [r9+120]
    movups xmm0, [r9+128]
    movups xmm1, [r9+144]
    movups xmm2, [r9+160]
    movups xmm3, [r9+176]
    movups xmm4, [r9+192]
    movups xmm5, [r9+208]
    movups xmm6, [r9+224]
    movups xmm7, [r9+240]
    movups xmm8, [r9+256]
    movups xmm9, [r9+272]
    movups xmm10, [r9+288]
    movups xmm11, [r9+304]
    movups xmm12, [r9+320]
    movups xmm13, [r9+336]
    movups xmm14, [r9+352]
    movups xmm15, [r9+368]
    mov r10, [r9+32]
    mov r11, gs:[0x28]
    lea rsp, [rsp+0x1c0]
    mov [r11], rsp
    mov r11, [r9+384]
    mov [r10-8], r11
    lea rsp, [r10-8]
    mov r10, [r9+80]
    mov r11, [r9+88]
    mov r9, [r9+72]
    ret
8:
    mov r10, rsp
    lea r11, [rsp-0x400]
    and r11, -16
    mov [r11], r10
    mov [r11+8], rax
    mov [r11+16], rcx
    mov [r11+24], rdx
    mov [r11+32], r8
    mov [r11+40], r9
    movups [r11+48], xmm0
    movups [r11+64], xmm1
    movups [r11+80], xmm2
    movups [r11+96], xmm3
    movups [r11+112], xmm4
    movups [r11+128], xmm5
    mov rsp, r11
    sub rsp, 32
    call rt_hybrid_thread_init
    add rsp, 32
    mov rax, [rsp+8]
    mov rcx, [rsp+16]
    mov rdx, [rsp+24]
    mov r8, [rsp+32]
    mov r9, [rsp+40]
    movups xmm0, [rsp+48]
    movups xmm1, [rsp+64]
    movups xmm2, [rsp+80]
    movups xmm3, [rsp+96]
    movups xmm4, [rsp+112]
    movups xmm5, [rsp+128]
    mov r10, [rsp]
    mov rsp, r10
    mov r10, [rsp-16]
    mov r11, [rsp-24]
    jmp 7b
.att_syntax
)");

uint64_t rt_hybrid_tramp(unsigned idx);
extern int g_rt_tracing;

static thread_local int t_depth = 0;
void rt_bridge_dispatch(Ctx* c, unsigned idx) {
  static thread_local unsigned chain[1024];
  struct DG { DG() { t_depth++; } ~DG() { t_depth--; } } dg;
  static thread_local uint64_t rach[1024];
  if (t_depth < 1024) { chain[t_depth] = idx; rach[t_depth] = c->aux; }
  if (t_depth == 100000) { extern void rt_dump_native(); rt_dump_native(); fprintf(stderr, "[deep] chain:"); for (int i = 560; i < 600; i++) fprintf(stderr, " %llx", (unsigned long long)rt_table_va(chain[i])); fprintf(stderr, "%c RA:", 10); for (int i = 580; i < 600; i++) fprintf(stderr, " %llx", (unsigned long long)(rach[i] - g_delta)); fprintf(stderr, "%c", 10); abort(); }
  static int tr = -2, pass = -2;
  if (tr == -2) { const char* e = getenv("LIFT_TRACE"); tr = e ? atoi(e) : -1; }
  if (pass == -2) { const char* e = getenv("LIFT_PASS"); pass = e ? atoi(e) : -1; }
  if ((int)idx == tr) {
    fprintf(stderr, "[trace %u] IN rcx=%llx rdx=%llx r8=%llx r9=%llx rsp=%llx RA=%llx\n", idx, (unsigned long long)c->r[1], (unsigned long long)c->r[2], (unsigned long long)c->r[8], (unsigned long long)c->r[9], (unsigned long long)c->r[4], (unsigned long long)c->aux);
    g_rt_tracing = 1;
  }
  if ((int)idx == pass) rt_native_call(c, rt_hybrid_tramp(idx));
  else rt_table_fn(idx)(c);
  if ((int)idx == tr) {
    g_rt_tracing = 0;
    fprintf(stderr, "[trace %u] OUT rax=%llx rsp=%llx\n", idx, (unsigned long long)c->r[0], (unsigned long long)c->r[4]);
  }
}

static uint8_t* g_thunks = nullptr;
static uint8_t* g_tramps = nullptr;
uint64_t rt_hybrid_tramp(unsigned idx) { return g_tramps ? (uint64_t)(g_tramps + (size_t)idx * 48) : 0; }

// Patch the native entry of lifted function `idx` (preferred VA) so it jumps into the bridge.
// fn_size packs: low 16 bits = size of the function's first bytes, high 16 bits = length of the relocatable prologue.
bool rt_hybrid_patch(unsigned idx, uint64_t native_va, uint64_t fn_size) {
  unsigned psz = (unsigned)((fn_size >> 16) & 0x7fff);
  fn_size &= 0xffff;
  if (fn_size < 5) return false;
  uint8_t* thunk = g_thunks + (size_t)idx * 64;
  // mov dword ptr [rsp-8], idx ; jmp [rip+0] ; <8 byte bridge addr>
  thunk[0] = 0xC7; thunk[1] = 0x44; thunk[2] = 0x24; thunk[3] = 0xF8; *(uint32_t*)(thunk + 4) = idx;
  thunk[8] = 0xFF; thunk[9] = 0x25; *(uint32_t*)(thunk + 10) = 0;
  *(uint64_t*)(thunk + 14) = (uint64_t)&rt_native_bridge;
  uint8_t* site = (uint8_t*)native_va;
  int64_t rel = (int64_t)thunk - (int64_t)(site + 5);
  if (rel > INT32_MAX || rel < INT32_MIN) return false;
  if (psz) {   // trampoline = original prologue + jump back, used by LIFT_PASS (native pass-through)
    uint8_t* tr = g_tramps + (size_t)idx * 48;
    memcpy(tr, (void*)native_va, psz);
    tr[psz] = 0xFF; tr[psz + 1] = 0x25; *(uint32_t*)(tr + psz + 2) = 0;
    *(uint64_t*)(tr + psz + 6) = native_va + psz;
  }
  site[0] = 0xE9; *(int32_t*)(site + 1) = (int32_t)rel;
  return true;
}

void rt_hybrid_thread_init() {
  size_t sz = 16u << 20;
  uint8_t* st = (uint8_t*)VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  TState* ts = (TState*)calloc(1, sizeof(TState));
  ts->cxx_sp = ((uint64_t)st + sz - 64) & ~15ull;
  ((NT_TIB*)NtCurrentTeb())->ArbitraryUserPointer = ts;
}
void rt_hybrid_init(uint64_t image_base, uint64_t image_size) {
  rt_hybrid_thread_init();
  size_t tsz = (size_t)rt_table_count() * (64 + 48) + 4096;
  for (uint64_t hint = image_base - 0x20000000ull; hint > 0x10000; hint -= 0x1000000ull) {
    g_thunks = (uint8_t*)VirtualAlloc((void*)(hint), tsz, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (g_thunks) break;
  }
  if (!g_thunks) { fprintf(stderr, "[rt] cannot allocate thunks\n"); abort(); }
  g_tramps = g_thunks + (size_t)rt_table_count() * 64 + 64;
  DWORD old;
  VirtualProtect((void*)image_base, image_size, PAGE_EXECUTE_READWRITE, &old);
  g_gs = (uint64_t)NtCurrentTeb();
}
#endif
