# Disassembles one function out of the engine by symbol name.
#
# The engine ships stripped of source but not of symbols, so the fastest way
# to answer "what does the game do with this" is to read the code it actually
# runs. Import calls are resolved through the trampoline table, so a call to
# an unresolved import shows up as a branch into the PLT - printed by name
# where the dynamic symbol table can name it.
import sys, struct
from capstone import *

PATH = r"H:\dwnld\Mount & Blade - Warband\apk_lib\libMBExpMobile.so"


def read_elf(path):
    data = open(path, "rb").read()
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        name, typ, flags, addr, off, size, link, info, align, entsize = \
            struct.unpack_from("<10I", data, o)
        secs.append(dict(name=name, type=typ, addr=addr, off=off, size=size,
                         link=link, entsize=entsize))
    strtab = secs[e_shstrndx]
    def sname(n):
        end = data.index(b"\0", strtab["off"] + n)
        return data[strtab["off"] + n:end].decode()
    for s in secs:
        s["sname"] = sname(s["name"])
    return data, secs


def symbols(data, secs):
    out = {}
    for s in secs:
        if s["sname"] not in (".symtab", ".dynsym"):
            continue
        strs = secs[s["link"]]
        n = s["size"] // 16
        for i in range(n):
            o = s["off"] + i * 16
            nm, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, o)
            end = data.index(b"\0", strs["off"] + nm)
            name = data[strs["off"] + nm:end].decode(errors="replace")
            if name and value:
                out.setdefault(name, (value, size))
    return out


def main():
    want = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 400
    data, secs = read_elf(PATH)
    syms = symbols(data, secs)
    hits = [k for k in syms if want in k]
    if not hits:
        print("no symbol matching", want)
        return
    if len(hits) > 1 and want not in hits:
        print("matches:", *sorted(hits)[:20], sep="\n  ")
    name = want if want in syms else sorted(hits, key=len)[0]
    value, size = syms[name]
    thumb = value & 1
    addr = value & ~1
    # By address, find the section holding it.
    sec = next(s for s in secs
               if s["addr"] <= addr < s["addr"] + s["size"] and s["type"] == 1)
    off = sec["off"] + (addr - sec["addr"])
    length = size if size else count * 4
    code = data[off:off + length]

    by_addr = {}
    for k, (v, _sz) in syms.items():
        by_addr.setdefault(v & ~1, k)

    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
    md.detail = False
    print(f"{name} at 0x{addr:08X} ({'thumb' if thumb else 'arm'}), {length} bytes")
    shown = 0
    for ins in md.disasm(code, addr):
        tgt = ""
        if ins.mnemonic.startswith(("b", "bl")) and ins.op_str.startswith("#"):
            try:
                t = int(ins.op_str[1:], 0)
                if t in by_addr:
                    tgt = "   ; " + by_addr[t]
            except ValueError:
                pass
        print(f"  {ins.address:08X}  {ins.mnemonic:<8} {ins.op_str}{tgt}")
        shown += 1
        if shown >= count:
            break


main()
