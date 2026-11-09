// Windows x64 structured/C++ exception emulation for lifted code.
// RaiseException / RtlUnwindEx / RtlVirtualUnwind / RtlLookupFunctionEntry are implemented on top of the DLL's own
// .pdata/UNWIND_INFO and its personality routines (which are lifted guest code). Control transfer to a catch
// continuation is done by throwing a C++ GuestResume that is caught by the landing pad of the lifted function owning
// the target frame (see tools/lift/lift.py, EH wrapper).
#include "rt_full.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#pragma pack(push, 1)
struct RuntimeFunction { uint32_t begin, end, unwind; };
#pragma pack(pop)

// CONTEXT offsets
enum { C_FLAGS = 0x30, C_RAX = 0x78, C_RCX = 0x80, C_RDX = 0x88, C_RBX = 0x90, C_RSP = 0x98, C_RBP = 0xA0, C_RSI = 0xA8, C_RDI = 0xB0, C_R8 = 0xB8, C_RIP = 0xF8, C_XMM0 = 0x1A0, C_SIZE = 0x4D0 };
static const int kCtxReg[16] = {C_RAX, C_RCX, C_RDX, C_RBX, C_RSP, C_RBP, C_RSI, C_RDI, C_R8, C_R8 + 8, C_R8 + 16, C_R8 + 24, C_R8 + 32, C_R8 + 40, C_R8 + 48, C_R8 + 56};
static inline uint64_t& CR(uint8_t* ctx, int off) { return *(uint64_t*)(ctx + off); }

struct ExcState {
  uint8_t* record = nullptr;        // EXCEPTION_RECORD of the exception being dispatched
  uint8_t throwCtx[C_SIZE];         // context at the throw site (start of the walk)
};
static thread_local std::vector<ExcState> t_excStack;
static thread_local ExcState t_exc;   // current (top) state

// ---------------- function table ----------------
static uint32_t g_pdataRva = 0, g_pdataCount = 0;
static void initPdata() {
  if (g_pdataRva) return;
  // locate .pdata via the PE header (exception directory = data directory 3)
  uint8_t* mem = g_img.mem;
  int32_t lfanew = *(int32_t*)(mem + 0x3c);
  uint8_t* nt = mem + lfanew;
  uint16_t optsz = *(uint16_t*)(nt + 4 + 16);
  uint8_t* opt = nt + 4 + 20;
  uint32_t* dd = (uint32_t*)(opt + 112);   // data directories
  g_pdataRva = dd[3 * 2]; g_pdataCount = dd[3 * 2 + 1] / 12;
  (void)optsz;
}
static RuntimeFunction* lookupFunction(uint64_t pc) {
  initPdata();
  if (pc < g_img.base || pc >= g_img.base + g_img.size) return nullptr;
  uint32_t rva = (uint32_t)(pc - g_img.base);
  RuntimeFunction* t = (RuntimeFunction*)(g_img.mem + g_pdataRva);
  uint32_t lo = 0, hi = g_pdataCount;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (rva < t[mid].begin) hi = mid; else if (rva >= t[mid].end) lo = mid + 1; else return &t[mid];
  }
  return nullptr;
}

