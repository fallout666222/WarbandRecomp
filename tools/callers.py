# Who calls this function.
#
# The other direction from disasm_fn.py: given a symbol, find every BL or BLX
# in the executable segment whose target is it, and name the function each one
# sits in. Answers "what does the engine do with this" when the function
# itself only fetches something.
#
#   python callers.py _Z16NvGetGamepadAxesP7_JNIEnvP8_jobjectRi
import sys, struct, bisect
from capstone import *

PATH = r"H:\dwnld\Mount & Blade - Warband\apk_lib\libMBExpMobile.so"


def sections(data):
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        f = struct.unpack_from("<10I", data, o)
        secs.append(dict(zip(
            "name type flags addr off size link info align entsize".split(), f)))
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
        strtab = secs[s["link"]]
        for i in range(s["size"] // 16):
            o = s["off"] + i * 16
            name, value, size, info, other, shndx = struct.unpack_from(
                "<IIIBBH", data, o)
            if not value or (info & 0xF) != 2:
                continue
            end = data.index(b"\0", strtab["off"] + name)
            out.append((value & ~1, value & 1, size,
                        data[strtab["off"] + name:end].decode()))
    out.sort()
    return out


def main():
    want = sys.argv[1]
    data = open(PATH, "rb").read()
    secs = sections(data)
    syms = symbols(data, secs)
    by_name = {s[3]: s for s in syms}
    if want not in by_name:
        print("no such symbol")
        return
    target = by_name[want][0]
    print("%s at %#x" % (want, target))

    text = next(s for s in secs if s["sname"] == ".text")
    starts = [s[0] for s in syms]
    base = text["addr"]
    body = data[text["off"]:text["off"] + text["size"]]

    def owner(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i < 0:
            return "?"
        start, thumb, size, name = syms[i]
        return "%s+0x%X" % (name, addr - start)

    found = 0
    # ARM BL/BLX(imm): cond 1011 imm24, and 1111101H imm24.
    for off in range(0, len(body) - 3, 4):
        word, = struct.unpack_from("<I", body, off)
        top = word >> 24
        if (top & 0x0F) == 0x0B and (top >> 4) != 0xF:
            pass                      # BL, any condition
        elif (top & 0xFE) == 0xFA:
            pass                      # BLX immediate
        else:
            continue
        imm = word & 0x00FFFFFF
        if imm & 0x800000:
            imm -= 0x1000000
        at = base + off
        dest = at + 8 + imm * 4
        if (top & 0xFE) == 0xFA and (top & 1):
            dest += 2
        if dest == target:
            print("  called from %#x  %s" % (at, owner(at)))
            found += 1
    if not found:
        print("  no ARM-mode calls found (the caller may be Thumb)")


main()
