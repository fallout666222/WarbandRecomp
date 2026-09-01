#include "env.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <memory>
#include <unordered_map>
#include <utility>

#include "android_glue.h"
#include "crash.h"

namespace wb {
namespace {

// svc #0xFFFFFF is reserved: no thunk index can reach it, so it is a safe
// "the guest function you called has returned" marker.
constexpr u32 kReturnSwi = 0x00FFFFFF;
constexpr u32 kSvcAl = 0xEF000000;

// Which GuestThread the calling host thread is running.
thread_local GuestThread* t_current = nullptr;

}  // namespace

Env::~Env() { join_all(); }

int current_thread_id() { return t_current ? t_current->id + 1 : 1; }

GuestThread* current_guest_thread() { return t_current; }

// ------------------------------------------------------------------ threads
void Env::start(PageTable* page_table, Dynarmic::ExclusiveMonitor* monitor) {
  page_table_ = page_table;
  monitor_ = monitor;
  GuestThread* main = make_thread();
  if (!main) fatal("could not create the first guest CPU");
  t_current = main;
}

GuestThread& Env::self() {
  // Every path that reaches a callback runs on a thread that owns one.
  return *t_current;
}

GuestThread* Env::make_thread() {
  std::lock_guard<std::mutex> lock(threads_lock_);
  auto t = std::make_unique<GuestThread>();
  t->id = static_cast<int>(threads_.size());
  // Stacks descend from the top of the space, one slot each.
  t->stack_top = layout::kStackTop - static_cast<u32>(t->id) * layout::kStackSize;

  Dynarmic::A32::UserConfig config;
  config.callbacks = this;
  config.global_monitor = monitor_;
  config.processor_id = static_cast<std::size_t>(t->id);
  config.arch_version = Dynarmic::A32::ArchVersion::v7;
  config.always_little_endian = true;
  config.page_table = page_table_;
  config.enable_cycle_counting = true;
  config.code_cache_size = code_cache_mb_ * 1024 * 1024;
  // On Horizon the recompiler's memory comes from the kernel through a Jit
  // object, and running out of it throws rather than returning null. Left
  // uncaught on a guest thread that means std::terminate, which means an
  // error code on the console and no explanation at all.
  try {
    t->jit = std::make_unique<Dynarmic::A32::Jit>(config);
  } catch (const std::exception& ex) {
    std::printf("[env ] no recompiler for guest thread %d: %s (asked for "
                "%zu MiB of code cache)\n", t->id, ex.what(), code_cache_mb_);
    std::fflush(stdout);
    return nullptr;
  }

  // Start the floating-point unit the way Android starts it.
  //
  // Bionic puts VFP into what ARM calls RunFast: flush-to-zero and default
  // NaN. A denormal result becomes zero instead of being computed properly,
  // which is both much faster on the hardware and, more to the point here, a
  // different answer. Leaving the unit in strict IEEE mode is not the
  // conservative choice - it is a different machine from the one the engine
  // was compiled and tested against, and a loop that ends on the device
  // because a value collapsed to zero can grind on for thousands of
  // iterations without it.
  constexpr u32 kFpscrFlushToZero = 1u << 24;
  constexpr u32 kFpscrDefaultNaN = 1u << 25;
  t->jit->SetFpscr(ieee_floats_ ? 0u
                                : (kFpscrFlushToZero | kFpscrDefaultNaN));

  GuestThread* raw = t.get();
  threads_.push_back(std::move(t));
  return raw;
}

u32 Env::spawn_call(u32 entry, std::initializer_list<u32> args) {
  if (static_cast<int>(threads_.size()) >= max_threads_) {
    std::printf("[thr ] thread limit reached; cannot deliver callback 0x%08X\n",
                entry);
    return 0;
  }
  GuestThread* t = make_thread();
  if (!t) return 0;
  std::vector<u32> copy(args);
  std::lock_guard<std::mutex> lock(threads_lock_);
  host_threads_.emplace_back([this, t, entry, copy] {
    t_current = t;
    crash_arm_thread();
    std::printf("[thr ] callback thread %d -> %s\n", t->id,
                loader_.symbolize(entry).c_str());
    if (copy.size() >= 4) call(entry, {copy[0], copy[1], copy[2], copy[3]});
    else if (copy.size() == 3) call(entry, {copy[0], copy[1], copy[2]});
    else if (copy.size() == 2) call(entry, {copy[0], copy[1]});
    else if (copy.size() == 1) call(entry, {copy[0]});
    else call(entry);
    t->finished = true;
  });
  return static_cast<u32>(t->id);
}

u32 Env::spawn(u32 entry, u32 arg) {
  if (threads_.size() >= 16) {
    std::printf("[thr ] refusing to create more than 16 guest threads\n");
    return 0;
  }
  GuestThread* t = make_thread();
  if (!t) return 0;
  std::printf("[thr ] thread %d starting at 0x%08X (%s), arg 0x%08X, "
              "stack 0x%08X\n",
              t->id, entry, loader_.symbolize(entry).c_str(), arg,
              t->stack_top);
  std::lock_guard<std::mutex> lock(threads_lock_);
  host_threads_.emplace_back([this, t, entry, arg] { thread_body(t, entry, arg); });
  return static_cast<u32>(t->id);
}

void Env::thread_body(GuestThread* t, u32 entry, u32 arg) {
  t_current = t;
  crash_arm_thread();
  const u32 rv = call(entry, {arg});
  t->finished = true;
  std::printf("[thr ] thread %d finished, returned %u after %llu instructions\n",
              t->id, rv, (unsigned long long)t->consumed);
}

void Env::join_all() {
  std::vector<std::thread> take;
  {
    std::lock_guard<std::mutex> lock(threads_lock_);
    take.swap(host_threads_);
  }
  for (auto& h : take)
    if (h.joinable()) h.join();
}

int Env::live_threads() {
  std::lock_guard<std::mutex> lock(threads_lock_);
  int n = 0;
  for (const auto& t : threads_)
    if (!t->finished) ++n;
  return n;
}

// ------------------------------------------------------------- arg reader
//
// AAPCS base standard: r0-r3 hold the first four words, the rest spill to the
// stack. 64-bit values are 8-byte aligned, so they start in an even-numbered
// register (skipping one if necessary) or on an 8-byte boundary once spilled.
u32 Env::Args::next32() {
  if (core_ < 4) return regs_[core_++];
  if (!stack_started_) {
    stack_ = regs_[13];
    stack_started_ = true;
  }
  u32 v = e_.mem_.read32(stack_);
  stack_ += 4;
  return v;
}

u64 Env::Args::next64() {
  if (core_ < 4) {
    if (core_ & 1) ++core_;             // align to an even register
    if (core_ <= 2) {
      u32 lo = regs_[core_];
      u32 hi = regs_[core_ + 1];
      core_ += 2;
      return (u64(hi) << 32) | lo;
    }
    core_ = 4;                          // r3 alone cannot hold 64 bits
  }
  if (!stack_started_) {
    stack_ = regs_[13];
    stack_started_ = true;
  }
  stack_ = (stack_ + 7) & ~7u;
  u64 v = e_.mem_.read64(stack_);
  stack_ += 8;
  return v;
}

void Env::ret(u32 v) { jit()->Regs()[0] = v; }

void Env::ret64(u64 v) {
  auto& r = jit()->Regs();
  r[0] = static_cast<u32>(v);
  r[1] = static_cast<u32>(v >> 32);
}

void Env::retf(float v) {
  u32 bits;
  std::memcpy(&bits, &v, 4);
  jit()->Regs()[0] = bits;
}

// --------------------------------------------------------------- binding
void Env::bind_thunks() {
  std::unordered_map<std::string, ThunkFn> table;
  table.reserve(kThunkTableSize + kLibcTableSize + kThreadTableSize +
                kRuntimeTableSize);
  for (std::size_t i = 0; i < kThunkTableSize; ++i)
    table.emplace(kThunkTable[i].name, kThunkTable[i].fn);
  for (std::size_t i = 0; i < kLibcTableSize; ++i)
    table[kLibcTable[i].name] = kLibcTable[i].fn;
  for (std::size_t i = 0; i < kThreadTableSize; ++i)
    table[kThreadTable[i].name] = kThreadTable[i].fn;
  for (std::size_t i = 0; i < kRuntimeTableSize; ++i)
    table[kRuntimeTable[i].name] = kRuntimeTable[i].fn;
  for (std::size_t i = 0; i < kAndroidTableSize; ++i)
    table[kAndroidTable[i].name] = kAndroidTable[i].fn;
  for (std::size_t i = 0; i < kGlTableSize; ++i)
    table[kGlTable[i].name] = kGlTable[i].fn;
  // Last two win: the filesystem owns stdio, and the printf family owns
  // formatting - the libc table has only placeholders for both.
  for (std::size_t i = 0; i < kFsTableSize; ++i)
    table[kFsTable[i].name] = kFsTable[i].fn;
  for (std::size_t i = 0; i < kPrintfTableSize; ++i)
    table[kPrintfTable[i].name] = kPrintfTable[i].fn;
  for (std::size_t i = 0; i < kMathTableSize; ++i)
    table[kMathTable[i].name] = kMathTable[i].fn;
  for (std::size_t i = 0; i < kFmodTableSize; ++i)
    table[kFmodTable[i].name] = kFmodTable[i].fn;
  for (std::size_t i = 0; i < kInputTableSize; ++i)
    table[kInputTable[i].name] = kInputTable[i].fn;

  // Which trampolines are GL. Everything in the GL table whose name does not
  // start with "egl" talks to the driver and so needs the context current on
  // the calling thread; the EGL entries manage the context themselves.
  std::unordered_map<std::string, bool> is_gl;
  for (std::size_t i = 0; i < kGlTableSize; ++i) {
    const std::string name = kGlTable[i].name;
    is_gl[name] = name.compare(0, 3, "egl") != 0;
  }

  const u32 n = loader_.thunk_count();
  bound_.assign(n, nullptr);
  reported_.assign(n, false);
  gl_thunk_.assign(n, false);
  calls_ = std::vector<std::atomic<std::uint64_t>>(n);
  for (auto& c : calls_) c.store(0, std::memory_order_relaxed);

  u32 missing = 0;
  for (u32 i = 0; i < n; ++i) {
    auto it = table.find(loader_.thunk_name(i));
    if (it != table.end()) {
      bound_[i] = it->second;
      auto g = is_gl.find(loader_.thunk_name(i));
      gl_thunk_[i] = g != is_gl.end() && g->second;
    } else {
      ++missing;
    }
  }
  std::printf("[env ] %u of %u trampolines bound to the thunk table\n",
              n - missing, n);
  if (missing) {
    std::printf("[env ] %u have no entry at all:", missing);
    int shown = 0;
    for (u32 i = 0; i < n; ++i) {
      if (bound_[i]) continue;
      if (++shown > 80) { std::printf(" ..."); break; }
      std::printf(" %s", loader_.thunk_name(i).c_str());
    }
    std::printf("\n");
  }

  mem_.write32(layout::kReturnStub, kSvcAl | kReturnSwi);
  return_magic_ = layout::kReturnStub;
}

// ---------------------------------------------------------------- calling
u32 Env::call(u32 guest_pc, std::initializer_list<u32> args) {
  GuestThread& s = self();
  auto& regs = s.jit->Regs();
  int i = 0;
  for (u32 a : args)
    if (i < 4) regs[i++] = a;

  regs[13] = s.stack_top - 16;
  regs[14] = return_magic_;
  regs[15] = guest_pc & ~1u;

  auto cpsr = s.jit->Cpsr();
  if (guest_pc & 1) {
    cpsr |= (1u << 5);                  // T bit: enter in Thumb
  } else {
    cpsr &= ~(1u << 5);
  }
  s.jit->SetCpsr(cpsr);

  s.halt = false;
  s.consumed = 0;

  // A thunk that tail-calls guest code stops the CPU with the new PC already
  // in place, so keep running until the return stub fires or we give up.
  // Cycle counting compares the remaining count as a signed value, so the
  // budget must stay positive: handing it ~0 reads as -1 and Run() returns
  // immediately, having executed almost nothing.
  constexpr std::uint64_t kDefaultSlice = 1'000'000'000;
  for (std::uint64_t resumes = 0; ; ++resumes) {
    s.redirected = false;
    const std::uint64_t budget = tick_budget_ ? tick_budget_ : kDefaultSlice;
    s.ticks = budget;
    s.jit->ClearHalt();
    const auto reason = s.jit->Run();
    s.consumed += budget - s.ticks;
    if (s.halt || fatal_) return regs[0];
    if (s.redirected) continue;
    if (s.ticks == 0) {
      ++s.slices;
      if (trace_.load(std::memory_order_relaxed)) {
        // The return address as well as the program counter. One frame of
        // caller is often the whole answer: an interpreter loop looks the
        // same whoever asked for it, and who asked is the question.
        std::printf("[samp] t%d %s  <- %s\n", s.id,
                    loader_.symbolize(regs[15]).c_str(),
                    loader_.symbolize(regs[14]).c_str());
        std::fflush(stdout);
        continue;
      }
      // A spent budget used to be fatal, on the theory that a guest running
      // this long had hung. That was true while the engine was still loading;
      // it is wrong now that it reaches its main loop, which is meant to run
      // forever. A heartbeat keeps a genuine hang visible without stopping a
      // healthy run.
      if (s.slices % 8 == 0) {
        std::printf("[env ] t=%.1fs thread %d has run %llu billion "
                    "instructions, %llu M translated; pc 0x%08X in %s\n",
                    static_cast<double>(guest_monotonic_ns()) * 1e-9, s.id,
                    (unsigned long long)(s.consumed / 1'000'000'000),
                    (unsigned long long)(translated() / 1'000'000), regs[15],
                    loader_.symbolize(regs[15]).c_str());
        std::fflush(stdout);
      }
      continue;
    }
    std::printf("[env ] thread %d stopped early at 0x%08X (%s), reason %u\n",
                s.id, regs[15], loader_.symbolize(regs[15]).c_str(),
                static_cast<unsigned>(reason));
    return regs[0];
  }
}

void Env::redirect(u32 pc, u32 lr) {
  GuestThread& s = self();
  auto& regs = s.jit->Regs();
  regs[14] = lr;
  regs[15] = pc & ~1u;
  auto cpsr = s.jit->Cpsr();
  if (pc & 1) {
    cpsr |= (1u << 5);
  } else {
    cpsr &= ~(1u << 5);
  }
  s.jit->SetCpsr(cpsr);
  s.redirected = true;
  s.jit->HaltExecution();
}

void Env::fatal(const char* what) {
  std::printf("[env ] guest stopped: %s\n", what);
  fatal_ = true;
  self().jit->HaltExecution();
}

// The page table lets the recompiler reach guest memory directly instead of
// calling back for every load and store. The address space is one flat
// allocation, so each entry is base + page. Unmapped pages stay null and fall
// back to the callbacks, which is how the null guard keeps working.
std::unique_ptr<PageTable> make_page_table(Memory& mem) {
  auto table = std::make_unique<PageTable>();
  table->fill(nullptr);
  constexpr u32 kPageBits = 12;
  const std::size_t first = layout::kImageBase >> kPageBits;
  const std::size_t last = layout::kSpaceSize >> kPageBits;
  for (std::size_t page = first; page < last && page < table->size(); ++page)
    (*table)[page] = mem.base() + (page << kPageBits);
  return table;
}

// ------------------------------------------------------------- callbacks
void Env::CallSVC(std::uint32_t swi) {
  GuestThread& s = self();
  if (swi == kReturnSwi) {
    s.halt = true;
    s.jit->HaltExecution();
    return;
  }
  if (swi >= kJniSvcBase) {
    ret(jni_dispatch(*this, swi - kJniSvcBase));
    return;
  }
  if (swi >= bound_.size()) {
    std::printf("[env ] svc #%u out of range\n", swi);
    ret(0);
    return;
  }
  {
    // No lock here: this runs on every import call from every guest thread,
    // and a mutex would serialise the whole boundary. Both counters are
    // atomic, and a diagnostic histogram does not need more than that.
    calls_[swi].fetch_add(1, std::memory_order_relaxed);
    if (svc_budget_ && ++svc_total_ > svc_budget_) {
      std::printf("[env ] import-call budget of %llu reached\n",
                  (unsigned long long)svc_budget_);
      dump_calls();
      fatal("budget exhausted - see the histogram above");
      return;
    }
  }
  // With the log on and no cap, the last line written is the last thing that
  // happened - which is the only way to see anything when the process is
  // killed by something no handler can catch. Output is unbuffered for the
  // same reason, so this is slow, and that is why it is an option.
  if (call_log_.load(std::memory_order_relaxed)) {
    static std::atomic<std::uint64_t> shown{0};
    const std::uint64_t n = shown.fetch_add(1);
    if (call_log_limit_ == 0 || n < call_log_limit_)
      std::printf("[call] t%d %s\n", current_thread_id(),
                  loader_.thunk_name(swi).c_str());
  }
  ThunkFn fn = bound_[swi];
  if (!fn) {
    bool first = false;
    {
      std::lock_guard<std::mutex> lock(report_lock_);
      first = !reported_[swi];
      reported_[swi] = true;
    }
    if (first)
      std::printf("[env ] unimplemented: %s\n", loader_.thunk_name(swi).c_str());
    ret(0);
    return;
  }
  // A thunk that throws would otherwise take the process with it, and
  // std::terminate says nothing about which one or what the guest had asked
  // for. Almost always this is a length the guest computed wrongly reaching a
  // container: naming the import turns an unexplained death into a lead.
  try {
    if (gl_thunk_[swi]) {
      gl_thunk_enter(loader_.thunk_name(swi).c_str());
      fn(*this);
      gl_thunk_leave();
      return;
    }
    fn(*this);
  } catch (const std::exception& ex) {
    if (gl_thunk_[swi]) gl_thunk_leave();
    std::printf("[env ] %s threw %s; r0-r3 %08X %08X %08X %08X\n",
                loader_.thunk_name(swi).c_str(), ex.what(), self().jit->Regs()[0],
                self().jit->Regs()[1], self().jit->Regs()[2],
                self().jit->Regs()[3]);
    std::fflush(stdout);
    ret(0);
  }
}

void Env::guard(u32 addr, const char* how) {
  mem_callbacks_.fetch_add(1, std::memory_order_relaxed);
  if (addr >= layout::kImageBase) return;   // mapped; nothing to say
  std::lock_guard<std::mutex> lock(report_lock_);
  if (null_reports_ >= 24) return;
  ++null_reports_;
  const u32 pc = t_current ? t_current->jit->Regs()[15] : 0;
  std::printf("[null] %s at guest 0x%08X from pc 0x%08X (%s)\n", how, addr, pc,
              loader_.symbolize(pc).c_str());
}

void Env::dump_threads() {
  std::lock_guard<std::mutex> lock(threads_lock_);
  std::printf("[thr ] %zu guest threads:\n", threads_.size());
  for (const auto& t : threads_) {
    const u32 pc = t->jit->Regs()[15];
    std::printf("       %d %-9s pc 0x%08X  %-52s %llu instructions\n", t->id,
                t->finished ? "finished" : "running", pc,
                loader_.symbolize(pc).c_str(),
                (unsigned long long)t->consumed);
  }
}

void Env::stop_all() {
  fatal_ = true;
  std::lock_guard<std::mutex> lock(threads_lock_);
  for (const auto& t : threads_)
    if (!t->finished) t->jit->HaltExecution();
}

void Env::dump_calls(int top) const {
  std::vector<std::pair<std::uint64_t, u32>> v;
  for (u32 i = 0; i < calls_.size(); ++i) {
    const std::uint64_t n = calls_[i].load(std::memory_order_relaxed);
    if (n) v.push_back({n, i});
  }
  std::sort(v.rbegin(), v.rend());
  std::printf("[env ] %zu distinct imports called, %llu calls total\n", v.size(),
              (unsigned long long)svc_total_.load());
  for (int i = 0; i < top && i < (int)v.size(); ++i)
    std::printf("       %10llu  %-34s %s\n", (unsigned long long)v[i].first,
                loader_.thunk_name(v[i].second).c_str(),
                bound_[v[i].second] ? "" : "(no host implementation)");
}

void Env::InterpreterFallback(u32 pc, std::size_t num_instructions) {
  std::printf("[env ] interpreter fallback at 0x%08X (%s), %zu instructions - "
              "the recompiler does not cover something here\n",
              pc, loader_.symbolize(pc).c_str(), num_instructions);
  self().jit->HaltExecution();
}

void Env::ExceptionRaised(u32 pc, Dynarmic::A32::Exception exception) {
  const char* what = "exception";
  switch (exception) {
    case Dynarmic::A32::Exception::NoExecuteFault:
      what = "jumped to something that is not code";
      break;
    case Dynarmic::A32::Exception::UndefinedInstruction:
      what = "undefined instruction";
      break;
    case Dynarmic::A32::Exception::UnpredictableInstruction:
      what = "unpredictable instruction";
      break;
    case Dynarmic::A32::Exception::DecodeError:
      what = "decode error";
      break;
    default:
      break;
  }
  std::printf("[env ] %s at 0x%08X (%s); lr 0x%08X (%s)\n", what, pc,
              loader_.symbolize(pc).c_str(), self().jit->Regs()[14],
              loader_.symbolize(self().jit->Regs()[14]).c_str());
  fatal(what);
}

}  // namespace wb