// ---------------- unwinding ----------------
// returns the language handler (or 0); updates ctx to the caller's context; sets *establisher
static uint64_t virtualUnwind(uint32_t handlerFlags, RuntimeFunction* rf, uint8_t* ctx, uint64_t* handlerData, uint64_t* establisher) {
  uint8_t* base = g_img.mem;
  uint64_t rsp = CR(ctx, C_RSP);
  RuntimeFunction* cur = rf;
  uint64_t handler = 0;
  int frameReg = 0, frameOff = 0;
  bool first = true;
  uint64_t establisherFrame = rsp;
  // find the frame register definition along the chain first
  {
    RuntimeFunction* c = rf;
    for (int depth = 0; depth < 8 && c; depth++) {
      uint8_t* ui = base + c->unwind;
      if ((ui[3] & 0xf) && !frameReg) { frameReg = ui[3] & 0xf; frameOff = ui[3] >> 4; }
      if ((ui[0] >> 3) & 4) c = (RuntimeFunction*)(ui + 4 + ((ui[2] + 1) & ~1) * 2); else break;
    }
  }
  if (frameReg) establisherFrame = CR(ctx, kCtxReg[frameReg]) - (uint64_t)frameOff * 16;
  *establisher = establisherFrame;
  for (int depth = 0; depth < 16; depth++) {
    uint8_t* ui = base + cur->unwind;
    uint8_t flags = ui[0] >> 3, ncodes = ui[2];
    uint16_t* codes = (uint16_t*)(ui + 4);
    for (int i = 0; i < ncodes;) {
      uint16_t code = codes[i];
      int op = (code >> 8) & 0xf, info = code >> 12;
      switch (op) {
        case 0: CR(ctx, kCtxReg[info]) = L64(rsp); rsp += 8; i += 1; break;
        case 1: if (info == 0) { rsp += (uint64_t)codes[i + 1] * 8; i += 2; } else { rsp += (uint64_t)codes[i + 1] | ((uint64_t)codes[i + 2] << 16); i += 3; } break;
        case 2: rsp += (uint64_t)info * 8 + 8; i += 1; break;
        case 3: rsp = CR(ctx, kCtxReg[frameReg]) - (uint64_t)frameOff * 16; i += 1; break;
        case 4: CR(ctx, kCtxReg[info]) = L64(rsp + (uint64_t)codes[i + 1] * 8); i += 2; break;
        case 5: CR(ctx, kCtxReg[info]) = L64(rsp + ((uint64_t)codes[i + 1] | ((uint64_t)codes[i + 2] << 16))); i += 3; break;
        case 8: memcpy(ctx + C_XMM0 + info * 16, (void*)(rsp + (uint64_t)codes[i + 1] * 16), 16); i += 2; break;
        case 9: memcpy(ctx + C_XMM0 + info * 16, (void*)(rsp + ((uint64_t)codes[i + 1] | ((uint64_t)codes[i + 2] << 16))), 16); i += 3; break;
        case 10: i += 1; break;   // PUSH_MACHFRAME (not used by this DLL)
        default: i += 1; break;
      }
    }
    uint32_t* tail = (uint32_t*)(ui + 4 + ((ncodes + 1) & ~1) * 2);
    if (flags & 4) {   // chained
      RuntimeFunction* parent = (RuntimeFunction*)tail;
      cur = parent;
      first = false;
      continue;
    }
    if ((flags & handlerFlags) && handler == 0) { handler = g_img.base + tail[0]; *handlerData = (uint64_t)(uintptr_t)(tail + 1); }
    break;
  }
  (void)first;
  CR(ctx, C_RIP) = L64(rsp);
  CR(ctx, C_RSP) = rsp + 8;
  return handler;
}

static void ctxFromCtx(uint8_t* ctx, Ctx* c, uint64_t ripOverride, uint64_t rspOverride) {
  memset(ctx, 0, C_SIZE);
  *(uint32_t*)(ctx + C_FLAGS) = 0x10000F;
  for (int i = 0; i < 16; i++) CR(ctx, kCtxReg[i]) = c->r[i];
  for (int i = 0; i < 16; i++) memcpy(ctx + C_XMM0 + i * 16, &c->x[i], 16);
  CR(ctx, C_RIP) = ripOverride; CR(ctx, C_RSP) = rspOverride;
}

// ---------------- RtlLookupFunctionEntry / RtlVirtualUnwind / RtlCaptureContext ----------------
static void* RtlLookupFunctionEntry_(uint64_t pc, uint64_t* imageBase, void*) {
  RuntimeFunction* rf = lookupFunction(pc);
  *imageBase = rf ? g_img.base : 0;
  return rf;
}
static void* RtlVirtualUnwind_(uint32_t type, uint64_t imageBase, uint64_t pc, RuntimeFunction* rf, uint8_t* ctx, uint64_t* handlerData, uint64_t* establisher, void*) {
  uint64_t hd = 0, est = 0;
  uint64_t h = virtualUnwind(type, rf, ctx, &hd, &est);
  if (handlerData) *handlerData = hd;
  if (establisher) *establisher = est;
  return (void*)(uintptr_t)h;
}
static void RtlCaptureContext_(Ctx* c);   // needs the guest Ctx: implemented as a raw shim below

