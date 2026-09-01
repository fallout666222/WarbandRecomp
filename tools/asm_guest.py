#!/usr/bin/env python3
"""Assembles the guest-side routines and prints them as a C++ table.

The routines in guest_code.cpp are ARM32 machine code living in guest memory,
so that the hottest imports never cross the boundary at all. Writing those
words by hand works for four instructions and stops working at ten - and a
wrong encoding is a fault deep inside the recompiler with nothing to point at.

So they are written here as assembly, assembled with keystone, and pasted into
guest_code.cpp with the source kept beside the bytes. Run:

    python tools/asm_guest.py

and put the output where the marker says.

    pip install keystone-engine capstone
"""

import sys

try:
    import keystone
except ImportError:
    sys.exit("pip install keystone-engine")

try:
    import capstone
except ImportError:
    capstone = None

# name -> assembly. AAPCS with softfp: a float argument arrives in r0 and the
# result goes back in r0, which is why every one of these starts and ends by
# moving between a core register and a VFP one.
ROUTINES = {
    # float fabsf(float x) - clearing the sign bit is the whole operation, and
    # it needs no VFP at all.
    "fabsf": """
        bic   r0, r0, #0x80000000
        bx    lr
    """,

    # float sqrtf(float x)
    "sqrtf": """
        vmov  s0, r0
        vsqrt.f32 s1, s0
        vmov  r0, s1
        bx    lr
    """,

    # float truncf(float x) - round toward zero, which is what VCVT does.
    #
    # The guard matters: above 2^23 a float has no fractional part left, and
    # VCVT would saturate rather than pass the value through. Infinities and
    # NaNs take the same exit.
    "truncf": """
        bic   r1, r0, #0x80000000
        cmp   r1, #0x4B000000
        bxge  lr
        vmov  s0, r0
        vcvt.s32.f32 s1, s0
        vcvt.f32.s32 s2, s1
        vmov  r0, s2
        bx    lr
    """,

    # float floorf(float x) - round toward minus infinity.
    #
    # ARMv7 has no rounding-mode-selecting convert (VRINTM is ARMv8), so this
    # truncates and then steps down by one when the truncation rounded the
    # wrong way, which it does for every negative non-integer.
    "floorf": """
        bic   r1, r0, #0x80000000
        cmp   r1, #0x4B000000
        bxge  lr
        vmov  s0, r0
        vcvt.s32.f32 s1, s0
        vcvt.f32.s32 s2, s1
        vmov.f32 s3, #1.0
        vcmp.f32 s0, s2
        vmrs  APSR_nzcv, fpscr
        vsublt.f32 s2, s2, s3
        vmov  r0, s2
        bx    lr
    """,

    # float ceilf(float x) - the same, stepping up instead.
    "ceilf": """
        bic   r1, r0, #0x80000000
        cmp   r1, #0x4B000000
        bxge  lr
        vmov  s0, r0
        vcvt.s32.f32 s1, s0
        vcvt.f32.s32 s2, s1
        vmov.f32 s3, #1.0
        vcmp.f32 s0, s2
        vmrs  APSR_nzcv, fpscr
        vaddgt.f32 s2, s2, s3
        vmov  r0, s2
        bx    lr
    """,
    # void* memset(void* d, int c, size_t n)
    #
    # The hottest import there is: eight hundred thousand calls out of every
    # two million, and measurement says the mean block is seventy-four bytes
    # with ninety-nine per cent of them between sixty-five and a hundred and
    # twenty-eight. At that size the boundary crossing costs more than the
    # work, which is the whole argument for doing it here.
    #
    # r0 is kept for the return, r3 walks. The byte is spread across a word so
    # the middle can go four bytes at a time, and the cursor is brought to a
    # four-byte boundary first because an unaligned str is slower where it is
    # allowed at all.
    "memset": """
        mov   r3, r0
        and   r1, r1, #0xff
        orr   r1, r1, r1, lsl #8
        orr   r1, r1, r1, lsl #16
        cmp   r2, #16
        blo   ms_tail
    ms_align:
        tst   r3, #3
        beq   ms_words
        strb  r1, [r3], #1
        sub   r2, r2, #1
        b     ms_align
    ms_words:
        cmp   r2, #16
        blo   ms_tail
        str   r1, [r3], #4
        str   r1, [r3], #4
        str   r1, [r3], #4
        str   r1, [r3], #4
        sub   r2, r2, #16
        b     ms_words
    ms_tail:
        cmp   r2, #0
        bxeq  lr
    ms_byte:
        strb  r1, [r3], #1
        subs  r2, r2, #1
        bne   ms_byte
        bx    lr
    """,

    # size_t strlen(const char* s)
    #
    # Byte at a time on purpose. The engine's strings are short - names,
    # paths, tokens out of a text file - and a word-at-a-time version would
    # need alignment handling and a zero-byte test that costs more than it
    # saves at this length.
    "strlen": """
        mov   r1, r0
    sl_loop:
        ldrb  r2, [r1], #1
        cmp   r2, #0
        bne   sl_loop
        sub   r0, r1, r0
        sub   r0, r0, #1
        bx    lr
    """,

    # int strcmp(const char* a, const char* b)
    #
    # ldrb zero-extends, which is what makes this an unsigned comparison, as
    # the standard requires. The result only has to have the right sign.
    "strcmp": """
    sc_loop:
        ldrb  r2, [r0], #1
        ldrb  r3, [r1], #1
        cmp   r2, r3
        bne   sc_diff
        cmp   r2, #0
        bne   sc_loop
        mov   r0, #0
        bx    lr
    sc_diff:
        sub   r0, r2, r3
        bx    lr
    """,

    # int memcmp(const void* a, const void* b, size_t n)
    "memcmp": """
        cmp   r2, #0
        beq   mc_same
    mc_loop:
        ldrb  r3, [r0], #1
        ldrb  r12, [r1], #1
        cmp   r3, r12
        bne   mc_diff
        subs  r2, r2, #1
        bne   mc_loop
    mc_same:
        mov   r0, #0
        bx    lr
    mc_diff:
        sub   r0, r3, r12
        bx    lr
    """,
}


