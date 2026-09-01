// Hand-written libc, allocator and runtime support.
//
// The inventory marks these symbols GUEST, meaning an ARM32 libc image should
// eventually own them so they never cross the boundary. That is the right end
// state for throughput, but it is not required for correctness: what has to
// be a guest value is the *address* a function hands back, not the code that
// computes it. A host thunk allocating out of the guest arena returns a
// 32-bit guest address and is entirely legitimate - and it gets the engine
// running without first building a cross-compiled libc.
//
// Anything here that turns out to be hot is a candidate for moving guest-side
// later. Nothing else has to change when it does.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "env.h"

namespace wb {
namespace {

// ---------------------------------------------------------------- allocator
//
// Segregated free lists with boundary tags - dlmalloc's shape, cut down.
//
// The first version of this was first-fit over one block chain: allocation
// walked the heap from the bottom, and free walked the whole heap to
// coalesce. That is fine for a few thousand blocks and quadratic for a few
// hundred thousand, which is what loading a campaign asks for. The symptom
// was not a crash but a stop: the engine read troops.txt and scenes.txt and
// then simply stayed inside malloc, and no guest instruction retired again.
//
// So: free blocks are threaded onto lists by size class, allocation takes the
// first block from the smallest class that must fit, and free coalesces with
// its neighbours in constant time - forward through the next header, backward
// through a footer the previous block leaves behind. Blocks are carved off an
// untouched tail until there is nothing left to carve.
//
// Layout of a block at `at`:
//
//   +0  size    payload bytes, a multiple of 8
//   +4  flags   bit 0 this block in use, bit 1 the previous block in use
//   +8  payload, or - while free - next, prev, and a footer holding the size
//       again in the last four bytes
constexpr u32 kHeaderSize = 8;
constexpr u32 kFooterSize = 4;
constexpr u32 kMinPayload = 16;      // next + prev + footer, rounded up
constexpr u32 kInUse = 1;
constexpr u32 kPrevInUse = 2;
constexpr int kBins = 32;

// Two game threads allocate concurrently, so every entry point takes the
// lock. Without it they hand out overlapping blocks and the engine quietly
// scribbles over its own structures.
class GuestHeap {
 public:
  void init(Memory& mem) {
    std::lock_guard<std::mutex> g(lock_);
    mem_ = &mem;
    base_ = top_ = layout::kHeapBase;
    limit_ = layout::kHeapLimit;
    for (int i = 0; i < kBins; ++i) bins_[i] = 0;
    ready_ = true;
  }

  u32 alloc(u32 want) {
    std::lock_guard<std::mutex> g(lock_);
    return unlocked_alloc(want);
  }

  void release(u32 addr) {
    std::lock_guard<std::mutex> g(lock_);
    unlocked_release(addr);
  }

  void unlocked_release(u32 addr) {
    if (!addr || !ready_) return;
    u32 at = addr - kHeaderSize;
    if (at < base_ || at >= top_) {
      std::printf("[heap] free of a pointer outside the arena: 0x%08X\n", addr);
      return;
    }
    u32 size = mem_->read32(at);
    u32 flags = mem_->read32(at + 4);
    if (!(flags & kInUse)) {
      std::printf("[heap] double free at 0x%08X\n", addr);
      return;
    }
    live_ -= size;

    // Absorb the block after this one, if it is free.
    u32 next = at + kHeaderSize + size;
    if (next < top_ && !(mem_->read32(next + 4) & kInUse)) {
      const u32 next_size = mem_->read32(next);
      unlink(next, next_size);
      size += kHeaderSize + next_size;
    }
    // And the block before it, found through its footer.
    if (!(flags & kPrevInUse)) {
      const u32 prev_size = mem_->read32(at - kFooterSize);
      const u32 prev = at - kHeaderSize - prev_size;
      if (prev >= base_) {
        unlink(prev, prev_size);
        at = prev;
        size += kHeaderSize + prev_size;
        flags = mem_->read32(at + 4);
      }
    }

    // A block that reaches the tail is simply given back to it, which keeps
    // the free lists from filling up with the same memory over and over.
    if (at + kHeaderSize + size == top_) {
      top_ = at;
      return;
    }

    mem_->write32(at, size);
    mem_->write32(at + 4, flags & kPrevInUse);
    mem_->write32(at + kHeaderSize + size - kFooterSize, size);   // footer
    const u32 after = at + kHeaderSize + size;
    if (after < top_)
      mem_->write32(after + 4, mem_->read32(after + 4) & ~kPrevInUse);
    link(at, size);
  }

