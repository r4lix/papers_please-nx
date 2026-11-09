// Native replacements for hot, pure Haxe library functions of the game (startup is dominated by asset decoding
// running in lifted code: zlib inflate, PNG extraction, XXTEA).  Each override falls back to the lifted original
// whenever its preconditions are not met; RT_FP_VERIFY=1 runs both and compares.
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "rt_full.h"
#include "il2cpp_api.h"
#include <map>
#include <string>

extern uint64_t g_delta;
extern int g_hostFrame;
bool gfxCursorVisible();

namespace {

// ---------------- inflate (RFC1951, compact decoder after zlib's puff) ----------------
struct Huff { short count[16]; short symbol[320]; };
struct Inf {
  const uint8_t* in; size_t len, pos = 0; uint32_t bitbuf = 0; int bitcnt = 0;
  std::vector<uint8_t>* out; bool err = false;
  uint32_t bits(int need) {
    uint32_t v = bitbuf;
    while (bitcnt < need) {
      if (pos >= len) { err = true; return 0; }
      v |= (uint32_t)in[pos++] << bitcnt; bitcnt += 8;
    }
    bitbuf = v >> need; bitcnt -= need;
    return v & ((1u << need) - 1);
  }
};
static int construct(Huff* h, const short* length, int n) {
  for (int l = 0; l <= 15; l++) h->count[l] = 0;
  for (int s = 0; s < n; s++) h->count[length[s]]++;
  if (h->count[0] == n) return 0;
  int left = 1;
  for (int l = 1; l <= 15; l++) { left <<= 1; left -= h->count[l]; if (left < 0) return left; }
  short offs[16]; offs[1] = 0;
  for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
  for (int s = 0; s < n; s++) if (length[s] != 0) h->symbol[offs[length[s]]++] = (short)s;
  return left;
}
static int decode(Inf* s, const Huff* h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= 15; len++) {
    code |= (int)s->bits(1);
    if (s->err) return -1;
    int count = h->count[len];
    if (code - count < first) return h->symbol[index + (code - first)];
    index += count; first += count; first <<= 1; code <<= 1;
  }
  return -10;
}
static const short kLens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short kLext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short kDists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short kDext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static bool codes(Inf* s, const Huff* lc, const Huff* dc) {
  std::vector<uint8_t>& o = *s->out;
  for (;;) {
    int sym = decode(s, lc);
    if (sym < 0) return false;
    if (sym < 256) { o.push_back((uint8_t)sym); continue; }
    if (sym == 256) return true;
    sym -= 257;
    if (sym >= 29) return false;
    int len = kLens[sym] + (int)s->bits(kLext[sym]);
    int ds = decode(s, dc);
    if (ds < 0 || ds >= 30 || s->err) return false;
    size_t dist = (size_t)kDists[ds] + s->bits(kDext[ds]);
    if (s->err || dist > o.size()) return false;
    size_t from = o.size() - dist, n = o.size();
    o.resize(n + len);
    uint8_t* p = o.data();
    for (int k = 0; k < len; k++) p[n + k] = p[from + k];
  }
}
static Huff g_fixedL, g_fixedD; static bool g_fixedInit = false;
static bool inflateRaw(const uint8_t* in, size_t len, std::vector<uint8_t>& out) {
  Inf s; s.in = in; s.len = len; s.out = &out;
  int last;
  do {
    last = (int)s.bits(1);
    int type = (int)s.bits(2);
    if (s.err) return false;
    if (type == 0) {
      s.bitbuf = 0; s.bitcnt = 0;
      if (s.pos + 4 > s.len) return false;
      unsigned l = in[s.pos] | (in[s.pos + 1] << 8), nl = in[s.pos + 2] | (in[s.pos + 3] << 8);
      s.pos += 4;
      if (l != (~nl & 0xffff) || s.pos + l > s.len) return false;
      out.insert(out.end(), in + s.pos, in + s.pos + l); s.pos += l;
    } else if (type == 1) {
      if (!g_fixedInit) {
        short lengths[288]; int sy = 0;
        for (; sy < 144; sy++) lengths[sy] = 8;
        for (; sy < 256; sy++) lengths[sy] = 9;
        for (; sy < 280; sy++) lengths[sy] = 7;
        for (; sy < 288; sy++) lengths[sy] = 8;
        construct(&g_fixedL, lengths, 288);
        for (sy = 0; sy < 30; sy++) lengths[sy] = 5;
        construct(&g_fixedD, lengths, 30);
        g_fixedInit = true;
      }
      if (!codes(&s, &g_fixedL, &g_fixedD)) return false;
    } else if (type == 2) {
      static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      int nlen = (int)s.bits(5) + 257, ndist = (int)s.bits(5) + 1, ncode = (int)s.bits(4) + 4;
      if (s.err || nlen > 286 || ndist > 30) return false;
      short lengths[320]; int idx;
      for (idx = 0; idx < ncode; idx++) lengths[order[idx]] = (short)s.bits(3);
      for (; idx < 19; idx++) lengths[order[idx]] = 0;
      Huff lc, dc;
      if (construct(&lc, lengths, 19) != 0) return false;
      idx = 0;
      while (idx < nlen + ndist) {
        int sym = decode(&s, &lc);
        if (sym < 0) return false;
        if (sym < 16) lengths[idx++] = (short)sym;
        else {
          int len = 0, rep;
          if (sym == 16) { if (idx == 0) return false; len = lengths[idx - 1]; rep = 3 + (int)s.bits(2); }
          else if (sym == 17) rep = 3 + (int)s.bits(3);
          else rep = 11 + (int)s.bits(7);
          if (s.err || idx + rep > nlen + ndist) return false;
          while (rep--) lengths[idx++] = (short)len;
        }
      }
      if (lengths[256] == 0) return false;
      int e = construct(&lc, lengths, nlen);
      if (e < 0 || (e > 0 && nlen - lc.count[0] != 1)) return false;
      e = construct(&dc, lengths + nlen, ndist);
      if (e < 0 || (e > 0 && ndist - dc.count[0] != 1)) return false;
      if (!codes(&s, &lc, &dc)) return false;
    } else return false;
  } while (!last);
  return true;
}

