"""PE helpers for the x64 lifter: sections, relocs, pdata, imports."""
import struct, pefile


class PEImg:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=False)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.size = self.pe.OPTIONAL_HEADER.SizeOfImage
        self.entry = self.base + self.pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.secs = []
        for s in self.pe.sections:
            name = s.Name.rstrip(b'\0').decode()
            self.secs.append((name, self.base + s.VirtualAddress, max(s.Misc_VirtualSize, 0), s.get_data()))
        # full memory image
        self.mem = bytearray(self.size)
        for name, va, vsz, data in self.secs:
            off = va - self.base
            self.mem[off:off + len(data)] = data
        self.text_ranges = [(va, va + vsz) for name, va, vsz, d in self.secs if name in ('.text', 'il2cpp')]
        # relocations (DIR64)
        self.relocs = set()
        if hasattr(self.pe, 'DIRECTORY_ENTRY_BASERELOC'):
            for blk in self.pe.DIRECTORY_ENTRY_BASERELOC:
                for e in blk.entries:
                    if e.type == 10:
                        self.relocs.add(e.rva)
        # imports: IAT slot VA -> "dll!name"
        self.imports = {}
        for ent in self.pe.DIRECTORY_ENTRY_IMPORT:
            dll = ent.dll.decode()
            for imp in ent.imports:
                nm = imp.name.decode() if imp.name else 'ord%d' % imp.ordinal
                self.imports[imp.address] = (dll, nm)  # imp.address is a VA (ImageBase + rva)
        # exports
        self.exports = {}
        if hasattr(self.pe, 'DIRECTORY_ENTRY_EXPORT'):
            for e in self.pe.DIRECTORY_ENTRY_EXPORT.symbols:
                nm = e.name.decode() if e.name else 'ord%d' % e.ordinal
                self.exports[nm] = self.base + e.address
        # pdata
        self.pdata = []  # (begin, end, unwind) VAs
        for name, va, vsz, data in self.secs:
            if name == '.pdata':
                n = vsz // 12
                for i in range(n):
                    b, e, u = struct.unpack_from('<III', data, i * 12)
                    if b == 0 and e == 0:
                        break
                    self.pdata.append((self.base + b, self.base + e, self.base + u))

    def rd(self, va, n):
        o = va - self.base
        return bytes(self.mem[o:o + n])

    def u8(self, va): return self.mem[va - self.base]
    def u32(self, va): return struct.unpack_from('<I', self.mem, va - self.base)[0]
    def i32(self, va): return struct.unpack_from('<i', self.mem, va - self.base)[0]
    def u64(self, va): return struct.unpack_from('<Q', self.mem, va - self.base)[0]

    def in_code(self, va):
        for a, b in self.text_ranges:
            if a <= va < b:
                return True
        return False

    def unwind_info(self, uva):
        """returns (flags, chain_runtime_function or None, handler_rva or None)"""
        b0 = self.u8(uva)
        flags = b0 >> 3
        ncodes = self.u8(uva + 2)
        p = uva + 4 + ((ncodes + 1) & ~1) * 2
        chain = None
        handler = None
        if flags & 4:
            chain = (self.u32(p), self.u32(p + 4), self.u32(p + 8))
        elif flags & 3:
            handler = self.u32(p)
        return flags, chain, handler
