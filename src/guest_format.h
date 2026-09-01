// printf-family formatting for guest code.
//
// The engine formats constantly - error text, resource paths, shader names -
// so this is not a logging convenience. `vsnprintf` returning zero leaves the
// caller with an uninitialised buffer, and the damage shows up much later as
// a file that cannot be found or a technique that does not exist.
//
// Two argument sources, one formatter: the variadic entry points read from
// registers-then-stack per AAPCS, while the v-forms read from a va_list,
// which on ARM32 is simply a pointer into the caller's argument area.
#pragma once

#include <string>

#include "env.h"

namespace wb {

class ArgSource {
 public:
  virtual ~ArgSource() = default;
  virtual u32 next32() = 0;
  virtual u64 next64() = 0;
};

// Variadic call: the reader Env already knows how to walk.
class RegArgs final : public ArgSource {
 public:
  explicit RegArgs(Env::Args& a) : a_(a) {}
  u32 next32() override { return a_.next32(); }
  u64 next64() override { return a_.next64(); }

 private:
  Env::Args& a_;
};

// va_list form: a guest pointer walked forward, with 64-bit values aligned to
// 8 bytes the way AAPCS requires.
class VaListArgs final : public ArgSource {
 public:
  VaListArgs(Memory& mem, u32 ap) : mem_(mem), at_(ap) {}
  u32 next32() override {
    u32 v = mem_.read32(at_);
    at_ += 4;
    return v;
  }
  u64 next64() override {
    at_ = (at_ + 7) & ~7u;
    u64 v = mem_.read64(at_);
    at_ += 8;
    return v;
  }

 private:
  Memory& mem_;
  u32 at_;
};

// Formats `fmt` against `args`, reading any %s operands out of guest memory.
std::string guest_format(Env& e, ArgSource& args, const std::string& fmt);

}  // namespace wb
