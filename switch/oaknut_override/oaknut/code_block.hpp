// SPDX-FileCopyrightText: Copyright (c) 2022 merryhime <https://mary.rs>
// SPDX-License-Identifier: MIT
//
// Horizon (Nintendo Switch) support added for warband_nx.
//
// This file replaces oaknut's own code_block.hpp for the Switch build only;
// the PC build never sees it. It is a copy rather than a wrapper because the
// original is a single class with a per-platform body, and adding a platform
// means adding a branch to that body.
//
// Why it is needed
// ----------------
// Horizon does not let a page be writable and executable at the same time,
// and it has no mmap. JIT memory comes from the kernel through libnx's Jit
// object, and libnx picks one of two mechanisms:
//
//   JitType_SetProcessMemoryPermission - one region whose permissions flip
//       between RW and RX. jitGetRwAddr and jitGetRxAddr are the same address.
//   JitType_CodeMemory - the [4.0.0+] code-memory syscalls, which map the same
//       physical pages twice: a writable alias and an executable one at
//       *different* addresses, both live at once.
//
// libnx decides, not us, and on a current console with Atmosphere it decides
// code memory - title takeover or not. So the second case is the normal one
// and has to work. Emitting into one address and running another is exactly
// what oaknut's two-pointer CodeGenerator is for: wptr() is where instructions
// are written, xptr() is where they will be branched to, and every label and
// relocation is computed against xptr. dynarmic already constructs its
// generators that way, so the whole difference is which two pointers it is
// handed - see switch/patch_dynarmic.py.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>

#if defined(__SWITCH__)
#    include <switch.h>
// libnx defines BIT(n) as a macro, and oaknut has an assembler mnemonic of
// that name - so every later oaknut header that declares BIT fails to parse.
// This is the only place the two meet, because this is the only oaknut header
// that needs libnx at all.
#    ifdef BIT
#        undef BIT
#    endif
#elif defined(_WIN32)
#    define NOMINMAX
#    include <windows.h>
#elif defined(__APPLE__)
#    include <TargetConditionals.h>
#    include <libkern/OSCacheControl.h>
#    include <pthread.h>
#    include <sys/mman.h>
#    include <unistd.h>
#else
#    include <sys/mman.h>
#endif

namespace oaknut {

class CodeBlock {
public:
    explicit CodeBlock(std::size_t size)
        : m_size(size)
    {
#if defined(__SWITCH__)
        // Horizon wants the region page-aligned, and jitCreate fails outright
        // on a size that is not.
        m_size = (size + 0xFFF) & ~std::size_t(0xFFF);
        const Result rc = jitCreate(&m_jit, m_size);
        if (R_FAILED(rc)) {
            std::printf("[jit ] jitCreate(%zu bytes) failed: 2%03d-%04d\n",
                        m_size, R_MODULE(rc), R_DESCRIPTION(rc));
            std::fflush(stdout);
            throw std::bad_alloc{};
        }

        m_memory = (std::uint32_t*)jitGetRwAddr(&m_jit);
        m_exec = (std::uint32_t*)jitGetRxAddr(&m_jit);
        std::printf("[jit ] %zu KiB of code memory, %s (rw %p, rx %p)\n",
                    m_size / 1024,
                    m_memory == m_exec ? "one view" : "written and run through "
                                                      "separate aliases",
                    (void*)m_memory, (void*)m_exec);
        std::fflush(stdout);
        // Created writable. dynarmic emits before it ever runs anything.
        m_writable = true;
#elif defined(_WIN32)
        m_memory = (std::uint32_t*)VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        m_exec = m_memory;
#elif defined(__APPLE__)
#    if TARGET_OS_IPHONE
        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
#    else
        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE | MAP_JIT, -1, 0);
#    endif
        m_exec = m_memory;
#elif defined(__NetBSD__)
        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_MPROTECT(PROT_READ | PROT_WRITE | PROT_EXEC), MAP_ANON | MAP_PRIVATE, -1, 0);
        m_exec = m_memory;
#elif defined(__OpenBSD__)
        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
        m_exec = m_memory;
#else
        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
        m_exec = m_memory;
#endif

