# Lists the static constructors of the Switch build, in the order they run.
#
# The .nro dies during static initialisation and the log names the offender by
# runtime address. This turns that into a symbol: the log also prints where
# open_the_log_first ended up, which gives the load slide, and the rest is the
# ELF's symbol table.
#
#   python tools/init_array.py                 # list them all, in order
#   python tools/init_array.py 0x1234 0x5678   # runtime addr, runtime addr of
#                                              # open_the_log_first -> name
import struct
import sys

ELF = r"H:\dwnld\wb_switch\warband_nx.elf"


def sections(data):
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        name, typ, flags, addr, off, size, link, info, align, entsize = \
            struct.unpack_from("<IIQQQQIIQQ", data, o)
        secs.append(dict(name=name, type=typ, addr=addr, off=off, size=size,
                         link=link, entsize=entsize))
    st = secs[e_shstrndx]
    for s in secs:
        end = data.index(b"\0", st["off"] + s["name"])
        s["sname"] = data[st["off"] + s["name"]:end].decode()
    return secs


def symbols(data, secs):
    out = []
    for s in secs:
        if s["sname"] not in (".symtab", ".dynsym"):
            continue
        strs = secs[s["link"]]
        for i in range(s["size"] // 24):
            o = s["off"] + i * 24
            nm, info, other, shndx, value, size = \
                struct.unpack_from("<IBBHQQ", data, o)
            end = data.index(b"\0", strs["off"] + nm)
            name = data[strs["off"] + nm:end].decode(errors="replace")
            # $x and $d are the assembler's mapping symbols; they sit on top
            # of every function and would win every lookup.
            if name and value and not name.startswith("$"):
                out.append((value, size, name))
    out.sort()
    return out


def name_for(syms, addr):
    best = None
    for value, size, name in syms:
        if value <= addr and (size == 0 or addr < value + size):
            if best is None or value > best[0]:
                best = (value, name)
    if not best:
        return "?"
    off = addr - best[0]
    return best[1] + (f"+0x{off:X}" if off else "")


def main():
    data = open(ELF, "rb").read()
    secs = sections(data)
    syms = symbols(data, secs)

    init = next((s for s in secs if s["sname"] == ".init_array"), None)
    if not init:
        print("no .init_array in", ELF)
        return
    n = init["size"] // 8
    entries = [struct.unpack_from("<Q", data, init["off"] + i * 8)[0]
               for i in range(n)]

    if len(sys.argv) >= 3:
        want = int(sys.argv[1], 0)
        anchor_runtime = int(sys.argv[2], 0)
        anchor_link = next((v for v, _s, nm in syms
                            if "open_the_log_first" in nm), None)
        if anchor_link is None:
            print("open_the_log_first is not in the symbol table")
            return
        slide = anchor_runtime - anchor_link
        print(f"load slide 0x{slide:X}")
        print(f"0x{want:X} -> {name_for(syms, want - slide)}")
        return

    print(f"{n} static constructors, in the order they run:")
    for i, a in enumerate(entries, 1):
        print(f"  {i:4d}/{n}  0x{a:016X}  {name_for(syms, a)}")


main()
