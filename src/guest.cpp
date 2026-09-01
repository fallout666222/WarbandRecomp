#include "guest.h"

#include <cstdio>
#include <cstdlib>
#include <new>

namespace wb {

void* g_preallocated_space = nullptr;

Memory::Memory() {
  // One flat reservation.
  //
  // On the PC, calloc: .bss and untouched pages have to read as zero, and the
  // OS hands back lazily-committed pages rather than making the whole 2 GiB
  // resident up front.
  //
  // On Horizon that would be exactly wrong. The heap is sized once at process
  // start and every page in it is already zero, so calloc's guarantee is
  // free - but newlib does not know that and memsets the whole block, which
  // costs seconds and makes all 2 GiB resident before the game has read a
  // single file. malloc gets the same zeroed memory without the walk.
#if defined(WB_SWITCH)
  // Already carved out of the heap before newlib saw it - see
  // __libnx_initheap. Kernel heap pages arrive zeroed, which is the guarantee
  // calloc would have cost seconds to provide.
  base_ = static_cast<u8*>(g_preallocated_space);
  if (!base_) base_ = static_cast<u8*>(std::malloc(layout::kSpaceSize));
#else
  base_ = static_cast<u8*>(std::calloc(layout::kSpaceSize, 1));
#endif
  if (!base_) {
    std::fprintf(stderr, "[mem ] cannot reserve the %u MiB guest space\n",
                 layout::kSpaceSize >> 20);
    throw std::bad_alloc();
  }
}

Memory::~Memory() {
  // The carved arena is part of the heap region the loader gave us and was
  // never malloc'd, so it is not freed either.
  if (base_ != g_preallocated_space) std::free(base_);
  std::free(scratch_);
}

u8* Memory::bad(u32 addr) {
  if (!scratch_) scratch_ = static_cast<u8*>(std::calloc(1 << 16, 1));
  if (bad_count_ < 8)
    std::fprintf(stderr,
                 "[mem ] thunk was handed guest address 0x%08X, outside the "
                 "512 MiB space - serving scratch instead\n",
                 addr);
  ++bad_count_;
  return scratch_;
}

std::string Memory::str(u32 addr, size_t limit) const {
  if (addr == 0) return {};
  std::string out;
  for (size_t i = 0; i < limit; ++i) {
    u64 a = u64(addr) + i;
    if (a >= layout::kSpaceSize) break;
    char c = static_cast<char>(base_[a]);
    if (!c) break;
    out.push_back(c);
  }
  return out;
}

u32 BumpAllocator::alloc(u32 size, u32 align) {
  u32 addr = (next_ + align - 1) & ~(align - 1);
  if (u64(addr) + size > limit_) return 0;
  next_ = addr + size;
  return addr;
}

}  // namespace wb
