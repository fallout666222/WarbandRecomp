// Imports answered with guest ARM32 code rather than a host thunk.
//
// This is the first piece of the guest-side runtime the inventory always
// called for. It exists because measurement demanded it: integer division was
// the single hottest thing crossing the boundary, by an order of magnitude,
// and it never needed to cross at all.
#pragma once

#include <initializer_list>
#include <string>
#include <unordered_map>

#include "guest.h"

namespace wb {

class GuestCode {
 public:
  explicit GuestCode(Memory& mem);

  // Writes the routines into guest memory. Call before loading the engine,
  // so the loader can resolve imports against them.
  void build();

  // Starts the host thread that keeps the guest-visible clock current. The
  // clock readers build() emitted read that page, so this has to be running
  // before any guest code does.
  void start_clock();
  void stop_clock();

  // The loader's guest resolver: true if this name is answered by guest code.
  bool resolve(const std::string& name, u32* out) const;

  // Runs each routine against the host's own answer. Hand-written machine
  // code with no test is a liability: a wrong floorf would not crash, it
  // would quietly make the engine compute nonsense, and the nonsense would
  // surface somewhere else entirely.
  bool self_test(class Env& e) const;

  u32 end() const { return next_; }

 private:
  u32 emit(const char* name, std::initializer_list<u32> words);
  void build_clock();
  void build_maths();
  // memset, strlen, strcmp, memcmp. The boundary's biggest bill by call
  // count, and none of it work the host does better.
  void build_strings();

  Memory& mem_;
  u32 next_;
  std::unordered_map<std::string, u32> routines_;
};

}  // namespace wb