def main():
    ks = keystone.Ks(keystone.KS_ARCH_ARM,
                     keystone.KS_MODE_ARM | keystone.KS_MODE_LITTLE_ENDIAN)
    cs = None
    if capstone:
        cs = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)

    for name, source in ROUTINES.items():
        lines = [l.strip() for l in source.strip().splitlines()]
        lines = [l for l in lines if l and not l.startswith("@")]

        # The whole routine at once, so that labels and the branches to them
        # resolve. Assembling a line at a time - which this did while the
        # routines were four instructions long - puts every branch at offset
        # zero and produces code that looks right and jumps nowhere.
        encoding, _ = ks.asm("\n".join(lines))
        if encoding is None:
            sys.exit(f"{name}: did not assemble")
        blob = bytes(encoding)
        if len(blob) % 4:
            sys.exit(f"{name}: {len(blob)} bytes is not whole words")
        words = [int.from_bytes(blob[i:i + 4], "little")
                 for i in range(0, len(blob), 4)]

        # Labels take no space, so the source lines and the words only line up
        # once the labels are dropped.
        code_lines = [l for l in lines if not l.endswith(":")]
        if len(code_lines) != len(words):
            code_lines = ["" for _ in words]

        print(f'  emit("{name}", {{')
        for line, word in zip(code_lines, words):
            check = ""
            if cs:
                back = list(cs.disasm(word.to_bytes(4, "little"), 0))
                check = back[0].mnemonic + " " + back[0].op_str if back else "?"
            print(f"      0x{word:08X},   // {line:<26} {check}")
        print("  });")
        print()


if __name__ == "__main__":
    main()
