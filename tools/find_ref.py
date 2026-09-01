# Finds which function mentions a string.
#
# ARM code cannot hold a 32-bit address in an instruction, so every string the
# engine passes to something - a JNI field name, a file name, a shader
# uniform - is reached through a word in a literal pool near the code that
# uses it. Searching for that word is therefore the same as searching for the
# use, and the symbol table turns the answer into a name.
#
#   python find_ref.py gamepadAxisIndices
import os
import sys, struct, bisect

# The engine, where setup.bat puts it. Override with WB_SO.
PATH = os.environ.get(
    "WB_SO",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 os.pardir, "game", "libMBExpMobile.so"))


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

    def sname(n):
        end = data.index(b"\0", st["off"] + n)
        return data[st["off"] + n:end].decode()

    for s in secs:
        s["sname"] = sname(s["name"])
    return secs


def symbols(data, secs):
    out = []
    for s in secs:
        if s["sname"] not in (".symtab", ".dynsym"):
            continue
        strtab = secs[s["link"]]
        n = s["size"] // 16
        for i in range(n):
            o = s["off"] + i * 16
            name, value, size, info, other, shndx = struct.unpack_from(
                "<IIIBBH", data, o)
            if not value or (info & 0xF) != 2:      # STT_FUNC only
                continue
            end = data.index(b"\0", strtab["off"] + name)
            out.append((value & ~1, size,
                        data[strtab["off"] + name:end].decode()))
    out.sort()
    return out


def main():
    needle = sys.argv[1].encode()
    data = open(PATH, "rb").read()
    secs = sections(data)
    syms = symbols(data, secs)
    starts = [s[0] for s in syms]

    # Where the string itself lives, as a virtual address.
    hits = []
    at = data.find(needle)
    while at != -1:
        for s in secs:
            if s["off"] <= at < s["off"] + s["size"] and s["addr"]:
                hits.append(s["addr"] + (at - s["off"]))
                break
        at = data.find(needle, at + 1)
    if not hits:
        print("string not found")
        return
    print("string at", ", ".join(hex(h) for h in hits))

    # Every word in the file equal to one of those addresses is a pointer to
    # it; the function containing that word is the one that uses it.
    for va in hits:
        word = struct.pack("<I", va)
        at = data.find(word)
        while at != -1:
            for s in secs:
                if s["off"] <= at < s["off"] + s["size"] and s["addr"]:
                    ref = s["addr"] + (at - s["off"])
                    i = bisect.bisect_right(starts, ref) - 1
                    who = "?"
                    if i >= 0:
                        start, size, name = syms[i]
                        if size == 0 or ref < start + size + 64:
                            who = "%s+0x%X" % (name, ref - start)
                    print("  referenced at %#x in %s (%s)" % (ref, s["sname"], who))
                    break
            at = data.find(word, at + 1)


main()
