#!/usr/bin/env python3
"""Static x86-64 -> C++ lifter for GameAssembly.dll (see RESUME.md, phase B).

usage: lift.py <dll> <outdir> [--only 0xVA,0xVA...] [--shard N]
Generates outdir/lift_NNN.cpp, lift_decls.h, lift_table.cpp, lift_stats.txt
"""
import sys, os, re, struct, collections, time
from capstone import *
from capstone.x86 import *
sys.path.insert(0, os.path.dirname(__file__))
from pe import PEImg

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

C_, Z_, S_, O_, P_ = 1, 2, 4, 8, 16
ALLF = 31
FLAGVAR = {C_: 'fC', Z_: 'fZ', S_: 'fS', O_: 'fO', P_: 'fP'}

GPR64 = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi'] + ['r%d' % i for i in range(8, 16)]
GPR32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'] + ['r%dd' % i for i in range(8, 16)]
GPR16 = ['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di'] + ['r%dw' % i for i in range(8, 16)]
GPR8 = ['al', 'cl', 'dl', 'bl', 'spl', 'bpl', 'sil', 'dil'] + ['r%db' % i for i in range(8, 16)]
GPR8H = ['ah', 'ch', 'dh', 'bh']
REGMAP = {}
for i in range(16):
    REGMAP[GPR64[i]] = (i, 8, 0)
    REGMAP[GPR32[i]] = (i, 4, 0)
    REGMAP[GPR16[i]] = (i, 2, 0)
    REGMAP[GPR8[i]] = (i, 1, 0)
for i, n in enumerate(GPR8H):
    REGMAP[n] = (i, 1, 8)
XMMMAP = {'xmm%d' % i: i for i in range(16)}
XMMMAP.update({'ymm%d' % i: i for i in range(16)})

TY = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t', 8: 'uint64_t'}
STY = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t', 8: 'int64_t'}
MASK = {1: 0xff, 2: 0xffff, 4: 0xffffffff, 8: 0xffffffffffffffff}

CC_USE = {
    'o': O_, 'no': O_, 'b': C_, 'ae': C_, 'e': Z_, 'ne': Z_, 'be': C_ | Z_, 'a': C_ | Z_, 's': S_, 'ns': S_,
    'p': P_, 'np': P_, 'l': S_ | O_, 'ge': S_ | O_, 'le': Z_ | S_ | O_, 'g': Z_ | S_ | O_,
}
CC_ALIAS = {'c': 'b', 'nae': 'b', 'nc': 'ae', 'nb': 'ae', 'z': 'e', 'nz': 'ne', 'na': 'be', 'nbe': 'a', 'pe': 'p', 'po': 'np',
            'nge': 'l', 'nl': 'ge', 'ng': 'le', 'nle': 'g'}
CC_EXPR = {
    'o': 'fO', 'no': '!fO', 'b': 'fC', 'ae': '!fC', 'e': 'fZ', 'ne': '!fZ', 'be': '(fC|fZ)', 'a': '(!fC&&!fZ)', 's': 'fS', 'ns': '!fS',
    'p': 'fP', 'np': '!fP', 'l': '(fS!=fO)', 'ge': '(fS==fO)', 'le': '(fZ||(fS!=fO))', 'g': '(!fZ&&(fS==fO))',
}


def cc_of(mnem, prefix):
    cc = mnem[len(prefix):]
    return CC_ALIAS.get(cc, cc)


class Unimpl(Exception):
    pass


def hx(v):
    return '0x%xull' % v


class Insn:
    __slots__ = ('addr', 'size', 'mn', 'ops', 'raw', 'ci', 'prefix')


class FuncLift:
    def __init__(self, img, root, ranges, group):
        self.img = img
        self.root = root
        self.ranges = ranges      # list of (b,e) allowed regions or None
        self.group = group        # set of function-start VAs belonging to this function (entry points)
        self.insns = {}
        self.targets = set()      # local jump targets (labels)
        self.tails = {}           # addr -> target (tail jmps out)
        self.calls = set()
        self.tables = {}          # jmp addr -> list of target addrs
        self.leaders = set()

    def in_range(self, va):
        if self.ranges is None:
            return self.root <= va < self.root + 0x20000
        for b, e in self.ranges:
            if b <= va < e:
                return True
        return False


def decode_one(img, va):
    code = img.rd(va, 16)
    for ci in md.disasm(code, va, 1):
        i = Insn()
        i.addr = va
        i.size = ci.size
        i.mn = ci.mnemonic
        i.ops = ci.operands
        i.raw = ci.bytes
        i.ci = ci
        i.prefix = ci.prefix
        return i
    return None


TERMINATORS = ('ret', 'jmp', 'int3', 'ud2', 'hlt', 'retf', 'bnd ret', 'bnd jmp')


def is_jcc(mn):
    return mn.startswith('j') and mn != 'jmp' and mn not in ('jrcxz', 'jecxz')


def decode_func(img, fl, known_starts):
    """recursive-descent decode of fl; fills insns, targets, tails, calls"""
    work = list(fl.group)
    seen = set()
    while work:
        va = work.pop()
        while True:
            if va in fl.insns:
                break
            if not fl.in_range(va) and va not in fl.group:
                break
            ins = decode_one(img, va)
            if ins is None:
                break
            fl.insns[va] = ins
            mn = ins.mn
            nxt = va + ins.size
            if mn.startswith('bnd '):
                mn = mn[4:]
            if mn == 'ret' or mn == 'retf' or mn in ('int3', 'ud2', 'hlt'):
                break
            if mn == 'jmp':
                op = ins.ops[0]
                if op.type == X86_OP_IMM:
                    t = op.imm
                    local = (fl.in_range(t) and (t not in known_starts or t in fl.group or fl.ranges is not None and fl.in_range(t) and t not in known_starts))
                    if fl.ranges is None:
                        local = fl.in_range(t) and t not in known_starts and t > fl.root - 1
                    else:
                        local = fl.in_range(t)
                    if local:
                        fl.targets.add(t)
                        work.append(t)
                    else:
                        fl.tails[va] = t
                else:
                    tl = find_jump_table(img, fl, ins)
                    if tl is None:
                        UNRESOLVED.append(va)
                    if tl is not None:
                        fl.tables[va] = tl
                        for t in tl:
                            fl.targets.add(t)
                            work.append(t)
                break
            if is_jcc(mn) or mn in ('jrcxz', 'jecxz', 'loop'):
                t = ins.ops[0].imm
                if fl.ranges is None:
                    local = fl.in_range(t) and t not in known_starts and t > fl.root - 1
                else:
                    local = fl.in_range(t)
                if local:
                    fl.targets.add(t)
                    work.append(t)
                else:
                    fl.tails[va] = t
                va = nxt
                continue
            if mn == 'call':
                op = ins.ops[0]
                if op.type == X86_OP_IMM:
                    fl.calls.add(op.imm)
            va = nxt
    return fl


def find_jump_table(img, fl, ins):
    """pattern: lea B,[rip+X]; mov R32,[B+I*4+D]; add R,B; jmp R"""
    op = ins.ops[0]
    if op.type != X86_OP_REG:
        return None
    R = ins.ci.reg_name(op.reg)
    va = ins.addr
    # walk back over already-decoded straight-line instructions
    ends = {i.addr + i.size: i for i in fl.insns.values()}
    seq = []
    cur = va
    while cur in ends and len(seq) < 24:
        i = ends[cur]
        seq.append(i)
        cur = i.addr
    seq.reverse()
    seq = seq[-24:]
    base_val = None
    tbl_disp = None
    bound = None
    Rfull = R
    for i in reversed(seq):
        mn = i.mn
        ci = i.ci
        if mn == 'add' and len(i.ops) == 2 and i.ops[0].type == X86_OP_REG and i.ops[1].type == X86_OP_REG:
            a = ci.reg_name(i.ops[0].reg)
            b = ci.reg_name(i.ops[1].reg)
            if tbl_disp is None and base_val is None and a == Rfull:
                pass
        if mn in ('mov', 'movsxd') and len(i.ops) == 2 and i.ops[1].type == X86_OP_MEM and i.ops[0].type == X86_OP_REG and tbl_disp is None:
            m = i.ops[1].mem
            if m.index != 0 and m.scale == 4 and m.base != 0:
                tbl_disp = (ci.reg_name(m.base), m.disp, ci.reg_name(m.index))
        if mn == 'lea' and len(i.ops) == 2 and i.ops[1].type == X86_OP_MEM and tbl_disp is not None and base_val is None:
            m = i.ops[1].mem
            if ci.reg_name(i.ops[0].reg) == tbl_disp[0] and m.base == X86_REG_RIP:
                base_val = i.addr + i.size + m.disp
        if mn == 'cmp' and len(i.ops) == 2 and i.ops[1].type == X86_OP_IMM and bound is None:
            bound = i.ops[1].imm
    if tbl_disp is not None and base_val is None:
        # base register defined outside the straight-line window: use a unique rip-relative lea of that register in the function
        vals = set()
        for i2 in fl.insns.values():
            if i2.mn == 'lea' and len(i2.ops) == 2 and i2.ops[0].type == X86_OP_REG and i2.ops[1].type == X86_OP_MEM:
                m2 = i2.ops[1].mem
                if i2.ci.reg_name(i2.ops[0].reg) == tbl_disp[0] and m2.base == X86_REG_RIP:
                    vals.add(i2.addr + i2.size + m2.disp)
        if len(vals) == 1:
            base_val = vals.pop()
    if base_val is None or tbl_disp is None:
        return None
    table = base_val + tbl_disp[1]
    n = (bound + 1) if (bound is not None and 0 <= bound < 4096) else 4096
    out = []
    for k in range(n):
        try:
            e = img.i32(table + 4 * k)
        except Exception:
            break
        t = base_val + e
        if fl.ranges is None:
            ok = fl.root <= t < fl.root + 0x20000
        else:
            ok = fl.in_range(t)
        if not ok:
            break
        out.append(t)
    if not out:
        return None
    return sorted(set(out))


