# Teaches dynarmic's AArch64 backend to write code in one place and run it in
# another.
#
# Horizon's code-memory syscalls map the same physical pages twice - a writable
# alias and an executable one, at different addresses, both live at once - and
# libnx chooses that mechanism on a current console whether or not homebrew was
# launched with title takeover. dynarmic assumes one address: it builds its
# code generator as `code(mem.ptr(), mem.ptr())`, so everything it emits is
# branched to at the address it was written to, which on Horizon is memory
# nothing may execute.
#
# oaknut already supports the split - its generator takes a write pointer and
# an execute pointer and computes every label and relocation against the second
# - so the fix is which two pointers dynarmic passes. There are three of them
# in the address space, plus one loop that walks the buffer to disassemble it,
# and one more that is easy to miss: the AArch64 spin lock assembles itself
# into a 4 KiB block of its own during static initialisation, before main, and
# takes the two function pointers out of it the same wrong way. Missing that
# one gets you an instruction abort at the writable alias the first time the
# exclusive monitor is touched - which is the first guest constructor.
#
# This is a patch rather than a fork because dynarmic is a checkout, not part of
# this tree. It is idempotent: running it twice does nothing the second time.
#
#   python switch/patch_dynarmic.py [path-to-dynarmic]

import io
import os
import sys

DEFAULT = r"H:\dwnld\dynarmic"

# oaknut's own CodeBlock has one view, so it gains the two names dynarmic will
# now ask for. Our Switch build replaces this header entirely - see
# switch/oaknut_override - and it is the copy that knows the two can differ.
OAKNUT_OLD = """    std::uint32_t* ptr() const
    {
        return m_memory;
    }
"""
OAKNUT_NEW = """    std::uint32_t* ptr() const
    {
        return m_memory;
    }

    /// Where instructions are written, and where they are executed from. The
    /// same address here; a platform that cannot have both permissions on one
    /// page overrides this header and makes them differ.
    std::uint32_t* wptr() const
    {
        return m_memory;
    }

    std::uint32_t* xptr() const
    {
        return m_memory;
    }
"""

SPIN_LOCK = [
    ("        , code{mem.ptr(), mem.ptr()} {}",
     "        , code{mem.wptr(), mem.xptr()} {}"),
]

ADDRESS_SPACE = [
    ("        , code(mem.ptr(), mem.ptr())",
     "        , code(mem.wptr(), mem.xptr())"),
    ("    for (u32* ptr = mem.ptr(); ptr < code.xptr<u32*>(); ptr++) {",
     "    for (u32* ptr = mem.xptr(); ptr < code.xptr<u32*>(); ptr++) {"),
    ("        CodeGenerator c{mem.ptr(), mem.ptr()};",
     "        CodeGenerator c{mem.wptr(), mem.xptr()};"),
]


def patch(path, pairs, all_occurrences=False, done_marker=None):
    text = io.open(path, encoding="utf-8").read()
    # Some replacements keep the old text inside the new one - the oaknut
    # accessors are added *after* the ones already there - so "is the old text
    # still present" cannot decide whether the work is done. A marker that
    # only the new text contains can.
    if done_marker and done_marker in text:
        return 0
    changed = 0
    for old, new in pairs:
        if new in text and old not in text:
            continue                      # already done
        if old not in text:
            print("  ! not found in %s: %s" % (os.path.basename(path), old.strip()[:60]))
            continue
        count = text.count(old) if all_occurrences else 1
        text = text.replace(old, new, count)
        changed += count
    if changed:
        io.open(path, "w", encoding="utf-8").write(text)
    return changed


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    oaknut = os.path.join(root, "externals", "oaknut", "include", "oaknut",
                          "code_block.hpp")
    space = os.path.join(root, "src", "dynarmic", "backend", "arm64",
                         "address_space.cpp")
    spin = os.path.join(root, "src", "dynarmic", "common",
                        "spin_lock_arm64.cpp")
    for f in (oaknut, space, spin):
        if not os.path.exists(f):
            print("cannot find %s - is %s a dynarmic checkout?" % (f, root))
            return 1

    n = patch(oaknut, [(OAKNUT_OLD, OAKNUT_NEW)],
              done_marker="std::uint32_t* wptr() const")
    n += patch(space, ADDRESS_SPACE, all_occurrences=True)
    n += patch(spin, SPIN_LOCK)
    print("dynarmic: %d line%s changed for split code memory" %
          (n, "" if n == 1 else "s") if n else
          "dynarmic: already patched for split code memory")
    return 0


sys.exit(main())
