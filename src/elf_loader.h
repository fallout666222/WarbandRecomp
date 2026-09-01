// Loads libMBExpMobile.so into the guest address space.
//
// Verified against the real binary by tools/loader_ref.py, which reports:
//   two PT_LOAD segments (r-x 3714364 bytes, rw- 35908 + 37438968 zero-fill)
//   6587 relocations, only four kinds: RELATIVE 6200, JUMP_SLOT 369,
//                                      GLOB_DAT 13, ABS32 5
//   no PT_TLS, no DT_TEXTREL, DT_SYMBOLIC set
//   377 imports (373 functions + 4 data objects), 4406 exports
//   95 init_array entries to run before android_main
#pragma once

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "guest.h"

namespace wb {

struct DataImport {
  u32 addr;
  u32 size;
};

// sizeof(struct __sFILE) in 32-bit bionic. The engine reaches stdout as
// &__sF[1], so this stride is part of the ABI contract with the libc thunks.
constexpr u32 kBionicFileSize = 84;

class ElfLoader {
 public:
  // `resolve_guest` is consulted first: it maps a symbol name to an address
  // already present in guest memory (the guest libc). Anything it declines
  // gets a host trampoline instead. This is what keeps 156 of the 377
  // imports off the host boundary entirely.
  using GuestResolver = std::function<bool(const std::string&, u32*)>;

  ElfLoader(Memory& mem) : mem_(mem) {}

  bool load(const std::string& path, std::string* error);

  void set_guest_resolver(GuestResolver r) { resolve_guest_ = std::move(r); }

  // Symbol name for a trampoline index, as assigned during load.
  const std::string& thunk_name(u32 index) const { return thunk_names_[index]; }
  u32 thunk_count() const { return static_cast<u32>(thunk_names_.size()); }

  u32 lookup(const std::string& name) const;   // 0 if absent

  // Nearest exported symbol at or below `addr`, formatted "name+0x…".
  // The binary is not stripped, so this resolves almost anywhere in .text -
  // which is the difference between "stuck at 0x004F1C20" and knowing which
  // engine function is spinning.
  std::string symbolize(u32 addr) const;

  // The single executable segment. Anything outside it (plus the trampoline
  // block) is not code, and treating it as such turns a wild jump into an
  // immediate fault instead of the recompiler grinding through zero pages.
  u32 exec_begin() const { return exec_begin_; }
  u32 exec_end() const { return exec_end_; }

  // Where the trampoline bump allocator stopped, so the Android glue can keep
  // carving executable stubs out of the same block.
  u32 trampoline_next() const { return tramp_next_; }
  const std::vector<u32>& init_array() const { return init_array_; }
  u32 android_main() const { return lookup("android_main"); }

  // Data objects the engine imports but the guest libc must own. Sizes are
  // not declared in the ELF, so the guest libc has to agree with these.
  const std::map<std::string, DataImport>& data_imports() const {
    return data_imports_;
  }

 private:
  struct Sym {
    std::string name;
    u32 value, size;
    u8 type, bind;
    u16 shndx;
  };

  bool parse_headers(std::string* error);
  bool map_segments(std::string* error);
  bool parse_dynamic(std::string* error);
  bool bind_symbols(std::string* error);
  bool apply_relocations(std::string* error);

  Sym symbol(u32 index) const;
  const char* dynstr(u32 offset) const;
  u32 file_offset(u32 vaddr, std::string* error) const;
  bool resolve(u32 sym_index, u32* out, std::string* error);

  Memory& mem_;
  GuestResolver resolve_guest_;
  std::vector<u8> raw_;

  struct Phdr {
    u32 type, off, vaddr, filesz, memsz, flags, align;
  };
  std::vector<Phdr> phdrs_;

  u32 bias_ = layout::kImageBase;
  u32 exec_begin_ = 0, exec_end_ = 0;
  u32 tramp_next_ = 0;
  std::unordered_map<u32, u32> dynamic_;   // DT tag -> value
  u32 strtab_off_ = 0, symtab_off_ = 0, syment_ = 16, nsym_ = 0;

  std::unordered_map<std::string, u32> exports_;
  std::vector<std::pair<u32, std::string>> sorted_exports_;   // by address
  std::unordered_map<std::string, u32> imports_;   // name -> guest address
  std::vector<std::string> thunk_names_;           // index -> name
  std::map<std::string, DataImport> data_imports_;
  std::vector<u32> init_array_;
};

}  // namespace wb