  u32 resize(Memory& mem, u32 addr, u32 want) {
    if (!addr) return alloc(want);
    if (want == 0) {
      release(addr);
      return 0;
    }
    u32 old_size;
    {
      std::lock_guard<std::mutex> g(lock_);
      old_size = mem_->read32(addr - kHeaderSize);
      if (old_size >= want) return addr;
    }
    u32 fresh = alloc(want);
    if (!fresh) return 0;
    std::memcpy(mem.host<u8>(fresh), mem.host<u8>(addr), old_size);
    release(addr);
    return fresh;
  }

  u32 size_of(u32 addr) const {
    return addr ? mem_->read32(addr - kHeaderSize) : 0;
  }
  u32 live() const { return live_; }

  // Walks every block once and says where the memory went. Only ever called
  // when an allocation has already failed, so the cost does not matter and
  // the answer decides whether the arena is too small or something is
  // leaking.
  void report() const {
    struct Bucket { u32 count = 0; u64 bytes = 0; };
    Bucket used[kBins], freed[kBins];
    u32 used_blocks = 0, free_blocks = 0, largest = 0;
    u64 used_bytes = 0, free_bytes = 0;
    for (u32 at = base_; at + kHeaderSize <= top_;) {
      const u32 size = mem_->read32(at);
      if (size == 0) break;
      const bool in_use = (mem_->read32(at + 4) & kInUse) != 0;
      const int b = bin_for(size);
      if (in_use) {
        ++used_blocks;
        used_bytes += size;
        ++used[b].count;
        used[b].bytes += size;
        if (size > largest) largest = size;
      } else {
        ++free_blocks;
        free_bytes += size;
        ++freed[b].count;
        freed[b].bytes += size;
      }
      at += kHeaderSize + size;
    }
    std::printf("[heap] %u blocks in use holding %llu bytes; %u free holding "
                "%llu; largest live block %u\n",
                used_blocks, (unsigned long long)used_bytes, free_blocks,
                (unsigned long long)free_bytes, largest);
    for (int b = 0; b < kBins; ++b) {
      if (!used[b].count && !freed[b].count) continue;
      std::printf("[heap]   %8u..%-8u  %7u live (%llu bytes), %6u free\n",
                  16u << b, (32u << b) - 1, used[b].count,
                  (unsigned long long)used[b].bytes, freed[b].count);
    }
  }

 private:
  // Size classes are powers of two above a 16-byte floor, so bin b holds
  // sizes in [16 << b, 32 << b). Anything in a bin above the one a request
  // maps to is therefore certain to fit, which is what makes the search
  // constant-time in the common case.
  static int bin_for(u32 size) {
    u32 s = size >> 4;
    int b = 0;
    while (s > 1 && b < kBins - 1) {
      s >>= 1;
      ++b;
    }
    return b;
  }

  void link(u32 at, u32 size) {
    const int b = bin_for(size);
    const u32 head = bins_[b];
    mem_->write32(at + kHeaderSize, head);          // next
    mem_->write32(at + kHeaderSize + 4, 0);         // prev
    if (head) mem_->write32(head + kHeaderSize + 4, at);
    bins_[b] = at;
  }

  void unlink(u32 at, u32 size) {
    const u32 next = mem_->read32(at + kHeaderSize);
    const u32 prev = mem_->read32(at + kHeaderSize + 4);
    if (prev)
      mem_->write32(prev + kHeaderSize, next);
    else
      bins_[bin_for(size)] = next;
    if (next) mem_->write32(next + kHeaderSize + 4, prev);
  }

