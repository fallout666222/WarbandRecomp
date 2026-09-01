// A few imports answered with guest ARM32 code instead of a host thunk.
//
// `__aeabi_idiv` alone was called 2.2 million times during startup - more
// than every file, string and memory operation put together. Each one was a
// trap out of the JIT, a dispatch, and a return, to do one division.
//
// The fix is to stop crossing at all: these are pure arithmetic, so the
// import can resolve to a handful of ARM instructions living in guest memory.
// SDIV and UDIV are the ARMv7 integer-divide extension; the recompiler
// decodes them, and a division becomes one instruction again.
//
// The clock is the same story, larger. The engine's frame limiter spins on
// clock_gettime, which came to 393 million calls in a four-minute run - more
// than everything else together, and roughly two thirds of the render
// thread's time spent leaving and re-entering the recompiler. A clock cannot
// be computed in guest code, but it can be *read* there: a host thread
// publishes the time into a page of guest memory, and the guest routine is a
// seqlock read. Nothing crosses the boundary.
//
// This is the mechanism a real system uses for the same problem - Linux calls
// it the vDSO - applied to the calls that actually matter here. The loader
// consults this first through its guest resolver, and anything not listed
// still gets a trampoline.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>

#include "env.h"
#include "guest_code.h"

namespace wb {
namespace {

// ARM encodings, spelled out so the intent survives without an assembler.
constexpr u32 kSdivR0R0R1 = 0xE710F110;   // sdiv r0, r0, r1
constexpr u32 kUdivR0R0R1 = 0xE730F110;   // udiv r0, r0, r1
constexpr u32 kMovR2R0 = 0xE1A02000;      // mov  r2, r0
constexpr u32 kMlsR1R0R1R2 = 0xE0612190;  // mls  r1, r0, r1, r2
constexpr u32 kBxLr = 0xE12FFF1E;         // bx   lr

// The instructions the clock readers are built from. MOVW/MOVT is how a
// 32-bit constant reaches a register on ARMv7 without a literal pool, which
// keeps each routine a flat run of words.
constexpr u32 movw(u32 rd, u32 imm16) {
  return 0xE3000000u | ((imm16 >> 12) & 0xFu) << 16 | (rd << 12) |
         (imm16 & 0x0FFFu);
}
constexpr u32 movt(u32 rd, u32 imm16) {
  return 0xE3400000u | ((imm16 >> 12) & 0xFu) << 16 | (rd << 12) |
         (imm16 & 0x0FFFu);
}
constexpr u32 ldr(u32 rt, u32 rn, u32 off) {
  return 0xE5900000u | (rn << 16) | (rt << 12) | (off & 0x0FFFu);
}
// STR with the NE condition: the stores are skipped when the caller passed a
// null destination, which callers of time() legitimately do.
constexpr u32 strne(u32 rt, u32 rn, u32 off) {
  return 0x15800000u | (rn << 16) | (rt << 12) | (off & 0x0FFFu);
}
constexpr u32 tst_imm(u32 rn, u32 imm) {
  return 0xE3100000u | (rn << 16) | (imm & 0x0FFFu);
}
constexpr u32 cmp_imm(u32 rn, u32 imm) {
  return 0xE3500000u | (rn << 16) | (imm & 0x0FFFu);
}
constexpr u32 cmp_reg(u32 rn, u32 rm) {
  return 0xE1500000u | (rn << 16) | rm;
}
constexpr u32 mov_imm(u32 rd, u32 imm) {
  return 0xE3A00000u | (rd << 12) | (imm & 0x0FFFu);
}
constexpr u32 mov_reg(u32 rd, u32 rm) { return 0xE1A00000u | (rd << 12) | rm; }
// Backwards branch: `from` and `to` are word indices within the routine.
constexpr u32 bne(int from, int to) {
  return 0x1A000000u | (static_cast<u32>(to - from - 2) & 0x00FFFFFFu);
}

constexpr u32 kIp = 12;   // r12, the scratch register AAPCS lets us clobber

// Offsets within the time page.
constexpr u32 kSeq = 0, kSec = 4, kNsec = 8, kUsec = 12;

std::atomic<bool> g_clock_stop{false};
std::thread g_clock;
u32 g_seq = 0;

// One update of the page. Odd sequence while the fields are in flux, even
// once they agree; a reader that sees the same even value on both sides of
// its read knows it got a consistent pair.
void publish_once(Memory* mem) {
  const std::int64_t ns = guest_monotonic_ns();
  mem->write32(layout::kTimePage + kSeq, ++g_seq);          // odd: writing
  std::atomic_thread_fence(std::memory_order_release);
  mem->write32(layout::kTimePage + kSec, static_cast<u32>(ns / 1'000'000'000));
  mem->write32(layout::kTimePage + kNsec, static_cast<u32>(ns % 1'000'000'000));
  mem->write32(layout::kTimePage + kUsec,
               static_cast<u32>((ns / 1000) % 1'000'000));
  std::atomic_thread_fence(std::memory_order_release);
  mem->write32(layout::kTimePage + kSeq, ++g_seq);          // even: settled
}

// 200 microseconds is far finer than anything the engine measures, and the
// frame limiter this feeds polls thousands of times between updates.
void publish_time(Memory* mem) {
  while (!g_clock_stop.load(std::memory_order_relaxed)) {
    publish_once(mem);
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}

}  // namespace

GuestCode::GuestCode(Memory& mem)
    : mem_(mem), next_(layout::kGuestCodeBase) {}

u32 GuestCode::emit(const char* name, std::initializer_list<u32> words) {
  const u32 at = next_;
  if (at + words.size() * 4 >
      layout::kGuestCodeBase + layout::kGuestCodeSize) {
    std::printf("[gcod] out of guest code space for %s\n", name);
    return 0;
  }
  u32 p = at;
  for (u32 w : words) {
    mem_.write32(p, w);
    p += 4;
  }
  next_ = p;
  routines_[name] = at;
  return at;
}

void GuestCode::build() {
  // int __aeabi_idiv(int numerator, int denominator) -> r0
  emit("__aeabi_idiv", {kSdivR0R0R1, kBxLr});
  emit("__aeabi_uidiv", {kUdivR0R0R1, kBxLr});

  // __aeabi_idivmod returns the quotient in r0 and the remainder in r1.
  // r2 keeps the numerator so the remainder can be recovered with one
  // multiply-and-subtract.
  emit("__aeabi_idivmod", {kMovR2R0, kSdivR0R0R1, kMlsR1R0R1R2, kBxLr});
  emit("__aeabi_uidivmod", {kMovR2R0, kUdivR0R0R1, kMlsR1R0R1R2, kBxLr});

  build_maths();
  build_clock();
  build_strings();

  std::printf("[gcod] %zu routines in guest code at 0x%08X..0x%08X\n",
              routines_.size(), layout::kGuestCodeBase, next_);
}

// Maths the recompiler can do in a handful of instructions.
//
// The scene loader called floorf 1.1 million times in twenty megabytes of
// trace and almost nothing else - terrain generation is floorf-bound - and
// every one of those was a trap out of the recompiler to compute something
// ARM does in six instructions. It is the same lesson as the clock: hot and
// cheap belongs on this side of the boundary.
//
// ARMv7 has no rounding-mode convert - VRINTM and friends are ARMv8 - so
// floor and ceil truncate toward zero and then step by one when that rounded
// the wrong way. Above 2^23 a float has no fraction left, so those values,
// and infinities and NaNs with them, take an early exit instead of going
// through VCVT, which would saturate.
//
// The words come from tools/asm_guest.py, which assembles each routine from
// its source and disassembles it back to check. Hand-encoding VFP is not
// worth anyone's evening.
void GuestCode::build_maths() {
  emit("fabsf", {
      0xE3C00102,   // bic   r0, r0, #0x80000000  bic r0, r0, #0x80000000
      0xE12FFF1E,   // bx    lr                   bx lr
  });

  emit("sqrtf", {
      0xEE000A10,   // vmov  s0, r0               vmov s0, r0
      0xEEF10AC0,   // vsqrt.f32 s1, s0           vsqrt.f32 s1, s0
      0xEE100A90,   // vmov  r0, s1               vmov r0, s1
      0xE12FFF1E,   // bx    lr                   bx lr
  });

  emit("truncf", {
      0xE3C01102,   // bic   r1, r0, #0x80000000  bic r1, r0, #0x80000000
      0xE351044B,   // cmp   r1, #0x4B000000      cmp r1, #0x4b000000
      0xA12FFF1E,   // bxge  lr                   bxge lr
      0xEE000A10,   // vmov  s0, r0               vmov s0, r0
      0xEEFD0AC0,   // vcvt.s32.f32 s1, s0        vcvt.s32.f32 s1, s0
      0xEEB81AE0,   // vcvt.f32.s32 s2, s1        vcvt.f32.s32 s2, s1
      0xEE110A10,   // vmov  r0, s2               vmov r0, s2
      0xE12FFF1E,   // bx    lr                   bx lr
  });

  emit("floorf", {
      0xE3C01102,   // bic   r1, r0, #0x80000000  bic r1, r0, #0x80000000
      0xE351044B,   // cmp   r1, #0x4B000000      cmp r1, #0x4b000000
      0xA12FFF1E,   // bxge  lr                   bxge lr
      0xEE000A10,   // vmov  s0, r0               vmov s0, r0
      0xEEFD0AC0,   // vcvt.s32.f32 s1, s0        vcvt.s32.f32 s1, s0
      0xEEB81AE0,   // vcvt.f32.s32 s2, s1        vcvt.f32.s32 s2, s1
      0xEEF71A00,   // vmov.f32 s3, #1.0          vmov.f32 s3, #1.000000e+00
      0xEEB40A41,   // vcmp.f32 s0, s2            vcmp.f32 s0, s2
      0xEEF1FA10,   // vmrs  APSR_nzcv, fpscr     vmrs apsr_nzcv, fpscr
      0xBE311A61,   // vsublt.f32 s2, s2, s3      vsublt.f32 s2, s2, s3
      0xEE110A10,   // vmov  r0, s2               vmov r0, s2
      0xE12FFF1E,   // bx    lr                   bx lr
  });

  emit("ceilf", {
      0xE3C01102,   // bic   r1, r0, #0x80000000  bic r1, r0, #0x80000000
      0xE351044B,   // cmp   r1, #0x4B000000      cmp r1, #0x4b000000
      0xA12FFF1E,   // bxge  lr                   bxge lr
      0xEE000A10,   // vmov  s0, r0               vmov s0, r0
      0xEEFD0AC0,   // vcvt.s32.f32 s1, s0        vcvt.s32.f32 s1, s0
      0xEEB81AE0,   // vcvt.f32.s32 s2, s1        vcvt.f32.s32 s2, s1
      0xEEF71A00,   // vmov.f32 s3, #1.0          vmov.f32 s3, #1.000000e+00
      0xEEB40A41,   // vcmp.f32 s0, s2            vcmp.f32 s0, s2
      0xEEF1FA10,   // vmrs  APSR_nzcv, fpscr     vmrs apsr_nzcv, fpscr
      0xCE311A21,   // vaddgt.f32 s2, s2, s3      vaddgt.f32 s2, s2, s3
      0xEE110A10,   // vmov  r0, s2               vmov r0, s2
      0xE12FFF1E,   // bx    lr                   bx lr
  });
}

// The seqlock read, twice: once for timespec and once for timeval. They
// differ only in which register holds the destination and whether the second
// field is nanoseconds or microseconds.
//
//        movw r2, #lo(page)
//        movt r2, #hi(page)
//   2:   ldr  r3, [r2]          ; sequence
//        tst  r3, #1            ; odd means a write is in progress
//        bne  2
//        ldr  <a>, [r2, #4]     ; seconds
//        ldr  ip, [r2, #off]    ; nanoseconds or microseconds
//        cmp  <dst>, #0
//        strne <a>, [<dst>]
//        strne ip, [<dst>, #4]
//        ldr  ip, [r2]          ; sequence again
//        cmp  r3, ip            ; changed? the read was torn, try again
//        bne  2
//        mov  r0, #0
//        bx   lr
void GuestCode::build_clock() {
  const u32 page = layout::kTimePage;

  auto reader = [&](const char* name, u32 dst, u32 tmp, u32 second_field) {
    emit(name, {
        movw(2, page & 0xFFFF),
        movt(2, page >> 16),
        ldr(3, 2, kSeq),            // 2: retry
        tst_imm(3, 1),
        bne(4, 2),
        ldr(tmp, 2, kSec),
        ldr(kIp, 2, second_field),
        cmp_imm(dst, 0),
        strne(tmp, dst, 0),
        strne(kIp, dst, 4),
        ldr(kIp, 2, kSeq),
        cmp_reg(3, kIp),
        bne(12, 2),
        mov_imm(0, 0),
        kBxLr,
    });
  };

  // int clock_gettime(clockid_t clk, struct timespec *ts): ts is r1, and the
  // clock id is ignored - there is only one clock here.
  reader("clock_gettime", /*dst=*/1, /*tmp=*/0, kNsec);
  // int gettimeofday(struct timeval *tv, struct timezone *tz): tv is r0.
  reader("gettimeofday", /*dst=*/0, /*tmp=*/1, kUsec);

  // time_t time(time_t *out): one word, so no sequence check is needed - a
  // seconds field cannot be read half-written.
  emit("time", {
      movw(2, page & 0xFFFF),
      movt(2, page >> 16),
      ldr(1, 2, kSec),
      cmp_imm(0, 0),
      strne(1, 0, 0),
      mov_reg(0, 1),
      kBxLr,
  });
}

void GuestCode::stop_clock() {
  g_clock_stop.store(true, std::memory_order_relaxed);
  if (g_clock.joinable()) g_clock.join();
}

void GuestCode::start_clock() {
  mem_.zero(layout::kTimePage, 16);
  // Publish once before anything can read: a guest that starts life seeing
  // time zero and then jumps forward confuses every elapsed-time measurement
  // the engine makes in its first frame.
  g_seq = 0;
  publish_once(&mem_);
  // Joinable, not detached. The page it writes lives in the guest arena,
  // and the arena is freed when the process winds down - a detached writer
  // outlives it by a few hundred microseconds and faults on the way out,
  // which reads as a crash at the end of a run that went perfectly well.
  g_clock_stop.store(false);
  g_clock = std::thread(publish_time, &mem_);
  std::printf("[gcod] clock published at 0x%08X, updated every 200us\n",
              layout::kTimePage);
}

namespace {

float bits_to_float(u32 b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

u32 float_to_bits(float f) {
  u32 b;
  std::memcpy(&b, &f, 4);
  return b;
}

}  // namespace

// The string and memory routines, which are the boundary's biggest bill.
//
// Measured on the console: of two million import calls during loading,
// memset alone is eight hundred and forty thousand, and strcmp, memcmp and
// strlen together another hundred and seventy. None of them is hard work -
// the cost is the crossing, not the copying, and the sizes say so: the mean
// memset is seventy-four bytes and ninety-nine per cent of them are under a
// hundred and twenty-eight.
//
// memcpy is deliberately not here. Its mean block is fourteen hundred bytes
// with a long tail past a kilobyte, and at that size the host's vectorised
// copy beats anything worth hand-writing by more than the crossing costs.
//
// Written as assembly in tools/asm_guest.py and pasted here with the source
// beside the bytes; do not edit the words.
void GuestCode::build_strings() {
  emit("memset", {
      0xE1A03000,   // mov   r3, r0               mov r3, r0
      0xE20110FF,   // and   r1, r1, #0xff        and r1, r1, #0xff
      0xE1811401,   // orr   r1, r1, r1, lsl #8   orr r1, r1, r1, lsl #8
      0xE1811801,   // orr   r1, r1, r1, lsl #16  orr r1, r1, r1, lsl #16
      0xE3520010,   // cmp   r2, #16              cmp r2, #0x10
      0x3A00000C,   // blo   ms_tail              blo #0x38
      0xE3130003,   // tst   r3, #3               tst r3, #3
      0x0A000002,   // beq   ms_words             beq #0x10
      0xE4C31001,   // strb  r1, [r3], #1         strb r1, [r3], #1
      0xE2422001,   // sub   r2, r2, #1           sub r2, r2, #1
      0xEAFFFFFA,   // b     ms_align             b #0xfffffff0
      0xE3520010,   // cmp   r2, #16              cmp r2, #0x10
      0x3A000005,   // blo   ms_tail              blo #0x1c
      0xE4831004,   // str   r1, [r3], #4         str r1, [r3], #4
      0xE4831004,   // str   r1, [r3], #4         str r1, [r3], #4
      0xE4831004,   // str   r1, [r3], #4         str r1, [r3], #4
      0xE4831004,   // str   r1, [r3], #4         str r1, [r3], #4
      0xE2422010,   // sub   r2, r2, #16          sub r2, r2, #0x10
      0xEAFFFFF7,   // b     ms_words             b #0xffffffe4
      0xE3520000,   // cmp   r2, #0               cmp r2, #0
      0x012FFF1E,   // bxeq  lr                   bxeq lr
      0xE4C31001,   // strb  r1, [r3], #1         strb r1, [r3], #1
      0xE2522001,   // subs  r2, r2, #1           subs r2, r2, #1
      0x1AFFFFFC,   // bne   ms_byte              bne #0xfffffff8
      0xE12FFF1E,   // bx    lr                   bx lr
  });

  emit("strlen", {
      0xE1A01000,   // mov   r1, r0               mov r1, r0
      0xE4D12001,   // ldrb  r2, [r1], #1         ldrb r2, [r1], #1
      0xE3520000,   // cmp   r2, #0               cmp r2, #0
      0x1AFFFFFC,   // bne   sl_loop              bne #0xfffffff8
      0xE0410000,   // sub   r0, r1, r0           sub r0, r1, r0
      0xE2400001,   // sub   r0, r0, #1           sub r0, r0, #1
      0xE12FFF1E,   // bx    lr                   bx lr
  });

  emit("strcmp", {
      0xE4D02001,   // ldrb  r2, [r0], #1         ldrb r2, [r0], #1
      0xE4D13001,   // ldrb  r3, [r1], #1         ldrb r3, [r1], #1
      0xE1520003,   // cmp   r2, r3               cmp r2, r3
      0x1A000003,   // bne   sc_diff              bne #0x14
      0xE3520000,   // cmp   r2, #0               cmp r2, #0
      0x1AFFFFF9,   // bne   sc_loop              bne #0xffffffec
      0xE3A00000,   // mov   r0, #0               mov r0, #0
      0xE12FFF1E,   // bx    lr                   bx lr
      0xE0420003,   // sub   r0, r2, r3           sub r0, r2, r3
      0xE12FFF1E,   // bx    lr                   bx lr
  });

  emit("memcmp", {
      0xE3520000,   // cmp   r2, #0               cmp r2, #0
      0x0A000005,   // beq   mc_same              beq #0x1c
      0xE4D03001,   // ldrb  r3, [r0], #1         ldrb r3, [r0], #1
      0xE4D1C001,   // ldrb  r12, [r1], #1        ldrb ip, [r1], #1
      0xE153000C,   // cmp   r3, r12              cmp r3, ip
      0x1A000003,   // bne   mc_diff              bne #0x14
      0xE2522001,   // subs  r2, r2, #1           subs r2, r2, #1
      0x1AFFFFF9,   // bne   mc_loop              bne #0xffffffec
      0xE3A00000,   // mov   r0, #0               mov r0, #0
      0xE12FFF1E,   // bx    lr                   bx lr
      0xE043000C,   // sub   r0, r3, r12          sub r0, r3, ip
      0xE12FFF1E,   // bx    lr                   bx lr
  });
}

bool GuestCode::self_test(Env& e) const {
  struct Case {
    const char* name;
    float (*host)(float);
  };
  static const Case kCases[] = {
      {"fabsf", [](float x) { return std::fabs(x); }},
      {"sqrtf", [](float x) { return std::sqrt(x); }},
      {"truncf", [](float x) { return std::trunc(x); }},
      {"floorf", [](float x) { return std::floor(x); }},
      {"ceilf", [](float x) { return std::ceil(x); }},
  };
  // Values chosen to exercise the parts that are easy to get wrong: negatives
  // either side of an integer, exact integers, the 2^23 boundary where the
  // early exit takes over, and zero.
  static const float kInputs[] = {
      0.0f,      1.0f,        -1.0f,      0.5f,      -0.5f,
      1.5f,      -1.5f,       2.9999f,    -2.9999f,  3.0f,
      -3.0f,     1234.567f,   -1234.567f, 8388607.5f, 8388608.0f,
      -8388608.0f, 16777216.0f, 1e-30f,   -1e-30f,   1e30f,
  };

  int failures = 0;
  for (const Case& c : kCases) {
    u32 addr = 0;
    if (!resolve(c.name, &addr)) continue;
    for (float x : kInputs) {
      // sqrt of a negative is not a number, and comparing two NaNs proves
      // nothing about the routine.
      if (x < 0.0f && std::strcmp(c.name, "sqrtf") == 0) continue;
      const u32 got = e.call(addr, {float_to_bits(x)});
      const float want = c.host(x);
      const float have = bits_to_float(got);
      const bool same = (want == have) ||
                        (want != want && have != have);   // both NaN
      if (!same) {
        std::printf("[gcod] %s(%g) = %g, host says %g\n", c.name,
                    static_cast<double>(x), static_cast<double>(have),
                    static_cast<double>(want));
        ++failures;
      }
    }
  }
  // The string and memory routines, against the host's own.
  //
  // These are the ones worth testing hardest: a wrong floorf is visible as a
  // model in the wrong place, but a memset that writes one byte too many is
  // silent for hours and then corrupts something unrelated. So the checks
  // include the boundaries the code actually branches on - nothing, one byte,
  // either side of the four-byte alignment step, either side of the sixteen
  // that turns on the word loop - and the byte past the end every time.
  //
  // The scratch lives at the base of the guest heap. Nothing has been
  // allocated yet at this point in startup, and by the time anything is, this
  // has long finished.
  {
    const u32 kScratch = layout::kHeapBase;
    const u32 kOther = kScratch + 4096;
    u32 memset_at = 0, strlen_at = 0, strcmp_at = 0, memcmp_at = 0;
    resolve("memset", &memset_at);
    resolve("strlen", &strlen_at);
    resolve("strcmp", &strcmp_at);
    resolve("memcmp", &memcmp_at);

    static const u32 kSizes[] = {0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 31, 32,
                                 63, 64, 74, 127, 128, 129, 256};
    if (memset_at) {
      for (u32 n : kSizes) {
        for (u32 skew = 0; skew < 4; ++skew) {
          const u32 at = kScratch + skew;
          for (u32 i = 0; i < n + 8; ++i) e.mem().write8(at + i, 0x5A);
          const u32 got = e.call(memset_at, {at, 0xAB, n});
          bool ok = got == at;
          for (u32 i = 0; ok && i < n; ++i)
            ok = e.mem().read8(at + i) == 0xAB;
          for (u32 i = n; ok && i < n + 8; ++i)
            ok = e.mem().read8(at + i) == 0x5A;
          if (!ok) {
            std::printf("[gcod] memset(%u bytes, offset %u) is wrong\n", n,
                        skew);
            ++failures;
          }
        }
      }
    }

    if (strlen_at) {
      for (u32 n : kSizes) {
        for (u32 i = 0; i < n; ++i) e.mem().write8(kScratch + i, 'x');
        e.mem().write8(kScratch + n, 0);
        const u32 got = e.call(strlen_at, {kScratch});
        if (got != n) {
          std::printf("[gcod] strlen of %u characters answered %u\n", n, got);
          ++failures;
        }
      }
    }

    // Pairs that differ nowhere, at the front, in the middle, at the end, and
    // only in length - which is the case a naive loop runs off the end of.
    static const char* const kPairs[][2] = {
        {"", ""},          {"a", "a"},        {"a", "b"},
        {"b", "a"},        {"", "a"},         {"a", ""},
        {"abc", "abd"},    {"abc", "abc"},    {"abcd", "abc"},
        {"abc", "abcd"},   {"zzz", "azzz"},   {"Modules", "Modules"},
        {"Modules", "modules"}, {"\x01", "\x7f"}, {"\x80", "\x01"},
    };
    for (const auto& pair : kPairs) {
      const std::size_t la = std::strlen(pair[0]), lb = std::strlen(pair[1]);
      e.mem().copy_in(kScratch, pair[0], la + 1);
      e.mem().copy_in(kOther, pair[1], lb + 1);
      if (strcmp_at) {
        const int got = static_cast<int>(e.call(strcmp_at, {kScratch, kOther}));
        const int want = std::strcmp(pair[0], pair[1]);
        if ((got > 0) != (want > 0) || (got < 0) != (want < 0)) {
          std::printf("[gcod] strcmp(\"%s\", \"%s\") = %d, host says %d\n",
                      pair[0], pair[1], got, want);
          ++failures;
        }
      }
      if (memcmp_at) {
        const std::size_t n = la < lb ? la : lb;
        const int got =
            static_cast<int>(e.call(memcmp_at, {kScratch, kOther,
                                                static_cast<u32>(n)}));
        const int want = n ? std::memcmp(pair[0], pair[1], n) : 0;
        if ((got > 0) != (want > 0) || (got < 0) != (want < 0)) {
          std::printf("[gcod] memcmp(\"%s\", \"%s\", %zu) = %d, host says %d\n",
                      pair[0], pair[1], n, got, want);
          ++failures;
        }
      }
    }
  }

  if (failures)
    std::printf("[gcod] %d guest routines disagree with the host\n",
                failures);
  else
    std::printf("[gcod] guest routines agree with the host on every case\n");
  return failures == 0;
}

bool GuestCode::resolve(const std::string& name, u32* out) const {
  auto it = routines_.find(name);
  if (it == routines_.end()) return false;
  *out = it->second;
  return true;
}

}  // namespace wb
