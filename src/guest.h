// Guest address space: layout and flat memory access.
//
// The guest is a 32-bit ARM world living inside one contiguous host
// allocation. Address translation is therefore a single add, which is what
// makes the whole thunk layer cheap. Guest address 0 stays unmapped so null
// derefs fault instead of silently reading the image.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace wb {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

// ---------------------------------------------------------------- layout
//
// The engine image measures 39.3 MiB (3.5 MiB of code, the rest .bss), so
// almost all of this is heap for the guest allocator - and the heap is what
// sizes it.
//
// Loading a campaign, the engine holds around 556 000 live allocations
// totalling 456 MiB, nearly all of them from operator new: a million calls
// during the load, fewer than half ever freed while the game is running.
// The first attempt gave it 512 MiB in total and it ran out partway through
// "Loading Setting Data", which is not a leak on either side - that is simply
// what the campaign costs.
//
// 2 GiB is therefore the working size. It is lazily committed, so the
// resident cost is what the guest actually touches, and the guest is 32-bit
// so there is room to grow again below the 4 GiB ceiling if a mod needs it.
// On the Switch this rules out applet mode: the .nro must be launched with
// title takeover, where a process can have around 3.2 GiB.
//
// Overridable at build time with -DWB_GUEST_MB=..., because how much a Switch
// will actually hand over depends on how the homebrew was launched, and
// finding that out means trying.
#if !defined(WB_GUEST_MB)
#define WB_GUEST_MB 2048
#endif

namespace layout {
constexpr u32 kSpaceSize = static_cast<u32>(WB_GUEST_MB) << 20;

constexpr u32 kNullGuard = 0x00000000;   // 1 MiB left unmapped
constexpr u32 kImageBase = 0x00100000;   // libMBExpMobile.so
// The image measures 0x0274DB0C - 3.5 MiB of code then 35.7 MiB of .bss -
// so it ends at 0x0284DB0C. 48 MiB of room leaves headroom for a differently
// built binary without moving everything below.
constexpr u32 kImageLimit = 0x03100000;

constexpr u32 kTrampolines = 0x03100000; // svc/bx-lr pairs, 8 bytes each
constexpr u32 kTrampSize = 0x00002000;
constexpr u32 kDataImports = 0x03102000; // __sF, _ctype_, _tolower_tab_, ...
constexpr u32 kDataSize = 0x00002000;

// The clock, published into guest memory so reading it costs no boundary
// crossing. A host thread keeps it current; the layout is a seqlock:
//
//   +0  sequence (odd while being written)
//   +4  seconds
//   +8  nanoseconds
//   +12 microseconds
constexpr u32 kTimePage = 0x0310E000;

// svc #0xFFFFFF lives here: the marker that a guest call has returned.
constexpr u32 kReturnStub = 0x0310F000;

// Guest ARM32 routines that answer imports directly - integer division and
// anything else too hot to cross the boundary for.
constexpr u32 kGuestCodeBase = 0x03110000;
constexpr u32 kGuestCodeSize = 0x00001000;

constexpr u32 kLibcBase = 0x03111000;    // guest-side libc / libm / libgcc
constexpr u32 kLibcLimit = 0x03800000;

constexpr u32 kHeapBase = 0x03800000;    // guest allocator arena
constexpr u32 kStackTop = kSpaceSize;    // stacks grow down from here
constexpr u32 kStackSize = 0x00100000;   // 1 MiB per guest thread
constexpr u32 kHeapLimit = kStackTop - (16 * kStackSize);
}  // namespace layout

// The guest space, when the platform has already set it aside.
//
// Horizon sizes the process heap once, before main, and hands it to newlib -
// after which asking malloc for two gigabytes of it in one piece does not
// work. The heap is where the memory is, so the arena is taken off the front
// of it at that moment instead, before newlib ever sees the region. Null on
// platforms that just allocate.
extern void* g_preallocated_space;

// ---------------------------------------------------------------- memory
class Memory {
 public:
  Memory();
  ~Memory();