# ---------------------------------------------------------------------------------------------------------------
# flags use/def
def flag_info(ins):
    """(use, kill_def, may_def)"""
    mn = ins.mn
    if mn.startswith('bnd '):
        mn = mn[4:]
    if mn.startswith('lock '):
        mn = mn[5:]
    if mn.startswith('rep'):
        return 0, 0, 0
    if is_jcc(mn):
        return CC_USE[cc_of(mn, 'j')], 0, 0
    if mn.startswith('cmov'):
        return CC_USE[cc_of(mn, 'cmov')], 0, 0
    if mn.startswith('set'):
        return CC_USE[cc_of(mn, 'set')], 0, 0
    if mn in ('add', 'sub', 'cmp', 'and', 'or', 'xor', 'test', 'neg', 'xadd', 'cmpxchg', 'imul', 'mul', 'cmpxchg16b'):
        return 0, ALLF, ALLF
    if mn in ('adc', 'sbb'):
        return C_, ALLF, ALLF
    if mn in ('inc', 'dec'):
        return 0, ALLF & ~C_, ALLF & ~C_
    if mn in ('shl', 'shr', 'sar', 'sal'):
        cnt = ins.ops[1] if len(ins.ops) > 1 else None
        if cnt is not None and cnt.type == X86_OP_IMM and (cnt.imm & 63) != 0:
            return 0, ALLF, ALLF
        return 0, 0, ALLF
    if mn in ('rol', 'ror'):
        return 0, 0, C_ | O_
    if mn in ('rcl', 'rcr'):
        return C_, 0, C_ | O_
    if mn in ('bt', 'bts', 'btr', 'btc'):
        return 0, C_, C_
    if mn in ('bsr', 'bsf'):
        return 0, Z_, Z_
    if mn in ('comisd', 'ucomisd', 'comiss', 'ucomiss', 'vcomisd', 'vucomisd', 'vcomiss', 'vucomiss'):
        return 0, ALLF, ALLF
    if mn in ('stc', 'clc'):
        return 0, C_, C_
    if mn == 'cmc':
        return C_, 0, C_
    if mn in ('sahf', 'popfq'):
        return 0, ALLF, ALLF
    if mn in ('lahf', 'pushfq'):
        return ALLF, 0, 0
    if mn in ('sbb', 'adc'):
        return C_, ALLF, ALLF
    if mn in ('pcmpistri',):
        return 0, ALLF, ALLF
    return 0, 0, 0


def compute_liveness(fl, order):
    succ = {}
    for a in order:
        ins = fl.insns[a]
        mn = ins.mn[4:] if ins.mn.startswith('bnd ') else ins.mn
        nxt = a + ins.size
        s = []
        if mn in ('ret', 'retf', 'int3', 'ud2', 'hlt'):
            pass
        elif mn == 'jmp':
            if a in fl.tables:
                s = []
            elif a in fl.tails:
                s = []
            elif ins.ops[0].type == X86_OP_IMM and ins.ops[0].imm in fl.targets:
                s = [ins.ops[0].imm]
        elif is_jcc(mn) or mn in ('jrcxz', 'jecxz', 'loop'):
            s = [nxt] if a in fl.tails else [ins.ops[0].imm, nxt]
        else:
            s = [nxt]
        succ[a] = [x for x in s if x in fl.insns]
    use = {}
    kill = {}
    for a in order:
        u, k, m = flag_info(fl.insns[a])
        use[a] = u
        kill[a] = k
    live_in = {a: 0 for a in order}
    live_out = {a: 0 for a in order}
    changed = True
    rev = list(reversed(order))
    while changed:
        changed = False
        for a in rev:
            lo = 0
            for s in succ[a]:
                lo |= live_in[s]
            li = use[a] | (lo & ~kill[a])
            if lo != live_out[a] or li != live_in[a]:
                live_out[a] = lo
                live_in[a] = li
                changed = True
    return live_out


