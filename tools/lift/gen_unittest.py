#!/usr/bin/env python3
"""Generate instruction-level differential tests for the lifter.

For every distinct instruction form found in the DLL's code we emit
  * the raw bytes (executed natively by unit_main.cpp), and
  * the lifted C++ (via lift.Emitter),
and unit_main.cpp compares registers / xmm / flags / scratch memory for random inputs.
usage: gen_unittest.py <dll> <outdir> [--max N]
"""
import sys, os, struct, collections, random
from capstone import *
from capstone.x86 import *
sys.path.insert(0, os.path.dirname(__file__))
import lift
from lift import *

SKIP_MN = ('jmp', 'call', 'ret', 'push', 'pop', 'leave', 'int3', 'ud2', 'hlt', 'cpuid', 'rdtsc', 'xgetbv', 'syscall', 'int',
           'enter', 'lfence', 'mfence', 'sfence', 'nop', 'pause', 'prefetchw', 'prefetcht0', 'prefetcht1', 'prefetchnta', 'endbr64',
           'cli', 'sti', 'in', 'out', 'ins', 'outs', 'std', 'cld', 'pushfq', 'popfq', 'lahf', 'sahf', 'xlatb', 'iretd', 'retf',
           'stmxcsr', 'ldmxcsr', 'vstmxcsr', 'vldmxcsr', 'fnstcw', 'fldcw', 'fnstsw', 'fnclex', 'fninit', 'fnsave', 'fnstenv', 'vzeroupper',
           'movntps', 'movntdq', 'movntpd', 'wait', 'sldt', 'str', 'lldt', 'ltr', 'lar', 'rdmsr', 'wrmsr')
SKIP_PFX = ('rep', 'repne', 'repe', 'j', 'loop', 'stos', 'movs', 'lods', 'scas', 'cmps', 'ins', 'outs', 'f', 'cmpxchg16b')


def usable(ci):
    mn = ci.mnemonic
    if mn.startswith('lock '):
        mn = mn[5:]
    if mn.startswith('bnd ') or mn.startswith('.'):
        return False
    if mn in SKIP_MN or any(mn.startswith(p) for p in SKIP_PFX) and mn not in ('movsx', 'movsxd', 'movsd', 'movss', 'movsb'):
        return False
    if mn in ('movsb', 'movsw', 'movsq'):
        return False
    if mn == 'movsd' and not any(o.type == X86_OP_REG and ci.reg_name(o.reg).startswith('xmm') for o in ci.operands):
        return False
    for o in ci.operands:
        if o.type == X86_OP_REG:
            n = ci.reg_name(o.reg)
            if n in ('rsp', 'esp', 'sp', 'spl', 'rip', 'eip') or n.startswith(('cr', 'dr', 'st', 'mm', 'k', 'seg', 'es', 'cs', 'ss', 'ds', 'fs', 'gs', 'ymm', 'zmm')):
                return False
        if o.type == X86_OP_MEM:
            m = o.mem
            if m.segment != 0 or m.base == X86_REG_RIP or m.base in (X86_REG_RSP, X86_REG_ESP):
                return False
            if m.base != 0 and ci.reg_name(m.base) not in REGMAP:
                return False
            if m.index != 0 and (ci.reg_name(m.index) not in REGMAP or ci.reg_name(m.index) in ('rsp', 'esp')):
                return False
            if m.base == 0 and m.index == 0:
                return False
    if mn.startswith('v') and any(o.type == X86_OP_REG and ci.reg_name(o.reg).startswith('ymm') for o in ci.operands):
        return False
    return True


def key_of(ci):
    parts = [ci.mnemonic]
    for o in ci.operands:
        if o.type == X86_OP_REG:
            parts.append('r:' + ci.reg_name(o.reg))
        elif o.type == X86_OP_IMM:
            v = o.imm
            parts.append('i:%d:%s' % (o.size, v if -16 <= v <= 64 else ('big' if v > 0 else 'nbig')))
        elif o.type == X86_OP_MEM:
            m = o.mem
            parts.append('m:%d:%s:%s:%d' % (o.size, ci.reg_name(m.base) if m.base else '-', ci.reg_name(m.index) if m.index else '-', m.scale))
    return '|'.join(parts)