// ---------------- dispatch ----------------
struct DispatcherContext { uint64_t controlPc, imageBase, funcEntry, establisher, targetIp, contextRecord, languageHandler, handlerData, historyTable; uint32_t scopeIndex, fill; };

[[noreturn]] static void unhandled(uint8_t* rec) {
  uint32_t code = *(uint32_t*)rec;
  uint64_t addr = *(uint64_t*)(rec + 0x10);
  fprintf(stderr, "[rt] UNHANDLED exception %08x at %llx (va %llx)\n", code, (unsigned long long)addr, (unsigned long long)(addr - g_delta));
  // dump a short guest stack walk
  uint8_t ctx[C_SIZE]; memcpy(ctx, t_exc.throwCtx, C_SIZE);
  for (int i = 0; i < 24; i++) {
    uint64_t pc = CR(ctx, C_RIP);
    fprintf(stderr, "   frame %d pc %llx (va %llx)\n", i, (unsigned long long)pc, (unsigned long long)(pc - g_delta));
    RuntimeFunction* rf = lookupFunction(pc);
    if (!rf) { CR(ctx, C_RIP) = L64(CR(ctx, C_RSP)); CR(ctx, C_RSP) += 8; if (!CR(ctx, C_RIP)) break; continue; }
    uint64_t hd, est; virtualUnwind(3, rf, ctx, &hd, &est);
    if (!CR(ctx, C_RIP)) break;
  }
  fflush(stderr);
  abort();
}

static int callHandler(uint64_t handler, uint8_t* rec, uint64_t establisher, uint8_t* frameCtx, DispatcherContext* dc) {
  return rt_guest_call<int>(handler, (void*)rec, (void*)(uintptr_t)establisher, (void*)frameCtx, (void*)dc);
}

static void doRaise(Ctx* c, uint32_t code, uint32_t flags, uint32_t nargs, const uint64_t* args) {
  if (code == 0x406D1388u) return;   // SetThreadName
  uint8_t* rec = (uint8_t*)calloc(1, 0x98);
  *(uint32_t*)rec = code; *(uint32_t*)(rec + 4) = flags;
  uint64_t ra = L64(c->r[4]);
  *(uint64_t*)(rec + 0x10) = ra;
  *(uint32_t*)(rec + 0x18) = nargs > 15 ? 15 : nargs;
  for (uint32_t i = 0; i < nargs && i < 15; i++) *(uint64_t*)(rec + 0x20 + 8 * i) = args[i];
  t_excStack.push_back(t_exc);
  t_exc.record = rec;
  ctxFromCtx(t_exc.throwCtx, c, ra, c->r[4] + 8);
  uint8_t ctx[C_SIZE]; memcpy(ctx, t_exc.throwCtx, C_SIZE);
  for (int depth = 0; depth < 4096; depth++) {
    uint64_t pc = CR(ctx, C_RIP);
    RuntimeFunction* rf = lookupFunction(pc);
    if (!rf) { if (!pc) break; CR(ctx, C_RIP) = L64(CR(ctx, C_RSP)); CR(ctx, C_RSP) += 8; if (!CR(ctx, C_RIP)) break; continue; }
    uint8_t frameCtx[C_SIZE]; memcpy(frameCtx, ctx, C_SIZE);
    uint64_t hd = 0, est = 0;
    uint64_t h = virtualUnwind(1, rf, ctx, &hd, &est);   // ctx is now the caller's context
    if (h) {
      DispatcherContext dc{pc, g_img.base, (uint64_t)(uintptr_t)rf, est, 0, (uint64_t)(uintptr_t)frameCtx, h, hd, 0, 0, 0};
      int d = callHandler(h, rec, est, frameCtx, &dc);
      if (d == 0) { fprintf(stderr, "[rt] handler requested ExceptionContinueExecution\n"); abort(); }
    }
    if (!CR(ctx, C_RIP)) break;
  }
  unhandled(rec);
}

static void RaiseException_(Ctx* c) {
  // raw shim: lifted calling convention (rcx=code, rdx=flags, r8=nargs, r9=args*)
  doRaise(c, (uint32_t)c->r[1], (uint32_t)c->r[2], (uint32_t)c->r[8], (const uint64_t*)(uintptr_t)c->r[9]);
  c->r[4] += 8;
}