# ---------------------------------------------------------------------------------------------------------------
class Emitter:
    def __init__(self, img, fl, lifted_starts, fnname):
        self.img = img
        self.fl = fl
        self.starts = lifted_starts
        self.used_gpr = set([4])
        self.used_xmm = set()
        self.lines = []
        self.tmp = 0
        self.flags_used = 0
        self.fnname = fnname

    # ---- operand helpers ----
    def reg_idx(self, ci, reg):
        n = ci.reg_name(reg)
        if n in REGMAP:
            return REGMAP[n]
        return None

    def gpr_read(self, ci, op):
        n = ci.reg_name(op.reg)
        if n in REGMAP:
            i, sz, sh = REGMAP[n]
            self.used_gpr.add(i)
            if sz == 8:
                return 'r%d' % i
            if sh:
                return '(uint8_t)(r%d>>8)' % i
            return '(%s)r%d' % (TY[sz], i)
        raise Unimpl('reg ' + n)

    def xmm_of(self, ci, op):
        n = ci.reg_name(op.reg)
        if n in XMMMAP:
            self.used_xmm.add(XMMMAP[n])
            return 'x%d' % XMMMAP[n]
        raise Unimpl('xmmreg ' + n)

    def is_xmm(self, ci, op):
        return op.type == X86_OP_REG and ci.reg_name(op.reg) in XMMMAP

    def addr_expr(self, ins, op, want_seg=True):
        ci = ins.ci
        m = op.mem
        parts = []
        if m.base == X86_REG_RIP:
            return 'A(%s)' % hx((ins.addr + ins.size + m.disp) & 0xffffffffffffffff)
        if m.base != 0:
            parts.append(self.gpr_read(ci, type('o', (), {'reg': m.base})()) if ci.reg_name(m.base) in REGMAP else None)
            if parts[-1] is None:
                raise Unimpl('base reg')
        if m.index != 0:
            ix = ci.reg_name(m.index)
            if ix not in REGMAP:
                raise Unimpl('index reg')
            e = self.gpr_read(ci, type('o', (), {'reg': m.index})())
            if m.scale != 1:
                e = '(%s*%d)' % (e, m.scale)
            parts.append(e)
        if m.disp != 0 or not parts:
            parts.append('(uint64_t)(int64_t)(%d)' % m.disp)
        e = '+'.join(parts)
        if want_seg and m.segment != 0:
            sn = ci.reg_name(m.segment)
            if sn == 'gs':
                e = 'rt_gs()+' + e
            else:
                raise Unimpl('segment ' + sn)
        return '(uint64_t)(%s)' % e

    def rd(self, ins, op, size=None):
        ci = ins.ci
        if op.type == X86_OP_REG:
            e = self.gpr_read(ci, op)
            if size is not None and size != REGMAP[ci.reg_name(op.reg)][1]:
                raise Unimpl('size mismatch')
            return e
        if op.type == X86_OP_IMM:
            sz = size or op.size
            return '(%s)%s' % (TY[sz], hx(op.imm & MASK[sz]))
        if op.type == X86_OP_MEM:
            sz = size or op.size
            return 'L%d(%s)' % (sz * 8, self.addr_expr(ins, op))
        raise Unimpl('op type')

    def wr(self, ins, op, val):
        ci = ins.ci
        if op.type == X86_OP_REG:
            n = ci.reg_name(op.reg)
            i, sz, sh = REGMAP[n]
            self.used_gpr.add(i)
            if sz == 8:
                return 'r%d = %s;' % (i, val)
            if sz == 4:
                return 'r%d = (uint32_t)(%s);' % (i, val)
            if sz == 2:
                return 'r%d = (r%d & ~0xffffull) | (uint16_t)(%s);' % (i, i, val)
            if sh:
                return 'r%d = (r%d & ~0xff00ull) | ((uint64_t)(uint8_t)(%s) << 8);' % (i, i, val)
            return 'r%d = (r%d & ~0xffull) | (uint8_t)(%s);' % (i, i, val)
        if op.type == X86_OP_MEM:
            return 'S%d(%s, %s);' % (op.size * 8, self.addr_expr(ins, op), val)
        raise Unimpl('wr type')

    def opsize(self, ins, op):
        return op.size

    def pf(self, tvar):
        return 'PARITY(%s)' % tvar

    def flagset(self, lv, S, tv, cf=None, of=None, a=None, b=None):
        """lines for flags in lv computed from result tv (type T, size S bytes); cf/of given as expressions"""
        out = []
        bits = S * 8
        if lv & Z_:
            out.append('fZ = (%s==0);' % tv)
        if lv & S_:
            out.append('fS = (%s>>%d)&1;' % (tv, bits - 1))
        if lv & P_:
            out.append('fP = PARITY(%s);' % tv)
        if lv & C_ and cf is not None:
            out.append('fC = (%s);' % cf)
        if lv & O_ and of is not None:
            out.append('fO = (%s);' % of)
        return out

    def newtmp(self):
        self.tmp += 1
        return 't%d' % self.tmp

    # ---- main emit for one instruction ----
    def emit(self, ins, lv):
        mn = ins.mn
        ci = ins.ci
        ops = ins.ops
        lock = False
        if mn.startswith('lock '):
            lock = True
            mn = mn[5:]
        if mn.startswith('bnd '):
            mn = mn[4:]
        rep = None
        if mn.startswith('rep ') or mn.startswith('repne ') or mn.startswith('repe '):
            rep, mn = mn.split(' ', 1)
        h = getattr(self, 'i_' + mn.replace(' ', '_'), None)
        if h is not None:
            return h(ins, ops, lv, lock) if not rep else h(ins, ops, lv, lock, rep)
        if is_jcc(mn):
            return None
        raise Unimpl(mn)

    # ---------------- simple data movement ----------------
    def i_nop(self, ins, ops, lv, lock): return []
    def i_fnop(self, ins, ops, lv, lock): return []
    def i_pause(self, ins, ops, lv, lock): return []
    def i_lfence(self, ins, ops, lv, lock): return ['__sync_synchronize();']
    def i_mfence(self, ins, ops, lv, lock): return ['__sync_synchronize();']
    def i_sfence(self, ins, ops, lv, lock): return ['__sync_synchronize();']
    def i_vzeroupper(self, ins, ops, lv, lock): return []
    def i_prefetchw(self, ins, ops, lv, lock): return []
    def i_prefetcht0(self, ins, ops, lv, lock): return []
    def i_prefetcht1(self, ins, ops, lv, lock): return []
    def i_prefetchnta(self, ins, ops, lv, lock): return []
    def i_cld(self, ins, ops, lv, lock): return []
    def i_endbr64(self, ins, ops, lv, lock): return []

    def i_mov(self, ins, ops, lv, lock):
        d, s = ops
        sz = d.size
        return [self.wr(ins, d, self.rd(ins, s, sz))]
    i_movabs = i_mov

    def i_movzx(self, ins, ops, lv, lock):
        d, s = ops
        return [self.wr(ins, d, '(%s)%s' % (TY[d.size], self.rd(ins, s, s.size)))]

    def i_movsx(self, ins, ops, lv, lock):
        d, s = ops
        return [self.wr(ins, d, '(%s)(%s)%s' % (TY[d.size], STY[s.size], self.rd(ins, s, s.size)))]
    i_movsxd = i_movsx

    def i_lea(self, ins, ops, lv, lock):
        d, s = ops
        e = self.addr_expr(ins, s, want_seg=False)
        return [self.wr(ins, d, '(%s)%s' % (TY[d.size], e))]

    def i_xchg(self, ins, ops, lv, lock):
        a, b = ops
        sz = a.size
        t = self.newtmp()
        if a.type == X86_OP_MEM or b.type == X86_OP_MEM:
            m, r = (a, b) if a.type == X86_OP_MEM else (b, a)
            return ['{ %s %s = __atomic_exchange_n((%s*)%s, %s, __ATOMIC_SEQ_CST); %s }' % (TY[sz], t, TY[sz], self.addr_expr(ins, m), self.rd(ins, r, sz), self.wr(ins, r, t))]
        return ['{ %s %s = %s; %s %s }' % (TY[sz], t, self.rd(ins, a, sz), self.wr(ins, a, self.rd(ins, b, sz)), self.wr(ins, b, t))]

    def i_push(self, ins, ops, lv, lock):
        v = self.rd(ins, ops[0], 8) if ops[0].type != X86_OP_IMM else '(uint64_t)(int64_t)(%d)' % ops[0].imm
        return ['{ uint64_t v = %s; r4 -= 8; S64(r4, v); }' % v]

    def i_pop(self, ins, ops, lv, lock):
        return ['{ uint64_t v = L64(r4); r4 += 8; %s }' % self.wr(ins, ops[0], 'v')]

    def i_leave(self, ins, ops, lv, lock):
        self.used_gpr.update([4, 5])
        return ['r4 = r5; r5 = L64(r4); r4 += 8;']

    def i_cdq(self, ins, ops, lv, lock):
        self.used_gpr.update([0, 2])
        return ['r2 = (uint32_t)((int32_t)r0 >> 31);']

    def i_cqo(self, ins, ops, lv, lock):
        self.used_gpr.update([0, 2])
        return ['r2 = (uint64_t)((int64_t)r0 >> 63);']

    def i_cdqe(self, ins, ops, lv, lock):
        self.used_gpr.add(0)
        return ['r0 = (uint64_t)(int64_t)(int32_t)r0;']

    def i_cwde(self, ins, ops, lv, lock):
        self.used_gpr.add(0)
        return ['r0 = (uint32_t)(int32_t)(int16_t)r0;']

    def i_cwd(self, ins, ops, lv, lock):
        self.used_gpr.update([0, 2])
        return ['r2 = (r2 & ~0xffffull) | (uint16_t)((int16_t)r0 >> 15);']

    def i_bswap(self, ins, ops, lv, lock):
        d = ops[0]
        if d.size == 8:
            return [self.wr(ins, d, '__builtin_bswap64(%s)' % self.rd(ins, d, 8))]
        return [self.wr(ins, d, '__builtin_bswap32(%s)' % self.rd(ins, d, 4))]

    # ---------------- ALU ----------------
    def alu(self, ins, ops, lv, kind, write=True, lock=False):
        d, s = ops
        S = d.size
        T = TY[S]
        bits = S * 8
        lines = []
        a = self.rd(ins, d, S)
        b = self.rd(ins, s, S) if s.type != X86_OP_IMM else '(%s)%s' % (T, hx(s.imm & MASK[S]))
        fl = []
        if kind in ('add', 'sub'):
            op = '+' if kind == 'add' else '-'
            if lock and d.type == X86_OP_MEM:
                lines.append('%s a = __atomic_fetch_%s((%s*)%s, %s, __ATOMIC_SEQ_CST); %s t = a %s %s;' % (T, kind, T, self.addr_expr(ins, d), b, T, op, 'b0'))
                lines[-1] = lines[-1].replace('b0', 'b')
                lines.insert(0, '%s b = %s;' % (T, b))
                lines[1] = lines[1].replace(', %s, __ATOMIC' % b, ', b, __ATOMIC')
                write = False
            else:
                lines.append('%s a = %s; %s b = %s; %s t = (%s)(a %s b);' % (T, a, T, b, T, T, op))
            if kind == 'add':
                fl = self.flagset(lv, S, 't', cf='t<a', of='((a^t)&(b^t))>>%d&1' % (bits - 1))
            else:
                fl = self.flagset(lv, S, 't', cf='a<b', of='((a^b)&(a^t))>>%d&1' % (bits - 1))
            if write:
                lines.append(self.wr(ins, d, 't'))
        elif kind in ('adc', 'sbb'):
            op = '+' if kind == 'adc' else '-'
            self.flags_used |= C_
            lines.append('%s a = %s; %s b = %s; %s cin = fC; %s t = (%s)(a %s b %s cin);' % (T, a, T, b, T, T, T, op, op))
            if kind == 'adc':
                fl = self.flagset(lv, S, 't', cf='cin ? (t<=a) : (t<a)', of='((a^t)&(b^t))>>%d&1' % (bits - 1))
            else:
                fl = self.flagset(lv, S, 't', cf='cin ? (a<=b) : (a<b)', of='((a^b)&(a^t))>>%d&1' % (bits - 1))
            lines.append(self.wr(ins, d, 't'))
        elif kind in ('and', 'or', 'xor', 'test'):
            op = {'and': '&', 'or': '|', 'xor': '^', 'test': '&'}[kind]
            if lock and d.type == X86_OP_MEM:
                lines.append('%s b = %s; %s a = __atomic_fetch_%s((%s*)%s, b, __ATOMIC_SEQ_CST); %s t = a %s b;' % (T, b, T, kind, T, self.addr_expr(ins, d), T, op))
                write = False
            else:
                lines.append('%s a = %s; %s b = %s; %s t = (%s)(a %s b);' % (T, a, T, b, T, T, op))
            fl = self.flagset(lv, S, 't', cf='0', of='0')
            if write and kind != 'test':
                lines.append(self.wr(ins, d, 't'))
        elif kind == 'cmp':
            lines.append('%s a = %s; %s b = %s; %s t = (%s)(a - b);' % (T, a, T, b, T, T))
            fl = self.flagset(lv, S, 't', cf='a<b', of='((a^b)&(a^t))>>%d&1' % (bits - 1))
        return ['{ ' + ' '.join(lines + fl) + ' }']

    def i_add(self, ins, ops, lv, lock): return self.alu(ins, ops, lv, 'add', lock=lock)
    def i_sub(self, ins, ops, lv, lock): return self.alu(ins, ops, lv, 'sub', lock=lock)
    def i_adc(self, ins, ops, lv, lock): return self.alu(ins, ops, lv, 'adc')
    def i_sbb(self, ins, ops, lv, lock): return self.alu(ins, ops, lv, 'sbb')
    def i_and(self, ins, ops, lv, lock): return self.alu(ins, ops, lv, 'and', lock=lock)
    def i_or(self, ins, ops, lv, lock): return self.alu(ins, ops, lv, 'or', lock=lock)
    def i_xor(self, ins, ops, lv, lock): return self.alu(ins, ops, lv, 'xor', lock=lock)
    def i_test(self, ins, ops, lv, lock): return self.alu(ins, ops, lv, 'test', write=False)
    def i_cmp(self, ins, ops, lv, lock): return self.alu(ins, ops, lv, 'cmp', write=False)

    def i_inc(self, ins, ops, lv, lock): return self.incdec(ins, ops, lv, lock, '+')
    def i_dec(self, ins, ops, lv, lock): return self.incdec(ins, ops, lv, lock, '-')

    def incdec(self, ins, ops, lv, lock, op):
        d = ops[0]
        S = d.size
        T = TY[S]
        bits = S * 8
        lines = []
        if lock and d.type == X86_OP_MEM:
            lines.append('%s a = __atomic_fetch_%s((%s*)%s, 1, __ATOMIC_SEQ_CST); %s t = a %s 1;' % (T, 'add' if op == '+' else 'sub', T, self.addr_expr(ins, d), T, op))
            w = []
        else:
            lines.append('%s a = %s; %s t = (%s)(a %s 1);' % (T, self.rd(ins, d, S), T, T, op))
            w = [self.wr(ins, d, 't')]
        ofe = ('t==(%s)%s' % (T, hx(1 << (bits - 1)))) if op == '+' else ('a==(%s)%s' % (T, hx(1 << (bits - 1))))
        fl = self.flagset(lv & ~C_, S, 't', of=ofe)
        return ['{ ' + ' '.join(lines + w + fl) + ' }']

    def i_neg(self, ins, ops, lv, lock):
        d = ops[0]
        S = d.size
        T = TY[S]
        bits = S * 8
        fl = self.flagset(lv, S, 't', cf='a!=0', of='a==(%s)%s' % (T, hx(1 << (bits - 1))))
        return ['{ %s a = %s; %s t = (%s)(0 - a); %s %s }' % (T, self.rd(ins, d, S), T, T, self.wr(ins, d, 't'), ' '.join(fl))]

    def i_not(self, ins, ops, lv, lock):
        d = ops[0]
        S = d.size
        return [self.wr(ins, d, '(%s)~%s' % (TY[S], self.rd(ins, d, S)))]

    def shift(self, ins, ops, lv, kind):
        d = ops[0]
        S = d.size
        T = TY[S]
        bits = S * 8
        cnt = ops[1]
        immcnt = cnt.type == X86_OP_IMM
        cm = 63 if S == 8 else 31
        if immcnt:
            c = cnt.imm & cm
            if c == 0:
                return []
            cexpr = str(c)
        else:
            cexpr = '(r1&%d)' % cm
            self.used_gpr.add(1)
        lines = ['%s a = %s; unsigned cnt = %s;' % (T, self.rd(ins, d, S), cexpr)]
        if kind in ('shl', 'sal'):
            lines.append('%s t = (%s)(a << cnt);' % (T, T))
            cf = '(a>>(%d-cnt))&1' % bits
            of = '((t>>%d)&1)^((a>>(%d-cnt))&1)' % (bits - 1, bits)
        elif kind == 'shr':
            lines.append('%s t = (%s)(a >> cnt);' % (T, T))
            cf = '(a>>(cnt-1))&1'
            of = '(a>>%d)&1' % (bits - 1)
        else:
            lines.append('%s t = (%s)((%s)a >> cnt);' % (T, T, STY[S]))
            cf = '((%s)a>>(cnt-1))&1' % STY[S]
            of = '0'
        fl = self.flagset(lv, S, 't', cf=cf, of=of)
        w = self.wr(ins, d, 't')
        if immcnt:
            return ['{ ' + ' '.join(lines + [w] + fl) + ' }']
        # variable count: count==0 leaves flags alone but a 32-bit destination register is still zero-extended
        return ['{ ' + lines[0] + ' ' + self._varshift(lines[1:], w, fl, T) + ' }']

    def _varshift(self, rest, w, fl, T):
        calc = rest[0]
        return calc.replace('%s t = (' % T, '%s t = cnt ? (' % T, 1).rstrip(';') + ' : a; ' + w + ' if (cnt) { ' + ' '.join(fl) + ' }'

    def i_shl(self, ins, ops, lv, lock): return self.shift(ins, ops, lv, 'shl')
    def i_sal(self, ins, ops, lv, lock): return self.shift(ins, ops, lv, 'shl')
    def i_shr(self, ins, ops, lv, lock): return self.shift(ins, ops, lv, 'shr')
    def i_sar(self, ins, ops, lv, lock): return self.shift(ins, ops, lv, 'sar')

    def rot(self, ins, ops, lv, kind):
        d = ops[0]
        S = d.size
        T = TY[S]
        bits = S * 8
        cnt = ops[1]
        if cnt.type == X86_OP_IMM:
            c = cnt.imm & (63 if S == 8 else 31)
            cexpr = str(c)
        else:
            cexpr = '(r1&%d)' % (63 if S == 8 else 31)
            self.used_gpr.add(1)
        l = ['%s a = %s; unsigned cnt = %s %% %d;' % (T, self.rd(ins, d, S), cexpr, bits)]
        if kind == 'rol':
            l.append('%s t = cnt ? (%s)((a << cnt) | (a >> (%d-cnt))) : a;' % (T, T, bits))
            cf = 't&1'
            of = '((t>>%d)&1)^(t&1)' % (bits - 1)
        else:
            l.append('%s t = cnt ? (%s)((a >> cnt) | (a << (%d-cnt))) : a;' % (T, T, bits))
            cf = '(t>>%d)&1' % (bits - 1)
            of = '((t>>%d)^(t>>%d))&1' % (bits - 1, bits - 2)
        fl = []
        if lv & C_: fl.append('if (cnt) fC = (%s);' % cf)
        if lv & O_: fl.append('if (cnt) fO = (%s);' % of)
        return ['{ ' + ' '.join(l + [self.wr(ins, d, 't')] + fl) + ' }']

    def i_rol(self, ins, ops, lv, lock): return self.rot(ins, ops, lv, 'rol')
    def i_ror(self, ins, ops, lv, lock): return self.rot(ins, ops, lv, 'ror')

    def i_imul(self, ins, ops, lv, lock):
        if len(ops) == 1:
            s = ops[0]
            S = s.size
            if S == 8:
                self.used_gpr.update([0, 2])
                return ['{ __int128 p = (__int128)(int64_t)r0 * (int64_t)%s; r0 = (uint64_t)p; r2 = (uint64_t)(p>>64); %s }' % (self.rd(ins, s, 8), ' '.join(self.flagset(lv, 8, 'r0', cf='(p != (__int128)(int64_t)r0)', of='(p != (__int128)(int64_t)r0)')))]
            if S == 4:
                self.used_gpr.update([0, 2])
                return ['{ int64_t p = (int64_t)(int32_t)r0 * (int32_t)%s; r0 = (uint32_t)p; r2 = (uint32_t)(p>>32); %s }' % (self.rd(ins, s, 4), ' '.join(self.flagset(lv, 4, '(uint32_t)p', cf='(p != (int64_t)(int32_t)p)', of='(p != (int64_t)(int32_t)p)')))]
            raise Unimpl('imul1 %d' % S)
        if len(ops) == 2:
            d, s = ops
            a, b = d, s
        else:
            d, a, b = ops
        S = d.size
        T = TY[S]
        sa = '(%s)%s' % (STY[S], self.rd(ins, a, S))
        sb = '(%s)%s' % (STY[S], self.rd(ins, b, S)) if b.type != X86_OP_IMM else '(%s)%s' % (STY[S], hx(b.imm & MASK[S]))
        if S == 8:
            return ['{ __int128 p = (__int128)%s * %s; %s t = (%s)p; %s %s }' % (sa, sb, T, T, self.wr(ins, d, 't'), ' '.join(self.flagset(lv, S, 't', cf='(p != (__int128)(int64_t)t)', of='(p != (__int128)(int64_t)t)')))]
        wide = {4: 'int64_t', 2: 'int32_t'}[S]
        return ['{ %s p = (%s)%s * (%s)%s; %s t = (%s)p; %s %s }' % (wide, wide, sa, wide, sb, T, T, self.wr(ins, d, 't'), ' '.join(self.flagset(lv, S, 't', cf='(p != (%s)(%s)t)' % (wide, STY[S]), of='(p != (%s)(%s)t)' % (wide, STY[S]))))]

    def i_mul(self, ins, ops, lv, lock):
        s = ops[0]
        S = s.size
        self.used_gpr.update([0, 2])
        fl = ' '.join(self.flagset(lv, S, 'r0' if S == 8 else '(uint32_t)r0', cf='(r2!=0)', of='(r2!=0)'))
        if S == 8:
            return ['{ unsigned __int128 p = (unsigned __int128)r0 * %s; r0 = (uint64_t)p; r2 = (uint64_t)(p>>64); %s }' % (self.rd(ins, s, 8), fl)]
        if S == 4:
            return ['{ uint64_t p = (uint64_t)(uint32_t)r0 * %s; r0 = (uint32_t)p; r2 = (uint32_t)(p>>32); %s }' % (self.rd(ins, s, 4), fl)]
        raise Unimpl('mul %d' % S)

    def i_div(self, ins, ops, lv, lock):
        s = ops[0]
        S = s.size
        self.used_gpr.update([0, 2])
        if S == 8:
            return ['{ uint64_t d = %s; div64(&r0, &r2, d); }' % self.rd(ins, s, 8)]
        if S == 4:
            return ['{ uint32_t d = %s; uint64_t n = ((uint64_t)(uint32_t)r2 << 32) | (uint32_t)r0; r0 = (uint32_t)(n / d); r2 = (uint32_t)(n %% d); }' % self.rd(ins, s, 4)]
        raise Unimpl('div %d' % S)

    def i_idiv(self, ins, ops, lv, lock):
        s = ops[0]
        S = s.size
        self.used_gpr.update([0, 2])
        if S == 8:
            return ['{ int64_t d = %s; idiv64(&r0, &r2, d); }' % self.rd(ins, s, 8)]
        if S == 4:
            return ['{ int32_t d = %s; int64_t n = (int64_t)(((uint64_t)(uint32_t)r2 << 32) | (uint32_t)r0); r0 = (uint32_t)(int32_t)(n / d); r2 = (uint32_t)(int32_t)(n %% d); }' % self.rd(ins, s, 4)]
        raise Unimpl('idiv %d' % S)

    def bitop(self, ins, ops, lv, kind):
        d, s = ops
        S = d.size
        T = TY[S]
        if d.type == X86_OP_MEM and s.type != X86_OP_IMM:
            raise Unimpl('bt mem,reg')
        idx = ('(%s)' % hx(s.imm)) if s.type == X86_OP_IMM else self.rd(ins, s, S)
        l = ['%s a = %s; unsigned bit = (unsigned)(%s) %% %d; fC = (a>>bit)&1;' % (T, self.rd(ins, d, S), idx, S * 8)]
        if kind == 'bts':
            l.append(self.wr(ins, d, '(%s)(a | ((%s)1<<bit))' % (T, T)))
        elif kind == 'btr':
            l.append(self.wr(ins, d, '(%s)(a & ~((%s)1<<bit))' % (T, T)))
        elif kind == 'btc':
            l.append(self.wr(ins, d, '(%s)(a ^ ((%s)1<<bit))' % (T, T)))
        return ['{ ' + ' '.join(l) + ' }']

    def i_bt(self, ins, ops, lv, lock): return self.bitop(ins, ops, lv, 'bt')
    def i_bts(self, ins, ops, lv, lock): return self.bitop(ins, ops, lv, 'bts')
    def i_btr(self, ins, ops, lv, lock): return self.bitop(ins, ops, lv, 'btr')
    def i_btc(self, ins, ops, lv, lock): return self.bitop(ins, ops, lv, 'btc')

    def i_bsr(self, ins, ops, lv, lock):
        d, s = ops
        S = s.size
        return ['{ %s a = %s; fZ = (a==0); if (a) { %s } }' % (TY[S], self.rd(ins, s, S), self.wr(ins, d, '(%s)bsr64(a)' % TY[d.size]))]

    def i_bsf(self, ins, ops, lv, lock):
        d, s = ops
        S = s.size
        return ['{ %s a = %s; fZ = (a==0); if (a) { %s } }' % (TY[S], self.rd(ins, s, S), self.wr(ins, d, '(%s)bsf64(a)' % TY[d.size]))]

    def i_xadd(self, ins, ops, lv, lock):
        d, s = ops
        S = d.size
        T = TY[S]
        if d.type == X86_OP_MEM:
            fl = self.flagset(lv, S, 't', cf='t<a', of='((a^t)&(b^t))>>%d&1' % (S * 8 - 1))
            return ['{ %s b = %s; %s a = __atomic_fetch_add((%s*)%s, b, __ATOMIC_SEQ_CST); %s t = (%s)(a + b); %s %s }' % (T, self.rd(ins, s, S), T, T, self.addr_expr(ins, d), T, T, self.wr(ins, s, 'a'), ' '.join(fl))]
        fl = self.flagset(lv, S, 't', cf='t<a', of='((a^t)&(b^t))>>%d&1' % (S * 8 - 1))
        return ['{ %s a = %s; %s b = %s; %s t = (%s)(a + b); %s %s %s }' % (T, self.rd(ins, d, S), T, self.rd(ins, s, S), T, T, self.wr(ins, s, 'a'), self.wr(ins, d, 't'), ' '.join(fl))]

    def i_cmpxchg(self, ins, ops, lv, lock):
        d, s = ops
        S = d.size
        T = TY[S]
        self.used_gpr.add(0)
        acc = {1: '(uint8_t)r0', 2: '(uint16_t)r0', 4: '(uint32_t)r0', 8: 'r0'}[S]
        accw = {1: 'r0 = (r0 & ~0xffull) | (uint8_t)old;', 2: 'r0 = (r0 & ~0xffffull) | (uint16_t)old;', 4: 'r0 = (uint32_t)old;', 8: 'r0 = old;'}[S]
        fl = self.flagset(lv, S, 't', cf='expected<old', of='((expected^old)&(expected^t))>>%d&1' % (S * 8 - 1))
        if d.type == X86_OP_MEM:
            return ['{ %s expected = %s; %s desired = %s; %s old = expected; bool ok = __atomic_compare_exchange_n((%s*)%s, &old, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); %s t = (%s)(expected - old); fZ = ok; if (!ok) { %s } %s }' % (T, acc, T, self.rd(ins, s, S), T, T, self.addr_expr(ins, d), T, T, accw, ' '.join(f for f in fl if not f.startswith('fZ')))]
        raise Unimpl('cmpxchg reg')

    def i_cmpxchg16b(self, ins, ops, lv, lock):
        self.used_gpr.update([0, 1, 2, 3])
        return ['fZ = rt_cmpxchg16(%s, &r0, &r2, r3, r1);' % self.addr_expr(ins, ops[0])]

    def cc_expr(self, mn, prefix):
        cc = cc_of(mn, prefix)
        self.flags_used |= CC_USE[cc]
        return CC_EXPR[cc]

    def i_cmov_common(self, ins, ops, lv, mn):
        d, s = ops
        c = self.cc_expr(mn, 'cmov')
        S = d.size
        return ['{ %s v = %s ? %s : %s; %s }' % (TY[S], c, self.rd(ins, s, S), self.rd(ins, d, S), self.wr(ins, d, 'v'))]

    def i_set_common(self, ins, ops, lv, mn):
        return [self.wr(ins, ops[0], '(%s) ? 1 : 0' % self.cc_expr(mn, 'set'))]

    def __getattr__(self, name):
        if name.startswith('i_cmov'):
            mn = name[2:]
            return lambda ins, ops, lv, lock, _mn=mn: self.i_cmov_common(ins, ops, lv, _mn)
        if name.startswith('i_set'):
            mn = name[2:]
            return lambda ins, ops, lv, lock, _mn=mn: self.i_set_common(ins, ops, lv, _mn)
        raise AttributeError(name)

    # ---------------- string ops ----------------
    def i_stosb(self, ins, ops, lv, lock, rep=None): return self.stos(1, rep)
    def i_stosw(self, ins, ops, lv, lock, rep=None): return self.stos(2, rep)
    def i_stosd(self, ins, ops, lv, lock, rep=None): return self.stos(4, rep)
    def i_stosq(self, ins, ops, lv, lock, rep=None): return self.stos(8, rep)

    def stos(self, S, rep):
        self.used_gpr.update([0, 1, 7])
        acc = {1: '(uint8_t)r0', 2: '(uint16_t)r0', 4: '(uint32_t)r0', 8: 'r0'}[S]
        if rep:
            return ['while (r1) { S%d(r7, %s); r7 += %d; r1--; }' % (S * 8, acc, S)]
        return ['S%d(r7, %s); r7 += %d;' % (S * 8, acc, S)]

    def i_movsb(self, ins, ops, lv, lock, rep=None): return self.movs(1, rep)
    def i_movsw(self, ins, ops, lv, lock, rep=None): return self.movs(2, rep)
    def i_movsd(self, ins, ops, lv, lock, rep=None):
        if ops and not rep and len(ops) == 2 and (ops[0].type == X86_OP_REG and self.is_xmm(ins.ci, ops[0]) or ops[1].type == X86_OP_REG and self.is_xmm(ins.ci, ops[1])):
            return self.x_movs(ins, ops, 8)
        return self.movs(4, rep)
    def i_movsq(self, ins, ops, lv, lock, rep=None): return self.movs(8, rep)

    def movs(self, S, rep):
        self.used_gpr.update([1, 6, 7])
        if rep:
            return ['while (r1) { S%d(r7, L%d(r6)); r7 += %d; r6 += %d; r1--; }' % (S * 8, S * 8, S, S)]
        return ['S%d(r7, L%d(r6)); r7 += %d; r6 += %d;' % (S * 8, S * 8, S, S)]

    def i_cpuid(self, ins, ops, lv, lock):
        self.used_gpr.update([0, 1, 2, 3])
        return ['{ uint32_t a = (uint32_t)r0, b = 0, c = (uint32_t)r1, d = 0; rt_cpuid(&a, &b, &c, &d); r0 = a; r3 = b; r1 = c; r2 = d; }']

    def i_xgetbv(self, ins, ops, lv, lock):
        self.used_gpr.update([0, 1, 2])
        return ['r0 = 3; r2 = 0;']

    def i_rdtsc(self, ins, ops, lv, lock):
        self.used_gpr.update([0, 2])
        return ['{ uint64_t t = rt_rdtsc(); r0 = (uint32_t)t; r2 = (uint32_t)(t>>32); }']

    def i_int(self, ins, ops, lv, lock):
        if ops[0].imm == 0x29:
            self.used_gpr.update([1, 4])
            return ['SPILL; rt_fastfail(c, %s);' % hx(ins.addr)]
        raise Unimpl('int')

    def i_stmxcsr(self, ins, ops, lv, lock):
        return ['S32(%s, 0x1f80);' % self.addr_expr(ins, ops[0])]
    i_vstmxcsr = i_stmxcsr

    def i_ldmxcsr(self, ins, ops, lv, lock):
        return []
    i_vldmxcsr = i_ldmxcsr

    def _cmp_alias(self, ins, ops, pred, sfx):
        fn = 'sse_cmp' + sfx
        if len(ops) == 3:
            d, a, b = ops
            return [self.xd(ins, d, '%s(%s, %s, %d)' % (fn, self.xs(ins, a), self.xs(ins, b), pred))]
        d, b = ops
        return [self.xd(ins, d, '%s(%s, %s, %d)' % (fn, self.xs(ins, d), self.xs(ins, b), pred))]

    def i_cmplesd(self, ins, ops, lv, lock): return self._cmp_alias(ins, ops, 2, 'sd')
    def i_cmpnltsd(self, ins, ops, lv, lock): return self._cmp_alias(ins, ops, 5, 'sd')
    i_vcmplesd = i_cmplesd
    i_vcmpnltsd = i_cmpnltsd

    def i_vmovlhps(self, ins, ops, lv, lock):
        d, a, b = ops
        return [self.xd(ins, d, 'sse_movlhps(%s, %s)' % (self.xs(ins, a), self.xs(ins, b)))]

    def i_rcr(self, ins, ops, lv, lock):
        raise Unimpl('rcr')

    def i_fnstcw(self, ins, ops, lv, lock):
        return ['S16(%s, 0x27f);' % self.addr_expr(ins, ops[0])]
    def i_fldcw(self, ins, ops, lv, lock): return []
    def i_fnclex(self, ins, ops, lv, lock): return []
    def i_fninit(self, ins, ops, lv, lock): return []
    def i_fnstenv(self, ins, ops, lv, lock):
        return ['memset((void*)%s, 0, 28);' % self.addr_expr(ins, ops[0])]
    def i_fnsave(self, ins, ops, lv, lock):
        return ['memset((void*)%s, 0, 108);' % self.addr_expr(ins, ops[0])]
    def i_fnstsw(self, ins, ops, lv, lock):
        if ops[0].type == X86_OP_REG:
            self.used_gpr.add(0)
            return ['r0 = r0 & ~0xffffull;']
        return ['S16(%s, 0);' % self.addr_expr(ins, ops[0])]

    def i_ud2(self, ins, ops, lv, lock): return ['rt_unimpl(%s, "ud2");' % hx(ins.addr)]
    def i_int3(self, ins, ops, lv, lock): return ['rt_unimpl(%s, "int3");' % hx(ins.addr)]

    def i_sahf(self, ins, ops, lv, lock):
        self.used_gpr.add(0)
        return ['{ uint8_t ah = (uint8_t)(r0>>8); fC = ah&1; fP = (ah>>2)&1; fZ = (ah>>6)&1; fS = (ah>>7)&1; }']

    def i_lahf(self, ins, ops, lv, lock):
        self.used_gpr.add(0)
        self.flags_used |= ALLF
        return ['r0 = (r0 & ~0xff00ull) | ((uint64_t)(uint8_t)(fC | 2 | (fP<<2) | (fZ<<6) | (fS<<7)) << 8);']

    def i_stc(self, ins, ops, lv, lock): return ['fC = 1;']
    def i_clc(self, ins, ops, lv, lock): return ['fC = 0;']
    def i_cmc(self, ins, ops, lv, lock):
        self.flags_used |= C_
        return ['fC = !fC;']

    # ---------------- SSE ----------------
    def xs(self, ins, op, size=16):
        """source operand as V128 expr"""
        ci = ins.ci
        if op.type == X86_OP_REG and self.is_xmm(ci, op):
            return self.xmm_of(ci, op)
        if op.type == X86_OP_MEM:
            return 'ld_x(%d, %s)' % (min(op.size or size, 16), self.addr_expr(ins, op))
        raise Unimpl('xmm src')

    def xd(self, ins, op, val):
        ci = ins.ci
        if op.type == X86_OP_REG and self.is_xmm(ci, op):
            return '%s = %s;' % (self.xmm_of(ci, op), val)
        if op.type == X86_OP_MEM:
            return 'st_x(%d, %s, %s);' % (min(op.size, 16), self.addr_expr(ins, op), val)
        raise Unimpl('xmm dst')

    def x_mov128(self, ins, ops, lv, lock):
        d, s = ops
        if d.type == X86_OP_MEM:
            return ['st_x(16, %s, %s);' % (self.addr_expr(ins, d), self.xs(ins, s))]
        return [self.xd(ins, d, 'ld_x(16, %s)' % self.addr_expr(ins, s) if s.type == X86_OP_MEM else self.xs(ins, s))]

    i_movaps = i_movups = i_movdqa = i_movdqu = i_movapd = i_movupd = x_mov128
    i_vmovaps = i_vmovups = i_vmovdqa = i_vmovdqu = i_vmovapd = i_vmovupd = x_mov128
    i_movntps = i_movntdq = i_movntpd = x_mov128

    def x_movs(self, ins, ops, sz):
        if len(ops) == 3:   # VEX: dst, src1, src2 (reg,reg,reg)
            d, a, b = ops
            return [self.xd(ins, d, '({ V128 r = %s; memcpy(&r, &%s, %d); r; })' % (self.xs(ins, a), self.xs(ins, b), sz))]
        d, s = ops
        if d.type == X86_OP_MEM:
            return ['st_x(%d, %s, %s);' % (sz, self.addr_expr(ins, d), self.xs(ins, s))]
        if s.type == X86_OP_MEM:
            return [self.xd(ins, d, 'ld_x(%d, %s)' % (sz, self.addr_expr(ins, s)))]
        return [self.xd(ins, d, '({ V128 r = %s; memcpy(&r, &%s, %d); r; })' % (self.xs(ins, d), self.xs(ins, s), sz))]

    def i_movss(self, ins, ops, lv, lock): return self.x_movs(ins, ops, 4)
    def i_vmovss(self, ins, ops, lv, lock): return self.x_movs(ins, ops, 4)
    def i_vmovsd(self, ins, ops, lv, lock): return self.x_movs(ins, ops, 8)

    def x_movdq(self, ins, ops, lv, lock):
        d, s = ops
        ci = ins.ci
        if self.is_xmm(ci, d):
            if s.type == X86_OP_REG and self.is_xmm(ci, s):
                return [self.xd(ins, d, 'x_from_u64(%s.q[0])' % self.xs(ins, s))]   # movq xmm,xmm
            sz = s.size if s.type == X86_OP_MEM else REGMAP[ci.reg_name(s.reg)][1]
            if sz == 8:
                return [self.xd(ins, d, 'x_from_u64(%s)' % self.rd(ins, s, 8))]
            return [self.xd(ins, d, 'x_from_u32(%s)' % self.rd(ins, s, 4))]
        sz = d.size if d.type == X86_OP_MEM else REGMAP[ci.reg_name(d.reg)][1]
        if self.is_xmm(ci, s):
            if sz == 8:
                return [self.wr(ins, d, '%s.q[0]' % self.xs(ins, s))]
            return [self.wr(ins, d, '%s.d[0]' % self.xs(ins, s))]
        raise Unimpl('movd form')

    i_movd = i_movq = i_vmovd = i_vmovq = x_movdq

    def x_arith(self, name):
        def f(ins, ops, lv, lock):
            if len(ops) == 3:
                d, a, b = ops
                return [self.xd(ins, d, '%s(%s, %s)' % (name, self.xs(ins, a), self.xs(ins, b)))]
            d, s = ops
            return [self.xd(ins, d, '%s(%s, %s)' % (name, self.xs(ins, d), self.xs(ins, s)))]
        return f

    def x_unary(self, name):   # dst = f(src)  (2 operands)
        def f(ins, ops, lv, lock):
            d, s = ops[0], ops[-1]
            return [self.xd(ins, d, '%s(%s)' % (name, self.xs(ins, s)))]
        return f

    def x_imm(self, name, binary=False):    # dst = f(dst_or_src1, src, imm) / f(src, imm)
        def f(ins, ops, lv, lock):
            if len(ops) == 4:
                d, a, b, im = ops
                return [self.xd(ins, d, '%s(%s, %s, %d)' % (name, self.xs(ins, a), self.xs(ins, b), im.imm))]
            if len(ops) == 3:
                d, s, im = ops
                if binary:
                    return [self.xd(ins, d, '%s(%s, %s, %d)' % (name, self.xs(ins, d), self.xs(ins, s), im.imm))]
                return [self.xd(ins, d, '%s(%s, %d)' % (name, self.xs(ins, s), im.imm))]
            raise Unimpl('imm form')
        return f

    def x_shift_imm(self, name):
        def f(ins, ops, lv, lock):
            if len(ops) == 3:
                d, s, im = ops
                return [self.xd(ins, d, '%s(%s, %d)' % (name, self.xs(ins, s), im.imm))]
            d, im = ops
            cnt = str(im.imm) if im.type == X86_OP_IMM else '%s.q[0]' % self.xs(ins, im)
            return [self.xd(ins, d, '%s(%s, %s)' % (name, self.xs(ins, d), cnt))]
        return f

    def _init_sse(self):
        pass

    def x_cmp(self, ins, ops, lv, lock, dbl, name_ord):
        a, b = ops[0], ops[1]
        ci = ins.ci
        sa = '%s.%s[0]' % (self.xs(ins, a), 'df' if dbl else 'f')
        if b.type == X86_OP_MEM:
            sb = '%s.%s[0]' % (self.xs(ins, b), 'df' if dbl else 'f')
        else:
            sb = '%s.%s[0]' % (self.xs(ins, b), 'df' if dbl else 'f')
        T = 'double' if dbl else 'float'
        out = ['%s a = %s, b = %s; bool un = (a != a) || (b != b);' % (T, sa, sb)]
        if lv & Z_: out.append('fZ = un || (a == b);')
        if lv & P_: out.append('fP = un;')
        if lv & C_: out.append('fC = un || (a < b);')
        if lv & O_: out.append('fO = 0;')
        if lv & S_: out.append('fS = 0;')
        return ['{ ' + ' '.join(out) + ' }']

    def i_comisd(self, ins, ops, lv, lock): return self.x_cmp(ins, ops, lv, lock, True, 0)
    def i_ucomisd(self, ins, ops, lv, lock): return self.x_cmp(ins, ops, lv, lock, True, 0)
    def i_comiss(self, ins, ops, lv, lock): return self.x_cmp(ins, ops, lv, lock, False, 0)
    def i_ucomiss(self, ins, ops, lv, lock): return self.x_cmp(ins, ops, lv, lock, False, 0)
    i_vcomisd = i_vucomisd = i_comisd
    i_vcomiss = i_vucomiss = i_comiss

    def x_cvt_i2f(self, ins, ops, lv, lock, dbl):
        if len(ops) == 3:
            d, a, s = ops
            base = self.xs(ins, a)
        else:
            d, s = ops
            base = self.xs(ins, d)
        sz = s.size if s.type == X86_OP_MEM else REGMAP[ins.ci.reg_name(s.reg)][1]
        v = self.rd(ins, s, sz)
        conv = '(double)(%s)%s' % (STY[sz], v) if dbl else '(float)(%s)%s' % (STY[sz], v)
        return [self.xd(ins, d, '({ V128 r = %s; r.%s[0] = %s; r; })' % (base, 'df' if dbl else 'f', conv))]

    def i_cvtsi2sd(self, ins, ops, lv, lock): return self.x_cvt_i2f(ins, ops, lv, lock, True)
    def i_cvtsi2ss(self, ins, ops, lv, lock): return self.x_cvt_i2f(ins, ops, lv, lock, False)
    i_vcvtsi2sd = i_cvtsi2sd
    i_vcvtsi2ss = i_cvtsi2ss

    def x_cvt_f2i(self, fn):
        def f(ins, ops, lv, lock):
            d, s = ops
            sz = REGMAP[ins.ci.reg_name(d.reg)][1]
            return [self.wr(ins, d, '(%s)%s(%s)' % (TY[sz], fn + ('64' if sz == 8 else '32'), '%s.%s[0]' % (self.xs(ins, s), 'df' if 'd2' in fn else 'f')))]
        return f

    def x_cvt_f2f(self, src_dbl):
        def f(ins, ops, lv, lock):
            if len(ops) == 3:
                d, a, s = ops
                base = self.xs(ins, a)
            else:
                d, s = ops
                base = self.xs(ins, d)
            sv = self.xs(ins, s)
            if src_dbl:   # cvtsd2ss
                return [self.xd(ins, d, '({ V128 r = %s; r.f[0] = (float)%s.df[0]; r; })' % (base, sv))]
            return [self.xd(ins, d, '({ V128 r = %s; r.df[0] = (double)%s.f[0]; r; })' % (base, sv))]
        return f

    def x_fma(self, order, dbl, neg_prod, neg_add):
        def f(ins, ops, lv, lock):
            d, a, b = ops
            xa, xb, xc = self.xs(ins, d), self.xs(ins, a), self.xs(ins, b)
            fld = 'df' if dbl else 'f'
            # 132: d = d*c + a ; 213: d = a*d + c ; 231: d = a*c + d    (operands: d=op1, a=op2, b=op3)
            if order == '132':
                p, q, r = '%s.%s[0]' % (xa, fld), '%s.%s[0]' % (xc, fld), '%s.%s[0]' % (xb, fld)
            elif order == '213':
                p, q, r = '%s.%s[0]' % (xb, fld), '%s.%s[0]' % (xa, fld), '%s.%s[0]' % (xc, fld)
            else:
                p, q, r = '%s.%s[0]' % (xb, fld), '%s.%s[0]' % (xc, fld), '%s.%s[0]' % (xa, fld)
            # recompute properly below
            if order == '132':
                m1, m2, ad = '%s.%s[0]' % (xa, fld), '%s.%s[0]' % (xc, fld), '%s.%s[0]' % (xb, fld)
            elif order == '213':
                m1, m2, ad = '%s.%s[0]' % (xb, fld), '%s.%s[0]' % (xa, fld), '%s.%s[0]' % (xc, fld)
            else:
                m1, m2, ad = '%s.%s[0]' % (xb, fld), '%s.%s[0]' % (xc, fld), '%s.%s[0]' % (xa, fld)
            # Intel: vfmadd132: dst = dst*src3 + src2 ; 213: dst = src2*dst + src3 ; 231: dst = src2*src3 + dst
            # with ops = (dst, src2, src3) -> xa=dst, xb=src2, xc=src3
            if order == '132':
                m1, m2, ad = '%s.%s[0]' % (xa, fld), '%s.%s[0]' % (xc, fld), '%s.%s[0]' % (xb, fld)
            elif order == '213':
                m1, m2, ad = '%s.%s[0]' % (xb, fld), '%s.%s[0]' % (xa, fld), '%s.%s[0]' % (xc, fld)
            else:
                m1, m2, ad = '%s.%s[0]' % (xb, fld), '%s.%s[0]' % (xc, fld), '%s.%s[0]' % (xa, fld)
            fn = 'fma_sd' if dbl else 'fma_ss'
            return [self.xd(ins, d, '%s(%s, %s, %s, %s, %s, %s)' % (fn, xa, m1, m2, ad, 'true' if neg_prod else 'false', 'true' if neg_add else 'false'))]
        return f

    def i_pextrw(self, ins, ops, lv, lock):
        d, s, im = ops
        return [self.wr(ins, d, '(uint32_t)%s.w[%d]' % (self.xs(ins, s), im.imm & 7))]

    def i_pmovmskb(self, ins, ops, lv, lock):
        d, s = ops
        return [self.wr(ins, d, 'sse_pmovmskb(%s)' % self.xs(ins, s))]
    i_vpmovmskb = i_pmovmskb

    def i_movmskps(self, ins, ops, lv, lock):
        return [self.wr(ins, ops[0], 'sse_movmskps(%s)' % self.xs(ins, ops[1]))]

    def i_movmskpd(self, ins, ops, lv, lock):
        return [self.wr(ins, ops[0], 'sse_movmskpd(%s)' % self.xs(ins, ops[1]))]

    def i_movlhps(self, ins, ops, lv, lock): return self.x_arith('sse_movlhps')(ins, ops, lv, lock)
    def i_movhlps(self, ins, ops, lv, lock): return self.x_arith('sse_movhlps')(ins, ops, lv, lock)

    def i_movhpd(self, ins, ops, lv, lock):
        d, s = ops
        if d.type == X86_OP_MEM:
            return ['S64(%s, %s.q[1]);' % (self.addr_expr(ins, d), self.xs(ins, s))]
        return [self.xd(ins, d, '({ V128 r = %s; r.q[1] = L64(%s); r; })' % (self.xs(ins, d), self.addr_expr(ins, s)))]

    def i_movlpd(self, ins, ops, lv, lock):
        d, s = ops
        if d.type == X86_OP_MEM:
            return ['S64(%s, %s.q[0]);' % (self.addr_expr(ins, d), self.xs(ins, s))]
        return [self.xd(ins, d, '({ V128 r = %s; r.q[0] = L64(%s); r; })' % (self.xs(ins, d), self.addr_expr(ins, s)))]
    i_movlps = i_movlpd
    i_movhps = i_movhpd

    def i_pcmpistri(self, ins, ops, lv, lock):
        a, b, im = ops
        self.used_gpr.add(1)
        return ['{ uint32_t f; r1 = sse_pcmpistri(%s, %s, %d, &f); fC = f&1; fZ = (f>>1)&1; fS = (f>>2)&1; fO = (f>>3)&1; fP = 0; }' % (self.xs(ins, a), self.xs(ins, b), im.imm)]