  u32 unlocked_alloc(u32 want) {
    if (!ready_) return 0;
    want = (want + 7) & ~7u;
    if (want < kMinPayload) want = kMinPayload;

    const int first = bin_for(want);
    for (int b = first; b < kBins; ++b) {
      // Only the first bin can hold blocks too small for the request, so only
      // it needs a search - and that search is bounded, because a bin full of
      // near-misses must not become the old linear scan by another name.
      int guard = 0;
      for (u32 at = bins_[b]; at && guard < 64; ++guard) {
        const u32 size = mem_->read32(at);
        const u32 next = mem_->read32(at + kHeaderSize);
        if (size >= want) {
          unlink(at, size);
          return carve(at, size, want);
        }
        at = next;
      }
    }

    // Nothing recycled fits: take fresh memory from the tail.
    if (u64(top_) + kHeaderSize + want > limit_) {
      std::printf("[heap] out of memory: %u bytes requested, %u live\n", want,
                  live_);
      static bool once = false;
      if (!once) {
        once = true;
        report();
      }
      return 0;
    }
    // The block just below the tail is always in use: a free one touching the
    // tail is given straight back to it rather than binned, so there is never
    // a free block there to coalesce with.
    const u32 at = top_;
    top_ = at + kHeaderSize + want;
    mem_->write32(at, want);
    mem_->write32(at + 4, kInUse | kPrevInUse);
    live_ += want;
    return at + kHeaderSize;
  }

  // Turns a free block into an allocated one, returning the tail to the free
  // lists when there is enough of it to be worth having.
  u32 carve(u32 at, u32 size, u32 want) {
    const u32 flags = mem_->read32(at + 4) & kPrevInUse;
    if (size >= want + kHeaderSize + kMinPayload) {
      const u32 tail = at + kHeaderSize + want;
      const u32 tail_size = size - want - kHeaderSize;
      mem_->write32(at, want);
      mem_->write32(at + 4, kInUse | flags);
      mem_->write32(tail, tail_size);
      mem_->write32(tail + 4, kPrevInUse);
      mem_->write32(tail + kHeaderSize + tail_size - kFooterSize, tail_size);
      link(tail, tail_size);
      live_ += want;
      return at + kHeaderSize;
    }
    mem_->write32(at, size);
    mem_->write32(at + 4, kInUse | flags);
    const u32 after = at + kHeaderSize + size;
    if (after < top_)
      mem_->write32(after + 4, mem_->read32(after + 4) | kPrevInUse);
    live_ += size;
    return at + kHeaderSize;
  }

