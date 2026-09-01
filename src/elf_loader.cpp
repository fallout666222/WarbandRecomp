#include "elf_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wb {
namespace {

constexpr u32 PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7;

constexpr u32 DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_STRTAB = 5, DT_SYMTAB = 6,
              DT_SYMENT = 11, DT_INIT = 12, DT_TEXTREL = 22, DT_JMPREL = 23,
              DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27, DT_REL = 17,
              DT_RELSZ = 18;

constexpr u32 R_ARM_ABS32 = 2, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22,
              R_ARM_RELATIVE = 23;

constexpr u8 STT_OBJECT = 1;
constexpr u8 STB_WEAK = 2;

// Two ARM instructions. The svc immediate is the thunk index; the JIT traps
// on it and hands control to the host table. `bx lr` then returns to the
// engine as if an ordinary function had been called.
constexpr u32 kSvcAl = 0xEF000000;
constexpr u32 kBxLr = 0xE12FFF1E;

template <typename T>
T fetch(const std::vector<u8>& b, size_t off) {
  T v{};
  std::memcpy(&v, b.data() + off, sizeof(T));
  return v;
}

// Bionic declares these without a size, so the loader has to know how big
// they are. The values follow 32-bit bionic; the libc thunks fill them in and
// must agree. Getting __sF wrong is the dangerous one - the engine derives
// stdin/stdout/stderr as &__sF[0..2], so the stride has to match.
u32 known_data_size(const std::string& name) {
  if (name == "__sF") return 3 * kBionicFileSize;
  if (name == "_ctype_") return 1 + 256;          // indexed [c + 1]
  if (name == "_tolower_tab_") return (1 + 256) * 2;   // shorts
  if (name == "__stack_chk_guard") return 4;
  return 4;
}

}  // namespace

const char* ElfLoader::dynstr(u32 offset) const {
  return reinterpret_cast<const char*>(raw_.data() + strtab_off_ + offset);
}

u32 ElfLoader::file_offset(u32 vaddr, std::string* error) const {
  for (const auto& p : phdrs_) {
    if (p.type == PT_LOAD && vaddr >= p.vaddr && vaddr < p.vaddr + p.filesz)
      return p.off + (vaddr - p.vaddr);
  }
  if (error) *error = "virtual address outside any loadable segment";
  return 0;
}

ElfLoader::Sym ElfLoader::symbol(u32 index) const {
  size_t o = symtab_off_ + size_t(index) * syment_;
  Sym s;
  u32 nm = fetch<u32>(raw_, o);
  s.value = fetch<u32>(raw_, o + 4);
  s.size = fetch<u32>(raw_, o + 8);
  u8 info = raw_[o + 12];
  s.type = info & 0xF;
  s.bind = info >> 4;
  s.shndx = fetch<u16>(raw_, o + 14);
  s.name = nm ? dynstr(nm) : std::string();
  return s;
}

bool ElfLoader::load(const std::string& path, std::string* error) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    *error = "cannot open " + path;
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  raw_.resize(size_t(n));
  size_t got = std::fread(raw_.data(), 1, raw_.size(), f);
  std::fclose(f);
  if (got != raw_.size()) {
    *error = "short read";
    return false;
  }
  return parse_headers(error) && map_segments(error) && parse_dynamic(error) &&
         bind_symbols(error) && apply_relocations(error);
}

bool ElfLoader::parse_headers(std::string* error) {
  if (raw_.size() < 52 || std::memcmp(raw_.data(), "\x7f" "ELF", 4) != 0) {
    *error = "not an ELF file";
    return false;
  }
  if (raw_[4] != 1 || raw_[5] != 1) {
    *error = "expected a 32-bit little-endian object";
    return false;
  }
  if (fetch<u16>(raw_, 18) != 40) {
    *error = "expected EM_ARM";
    return false;
  }
  u32 phoff = fetch<u32>(raw_, 28);
  u16 phentsize = fetch<u16>(raw_, 42);
  u16 phnum = fetch<u16>(raw_, 44);
  for (u16 i = 0; i < phnum; ++i) {
    size_t o = phoff + size_t(i) * phentsize;
    Phdr p;
    p.type = fetch<u32>(raw_, o);
    p.off = fetch<u32>(raw_, o + 4);
    p.vaddr = fetch<u32>(raw_, o + 8);
    p.filesz = fetch<u32>(raw_, o + 16);
    p.memsz = fetch<u32>(raw_, o + 20);
    p.flags = fetch<u32>(raw_, o + 24);
    p.align = fetch<u32>(raw_, o + 28);
    phdrs_.push_back(p);
    if (p.type == PT_TLS) {
      // The 2014 build has none. If a future binary does, the guest libc
      // needs a TLS block set up before any constructor runs.
      *error = "PT_TLS present - guest TLS is not implemented";
      return false;
    }
  }
  return true;
}