def _install_sse():
    E = Emitter
    ar = lambda n: (lambda self, ins, ops, lv, lock, _n=n: self.x_arith(_n)(ins, ops, lv, lock))
    names = {}
    for nm in ['add', 'sub', 'mul', 'div', 'min', 'max']:
        for sfx in ['sd', 'ss', 'ps', 'pd']:
            names[nm + sfx] = 'sse_' + nm + sfx
    names['sqrtsd'] = 'sse_sqrtsd'; names['sqrtss'] = 'sse_sqrtss'
    for k, v in {'andps': 'sse_and', 'andpd': 'sse_and', 'pand': 'sse_and', 'orps': 'sse_or', 'orpd': 'sse_or', 'por': 'sse_or',
                 'xorps': 'sse_xor', 'xorpd': 'sse_xor', 'pxor': 'sse_xor', 'andnps': 'sse_andn', 'andnpd': 'sse_andn', 'pandn': 'sse_andn',
                 'unpcklps': 'sse_unpcklps', 'unpckhps': 'sse_unpckhps', 'unpcklpd': 'sse_unpcklpd', 'unpckhpd': 'sse_unpckhpd',
                 'punpcklbw': 'sse_punpcklbw', 'punpcklwd': 'sse_punpcklwd', 'punpckhwd': 'sse_punpckhwd', 'punpckldq': 'sse_punpckldq', 'punpcklqdq': 'sse_punpcklqdq',
                 'paddq': 'sse_paddq', 'psubq': 'sse_psubq', 'paddd': 'sse_paddd', 'psubd': 'sse_psubd', 'pcmpeqb': 'sse_pcmpeqb', 'pcmpeqw': 'sse_pcmpeqw', 'pcmpeqd': 'sse_pcmpeqd'}.items():
        names[k] = v
    for k, v in list(names.items()):
        setattr(E, 'i_' + k, ar(v))
        setattr(E, 'i_v' + k, ar(v))
    un = lambda n: (lambda self, ins, ops, lv, lock, _n=n: self.x_unary(_n)(ins, ops, lv, lock))
    for k, v in {'cvtdq2pd': 'sse_cvtdq2pd', 'cvtdq2ps': 'sse_cvtdq2ps', 'cvtps2pd': 'sse_cvtps2pd', 'cvtpd2ps': 'sse_cvtpd2ps', 'cvttpd2dq': 'sse_cvttpd2dq',
                 'cvtpd2dq': 'sse_cvtpd2dq', 'cvttps2dq': 'sse_cvttps2dq', 'pmovsxdq': 'sse_pmovsxdq'}.items():
        setattr(E, 'i_' + k, un(v))
        setattr(E, 'i_v' + k, un(v))
    im = lambda n, b: (lambda self, ins, ops, lv, lock, _n=n, _b=b: self.x_imm(_n, _b)(ins, ops, lv, lock))
    for k, v, b in [('shufps', 'sse_shufps', True), ('shufpd', 'sse_shufpd', True), ('pshufd', 'sse_pshufd', False), ('pshuflw', 'sse_pshuflw', False), ('pshufhw', 'sse_pshufhw', False), ('cmpsd', 'sse_cmpsd', True), ('cmpss', 'sse_cmpss', True)]:
        setattr(E, 'i_' + k, im(v, b))
        setattr(E, 'i_v' + k, im(v, b))
    sh = lambda n: (lambda self, ins, ops, lv, lock, _n=n: self.x_shift_imm(_n)(ins, ops, lv, lock))
    for k, v in {'psrldq': 'sse_psrldq', 'pslldq': 'sse_pslldq', 'psrlq': 'sse_psrlq', 'psllq': 'sse_psllq', 'psrld': 'sse_psrld', 'pslld': 'sse_pslld', 'psrad': 'sse_psrad'}.items():
        setattr(E, 'i_' + k, sh(v))
        setattr(E, 'i_v' + k, sh(v))
    for k, fn in {'cvttsd2si': 'cvtt_d2i', 'cvtsd2si': 'cvt_d2i', 'cvttss2si': 'cvtt_f2i', 'cvtss2si': 'cvt_f2i'}.items():
        g = (lambda fn_: (lambda self, ins, ops, lv, lock: self.x_cvt_f2i(fn_)(ins, ops, lv, lock)))(fn)
        setattr(E, 'i_' + k, g)
        setattr(E, 'i_v' + k, g)
    setattr(E, 'i_cvtsd2ss', lambda self, ins, ops, lv, lock: self.x_cvt_f2f(True)(ins, ops, lv, lock))
    setattr(E, 'i_vcvtsd2ss', lambda self, ins, ops, lv, lock: self.x_cvt_f2f(True)(ins, ops, lv, lock))
    setattr(E, 'i_cvtss2sd', lambda self, ins, ops, lv, lock: self.x_cvt_f2f(False)(ins, ops, lv, lock))
    setattr(E, 'i_vcvtss2sd', lambda self, ins, ops, lv, lock: self.x_cvt_f2f(False)(ins, ops, lv, lock))
    for order in ('132', '213', '231'):
        for kind, (np_, na) in {'madd': (False, False), 'msub': (False, True), 'nmadd': (True, False), 'nmsub': (True, True)}.items():
            for sfx, dbl in (('sd', True), ('ss', False)):
                nm = 'vf%s%s%s' % (kind, order, sfx)
                setattr(E, 'i_' + nm, (lambda o, d, a, b: (lambda self, ins, ops, lv, lock: self.x_fma(o, d, a, b)(ins, ops, lv, lock)))(order, dbl, np_, na))


