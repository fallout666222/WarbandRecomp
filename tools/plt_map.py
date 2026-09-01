# Prints which import each PLT stub in the engine stands for.
#
# Static reading of the engine keeps running into "bl #0x55b2c" - a call into
# the PLT, which says nothing on its own. .rel.plt names every one of them.
import os
import struct, sys

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
    for s in secs:
        end = data.index(b"\0", st["off"] + s["name"])
        s["sname"] = data[st["off"] + s["name"]:end].decode()
    return secs


def main():
    data = open(PATH, "rb").read()
    secs = sections(data)
    rel = next(s for s in secs if s["sname"] == ".rel.plt")
    dynsym = secs[rel["link"]]
    dynstr = secs[dynsym["link"]]
    plt = next(s for s in secs if s["sname"] == ".plt")

    def symname(i):
        nm, = struct.unpack_from("<I", data, dynsym["off"] + i * 16)
        end = data.index(b"\0", dynstr["off"] + nm)
        return data[dynstr["off"] + nm:end].decode(errors="replace")

    # ARM PLT: a 20-byte header, then 12 bytes per entry, in .rel.plt order.
    n = rel["size"] // 8
    want = sys.argv[1:]
    for i in range(n):
        _off, info = struct.unpack_from("<II", data, rel["off"] + i * 8)
        name = symname(info >> 8)
        stub = plt["addr"] + 20 + i * 12
        text = f"0x{stub:08X}  {name}"
        if not want or any(w.lower() in text.lower() for w in want):
            print(text)


main()