// ---------------- helpers for managed haxe.io.Bytes ----------------
// Bytes: [0x10] int length, [0x18] byte[] b;  byte[]: [0x18] uint64 length, data at [0x20]
static const uint64_t VA_BYTES_ALLOC = 0x1806ab6b0ull;
static inline uint8_t* bytesData(uint8_t* b, int* len) {
  if (!b) return nullptr;
  uint8_t* arr = *(uint8_t**)(b + 0x18);
  if (!arr) return nullptr;
  int n = *(int*)(b + 0x10); uint64_t al = *(uint64_t*)(arr + 0x18);
  if (n < 0 || (uint64_t)n > al) n = (int)al;
  *len = n; return arr + 0x20;
}

struct SpScope { uint64_t saved; SpScope(uint64_t sp) : saved(t_guest_sp) { t_guest_sp = sp; } ~SpScope() { t_guest_sp = saved; } };

static LiftFn g_origUncompress = nullptr;
static bool g_verify = false;
static long g_nUnc = 0, g_nUncFallback = 0; static double g_bytesIn = 0, g_bytesOut = 0;

static void ovrUncompress(Ctx* c) {
  uint8_t* sb = (uint8_t*)(uintptr_t)c->r[1];
  int len = 0; uint8_t* data = bytesData(sb, &len);
  std::vector<uint8_t> out;
  bool ok = false;
  if (data && len >= 6 && (data[0] & 15) == 8 && !(data[1] & 32) && ((data[0] << 8) | data[1]) % 31 == 0) {
    std::vector<uint8_t> in(data + 2, data + len);   // copy: no managed pointer is held across the allocation below
    out.reserve((size_t)len * 4);
    ok = inflateRaw(in.data(), in.size(), out);
  }
  if (!ok) { g_nUncFallback++; g_origUncompress(c); return; }
  SpScope sp(c->r[4]);
  uint8_t* res = rt_guest_call<uint8_t*>(VA_BYTES_ALLOC + g_delta, (int)out.size());
  int rl = 0; uint8_t* rd = bytesData(res, &rl);
  memcpy(rd, out.data(), out.size());
  g_nUnc++; g_bytesIn += len; g_bytesOut += (double)out.size();
  if (g_verify) {
    Ctx c2 = *c; g_origUncompress(&c2);
    int ol = 0; uint8_t* od = bytesData((uint8_t*)(uintptr_t)c2.r[0], &ol);
    if (ol != (int)out.size() || memcmp(od, out.data(), out.size())) fprintf(stderr, "[fastpath] UNCOMPRESS MISMATCH len %d vs %zu\n", ol, out.size());
  }
  c->r[0] = (uint64_t)(uintptr_t)res;
  c->r[4] += 8;
}


