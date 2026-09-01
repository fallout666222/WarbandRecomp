// ARM EABI helpers and clocks.
//
// The EABI division and conversion helpers have their own return conventions,
// which is the whole reason they need hand-written thunks rather than a
// generated stub: __aeabi_uldivmod hands back a quotient in r0:r1 *and* a
// remainder in r2:r3, and nothing in the generic machinery would know that.
//
// These are pure arithmetic, so they are prime candidates for moving into a
// guest ARM32 runtime later - every call is a boundary crossing today. They
// are here because the render thread hits them immediately and a stub
// returning zero silently produces wrong numbers rather than a visible fault.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "env.h"

namespace wb {
namespace {

double as_double(u64 bits) {
  double d;
  std::memcpy(&d, &bits, 8);
  return d;
}
u64 from_double(double d) {
  u64 bits;
  std::memcpy(&bits, &d, 8);
  return bits;
}
float as_float(u32 bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
u32 from_float(float f) {
  u32 bits;
  std::memcpy(&bits, &f, 4);
  return bits;
}

// r0:r1 = quotient, r2:r3 = remainder
void set_divmod64(Env& e, u64 quot, u64 rem) {
  auto& r = e.jit()->Regs();
  r[0] = static_cast<u32>(quot);
  r[1] = static_cast<u32>(quot >> 32);
  r[2] = static_cast<u32>(rem);
  r[3] = static_cast<u32>(rem >> 32);
}

// ------------------------------------------------------------- division
void t_idiv(Env& e) {
  Env::Args a(e);
  std::int32_t n = static_cast<std::int32_t>(a.next32());
  std::int32_t d = static_cast<std::int32_t>(a.next32());
  e.ret(d ? static_cast<u32>(n / d) : 0);
}

void t_uidiv(Env& e) {
  Env::Args a(e);
  u32 n = a.next32(), d = a.next32();
  e.ret(d ? n / d : 0);
}

void t_idivmod(Env& e) {
  Env::Args a(e);
  std::int32_t n = static_cast<std::int32_t>(a.next32());
  std::int32_t d = static_cast<std::int32_t>(a.next32());
  auto& r = e.jit()->Regs();
  r[0] = d ? static_cast<u32>(n / d) : 0;
  r[1] = d ? static_cast<u32>(n % d) : 0;
}

void t_uidivmod(Env& e) {
  Env::Args a(e);
  u32 n = a.next32(), d = a.next32();
  auto& r = e.jit()->Regs();
  r[0] = d ? n / d : 0;
  r[1] = d ? n % d : 0;
}

void t_ldivmod(Env& e) {
  Env::Args a(e);
  std::int64_t n = static_cast<std::int64_t>(a.next64());
  std::int64_t d = static_cast<std::int64_t>(a.next64());
  if (!d) {
    set_divmod64(e, 0, 0);
    return;
  }
  set_divmod64(e, static_cast<u64>(n / d), static_cast<u64>(n % d));
}

void t_uldivmod(Env& e) {
  Env::Args a(e);
  u64 n = a.next64(), d = a.next64();
  if (!d) {
    set_divmod64(e, 0, 0);
    return;
  }
  set_divmod64(e, n / d, n % d);
}

// ------------------------------------------------------ double arithmetic
void t_dadd(Env& e) {
  Env::Args a(e);
  double x = as_double(a.next64()), y = as_double(a.next64());
  e.ret64(from_double(x + y));
}

void t_ddiv(Env& e) {
  Env::Args a(e);
  double x = as_double(a.next64()), y = as_double(a.next64());
  e.ret64(from_double(x / y));
}

void t_dcmpgt(Env& e) {
  Env::Args a(e);
  double x = as_double(a.next64()), y = as_double(a.next64());
  e.ret(x > y ? 1 : 0);
}

// ----------------------------------------------------------- conversions
void t_i2d(Env& e) {
  Env::Args a(e);
  e.ret64(from_double(static_cast<double>(static_cast<std::int32_t>(a.next32()))));
}

void t_l2d(Env& e) {
  Env::Args a(e);
  e.ret64(from_double(static_cast<double>(static_cast<std::int64_t>(a.next64()))));
}

void t_l2f(Env& e) {
  Env::Args a(e);
  e.ret(from_float(static_cast<float>(static_cast<std::int64_t>(a.next64()))));
}

void t_ul2f(Env& e) {
  Env::Args a(e);
  e.ret(from_float(static_cast<float>(a.next64())));
}

void t_d2lz(Env& e) {
  Env::Args a(e);
  e.ret64(static_cast<u64>(static_cast<std::int64_t>(as_double(a.next64()))));
}

void t_f2lz(Env& e) {
  Env::Args a(e);
  e.ret64(static_cast<u64>(static_cast<std::int64_t>(as_float(a.next32()))));
}

// ----------------------------------------------------------------- clocks
//
// The engine measures frame time, so this has to advance monotonically and at
// roughly the right rate; it does not have to match wall clock. The base is
// shared with the condition-variable code through guest_monotonic_ns(), so a
// deadline the guest computed here means the same thing when it is waited on.
std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();

std::int64_t nanos_since_start() { return guest_monotonic_ns(); }

void t_clock_gettime(Env& e) {
  Env::Args a(e);
  a.next32();                       // clock id: monotonic either way here
  u32 ts = a.next32();
  std::int64_t ns = nanos_since_start();
  if (ts) {
    e.mem().write32(ts, static_cast<u32>(ns / 1'000'000'000));
    e.mem().write32(ts + 4, static_cast<u32>(ns % 1'000'000'000));
  }
  e.ret(0);
}

void t_gettimeofday(Env& e) {
  Env::Args a(e);
  u32 tv = a.next32();
  a.next32();                       // timezone: obsolete, always ignored
  std::int64_t ns = nanos_since_start();
  if (tv) {
    e.mem().write32(tv, static_cast<u32>(ns / 1'000'000'000));
    e.mem().write32(tv + 4, static_cast<u32>((ns / 1000) % 1'000'000));
  }
  e.ret(0);
}

void t_time(Env& e) {
  Env::Args a(e);
  u32 out = a.next32();
  u32 secs = static_cast<u32>(nanos_since_start() / 1'000'000'000);
  if (out) e.mem().write32(out, secs);
  e.ret(secs);
}

void t_usleep(Env& e) {
  Env::Args a(e);
  u32 us = a.next32();
  std::this_thread::sleep_for(std::chrono::microseconds(us));
  e.ret(0);
}

void t_nv_android_init(Env& e) { e.ret(0); }

}  // namespace

std::int64_t guest_monotonic_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now() - start_time)
      .count();
}

const ThunkEntry kRuntimeTable[] = {
    {"__aeabi_idiv", &t_idiv},
    {"__aeabi_uidiv", &t_uidiv},
    {"__aeabi_idivmod", &t_idivmod},
    {"__aeabi_uidivmod", &t_uidivmod},
    {"__aeabi_ldivmod", &t_ldivmod},
    {"__aeabi_uldivmod", &t_uldivmod},
    {"__gnu_ldivmod_helper", &t_ldivmod},
    {"__gnu_uldivmod_helper", &t_uldivmod},

    {"__aeabi_dadd", &t_dadd},
    {"__aeabi_ddiv", &t_ddiv},
    {"__aeabi_dcmpgt", &t_dcmpgt},
    {"__aeabi_i2d", &t_i2d},
    {"__aeabi_l2d", &t_l2d},
    {"__aeabi_l2f", &t_l2f},
    {"__aeabi_ul2f", &t_ul2f},
    {"__aeabi_d2lz", &t_d2lz},
    {"__aeabi_f2lz", &t_f2lz},

    {"clock_gettime", &t_clock_gettime},
    {"gettimeofday", &t_gettimeofday},
    {"time", &t_time},
    {"usleep", &t_usleep},

    {"nv_android_init", &t_nv_android_init},
};

const std::size_t kRuntimeTableSize =
    sizeof(kRuntimeTable) / sizeof(kRuntimeTable[0]);

}  // namespace wb