  Memory(const Memory&) = delete;
  Memory& operator=(const Memory&) = delete;

  bool valid(u32 addr, u32 len = 1) const {
    return addr >= layout::kImageBase && u64(addr) + len <= layout::kSpaceSize;
  }

  // Host view of a guest address. Returns nullptr for guest null so callers
  // can pass it straight through to APIs that accept null.
  //
  // Thunks hand these pointers to memcpy and friends, so a guest address that
  // has gone wrong would otherwise become a host segfault with no context.
  // Anything outside the arena is reported once and served from a scratch
  // page instead, which keeps the run alive long enough to say who did it.
  template <typename T = void>
  T* host(u32 addr) {
    if (addr == 0) return nullptr;
    if (addr >= layout::kSpaceSize) return reinterpret_cast<T*>(bad(addr));
    return reinterpret_cast<T*>(base_ + addr);
  }

  // True while no thunk has been handed an out-of-range address.
  bool sane() const { return bad_count_ == 0; }
  unsigned bad_count() const { return bad_count_; }

  // These are not on the recompiler's hot path - it reaches mapped memory
  // through the page table - so they can afford to check. Thunks call them
  // with addresses the guest computed, and one bad value would otherwise be
  // an unexplained host crash.
  bool in_range(u32 a, u32 n) const { return u64(a) + n <= layout::kSpaceSize; }

  // Largest length starting at `addr` that stays inside the arena. Block
  // operations must run through this: host() only validates the start, so a
  // plausible pointer with an implausible length would walk off the end of
  // the allocation and corrupt the host heap.
  u32 clamp(u32 addr, u32 n) const {
    if (addr >= layout::kSpaceSize) return 0;
    u64 room = layout::kSpaceSize - addr;
    return n <= room ? n : static_cast<u32>(room);
  }

  u8 read8(u32 a) const { return in_range(a, 1) ? base_[a] : 0; }
  u16 read16(u32 a) const {
    u16 v = 0;
    if (in_range(a, 2)) std::memcpy(&v, base_ + a, 2);
    return v;
  }
  u32 read32(u32 a) const {
    u32 v = 0;
    if (in_range(a, 4)) std::memcpy(&v, base_ + a, 4);
    return v;
  }
  u64 read64(u32 a) const {
    u64 v = 0;
    if (in_range(a, 8)) std::memcpy(&v, base_ + a, 8);
    return v;
  }

  void write8(u32 a, u8 v) { if (in_range(a, 1)) base_[a] = v; }
  void write16(u32 a, u16 v) { if (in_range(a, 2)) std::memcpy(base_ + a, &v, 2); }
  void write32(u32 a, u32 v) { if (in_range(a, 4)) std::memcpy(base_ + a, &v, 4); }
  void write64(u32 a, u64 v) { if (in_range(a, 8)) std::memcpy(base_ + a, &v, 8); }

  void copy_in(u32 dst, const void* src, size_t n) {
    if (in_range(dst, static_cast<u32>(n))) std::memcpy(base_ + dst, src, n);
  }
  void zero(u32 dst, size_t n) {
    if (in_range(dst, static_cast<u32>(n))) std::memset(base_ + dst, 0, n);
  }

  // Reads a NUL-terminated guest string. Bounded so a corrupt pointer cannot
  // walk off the end of the arena.
  std::string str(u32 addr, size_t limit = 4096) const;

  u8* base() { return base_; }

 private:
  u8* bad(u32 addr);

  u8* base_ = nullptr;
  u8* scratch_ = nullptr;      // somewhere harmless for bad pointers to land
  unsigned bad_count_ = 0;
};

// A bump allocator for things the loader itself needs to place in guest
// memory. The engine's own malloc lives in the guest libc and owns the heap
// arena; this is only for loader-owned blocks.
class BumpAllocator {
 public:
  BumpAllocator(u32 base, u32 limit) : next_(base), limit_(limit) {}
  u32 alloc(u32 size, u32 align = 16);
  u32 used() const { return next_; }

 private:
  u32 next_, limit_;
};

}  // namespace wb