  std::mutex lock_;
  Memory* mem_ = nullptr;
  u32 base_ = 0, top_ = 0, limit_ = 0, live_ = 0;
  u32 bins_[kBins] = {};
  bool ready_ = false;
};

GuestHeap g_heap;

// --------------------------------------------------------------- allocator
//
// Who is allocating, by return address. Almost nothing the engine allocates
// during a campaign load is ever freed, so this histogram is effectively a
// list of what is holding the heap - and the caller's name is the difference
// between "the arena is too small" and "one call site is running away".
struct Site {
  u64 count = 0;
  u64 bytes = 0;
};
std::mutex g_sites_lock;
std::unordered_map<u32, Site> g_sites;

void note_alloc(Env& e, u32 bytes) {
  const u32 lr = e.jit()->Regs()[14];
  std::lock_guard<std::mutex> g(g_sites_lock);
  Site& s = g_sites[lr];
  ++s.count;
  s.bytes += bytes;
}

void report_alloc_sites(Env& e, int top) {
  std::vector<std::pair<u32, Site>> all;
  {
    std::lock_guard<std::mutex> g(g_sites_lock);
    all.assign(g_sites.begin(), g_sites.end());
  }
  std::sort(all.begin(), all.end(), [](const auto& x, const auto& y) {
    return x.second.bytes > y.second.bytes;
  });
  std::printf("[heap] %zu distinct allocation sites; the largest:\n",
              all.size());
  for (int i = 0; i < top && i < static_cast<int>(all.size()); ++i)
    std::printf("[heap]   %10llu bytes in %8llu calls from %s\n",
                (unsigned long long)all[i].second.bytes,
                (unsigned long long)all[i].second.count,
                e.loader().symbolize(all[i].first).c_str());
}

void t_malloc(Env& e) {
  Env::Args a(e);
  const u32 want = a.next32();
  note_alloc(e, want);
  const u32 p = g_heap.alloc(want);
  if (!p) report_alloc_sites(e, 20);
  e.ret(p);
}

void t_free(Env& e) {
  Env::Args a(e);
  g_heap.release(a.next32());
  e.ret(0);
}

void t_calloc(Env& e) {
  Env::Args a(e);
  u32 n = a.next32(), sz = a.next32();
  u64 total = u64(n) * sz;
  if (total > 0xFFFFFFFFull) {
    e.ret(0);
    return;
  }
  note_alloc(e, static_cast<u32>(total));
  u32 p = g_heap.alloc(static_cast<u32>(total));
  if (p) e.mem().zero(p, static_cast<size_t>(total));
  e.ret(p);
}

void t_realloc(Env& e) {
  Env::Args a(e);
  u32 p = a.next32(), n = a.next32();
  e.ret(g_heap.resize(e.mem(), p, n));
}

// ------------------------------------------------------------------ memory
void t_memcpy(Env& e) {
  Env::Args a(e);
  u32 d = a.next32(), s = a.next32(), n = a.next32();
  n = std::min(e.mem().clamp(d, n), e.mem().clamp(s, n));
  if (n) std::memcpy(e.mem().host<u8>(d), e.mem().host<u8>(s), n);
  e.ret(d);
}

void t_memmove(Env& e) {
  Env::Args a(e);
  u32 d = a.next32(), s = a.next32(), n = a.next32();
  n = std::min(e.mem().clamp(d, n), e.mem().clamp(s, n));
  if (n) std::memmove(e.mem().host<u8>(d), e.mem().host<u8>(s), n);
  e.ret(d);
}

void t_memset(Env& e) {
  Env::Args a(e);
  u32 d = a.next32();
  int c = static_cast<int>(a.next32());
  u32 n = e.mem().clamp(d, a.next32());
  if (n) std::memset(e.mem().host<u8>(d), c, n);
  e.ret(d);
}

void t_memcmp(Env& e) {
  Env::Args a(e);
  u32 p = a.next32(), q = a.next32(), n = a.next32();
  n = std::min(e.mem().clamp(p, n), e.mem().clamp(q, n));
  e.ret(static_cast<u32>(
      n ? std::memcmp(e.mem().host<u8>(p), e.mem().host<u8>(q), n) : 0));
}

void t_memchr(Env& e) {
  Env::Args a(e);
  u32 p = a.next32();
  int c = static_cast<int>(a.next32());
  u32 n = e.mem().clamp(p, a.next32());
  if (!n) {
    e.ret(0);
    return;
  }
  const u8* base = e.mem().host<u8>(p);
  const void* hit = std::memchr(base, c, n);
  e.ret(hit ? p + static_cast<u32>(static_cast<const u8*>(hit) - base) : 0);
}

// ------------------------------------------------------------------ string
u32 guest_strlen(Memory& m, u32 p) {
  u32 n = 0;
  while (m.read8(p + n)) ++n;
  return n;
}

void t_strlen(Env& e) {
  Env::Args a(e);
  e.ret(guest_strlen(e.mem(), a.next32()));
}

void t_strcmp(Env& e) {
  Env::Args a(e);
  u32 p = a.next32(), q = a.next32();
  e.ret(static_cast<u32>(std::strcmp(e.mem().host<char>(p),
                                     e.mem().host<char>(q))));
}

void t_strncmp(Env& e) {
  Env::Args a(e);
  u32 p = a.next32(), q = a.next32(), n = a.next32();
  e.ret(static_cast<u32>(
      n ? std::strncmp(e.mem().host<char>(p), e.mem().host<char>(q), n) : 0));
}

void t_strcasecmp(Env& e) {
  Env::Args a(e);
  std::string x = e.mem().str(a.next32()), y = e.mem().str(a.next32());
  for (auto& c : x) c = static_cast<char>(std::tolower((unsigned char)c));
  for (auto& c : y) c = static_cast<char>(std::tolower((unsigned char)c));
  e.ret(static_cast<u32>(x.compare(y)));
}

void t_strncasecmp(Env& e) {
  Env::Args a(e);
  std::string x = e.mem().str(a.next32()), y = e.mem().str(a.next32());
  u32 n = a.next32();
  if (x.size() > n) x.resize(n);
  if (y.size() > n) y.resize(n);
  for (auto& c : x) c = static_cast<char>(std::tolower((unsigned char)c));
  for (auto& c : y) c = static_cast<char>(std::tolower((unsigned char)c));
  e.ret(static_cast<u32>(x.compare(y)));
}

void t_strcpy(Env& e) {
  Env::Args a(e);
  u32 d = a.next32(), s = a.next32();
  u32 n = guest_strlen(e.mem(), s);
  std::memcpy(e.mem().host<u8>(d), e.mem().host<u8>(s), n + 1);
  e.ret(d);
}

void t_strncpy(Env& e) {
  Env::Args a(e);
  u32 d = a.next32(), s = a.next32(), n = a.next32();
  u32 len = guest_strlen(e.mem(), s);
  u32 copy = len < n ? len : n;
  if (copy) std::memcpy(e.mem().host<u8>(d), e.mem().host<u8>(s), copy);
  if (copy < n) e.mem().zero(d + copy, n - copy);
  e.ret(d);
}

void t_strcat(Env& e) {
  Env::Args a(e);
  u32 d = a.next32(), s = a.next32();
  u32 dl = guest_strlen(e.mem(), d), sl = guest_strlen(e.mem(), s);
  std::memcpy(e.mem().host<u8>(d + dl), e.mem().host<u8>(s), sl + 1);
  e.ret(d);
}

void t_strncat(Env& e) {
  Env::Args a(e);
  u32 d = a.next32(), s = a.next32(), n = a.next32();
  u32 dl = guest_strlen(e.mem(), d), sl = guest_strlen(e.mem(), s);
  u32 copy = sl < n ? sl : n;
  if (copy) std::memcpy(e.mem().host<u8>(d + dl), e.mem().host<u8>(s), copy);
  e.mem().write8(d + dl + copy, 0);
  e.ret(d);
}

void t_strchr(Env& e) {
  Env::Args a(e);
  u32 p = a.next32();
  int c = static_cast<int>(a.next32()) & 0xFF;
  for (u32 i = 0;; ++i) {
    u8 v = e.mem().read8(p + i);
    if (v == c) {
      e.ret(p + i);
      return;
    }
    if (!v) break;
  }
  e.ret(0);
}

void t_strrchr(Env& e) {
  Env::Args a(e);
  u32 p = a.next32();
  int c = static_cast<int>(a.next32()) & 0xFF;
  u32 found = 0;
  for (u32 i = 0;; ++i) {
    u8 v = e.mem().read8(p + i);
    if (v == c) found = p + i;
    if (!v) break;
  }
  e.ret(found);
}

void t_strstr(Env& e) {
  Env::Args a(e);
  u32 h = a.next32(), n = a.next32();
  const char* hay = e.mem().host<char>(h);
  const char* hit = std::strstr(hay, e.mem().host<char>(n));
  e.ret(hit ? h + static_cast<u32>(hit - hay) : 0);
}

void t_strdup(Env& e) {
  Env::Args a(e);
  u32 s = a.next32();
  u32 n = guest_strlen(e.mem(), s);
  u32 p = g_heap.alloc(n + 1);
  if (p) std::memcpy(e.mem().host<u8>(p), e.mem().host<u8>(s), n + 1);
  e.ret(p);
}

void t_strlcat(Env& e) {
  Env::Args a(e);
  u32 d = a.next32(), s = a.next32(), n = a.next32();
  u32 dl = guest_strlen(e.mem(), d), sl = guest_strlen(e.mem(), s);
  if (dl < n) {
    u32 room = n - dl - 1;
    u32 copy = sl < room ? sl : room;
    if (copy) std::memcpy(e.mem().host<u8>(d + dl), e.mem().host<u8>(s), copy);
    e.mem().write8(d + dl + copy, 0);
  }
  e.ret(dl + sl);
}

// ------------------------------------------------------------- conversions
void t_atoi(Env& e) {
  Env::Args a(e);
  e.ret(static_cast<u32>(std::atoi(e.mem().str(a.next32()).c_str())));
}

void t_strtol(Env& e) {
  Env::Args a(e);
  u32 s = a.next32(), endp = a.next32();
  int base = static_cast<int>(a.next32());
  const char* start = e.mem().host<char>(s);
  char* end = nullptr;
  long v = std::strtol(start, &end, base);
  if (endp) e.mem().write32(endp, s + static_cast<u32>(end - start));
  e.ret(static_cast<u32>(v));
}

void t_strtoul(Env& e) {
  Env::Args a(e);
  u32 s = a.next32(), endp = a.next32();
  int base = static_cast<int>(a.next32());
  const char* start = e.mem().host<char>(s);
  char* end = nullptr;
  unsigned long v = std::strtoul(start, &end, base);
  if (endp) e.mem().write32(endp, s + static_cast<u32>(end - start));
  e.ret(static_cast<u32>(v));
}

void t_strtod(Env& e) {
  Env::Args a(e);
  u32 s = a.next32(), endp = a.next32();
  const char* start = e.mem().host<char>(s);
  char* end = nullptr;
  double v = std::strtod(start, &end);
  if (endp) e.mem().write32(endp, s + static_cast<u32>(end - start));
  u64 bits;
  std::memcpy(&bits, &v, 8);
  e.ret64(bits);
}

// -------------------------------------------------------- ctype / wide char
void t_tolower(Env& e) {
  Env::Args a(e);
  e.ret(static_cast<u32>(std::tolower(static_cast<int>(a.next32()))));
}
void t_isalnum(Env& e) {
  Env::Args a(e);
  e.ret(std::isalnum(static_cast<int>(a.next32())) ? 1 : 0);
}
void t_isalpha(Env& e) {
  Env::Args a(e);
  e.ret(std::isalpha(static_cast<int>(a.next32())) ? 1 : 0);
}
void t_isspace(Env& e) {
  Env::Args a(e);
  e.ret(std::isspace(static_cast<int>(a.next32())) ? 1 : 0);
}
void t_towlower(Env& e) {
  Env::Args a(e);
  e.ret(static_cast<u32>(std::towlower(a.next32())));
}
void t_towupper(Env& e) {
  Env::Args a(e);
  e.ret(static_cast<u32>(std::towupper(a.next32())));
}
void t_btowc(Env& e) {
  Env::Args a(e);
  u32 c = a.next32();
  e.ret(c == 0xFFFFFFFF ? 0xFFFFFFFF : (c & 0xFF));
}
void t_wctob(Env& e) {
  Env::Args a(e);
  u32 c = a.next32();
  e.ret(c < 0x100 ? c : 0xFFFFFFFF);
}

// wctype/iswctype: the engine only ever asks for the standard class names, so
// map them to small tokens and interpret those rather than plumbing locales.
enum WType { kNone = 0, kAlnum, kAlpha, kSpace, kDigit, kUpper, kLower, kPunct };

void t_wctype(Env& e) {
  Env::Args a(e);
  std::string name = e.mem().str(a.next32());
  u32 t = kNone;
  if (name == "alnum") t = kAlnum;
  else if (name == "alpha") t = kAlpha;
  else if (name == "space") t = kSpace;
  else if (name == "digit") t = kDigit;
  else if (name == "upper") t = kUpper;
  else if (name == "lower") t = kLower;
  else if (name == "punct") t = kPunct;
  e.ret(t);
}

void t_iswctype(Env& e) {
  Env::Args a(e);
  std::wint_t c = static_cast<std::wint_t>(a.next32());
  u32 t = a.next32();
  int r = 0;
  switch (t) {
    case kAlnum: r = std::iswalnum(c); break;
    case kAlpha: r = std::iswalpha(c); break;
    case kSpace: r = std::iswspace(c); break;
    case kDigit: r = std::iswdigit(c); break;
    case kUpper: r = std::iswupper(c); break;
    case kLower: r = std::iswlower(c); break;
    case kPunct: r = std::iswpunct(c); break;
    default: r = 0;
  }
  e.ret(r ? 1 : 0);
}

void t_setlocale(Env& e) {
  Env::Args a(e);
  a.next32();
  a.next32();
  e.ret(0);   // null: "locale unchanged", which the engine tolerates
}

void t_wcslen(Env& e) {
  Env::Args a(e);
  u32 p = a.next32(), n = 0;
  while (e.mem().read32(p + n * 4)) ++n;
  e.ret(n);
}

// ------------------------------------------------------------------- stdio
//
// __sF is the bionic FILE array. The engine derives stdout as &__sF[1], so a
// pointer is turned back into a stream by its offset from the array base.
u32 g_sF_base = 0;

std::FILE* stream_for(u32 guest_file) {
  if (!g_sF_base || guest_file < g_sF_base) return nullptr;
  u32 index = (guest_file - g_sF_base) / kBionicFileSize;
  switch (index) {
    case 1: return stdout;
    case 2: return stderr;
    default: return nullptr;
  }
}

void t_fwrite(Env& e) {
  Env::Args a(e);
  u32 p = a.next32(), size = a.next32(), count = a.next32(), f = a.next32();
  std::FILE* out = stream_for(f);
  u64 total = u64(size) * count;
  if (out && total) std::fwrite(e.mem().host<u8>(p), 1, size_t(total), out);
  e.ret(count);
}

void t_fputs(Env& e) {
  Env::Args a(e);
  u32 p = a.next32(), f = a.next32();
  std::FILE* out = stream_for(f);
  if (out) std::fputs(e.mem().str(p, 1 << 16).c_str(), out);
  e.ret(0);
}

void t_fputc(Env& e) {
  Env::Args a(e);
  u32 c = a.next32(), f = a.next32();
  std::FILE* out = stream_for(f);
  if (out) std::fputc(static_cast<int>(c), out);
  e.ret(c);
}

void t_fflush(Env& e) {
  Env::Args a(e);
  std::FILE* out = stream_for(a.next32());
  if (out) std::fflush(out);
  e.ret(0);
}

// printf-family: the format string and its arguments live guest-side and the
// variadic ABI differs, so this deliberately does not interpret them. It
// prints the literal format, which is enough to see what the engine is
// complaining about during bring-up.
void t_printf_like(Env& e, const char* who, bool has_stream) {
  Env::Args a(e);
  if (has_stream) a.next32();
  std::string fmt = e.mem().str(a.next32(), 1 << 12);
  std::printf("[guest %s] %s\n", who, fmt.c_str());
  e.ret(static_cast<u32>(fmt.size()));
}

void t_printf(Env& e) { t_printf_like(e, "printf", false); }
void t_fprintf(Env& e) { t_printf_like(e, "fprintf", true); }

// ------------------------------------------------------------------ runtime
void t_cxa_atexit(Env& e) {
  Env::Args a(e);
  a.next32();
  a.next32();
  a.next32();
  e.ret(0);   // destructors at exit are not interesting here
}

void t_cxa_finalize(Env& e) {
  Env::Args a(e);
  a.next32();
  e.ret(0);
}

void t_errno_location(Env& e) { e.ret(e.errno_addr()); }

void t_abort(Env& e) { e.fatal("abort()"); }

void t_exit(Env& e) {
  Env::Args a(e);
  u32 code = a.next32();
  char buf[64];
  std::snprintf(buf, sizeof(buf), "exit(%u)", code);
  e.fatal(buf);
}

void t_assert2(Env& e) {
  Env::Args a(e);
  std::string file = e.mem().str(a.next32());
  u32 line = a.next32();
  std::string fn = e.mem().str(a.next32());
  std::string msg = e.mem().str(a.next32());
  std::printf("[guest assert] %s:%u %s: %s\n", file.c_str(), line, fn.c_str(),
              msg.c_str());
  e.fatal("assertion failed in guest code");
}

void t_stack_chk_fail(Env& e) { e.fatal("stack smashing detected"); }

// pthread_once(control, routine): tail-call the routine so it runs as guest
// code, returning straight to whoever called pthread_once.
void t_pthread_once(Env& e) {
  auto& regs = e.jit()->Regs();
  u32 control = regs[0];
  u32 routine = regs[1];
  u32 lr = regs[14];
  if (e.mem().read32(control) != 0) {
    e.ret(0);
    return;
  }
  e.mem().write32(control, 1);
  e.redirect(routine, lr);
}

}  // namespace

// The loader hands out storage for the four imported data objects; this fills
// them with values the engine can actually use.
void init_libc_data(Env& e, const std::map<std::string, DataImport>& data) {
  g_heap.init(e.mem());

  for (const auto& [name, d] : data) {
    if (name == "__sF") {
      g_sF_base = d.addr;
      fs_set_stdio_base(d.addr);
      e.mem().zero(d.addr, d.size);
    } else if (name == "__stack_chk_guard") {
      e.mem().write32(d.addr, 0xDEADC0DE);
    } else if (name == "_ctype_") {
      // BSD-style table, indexed [c + 1]. Bit values follow bionic's ctype.h.
      constexpr u8 kU = 0x01, kL = 0x02, kN = 0x04, kS = 0x08, kP = 0x10,
                   kC = 0x20, kX = 0x40, kB = 0x80;
      e.mem().write8(d.addr, 0);   // the EOF slot
      for (int c = 0; c < 256; ++c) {
        u8 f = 0;
        if (std::isupper(c)) f |= kU;
        if (std::islower(c)) f |= kL;
        if (std::isdigit(c)) f |= kN;
        if (std::isspace(c)) f |= kS;
        if (std::ispunct(c)) f |= kP;
        if (std::iscntrl(c)) f |= kC;
        if (std::isxdigit(c)) f |= kX;
        if (c == ' ') f |= kB;
        e.mem().write8(d.addr + 1 + c, f);
      }
    } else if (name == "_tolower_tab_") {
      e.mem().write16(d.addr, 0xFFFF);   // EOF maps to itself
      for (int c = 0; c < 256; ++c)
        e.mem().write16(d.addr + 2 + c * 2,
                        static_cast<u16>(std::tolower(c)));
    }
  }

  // errno needs somewhere to live; park it just past the data imports.
  e.set_errno_addr(layout::kDataImports + layout::kDataSize - 16);
  e.mem().write32(e.errno_addr(), 0);
}

const ThunkEntry kLibcTable[] = {
    {"malloc", &t_malloc},
    {"free", &t_free},
    {"calloc", &t_calloc},
    {"realloc", &t_realloc},

    {"memcpy", &t_memcpy},
    {"memmove", &t_memmove},
    {"memset", &t_memset},
    {"memcmp", &t_memcmp},
    {"memchr", &t_memchr},

    {"strlen", &t_strlen},
    {"strcmp", &t_strcmp},
    {"strncmp", &t_strncmp},
    {"strcasecmp", &t_strcasecmp},
    {"strncasecmp", &t_strncasecmp},
    {"strcpy", &t_strcpy},
    {"strncpy", &t_strncpy},
    {"strcat", &t_strcat},
    {"strncat", &t_strncat},
    {"strchr", &t_strchr},
    {"strrchr", &t_strrchr},
    {"strstr", &t_strstr},
    {"strdup", &t_strdup},
    {"strlcat", &t_strlcat},

    {"atoi", &t_atoi},
    {"strtol", &t_strtol},
    {"strtoul", &t_strtoul},
    {"strtod", &t_strtod},

    {"tolower", &t_tolower},
    {"isalnum", &t_isalnum},
    {"isalpha", &t_isalpha},
    {"isspace", &t_isspace},
    {"towlower", &t_towlower},
    {"towupper", &t_towupper},
    {"btowc", &t_btowc},
    {"wctob", &t_wctob},
    {"wctype", &t_wctype},
    {"iswctype", &t_iswctype},
    {"setlocale", &t_setlocale},
    {"wcslen", &t_wcslen},

    {"fwrite", &t_fwrite},
    {"fputs", &t_fputs},
    {"fputc", &t_fputc},
    {"fflush", &t_fflush},
    {"printf", &t_printf},
    {"fprintf", &t_fprintf},

    {"__cxa_atexit", &t_cxa_atexit},
    {"__cxa_finalize", &t_cxa_finalize},
    {"__errno", &t_errno_location},
    {"abort", &t_abort},
    {"exit", &t_exit},
    {"__assert2", &t_assert2},
    {"__stack_chk_fail", &t_stack_chk_fail},
    {"pthread_once", &t_pthread_once},
};

const std::size_t kLibcTableSize = sizeof(kLibcTable) / sizeof(kLibcTable[0]);

}  // namespace wb