def main():
    dll, out = sys.argv[1], sys.argv[2]
    mx = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 100000
    img = PEImg(dll)
    os.makedirs(out, exist_ok=True)
    md2 = Cs(CS_ARCH_X86, CS_MODE_64)
    md2.detail = True
    md2.skipdata = True
    seen = {}
    for name, va, vsz, data in img.secs:
        if name not in ('.text', 'il2cpp'):
            continue
        off = 0
        n = len(data)
        # linear sweep per pdata function would be more exact; sweep from section start with resync on bad bytes
        for ci in md2.disasm(data, va):
            if usable(ci):
                k = key_of(ci)
                if k not in seen:
                    seen[k] = (ci.address, bytes(ci.bytes))
    print('distinct forms:', len(seen))
    items = sorted(seen.items())[:mx]
    tests = []
    shard = []
    shards = 0
    for idx, (k, (addr, raw)) in enumerate(items):
        ci = next(md2.disasm(raw, addr))
        ins = Insn()
        ins.addr = addr; ins.size = ci.size; ins.mn = ci.mnemonic; ins.ops = ci.operands; ins.raw = ci.bytes; ins.ci = ci; ins.prefix = ci.prefix
        fl = FuncLift(img, addr, [(addr, addr + ci.size)], {addr})
        E = Emitter(img, fl, set(), 'U')
        try:
            lines = E.emit(ins, ALLF)
        except Unimpl as e:
            tests.append((idx, raw, None, ins, str(e)))
            continue
        except Exception as e:
            tests.append((idx, raw, None, ins, 'EXC ' + repr(e)))
            continue
        if lines is None:
            continue
        body = ['void U_%d(Ctx* c) {' % idx]
        for i in range(16):
            body.append('  uint64_t r%d = c->r[%d];' % (i, i))
        for i in range(16):
            body.append('  V128 x%d = c->x[%d];' % (i, i))
        body.append('  uint8_t fC=c->aux&1, fP=(c->aux>>2)&1, fZ=(c->aux>>6)&1, fS=(c->aux>>7)&1, fO=(c->aux>>11)&1;')
        body.extend(lines)
        for i in range(16):
            body.append('  c->r[%d] = r%d;' % (i, i))
        for i in range(16):
            body.append('  c->x[%d] = x%d;' % (i, i))
        body.append('  c->aux = fC | (fP<<2) | (fZ<<6) | (fS<<7) | ((uint64_t)fO<<11);')
        body.append('}')
        shard.append('\n'.join(body))
        tests.append((idx, raw, True, ins, ''))
        if len(shard) >= 300:
            with open(os.path.join(out, 'unit_%03d.cpp' % shards), 'w') as f:
                f.write('#include "x64rt.h"\n#include "x64rt_sse.h"\n' + '\n'.join(shard))
            shards += 1
            shard = []
    if shard:
        with open(os.path.join(out, 'unit_%03d.cpp' % shards), 'w') as f:
            f.write('#include "x64rt.h"\n#include "x64rt_sse.h"\n' + '\n'.join(shard))
        shards += 1
    # table
    unimpl = collections.Counter()
    with open(os.path.join(out, 'unit_table.cpp'), 'w') as f:
        f.write('#include "x64rt.h"\n#include "unit_test.h"\n')
        for idx, raw, ok, ins, err in tests:
            if ok:
                f.write('void U_%d(Ctx*);\n' % idx)
        f.write('const UnitTest g_tests[] = {\n')
        n = 0
        for idx, raw, ok, ins, err in tests:
            if not ok:
                unimpl[ins.mn] += 1
                continue
            ci = ins.ci
            base = idx_ = -1
            scale = 1
            disp = 0
            msz = 0
            for o in ci.operands:
                if o.type == X86_OP_MEM:
                    m = o.mem
                    base = REGMAP[ci.reg_name(m.base)][0] if m.base else -1
                    idx_ = REGMAP[ci.reg_name(m.index)][0] if m.index else -1
                    scale = m.scale
                    disp = m.disp
                    msz = o.size
            mn = ci.mnemonic
            u, k, may = flag_info(ins)
            m0 = mn.split(' ')[-1] if mn.startswith('lock ') else mn
            if m0 in ('imul', 'mul'):
                may = C_ | O_
            elif m0 in ('bt', 'bts', 'btr', 'btc'):
                may = C_
            elif m0 in ('bsf', 'bsr'):
                may = Z_
            elif m0 in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
                cop = ci.operands[1] if len(ci.operands) > 1 else None
                if cop is None or cop.type != X86_OP_IMM or (cop.imm & (63 if ci.operands[0].size == 8 else 31)) != 1:
                    may &= ~O_
            u = 0
            f.write('{U_%d, {%s}, %d, %d, %d, %d, %d, %d, 0x%x, "%s"},\n' % (idx, ','.join(str(b) for b in raw), len(raw), base, idx_, scale, disp, msz, may | u, (ci.mnemonic + ' ' + ci.op_str).replace('"', "'")))
            n += 1
        f.write('{0,{0},0,0,0,0,0,0,0,0}};\nconst unsigned g_ntests = %d;\n' % n)
    print('tests', n, 'unimpl', sum(unimpl.values()), dict(unimpl.most_common(20)), 'shards', shards)


if __name__ == '__main__':
    main()