// ---------------- format.png.Tools.extract32 ----------------
static LiftFn g_origExtract32 = nullptr;
static long g_nExt = 0, g_nExtFallback = 0; static double g_pxOut = 0;
static const char* clsName(uint8_t* o) {
  static std::map<void*, std::string> cache;
  void* k = *(void**)o;
  auto it = cache.find(k);
  if (it == cache.end()) { const char* n = il2cpp_class_get_name((Il2CppClass*)k); it = cache.emplace(k, n ? n : "").first; }
  return it->second.c_str();
}
// haxe.lang.DynamicObject: [0x10] int[] hashes, [0x18] object[] dynamics, [0x20] int[] hashes_f, [0x28] double[] dynamics_f, [0x30] int len, [0x34] int len_f
static bool dynF(uint8_t* o, uint32_t hash, double* v) {
  uint8_t* hs = *(uint8_t**)(o + 0x20); uint8_t* ds = *(uint8_t**)(o + 0x28); int n = *(int*)(o + 0x34);
  if (!hs || !ds) return false;
  for (int i = 0; i < n; i++) if (*(uint32_t*)(hs + 0x20 + 4 * i) == hash) { *v = *(double*)(ds + 0x20 + 8 * i); return true; }
  return false;
}
static uint8_t* dynO(uint8_t* o, uint32_t hash) {
  uint8_t* hs = *(uint8_t**)(o + 0x10); uint8_t* ds = *(uint8_t**)(o + 0x18); int n = *(int*)(o + 0x30);
  if (!hs || !ds) return nullptr;
  for (int i = 0; i < n; i++) if (*(uint32_t*)(hs + 0x20 + 4 * i) == hash) return *(uint8_t**)(ds + 0x20 + 8 * i);
  return nullptr;
}
static bool strEq(uint8_t* s, const char* lit) {
  if (!s) return false;
  int n = *(int*)(s + 0x10); if ((size_t)n != strlen(lit)) return false;
  const uint16_t* ch = (const uint16_t*)(s + 0x14);
  for (int i = 0; i < n; i++) if (ch[i] != (uint8_t)lit[i]) return false;
  return true;
}
static bool decodePng(uint8_t* list, bool flip, std::vector<uint8_t>& out, int* pw, int* ph) {
  if (!list) return false;
  uint8_t* hdr = nullptr; std::vector<uint8_t> comp, pal, trns; bool havePal = false, haveTrns = false;
  for (uint8_t* n = *(uint8_t**)(list + 0x10); n; n = *(uint8_t**)(n + 0x18)) {
    uint8_t* ch = *(uint8_t**)(n + 0x10); if (!ch) continue;
    const char* cn = clsName(ch);
    if (!strcmp(cn, "Chunk_CHeader")) { if (!hdr) hdr = *(uint8_t**)(ch + 0x18); }
    else if (!strcmp(cn, "Chunk_CData")) { int l = 0; uint8_t* d = bytesData(*(uint8_t**)(ch + 0x18), &l); if (!d) return false; comp.insert(comp.end(), d, d + l); }
    else if (!strcmp(cn, "Chunk_CPalette")) { int l = 0; uint8_t* d = bytesData(*(uint8_t**)(ch + 0x18), &l); if (!d) return false; pal.assign(d, d + l); havePal = true; }
    else if (!strcmp(cn, "Chunk_CUnknown")) {
      if (strEq(*(uint8_t**)(ch + 0x18), "tRNS")) { int l = 0; uint8_t* d = bytesData(*(uint8_t**)(ch + 0x20), &l); if (!d) return false; trns.assign(d, d + l); haveTrns = true; }
    }
  }
  if (!hdr || comp.size() < 6 || strcmp(clsName(hdr), "DynamicObject")) return false;
  double dw, dh, db;
  if (!dynF(hdr, 0x4a62b606, &dw) || !dynF(hdr, 0x024c07e7, &dh) || !dynF(hdr, 0x66c79b46, &db)) return false;
  int W = (int)dw, H = (int)dh;
  if (db != 8 || W <= 0 || H <= 0 || W > 16384 || H > 16384) return false;
  { uint8_t* il = dynO(hdr, 0x5978cb91); if (il && *(uint8_t*)(il + 0x10)) return false; }   // interlaced
  uint8_t* col = dynO(hdr, 0x4a5c7163);
  if (!col) return false;
  const char* cc = clsName(col);
  int kind, bpp; bool alpha = false;
  if (!strcmp(cc, "Color_ColTrue")) { kind = 0; alpha = *(uint8_t*)(col + 0x18) != 0; bpp = alpha ? 4 : 3; }
  else if (!strcmp(cc, "Color_ColGrey")) { kind = 1; alpha = *(uint8_t*)(col + 0x18) != 0; bpp = alpha ? 2 : 1; }
  else if (!strcmp(cc, "Color_ColIndexed")) { kind = 2; bpp = 1; if (!havePal) return false; }
  else return false;
  if (!((comp[0] & 15) == 8 && !(comp[1] & 32) && ((comp[0] << 8) | comp[1]) % 31 == 0)) return false;
  std::vector<uint8_t> raw; raw.reserve((size_t)(W * bpp + 1) * H);
  if (!inflateRaw(comp.data() + 2, comp.size() - 2, raw)) return false;
  size_t stride = (size_t)W * bpp + 1;
  if (raw.size() < stride * H) return false;
  for (int y = 0; y < H; y++) {   // unfilter in place
    uint8_t* row = raw.data() + stride * y; int f = row[0]; uint8_t* cur = row + 1;
    const uint8_t* up = y ? raw.data() + stride * (y - 1) + 1 : nullptr;
    int n = W * bpp;
    switch (f) {
    case 0: break;
    case 1: for (int i = bpp; i < n; i++) cur[i] = (uint8_t)(cur[i] + cur[i - bpp]); break;
    case 2: if (up) for (int i = 0; i < n; i++) cur[i] = (uint8_t)(cur[i] + up[i]); break;
    case 3:
      for (int i = 0; i < n; i++) { int a = i >= bpp ? cur[i - bpp] : 0, b = up ? up[i] : 0; cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1)); }
      break;
    case 4:
      for (int i = 0; i < n; i++) {
        int a = i >= bpp ? cur[i - bpp] : 0, b = up ? up[i] : 0, c2 = (i >= bpp && up) ? up[i - bpp] : 0;
        int pp = a + b - c2, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c2);
        int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c2);
        cur[i] = (uint8_t)(cur[i] + pr);
      }
      break;
    default: return false;
    }
  }
  out.resize((size_t)W * H * 4);
  for (int y = 0; y < H; y++) {
    const uint8_t* src = raw.data() + stride * y + 1;
    uint8_t* dst = out.data() + (size_t)(flip ? H - 1 - y : y) * W * 4;
    if (kind == 0) {
      if (alpha) for (int x = 0; x < W; x++, src += 4, dst += 4) { dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3]; }
      else for (int x = 0; x < W; x++, src += 3, dst += 4) { dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 255; }
    } else if (kind == 1) {
      if (alpha) for (int x = 0; x < W; x++, src += 2, dst += 4) { dst[0] = dst[1] = dst[2] = src[0]; dst[3] = src[1]; }
      else for (int x = 0; x < W; x++, src += 1, dst += 4) { dst[0] = dst[1] = dst[2] = src[0]; dst[3] = 255; }
    } else {
      for (int x = 0; x < W; x++, src++, dst += 4) {
        size_t pi = *src;
        if (pi * 3 + 2 >= pal.size()) return false;
        dst[0] = pal[pi * 3 + 2]; dst[1] = pal[pi * 3 + 1]; dst[2] = pal[pi * 3];
        dst[3] = (haveTrns && pi < trns.size()) ? trns[pi] : 255;
      }
    }
  }
  *pw = W; *ph = H;
  return true;
}
static void ovrExtract32(Ctx* c) {
  uint8_t* list = (uint8_t*)(uintptr_t)c->r[1];
  uint8_t* given = (uint8_t*)(uintptr_t)c->r[2];
  bool flip = (c->r[8] & 0xff) != 0;
  std::vector<uint8_t> px; int W = 0, H = 0;
  if (!decodePng(list, flip, px, &W, &H)) { g_nExtFallback++; g_origExtract32(c); return; }
  SpScope sp(c->r[4]);
  uint8_t* res = given;
  if (given) { int gl = 0; uint8_t* gd = bytesData(given, &gl); if (!gd || gl < (int)px.size()) { g_nExtFallback++; g_origExtract32(c); return; } }
  else res = rt_guest_call<uint8_t*>(VA_BYTES_ALLOC + g_delta, (int)px.size());
  int rl = 0; uint8_t* rd = bytesData(res, &rl);
  if (!rd || rl < (int)px.size()) { g_nExtFallback++; g_origExtract32(c); return; }
  memcpy(rd, px.data(), px.size());
  g_nExt++; g_pxOut += (double)px.size();
  if (g_verify) {
    std::vector<uint8_t> keep(px);
    Ctx c2 = *c; g_origExtract32(&c2);
    int ol = 0; uint8_t* od = bytesData((uint8_t*)(uintptr_t)c2.r[0], &ol);
    if (ol != (int)keep.size() || memcmp(od, keep.data(), keep.size())) fprintf(stderr, "[fastpath] EXTRACT32 MISMATCH %dx%d len %d vs %zu\n", W, H, ol, keep.size());
  }
  c->r[0] = (uint64_t)(uintptr_t)res;
  c->r[4] += 8;
}
// ---------------- gamepad shortcuts (Vita layout): run the game's own click handlers from inside its per-frame updates ----------------
// R = Booth.shutterSwitch_onClick, L = Border.outerBoothButton_onClick (call next).  Both ignore their Button argument, so null is fine; running them inside
// the owner's update() means `this` is alive and the booth/border is the active one.
static LiftFn g_origBoothUpdate = nullptr, g_origBorderUpdate = nullptr;
static int g_padShortcutFrame[2] = {-1000, -1000};
static const uint64_t VA_BOOTH_SHUTTER_CLICK = 0x1801c9d90ull, VA_BORDER_OUTERBOOTH_CLICK = 0x180441700ull;
static void runShortcut(Ctx* c, int which, uint64_t va) {
  if (g_padShortcutFrame[which] < 0 || g_hostFrame - g_padShortcutFrame[which] > 30) return;   // stale presses (menu, loading) are dropped
  g_padShortcutFrame[which] = -1000;
  if (getenv("RT_PADLOG")) fprintf(stderr, "[pad] shortcut %d runs handler on %llx at frame %d\n", which, (unsigned long long)c->r[1], g_hostFrame);
  SpScope sp(c->r[4]);
  rt_guest_call<void>(va + g_delta, c->r[1], (uint64_t)0, (uint64_t)0);
}
static LiftFn g_origCursorDraw = nullptr;
static void ovrCursorDraw(Ctx* c) { if (!gfxCursorVisible()) { c->r[4] += 8; return; } g_origCursorDraw(c); }   // game-drawn cursor: hidden while touch / buttons / idle
static long g_nBoothUpd = 0, g_nBorderUpd = 0;
int g_boothUpdateFrame = -1000;   // last frame the booth ran update(): i.e. a day is being played
static void ovrBoothUpdate(Ctx* c) { g_nBoothUpd++; g_boothUpdateFrame = g_hostFrame; runShortcut(c, 0, VA_BOOTH_SHUTTER_CLICK); g_origBoothUpdate(c); }
static void ovrBorderUpdate(Ctx* c) { g_nBorderUpd++; runShortcut(c, 1, VA_BORDER_OUTERBOOTH_CLICK); g_origBorderUpdate(c); }
}  // namespace
bool gameInBooth() {
  bool v = g_hostFrame - g_boothUpdateFrame < 6;
  static bool last = false; if (v != last && getenv("RT_PADLOG")) fprintf(stderr, "[pad] in booth: %d at frame %d\n", (int)v, g_hostFrame);
  last = v; return v;
}
void padShortcut(int which) { if (which >= 0 && which < 2) g_padShortcutFrame[which] = g_hostFrame; }
struct FpEntry { uint64_t va; LiftFn* hook; LiftFn orig; };
extern const FpEntry g_fp_entries[];
static void install(uint64_t va, LiftFn fn, LiftFn* orig, const char* name) {
  for (const FpEntry* e = g_fp_entries; e->va; e++) if (e->va == va) { if (orig) *orig = e->orig; *e->hook = fn; return; }
  fprintf(stderr, "[fastpath] %s: not patchable (add it to tools/fp_vas.txt)\n", name);
}