bool ElfLoader::map_segments(std::string* error) {
  u32 hi = 0;
  for (const auto& p : phdrs_) {
    if (p.type != PT_LOAD) continue;
    u32 dst = bias_ + p.vaddr;
    if (!mem_.valid(dst, p.memsz)) {
      *error = "segment does not fit the guest address space";
      return false;
    }
    mem_.copy_in(dst, raw_.data() + p.off, p.filesz);
    if (p.memsz > p.filesz) mem_.zero(dst + p.filesz, p.memsz - p.filesz);
    hi = std::max(hi, p.vaddr + p.memsz);
    if (p.flags & 1) {          // PF_X
      exec_begin_ = dst;
      exec_end_ = dst + p.memsz;
    }
  }
  if (bias_ + hi > layout::kImageLimit) {
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "image needs 0x%08X and would end at 0x%08X, but the region "
                  "reserved for it stops at 0x%08X (short by %u bytes)",
                  hi, bias_ + hi, layout::kImageLimit,
                  bias_ + hi - layout::kImageLimit);
    *error = buf;
    return false;
  }
  return true;
}

bool ElfLoader::parse_dynamic(std::string* error) {
  const Phdr* dyn = nullptr;
  for (const auto& p : phdrs_)
    if (p.type == PT_DYNAMIC) dyn = &p;
  if (!dyn) {
    *error = "no PT_DYNAMIC";
    return false;
  }
  for (u32 i = 0; i < dyn->filesz / 8; ++i) {
    u32 tag = fetch<u32>(raw_, dyn->off + size_t(i) * 8);
    u32 val = fetch<u32>(raw_, dyn->off + size_t(i) * 8 + 4);
    if (tag == 0) break;
    if (tag == DT_NEEDED) continue;   // resolution is ours, not the linker's
    if (tag == DT_TEXTREL) {
      *error = "DT_TEXTREL - text relocations are not supported";
      return false;
    }
    dynamic_[tag] = val;
  }
  if (!dynamic_.count(DT_STRTAB) || !dynamic_.count(DT_SYMTAB)) {
    *error = "missing DT_STRTAB or DT_SYMTAB";
    return false;
  }
  strtab_off_ = file_offset(dynamic_[DT_STRTAB], error);
  symtab_off_ = file_offset(dynamic_[DT_SYMTAB], error);
  if (dynamic_.count(DT_SYMENT)) syment_ = dynamic_[DT_SYMENT];
  // Android lays STRTAB directly after SYMTAB, so the gap gives the count.
  nsym_ = (dynamic_[DT_STRTAB] - dynamic_[DT_SYMTAB]) / syment_;

  if (dynamic_.count(DT_INIT_ARRAY)) {
    u32 base = bias_ + dynamic_[DT_INIT_ARRAY];
    u32 count = dynamic_[DT_INIT_ARRAYSZ] / 4;
    for (u32 i = 0; i < count; ++i) init_array_.push_back(base + i * 4);
  }
  return true;
}

bool ElfLoader::bind_symbols(std::string* error) {
  BumpAllocator tramp(layout::kTrampolines,
                      layout::kTrampolines + layout::kTrampSize);
  BumpAllocator data(layout::kDataImports,
                     layout::kDataImports + layout::kDataSize);

  for (u32 i = 0; i < nsym_; ++i) {
    Sym s = symbol(i);
    if (s.name.empty()) continue;

    if (s.shndx != 0) {
      exports_.emplace(s.name, bias_ + s.value);
      continue;
    }
    if (imports_.count(s.name)) continue;

    // 1. the guest libc, if it has this symbol
    u32 guest_addr = 0;
    if (resolve_guest_ && resolve_guest_(s.name, &guest_addr)) {
      imports_[s.name] = guest_addr;
      continue;
    }
    // 2. an imported data object needs storage, not a trampoline
    if (s.type == STT_OBJECT) {
      u32 size = s.size ? s.size : known_data_size(s.name);
      u32 addr = data.alloc(size);
      if (!addr) {
        *error = "out of data-import storage";
        return false;
      }
      data_imports_[s.name] = DataImport{addr, size};
      imports_[s.name] = addr;
      continue;
    }
    // 3. otherwise a host thunk
    u32 index = static_cast<u32>(thunk_names_.size());
    u32 addr = tramp.alloc(8, 8);
    if (!addr) {
      *error = "out of trampoline space";
      return false;
    }
    mem_.write32(addr, kSvcAl | (index & 0x00FFFFFF));
    mem_.write32(addr + 4, kBxLr);
    thunk_names_.push_back(s.name);
    imports_[s.name] = addr;
  }

  tramp_next_ = tramp.used();

  sorted_exports_.reserve(exports_.size());
  for (const auto& [name, addr] : exports_)
    sorted_exports_.emplace_back(addr, name);
  std::sort(sorted_exports_.begin(), sorted_exports_.end());
  return true;
}