        if (m_memory == nullptr)
            throw std::bad_alloc{};
    }

    ~CodeBlock()
    {
        if (m_memory == nullptr)
            return;

#if defined(__SWITCH__)
        jitClose(&m_jit);
#elif defined(_WIN32)
        VirtualFree((void*)m_memory, 0, MEM_RELEASE);
#else
        munmap(m_memory, m_size);
#endif
    }

    CodeBlock(const CodeBlock&) = delete;
    CodeBlock& operator=(const CodeBlock&) = delete;
    CodeBlock(CodeBlock&&) = delete;
    CodeBlock& operator=(CodeBlock&&) = delete;

    /// Where instructions are written. The same as xptr() everywhere except
    /// Horizon with code memory.
    std::uint32_t* wptr() const
    {
        return m_memory;
    }

    /// Where they are executed from, and therefore what every branch, label
    /// and CodePtr must be expressed in.
    std::uint32_t* xptr() const
    {
        return m_exec;
    }

    std::uint32_t* ptr() const
    {
        return m_memory;
    }

    void protect()
    {
#if defined(__SWITCH__)
        // With two aliases there is nothing to transition: the writable one
        // and the executable one are both live for the lifetime of the block,
        // and libnx's transition for that type flushes the *whole* region's
        // caches - thirty-two megabytes of it, around every basic block. The
        // per-block invalidate() below is what keeps the caches honest.
        if (m_memory != m_exec)
            return;
        // Idempotent on purpose: dynarmic brackets every emission, and nested
        // or repeated brackets must not turn into unbalanced syscalls.
        if (!m_writable)
            return;
        jitTransitionToExecutable(&m_jit);
        m_writable = false;
#elif defined(__APPLE__) && !TARGET_OS_IPHONE
        pthread_jit_write_protect_np(1);
#elif defined(__APPLE__) || defined(__NetBSD__) || defined(__OpenBSD__)
        mprotect(m_memory, m_size, PROT_READ | PROT_EXEC);
#endif
    }

    void unprotect()
    {
#if defined(__SWITCH__)
        if (m_memory != m_exec)
            return;
        if (m_writable)
            return;
        jitTransitionToWritable(&m_jit);
        m_writable = true;
#elif defined(__APPLE__) && !TARGET_OS_IPHONE
        pthread_jit_write_protect_np(0);
#elif defined(__APPLE__) || defined(__NetBSD__) || defined(__OpenBSD__)
        mprotect(m_memory, m_size, PROT_READ | PROT_WRITE);
#endif
    }

    /// `mem` is an executable-view pointer: that is what dynarmic holds.
    void invalidate(std::uint32_t* mem, std::size_t size)
    {
#if defined(__APPLE__)
        sys_icache_invalidate(mem, size);
#elif defined(_WIN32)
        FlushInstructionCache(GetCurrentProcess(), mem, size);
#else
        // Horizon runs on AArch64 and reaches this path too: the cache
        // maintenance below is architectural, not OS-specific, and Horizon
        // leaves DC CVAU and IC IVAU available at EL0.
        //
        // The two halves are done through different addresses when the aliases
        // differ. The dirty lines are at the address that was written, and the
        // instruction fetch happens at the address that will be branched to.
        // AArch64 data caches behave as physically indexed, so cleaning either
        // alias would do - but saying which is which costs nothing and does
        // not depend on that.
        std::uint32_t* const wmem = mem + (m_memory - m_exec);

        static std::size_t icache_line_size = 0x10000, dcache_line_size = 0x10000;

        std::uint64_t ctr;
        __asm__ volatile("mrs %0, ctr_el0"
                         : "=r"(ctr));

        const std::size_t isize = icache_line_size = std::min<std::size_t>(icache_line_size, 4 << ((ctr >> 0) & 0xf));
        const std::size_t dsize = dcache_line_size = std::min<std::size_t>(dcache_line_size, 4 << ((ctr >> 16) & 0xf));

        const std::uintptr_t wend = (std::uintptr_t)wmem + size;
        const std::uintptr_t xend = (std::uintptr_t)mem + size;

        for (std::uintptr_t addr = ((std::uintptr_t)wmem) & ~(dsize - 1); addr < wend; addr += dsize) {
            __asm__ volatile("dc cvau, %0"
                             :
                             : "r"(addr)
                             : "memory");
        }
        __asm__ volatile("dsb ish\n"
                         :
                         :
                         : "memory");

        for (std::uintptr_t addr = ((std::uintptr_t)mem) & ~(isize - 1); addr < xend; addr += isize) {
            __asm__ volatile("ic ivau, %0"
                             :
                             : "r"(addr)
                             : "memory");
        }
        __asm__ volatile("dsb ish\nisb\n"
                         :
                         :
                         : "memory");
#endif
    }

    void invalidate_all()
    {
        invalidate(m_exec, m_size);
    }

protected:
    std::uint32_t* m_memory = nullptr;   // writable alias
    std::uint32_t* m_exec = nullptr;     // executable alias
    std::size_t m_size = 0;
#if defined(__SWITCH__)
    Jit m_jit{};
    bool m_writable = false;
#endif
};

}  // namespace oaknut