_install_sse()


# ---------------------------------------------------------------------------------------------------------------
def emit_function(img, fl, fn_lifted, fnname, stats):
    order = sorted(fl.insns)
    if not order:
        return None
    live_out = compute_liveness(fl, order)
    E = Emitter(img, fl, fn_lifted, fnname)
    body = []
    labels = set(fl.targets) | set(fl.group)
    if EH_WRAP and fl.root in EH_ROOTS:
        for a_, i_ in fl.insns.items():
            mn_ = i_.mn[4:] if i_.mn.startswith('bnd ') else i_.mn
            if mn_ == 'call' and (a_ + i_.size) in fl.insns:
                labels.add(a_ + i_.size)
    unimpl_here = []
    for idx, a in enumerate(order):
        ins = fl.insns[a]
        mn = ins.mn
        if mn.startswith('bnd '):
            mn = mn[4:]
        lv = ALLF if ALLFLAGS else live_out[a]
        if a in labels:
            body.append('L_%x:; RT_BARRIER();' % a)
        body.append('// %x: %s %s' % (a, ins.mn, ins.ci.op_str))
        nxt = a + ins.size
        try:
            if mn == 'ret':
                body.append('r4 += 8; SPILL; return;')
            elif mn == 'jmp':
                op = ins.ops[0]
                if a in fl.tables:
                    tgt = E.rd(ins, op, 8)
                    body.append('{ uint64_t t = %s - g_delta; switch (t) {' % tgt)
                    for t in fl.tables[a]:
                        body.append('case %s: goto L_%x;' % (hx(t), t))
                    body.append('default: rt_badjump(%s, t + g_delta); } }' % hx(a))
                elif op.type == X86_OP_IMM:
                    t = op.imm
                    if t in fl.targets:
                        body.append('goto L_%x;' % t)
                    else:
                        body.append('SPILL; %s; return;' % call_expr(t, fn_lifted))
                else:
                    tgt = E.rd(ins, op, 8)
                    body.append('{ uint64_t t = %s; SPILL; rt_call(c, t); return; }' % tgt)
            elif is_jcc(mn):
                cc = cc_of(mn, 'j')
                E.flags_used |= CC_USE[cc]
                if a in fl.tails:
                    body.append('if (%s) { SPILL; %s; return; }' % (CC_EXPR[cc], call_expr(fl.tails[a], fn_lifted)))
                else:
                    body.append('if (%s) goto L_%x;' % (CC_EXPR[cc], ins.ops[0].imm))
            elif mn in ('jrcxz', 'jecxz'):
                E.used_gpr.add(1)
                body.append('if (%s) goto L_%x;' % ('r1==0' if mn == 'jrcxz' else '(uint32_t)r1==0', ins.ops[0].imm))
            elif mn == 'call':
                op = ins.ops[0]
                ret = nxt
                if op.type == X86_OP_IMM:
                    body.append('r4 -= 8; S64(r4, A(%s)); SPILL; %s; RELOAD;' % (hx(ret), call_expr(op.imm, fn_lifted)))
                else:
                    tgt = E.rd(ins, op, 8)
                    body.append('{ uint64_t t = %s; r4 -= 8; S64(r4, A(%s)); SPILL; rt_call(c, t); RELOAD; }' % (tgt, hx(ret)))
            elif mn in ('int3', 'ud2', 'hlt'):
                body.append('rt_unimpl(%s, "%s");' % (hx(a), mn))
            else:
                lines = E.emit(ins, lv)
                body.extend(lines)
        except Unimpl as e:
            unimpl_here.append((a, ins.mn, str(e)))
            body.append('rt_unimpl(%s, "%s");' % (hx(a), (ins.mn + ' ' + ins.ci.op_str).replace('"', "'")))
            stats['unimpl'][ins.mn.split(' ')[-1] if ins.mn.startswith('rep') else ins.mn] += 1
        # fallthrough into non-adjacent next label? (sequential order, so adjacency holds unless gap)
        if mn not in ('ret', 'jmp', 'int3', 'ud2', 'hlt') and nxt not in fl.insns:
            body.append('rt_badjump(%s, %s);' % (hx(a), hx(nxt)))
    # header
    gp = sorted(E.used_gpr)
    xp = sorted(E.used_xmm)
    head = ['void %s(Ctx* c) {' % fnname]
    for i in gp:
        head.append('  uint64_t r%d = c->r[%d];' % (i, i))
    for i in xp:
        head.append('  V128 x%d = c->x[%d];' % (i, i))
    head.append('  uint8_t fC=0,fZ=0,fS=0,fO=0,fP=0; (void)fC;(void)fZ;(void)fS;(void)fO;(void)fP;')
    spill = ' '.join('c->r[%d]=r%d;' % (i, i) for i in gp) + ' ' + ' '.join('c->x[%d]=x%d;' % (i, i) for i in xp)
    reload_ = ' '.join('r%d=c->r[%d];' % (i, i) for i in gp) + ' ' + ' '.join('x%d=c->x[%d];' % (i, i) for i in xp)
    head.append('#define SPILL do { %s } while(0)' % spill)
    head.append('#define RELOAD do { %s } while(0)' % reload_)
    tail = ['#undef SPILL', '#undef RELOAD', '}']
    if EH_WRAP and fl.root in EH_ROOTS:
        head.append('  uint64_t ent_rsp = r4; uint64_t resume = 0;')
        pre = ['  for (;;) { try {']
        sw = ['  if (resume) { uint64_t rr = resume - g_delta; resume = 0; switch (rr) {']
        for t in sorted(labels):
            sw.append('  case %s: goto L_%x;' % (hx(t), t))
        sw.append('  default: rt_badjump(%s, rr); } }' % hx(fl.root))
        post = ['  } catch (GuestResume& e_) { if (e_.entry_rsp != ent_rsp) throw; for (int i_ = 0; i_ < 16; i_++) { c->r[i_] = e_.r[i_]; c->x[i_] = e_.x[i_]; } RELOAD; resume = e_.rip; } }']
        return '\n'.join(head + pre + sw + body + post + tail) + '\n', unimpl_here
    return '\n'.join(head + body + tail) + '\n', unimpl_here