bool ElfLoader::resolve(u32 sym_index, u32* out, std::string* error) {
  Sym s = symbol(sym_index);
  if (s.shndx != 0) {
    *out = bias_ + s.value;
    return true;
  }
  auto it = imports_.find(s.name);
  if (it != imports_.end()) {
    *out = it->second;
    return true;
  }
  if (s.bind == STB_WEAK) {
    *out = 0;   // an unresolved weak symbol is legitimately null
    return true;
  }
  *error = "unresolved symbol: " + s.name;
  return false;
}

bool ElfLoader::apply_relocations(std::string* error) {
  struct Block {
    u32 vaddr, size;
  };
  std::vector<Block> blocks;
  if (dynamic_.count(DT_REL))
    blocks.push_back({dynamic_[DT_REL], dynamic_[DT_RELSZ]});
  if (dynamic_.count(DT_JMPREL))
    blocks.push_back({dynamic_[DT_JMPREL], dynamic_[DT_PLTRELSZ]});

  for (const auto& b : blocks) {
    u32 off = file_offset(b.vaddr, error);
    for (u32 i = 0; i < b.size / 8; ++i) {
      u32 r_offset = fetch<u32>(raw_, off + size_t(i) * 8);
      u32 r_info = fetch<u32>(raw_, off + size_t(i) * 8 + 4);
      u32 type = r_info & 0xFF;
      u32 sym = r_info >> 8;
      u32 where = bias_ + r_offset;

      switch (type) {
        case 0:
          break;
        case R_ARM_RELATIVE:
          mem_.write32(where, mem_.read32(where) + bias_);
          break;
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT: {
          u32 v;
          if (!resolve(sym, &v, error)) return false;
          mem_.write32(where, v);
          break;
        }
        case R_ARM_ABS32: {
          u32 v;
          if (!resolve(sym, &v, error)) return false;
          mem_.write32(where, mem_.read32(where) + v);   // addend in place
          break;
        }
        default:
          *error = "unhandled relocation type " + std::to_string(type);
          return false;
      }
    }
  }
  return true;
}

u32 ElfLoader::lookup(const std::string& name) const {
  auto it = exports_.find(name);
  return it == exports_.end() ? 0 : it->second;
}

std::string ElfLoader::symbolize(u32 addr) const {
  // A thread parked inside an import shows a trampoline address, and matching
  // it against the export table gives a meaningless "_end+0x8B2B28". Naming
  // the import instead is the difference between a stuck thread and a stuck
  // thread you can do something about.
  if (addr >= layout::kTrampolines &&
      addr < layout::kTrampolines + layout::kTrampSize) {
    const u32 index = (addr - layout::kTrampolines) / 8;
    if (index < thunk_names_.size())
      return "import " + thunk_names_[index];
  }
  if (addr >= layout::kReturnStub && addr < layout::kReturnStub + 8)
    return "return stub";
  if (addr >= layout::kGuestCodeBase &&
      addr < layout::kGuestCodeBase + layout::kGuestCodeSize)
    return "guest-side import routine";
  if (sorted_exports_.empty()) return "?";
  auto it = std::upper_bound(
      sorted_exports_.begin(), sorted_exports_.end(), addr,
      [](u32 a, const std::pair<u32, std::string>& e) { return a < e.first; });
  if (it == sorted_exports_.begin()) return "?";
  --it;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "+0x%X", addr - it->first);
  return it->second + buf;
}

}  // namespace wb