static void RtlUnwindEx_(Ctx* c) {
  uint64_t targetFrame = c->r[1], targetIp = c->r[2];
  uint8_t* rec = (uint8_t*)(uintptr_t)c->r[8];
  uint64_t retVal = c->r[9];
  if (!rec) rec = t_exc.record;
  if (!rec) { fprintf(stderr, "[rt] RtlUnwindEx without active exception\n"); abort(); }
  *(uint32_t*)(rec + 4) |= 2;   // EXCEPTION_UNWINDING
  uint8_t ctx[C_SIZE]; memcpy(ctx, t_exc.throwCtx, C_SIZE);
  for (int depth = 0; depth < 4096; depth++) {
    uint64_t pc = CR(ctx, C_RIP);
    RuntimeFunction* rf = lookupFunction(pc);
    if (!rf) { if (!pc) break; CR(ctx, C_RIP) = L64(CR(ctx, C_RSP)); CR(ctx, C_RSP) += 8; if (!CR(ctx, C_RIP)) break; continue; }
    uint8_t frameCtx[C_SIZE]; memcpy(frameCtx, ctx, C_SIZE);
    uint64_t hd = 0, est = 0;
    uint64_t h = virtualUnwind(2, rf, ctx, &hd, &est);
    bool isTarget = targetFrame && est == targetFrame;
    if (h) {
      DispatcherContext dc{pc, g_img.base, (uint64_t)(uintptr_t)rf, est, isTarget ? targetIp : 0, (uint64_t)(uintptr_t)frameCtx, h, hd, 0, 0, 0};
      if (isTarget) *(uint32_t*)(rec + 4) |= 0x20;   // EXCEPTION_TARGET_UNWIND
      callHandler(h, rec, est, frameCtx, &dc);
    }
    if (isTarget) {
      GuestResume e;
      e.entry_rsp = CR(ctx, C_RSP) - 8;   // the caller context after unwinding this frame: rsp = entry_rsp + 8
      e.rip = targetIp;
      for (int i = 0; i < 16; i++) e.r[i] = CR(frameCtx, kCtxReg[i]);
      for (int i = 0; i < 16; i++) memcpy(&e.x[i], frameCtx + C_XMM0 + i * 16, 16);
      e.r[0] = retVal;
      *(uint32_t*)(rec + 4) &= ~0x22u;
      if (!t_excStack.empty()) { t_exc = t_excStack.back(); t_excStack.pop_back(); }
      throw e;
    }
    if (!CR(ctx, C_RIP)) break;
  }
  fprintf(stderr, "[rt] RtlUnwindEx: target frame %llx not found\n", (unsigned long long)targetFrame);
  abort();
}

static void RtlCaptureContext_Shim(Ctx* c) {
  uint8_t* ctx = (uint8_t*)(uintptr_t)c->r[1];
  ctxFromCtx(ctx, c, L64(c->r[4]), c->r[4] + 8);
  c->r[4] += 8;
}

// pc -> image base (RtlPcToFileHeader)
void rt_eh_register() {
  rt_register_named("kernel32!raiseexception", rt_make_shim([](Ctx* c, void*) { RaiseException_(c); }, nullptr, "RaiseException"));
  rt_register_named("kernel32!rtlunwindex", rt_make_shim([](Ctx* c, void*) { RtlUnwindEx_(c); c->r[4] += 8; }, nullptr, "RtlUnwindEx"));
  rt_register_named("kernel32!rtlunwind", rt_make_shim([](Ctx* c, void*) { RtlUnwindEx_(c); c->r[4] += 8; }, nullptr, "RtlUnwind"));
  rt_register_named("kernel32!rtlcapturecontext", rt_make_shim([](Ctx* c, void*) { RtlCaptureContext_Shim(c); }, nullptr, "RtlCaptureContext"));
  rt_register_named("kernel32!rtllookupfunctionentry", makeShim(RtlLookupFunctionEntry_, "RtlLookupFunctionEntry"));
  rt_register_named("kernel32!rtlvirtualunwind", makeShim(RtlVirtualUnwind_, "RtlVirtualUnwind"));
}