INDIRECT = False
ALLFLAGS = False
EH_WRAP = False
UNRESOLVED = []
EH_ROOTS = set()


def call_expr(t, fn_lifted):
    if t in fn_lifted and not INDIRECT:
        return 'F_%x(c)' % t
    return 'rt_call(c, A(%s))' % hx(t)


# ---------------------------------------------------------------------------------------------------------------
def build_functions(img, only=None):
    # chunk grouping via chained unwind info
    chunks = {}   # begin -> (begin, end)
    parent = {}
    for b, e, u in img.pdata:
        chunks[b] = (b, e)
        flags, chain, handler = img.unwind_info(u)
        if chain is not None:
            parent[b] = img.base + chain[0]
    def root(b):
        seen = 0
        while b in parent and seen < 20:
            b = parent[b]
            seen += 1
        return b
    groups = collections.defaultdict(list)
    for b in chunks:
        groups[root(b)].append(b)
    global EH_ROOTS
    EH_ROOTS = set()
    for b, e, u in img.pdata:
        flags, chain, handler = img.unwind_info(u)
        if handler:
            EH_ROOTS.add(root(b))
    return chunks, groups


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('dll')
    ap.add_argument('out')
    ap.add_argument('--only', default='')
    ap.add_argument('--shard', type=int, default=64)
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--indirect', action='store_true')
    ap.add_argument('--allflags', action='store_true')
    ap.add_argument('--eh', action='store_true')
    args = ap.parse_args()
    global INDIRECT
    INDIRECT = args.indirect
    global ALLFLAGS, EH_WRAP
    ALLFLAGS = args.allflags
    EH_WRAP = args.eh
    t0 = time.time()
    img = PEImg(args.dll)
    os.makedirs(args.out, exist_ok=True)
    chunks, groups = build_functions(img)
    known = set(chunks.keys())
    import bisect as _bis
    _pd = sorted(chunks.values())
    _pdb = [b for b, e in _pd]
    def in_pdata(t):
        i = _bis.bisect_right(_pdb, t) - 1
        return i >= 0 and _pd[i][0] <= t < _pd[i][1]
    pending = []   # (root, ranges, group_starts)
    for r, members in groups.items():
        pending.append((r, [chunks[m] for m in members], set(members)))
    pending.sort()
    queue_extra = []
    only = None
    if args.only:
        only = set(int(x, 16) for x in args.only.split(','))
        pending = [p for p in pending if p[0] in only]
    if args.limit:
        pending = pending[:args.limit]
    # address-taken functions: DIR64 relocation targets that point into code, exports
    if only is None:
        addr_taken = set()
        for rva in img.relocs:
            try:
                v = img.u64(img.base + rva)
            except Exception:
                continue
            if img.in_code(v) and not in_pdata(v) and v not in known:
                addr_taken.add(v)
        for v in img.exports.values():
            if img.in_code(v) and not in_pdata(v) and v not in known:
                addr_taken.add(v)
        for v in sorted(addr_taken):
            known.add(v)
            queue_extra.append((v, None, {v}))
        print('address-taken extra roots:', len(queue_extra))
    funcs = {}
    stats = {'unimpl': collections.Counter(), 'fn': 0, 'insn': 0}
    queue = list(pending) + queue_extra
    extra_seen = set()
    def drain():
        while queue:
            r, ranges, group = queue.pop()
            if r in funcs:
                continue
            fl = FuncLift(img, r, ranges, group)
            decode_func(img, fl, known)
            funcs[r] = fl
            for t in list(fl.calls) + list(fl.tails.values()):
                if t not in known and t not in funcs and t not in extra_seen and img.in_code(t) and only is None and not in_pdata(t):
                    extra_seen.add(t)
                    known.add(t)
                    queue.append((t, None, {t}))
    drain()
    # phase 2: rip-relative `lea` of code addresses (function pointers) and uncovered gaps after int3 padding
    if only is None:
        for rnd in range(4):
            covered = set()
            leas = set()
            for fl in funcs.values():
                for a, ins in fl.insns.items():
                    for k in range(ins.size):
                        covered.add(a + k)
                    if ins.mn == 'lea' and len(ins.ops) == 2 and ins.ops[1].type == X86_OP_MEM and ins.ops[1].mem.base == X86_REG_RIP:
                        t = a + ins.size + ins.ops[1].mem.disp
                        if img.in_code(t):
                            leas.add(t)
            new_roots = set()
            for t in leas:
                if t not in covered and t not in known and not in_pdata(t):
                    new_roots.add(t)
            # gap scan: 16-byte aligned (or int3-preceded) uncovered bytes inside code sections
            for (lo, hi) in img.text_ranges:
                a = lo
                while a < hi:
                    if a in covered or in_pdata(a):
                        a += 1
                        continue
                    # candidate start: previous byte is int3 padding (0xcc) or a covered end, and the byte itself is not cc/00
                    prev_ok = (a - 1) in covered or img.u8(a - 1) == 0xcc
                    b0 = img.u8(a)
                    if prev_ok and b0 not in (0xcc, 0x00) and a not in known:
                        ins = decode_one(img, a)
                        if ins is not None and not in_pdata(a):
                            new_roots.add(a)
                    a += 1
            new_roots = {t for t in new_roots if t not in known}
            print('phase 2 round', rnd, 'new roots', len(new_roots))
            if not new_roots:
                break
            for t in sorted(new_roots):
                known.add(t)
                extra_seen.add(t)
                queue.append((t, None, {t}))
            drain()
    if only is None and EH_WRAP:
        chunk_begins = sorted(chunks)
        def chunk_of(a):
            i = _bis.bisect_right(chunk_begins, a) - 1
            return chunk_begins[i] if i >= 0 and chunks[chunk_begins[i]][0] <= a < chunks[chunk_begins[i]][1] else None
        chunk_root = {}
        for r_, ms_ in groups.items():
            for m_ in ms_:
                chunk_root[m_] = r_
        extras = collections.defaultdict(set)
        for r_, fl in list(funcs.items()):
            insns = fl.insns
            for a, ins in insns.items():
                if ins.mn == 'lea' and len(ins.ops) == 2 and ins.ops[0].type == X86_OP_REG and ins.ops[0].reg == X86_REG_RAX and ins.ops[1].type == X86_OP_MEM and ins.ops[1].mem.base == X86_REG_RIP:
                    x = a + ins.size + ins.ops[1].mem.disp
                    nxt = a + ins.size
                    ok = False
                    for _ in range(4):
                        n_ = insns.get(nxt)
                        if n_ is None:
                            break
                        if n_.mn == 'ret':
                            ok = True
                            break
                        nxt += n_.size
                    if ok and img.in_code(x):
                        ch = chunk_of(x)
                        if ch is not None:
                            rt_ = chunk_root.get(ch)
                            if rt_ is not None and rt_ != r_ and rt_ in funcs and x not in funcs[rt_].insns:
                                extras[rt_].add(x)
        print('EH continuation entries:', sum(len(v) for v in extras.values()), 'in', len(extras), 'functions')
        for r_, xs in extras.items():
            old_fl = funcs[r_]
            fl = FuncLift(img, r_, old_fl.ranges, set(old_fl.group) | xs)
            decode_func(img, fl, known)
            funcs[r_] = fl
            for t in list(fl.calls) + list(fl.tails.values()):
                if t not in known and t not in funcs and t not in extra_seen and img.in_code(t) and not in_pdata(t):
                    extra_seen.add(t); known.add(t); queue.append((t, None, {t}))
        drain()
    print('decoded %d functions (%d extra non-pdata) in %.1fs' % (len(funcs), len(extra_seen), time.time() - t0))
    lifted = set(funcs.keys())
    shard_lines = collections.defaultdict(list)
    decls = []
    table = []
    nfn = 0
    errs = []
    for r in sorted(funcs):
        fl = funcs[r]
        name = 'F_%x' % r
        res = emit_function(img, fl, lifted, name, stats)
        if res is None:
            continue
        code, unimpl = res
        nfn += 1
        stats['insn'] += len(fl.insns)
        decls.append('void %s(Ctx*);' % name)
        sz0 = 0
        a0 = r
        while a0 in fl.insns and sz0 < 64:
            sz0 += fl.insns[a0].size
            a0 += fl.insns[a0].size
        psz = 0
        a0 = r
        while a0 in fl.insns and psz < 5:
            ii = fl.insns[a0]
            if ii.mn.startswith('j') or ii.mn.startswith('call') or ii.mn.startswith('ret') or 'rip' in ii.ci.op_str:
                psz = 0
                break
            psz += ii.size
            a0 += ii.size
        if psz < 5:
            psz = 0
        table.append((r, name, sz0 | (psz << 16) | ((1 << 31) if r in EH_ROOTS else 0)))
        shard_lines[nfn // args.shard].append(code)
    print('emitted %d functions, %d insns in %.1fs' % (nfn, stats['insn'], time.time() - t0))
    for k, v in shard_lines.items():
        with open(os.path.join(args.out, 'lift_%04d.cpp' % k), 'w') as f:
            f.write('#include "x64rt.h"\n#include "x64rt_sse.h"\n#include "lift_decls.h"\n')
            f.write('\n'.join(v))
    with open(os.path.join(args.out, 'lift_decls.h'), 'w') as f:
        f.write('#pragma once\n#include "x64rt.h"\n' + '\n'.join(decls) + '\n')
    with open(os.path.join(args.out, 'lift_table.cpp'), 'w') as f:
        f.write('#include "x64rt.h"\n#include "lift_decls.h"\nstruct LiftEntry { uint64_t va; LiftFn fn; unsigned size; };\nextern const LiftEntry g_lift_table[] = {\n')
        for va, nm, sz0 in table:
            f.write('{0x%xull,%s,%uu},\n' % (va, nm, sz0))
        f.write('{0,0,0}};\nextern const unsigned g_lift_count = %d;\n' % len(table))
    with open(os.path.join(args.out, 'lift_stats.txt'), 'w') as f:
        f.write('functions %d insns %d\nunimpl:\n' % (nfn, stats['insn']))
        for k, v in stats['unimpl'].most_common():
            f.write('  %s %d\n' % (k, v))
    print('unresolved indirect jumps:', len(UNRESOLVED), [hex(x) for x in UNRESOLVED[:20]])
    print('unimpl mnemonics:', dict(stats['unimpl'].most_common(40)))


if __name__ == '__main__':
    main()