void fastpathInstall() {
  if (getenv("RT_NOFASTPATH")) return;
  g_verify = getenv("RT_FP_VERIFY") != nullptr;
  install(0x1806845d0ull, ovrUncompress, &g_origUncompress, "haxe.zip.Uncompress.run");
  install(0x18095d660ull, ovrExtract32, &g_origExtract32, "format.png.Tools.extract32");
  install(0x180536a20ull, ovrCursorDraw, &g_origCursorDraw, "CursorEnt.draw");
  install(0x1801ca450ull, ovrBoothUpdate, &g_origBoothUpdate, "Booth.update");
  install(0x180445e40ull, ovrBorderUpdate, &g_origBorderUpdate, "Border.update");
}

void fastpathReport() {
  printf("[fastpath] booth.update %ld calls (last frame %d), border.update %ld calls\n", g_nBoothUpd, g_boothUpdateFrame, g_nBorderUpd);
  printf("[fastpath] extract32: %ld native (%.1f MB), %ld fallbacks\n", g_nExt, g_pxOut / 1048576.0, g_nExtFallback);
  printf("[fastpath] uncompress: %ld native (%.1f MB -> %.1f MB), %ld fallbacks\n", g_nUnc, g_bytesIn / 1048576.0, g_bytesOut / 1048576.0, g_nUncFallback);
}
