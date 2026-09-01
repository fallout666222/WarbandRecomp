// The dynarmic environment: memory callbacks, thunk dispatch, and the
// AAPCS argument reader the thunks are written against.
//
// The engine is built softfp (no Tag_ABI_VFP_args in .ARM.attributes), so
// float and double arguments arrive in core registers, not VFP. That is why
// Args below has no separate float sequence - a float is just next32()
// reinterpreted, and a double is next64().
//
// Guest threads: android_main spends 981 instructions on setup and then hands
// the game to threads it spawns, so pthread_create has to be real. Each guest
// thread gets its own Jit and its own guest stack while sharing memory, the
// page table and the thunk bindings - the shape an emulator uses for a
// multi-core guest. Per-run state therefore lives in GuestThread, and
// Env::self() finds the calling thread's.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "elf_loader.h"
#include "guest.h"

namespace wb {

class Env;

using ThunkFn = void (*)(Env&);

struct ThunkEntry {
  const char* name;
  ThunkFn fn;
};

// Generated stubs for the 221 imports that genuinely need the host.
extern const ThunkEntry kThunkTable[];
extern const std::size_t kThunkTableSize;

// Hand-written libc and runtime support, and the pthread family. Both win
// over the generated table.
extern const ThunkEntry kLibcTable[];
extern const std::size_t kLibcTableSize;
extern const ThunkEntry kThreadTable[];
extern const std::size_t kThreadTableSize;
extern const ThunkEntry kRuntimeTable[];
extern const std::size_t kRuntimeTableSize;
extern const ThunkEntry kAndroidTable[];
extern const std::size_t kAndroidTableSize;
extern const ThunkEntry kGlTable[];
extern const std::size_t kGlTableSize;
extern const ThunkEntry kFsTable[];
extern const std::size_t kFsTableSize;
extern const ThunkEntry kPrintfTable[];
extern const std::size_t kPrintfTableSize;
extern const ThunkEntry kMathTable[];
extern const std::size_t kMathTableSize;
extern const ThunkEntry kFmodTable[];
extern const std::size_t kFmodTableSize;
extern const ThunkEntry kInputTable[];
extern const std::size_t kInputTableSize;

// Where the extracted OBB tree lives, and where the guest's __sF array sits
// so stdout and stderr can be told apart from real files.
void fs_set_data_root(const std::string& path);
// Where anything the game writes goes - saves, settings, key bindings. Kept
// apart from the data root so the extracted OBB stays read-only in practice
// as well as in intent. Defaults to <data>/user.
void fs_set_save_root(const std::string& path);
void fs_set_stdio_base(u32 addr);

// Answers one JNI function-table call. Returns the value for r0.
u32 jni_dispatch(Env& e, u32 slot);

// CallSVC brackets every GL thunk with these: they take the GL lock and make
// sure the calling thread may talk to the driver at all. The name is only for
// the report when a call is dropped.
void gl_thunk_enter(const char* name);
void gl_thunk_leave();
// Calls glGetError after every GL thunk and names the one that failed. Off by
// default: each check is a pipeline flush.
void gl_set_error_checking(bool on);
// Drops every draw while keeping all other GL work. Separates "the scene is
// built wrong" from "the scene takes forever to build".
void gl_set_drawing(bool on);
// Waits for the GPU after every draw and names the slow ones. The only way to
// attribute a driver timeout to the draw that caused it.
void gl_set_draw_sync(bool on);
// Names every GL call before it is made. Verbose beyond reading, but the last
// line it writes is the call the graphics driver died in.
void gl_set_call_trace(bool on);
// Waits for the GPU every N draws. Keeps a heavy frame from arriving as one
// submission the driver's timeout will not sit through.
void gl_set_sync_every(int draws);
// Times every draw and adds the cost up per shader program, printing the
// frame's bill. Answers whether a frame the driver refuses to sit through is
// one impossible draw or a thousand ordinary ones.
void gl_set_profile(bool on);
// Before every draw, turns off any vertex attribute array whose indices reach
// past the end of the buffer it points at. The engine never disables an
// attribute array - it does not even import the call - so arrays left over
// from a previous, smaller mesh keep being fetched, and reading past a buffer
// is what hangs the GPU. On by default; this turns the repair off, leaving
// only the report.
void gl_set_guard_draws(bool on);
// Keeps a copy of every vertex buffer and reports the bounding box of the
// positions each draw reads. Triangles the size of the universe are the other
// way a small, legal draw takes longer than the driver will wait.
void gl_set_check_verts(bool on);
// How many draws have had a stale array turned off.
unsigned gl_stale_arrays();
// Ends the watchdog thread and waits for it. Called at shutdown: the thread
// is joinable, because Horizon has no working detach.
void gl_stop_watchdog();

// Maps a guest path onto the extracted data tree, with the module-then-common
// fallback and the doubled roots the engine produces. Shared with the audio
// layer, which opens files by the name the engine gives FMOD.
std::string fs_host_path(const std::string& guest);

// The screen the engine believes it has. Shared with the EGL layer so the
// value Java reports and the surface actually created agree.
u32 jni_screen_width();
u32 jni_screen_height();

// The monotonic clock the guest sees. Shared so that condition-variable
// deadlines are measured against the same base the guest used to compute
// them - otherwise every timed wait is nonsense.
std::int64_t guest_monotonic_ns();

// Which guest thread the calling host thread is running, 1-based, for
// pthread_self.
int current_thread_id();

// The calling host thread's guest thread, or null on a host-only thread such
// as a driver's. Only the crash reporter wants this - everything else runs on
// a thread that owns one and can use Env::self().
struct GuestThread;
GuestThread* current_guest_thread();

void init_libc_data(Env& e, const std::map<std::string, DataImport>& data);

using PageTable =
    std::array<std::uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;
std::unique_ptr<PageTable> make_page_table(Memory& mem);

// One emulated CPU. The thread that runs android_main is id 0.
struct GuestThread {
  std::unique_ptr<Dynarmic::A32::Jit> jit;
  int id = 0;
  u32 stack_top = 0;
  bool halt = false;
  bool redirected = false;
  bool finished = false;
  std::uint64_t ticks = 0;
  std::uint64_t consumed = 0;
  std::uint64_t slices = 0;         // how many budgets it has worked through
};

class Env final : public Dynarmic::A32::UserCallbacks {
 public:
  Env(Memory& mem, ElfLoader& loader) : mem_(mem), loader_(loader) {}
  ~Env();

  // Records the shared configuration and creates guest thread 0 bound to the
  // calling host thread. Must be called before anything runs.
  void start(PageTable* page_table, Dynarmic::ExclusiveMonitor* monitor);

  GuestThread& self();
  Dynarmic::A32::Jit* jit() { return self().jit.get(); }
  Memory& mem() { return mem_; }
  ElfLoader& loader() { return loader_; }

  void bind_thunks();

  // Runs a guest function on the calling thread until it returns.
  u32 call(u32 guest_pc, std::initializer_list<u32> args = {});

  // Starts a guest thread at entry(arg). Returns its id, 0 on failure.
  u32 spawn(u32 entry, u32 arg);
  // Runs a guest function once, on its own thread, with up to four arguments.
  // Used for callbacks the real Android would deliver asynchronously - an OBB
  // mount result, for instance - which cannot be called inline because
  // dynarmic's Run() is not reentrant.
  u32 spawn_call(u32 entry, std::initializer_list<u32> args);
  void join_all();
  int live_threads();

  // ------------------------------------------------------- argument reader
  class Args {
   public:
    explicit Args(Env& e) : e_(e), regs_(e.jit()->Regs()) {}

    u32 next32();
    u64 next64();                       // 8-byte aligned per AAPCS
    float nextf() { return bits<float>(next32()); }
    double nextd() { return bits<double>(next64()); }
    u32 next_ptr() { return next32(); }

    template <typename T>
    T* host_ptr() {
      return e_.mem_.host<T>(next32());
    }
    std::string next_str() { return e_.mem_.str(next32()); }

   private:
    template <typename T, typename S>
    static T bits(S s) {
      T t;
      std::memcpy(&t, &s, sizeof(T));
      return t;
    }
    Env& e_;
    std::array<std::uint32_t, 16>& regs_;
    int core_ = 0;
    u32 stack_ = 0;
    bool stack_started_ = false;
  };

  void ret(u32 v);
  void ret64(u64 v);
  void retf(float v);
  void retp(u32 guest_addr) { ret(guest_addr); }

  // Tail-calls guest code from inside a thunk. Run() is not reentrant, so
  // point the CPU at `pc` with `lr` as the return address and stop; the loop
  // in call() picks it up from there.
  void redirect(u32 pc, u32 lr);

  void fatal(const char* what);
  bool failed() const { return fatal_; }

  u32 errno_addr() const { return errno_addr_; }
  void set_errno_addr(u32 a) { errno_addr_ = a; }

  void set_glue(class AndroidGlue* g) { glue_ = g; }
  class AndroidGlue* glue() { return glue_; }

  // Strict IEEE instead of the device's flush-to-zero. A diagnostic only:
  // the device is the reference, and this is here to tell whether a wrong
  // number came from the difference.
  void set_ieee_floats(bool on) { ieee_floats_ = on; }
  void set_max_threads(int n) { max_threads_ = n; }
  void set_svc_budget(std::uint64_t n) { svc_budget_ = n; }
  void set_tick_budget(std::uint64_t n) { tick_budget_ = n; }
  // With tracing on, a spent tick budget is a sample point rather than a
  // failure: the pc is logged and execution continues. A small budget then
  // turns into a sampling profiler with symbol names, which is the cheapest
  // way to see where guest code actually goes.
  // Room for recompiled code, per guest thread. Too little and dynarmic
  // discards the lot and starts again, which looks exactly like the guest
  // being slow. Must be set before any thread is made.
  void set_code_cache_mb(std::size_t mb) { code_cache_mb_ = mb; }
  void set_trace(bool on) { trace_.store(on, std::memory_order_relaxed); }

  // Names every import call while it is on. Used to see exactly what the
  // engine does between two known points - opening a file and using its
  // contents, for instance.
  void set_call_log(bool on, std::uint64_t limit = 60) {
    call_log_limit_ = limit;
    call_log_.store(on);
  }
  bool call_log() const { return call_log_.load(); }
  void dump_calls(int top = 20) const;
  std::uint64_t mem_callbacks() const { return mem_callbacks_.load(); }
  // Snapshot of where every guest thread currently is. Racy by nature - the
  // threads keep running - but that is exactly what a watchdog wants.
  void dump_threads();
  void stop_all();
  std::uint64_t last_ticks() { return self().consumed; }

  // ------------------------------------------------ Dynarmic::UserCallbacks
  //
  // The page table serves every mapped page directly, so these only run for
  // the null guard - which is what makes a null dereference visible instead
  // of quietly reading zeroes out of the arena.
  std::uint8_t MemoryRead8(u32 a) override { guard(a, "read8"); return mem_.read8(a); }
  std::uint16_t MemoryRead16(u32 a) override { guard(a, "read16"); return mem_.read16(a); }
  std::uint32_t MemoryRead32(u32 a) override { guard(a, "read32"); return mem_.read32(a); }
  std::uint64_t MemoryRead64(u32 a) override { guard(a, "read64"); return mem_.read64(a); }
  void MemoryWrite8(u32 a, std::uint8_t v) override { guard(a, "write8"); mem_.write8(a, v); }
  void MemoryWrite16(u32 a, std::uint16_t v) override { guard(a, "write16"); mem_.write16(a, v); }
  void MemoryWrite32(u32 a, std::uint32_t v) override { guard(a, "write32"); mem_.write32(a, v); }
  void MemoryWrite64(u32 a, std::uint64_t v) override { guard(a, "write64"); mem_.write64(a, v); }

  bool MemoryWriteExclusive8(u32 a, std::uint8_t v, std::uint8_t x) override {
    return exclusive(a, v, x);
  }
  bool MemoryWriteExclusive16(u32 a, std::uint16_t v, std::uint16_t x) override {
    return exclusive(a, v, x);
  }
  bool MemoryWriteExclusive32(u32 a, std::uint32_t v, std::uint32_t x) override {
    return exclusive(a, v, x);
  }
  bool MemoryWriteExclusive64(u32 a, std::uint64_t v, std::uint64_t x) override {
    return exclusive(a, v, x);
  }

  // Only the engine's executable segment and the trampoline block are code.
  // Returning nullopt anywhere else raises a NoExecuteFault the moment
  // control goes astray, instead of translating page after page of zeroes.
  std::optional<std::uint32_t> MemoryReadCode(u32 addr) override {
    if ((addr >= exec_lo_ && addr < exec_hi_) ||
        (addr >= layout::kTrampolines &&
         addr < layout::kTrampolines + layout::kTrampSize) ||
        (addr >= layout::kReturnStub && addr < layout::kReturnStub + 8) ||
        (addr >= layout::kGuestCodeBase &&
         addr < layout::kGuestCodeBase + layout::kGuestCodeSize)) {
      // Counted, because dynarmic throws the whole code cache away when it
      // runs out of room and then retranslates everything it still needs.
      // That is invisible from outside - the guest makes progress, only far
      // slower - and this counter is what makes it visible: it should settle
      // near the size of the code actually reached, and grows without bound
      // if the cache is thrashing.
      translated_.fetch_add(1, std::memory_order_relaxed);
      return mem_.read32(addr);
    }
    return std::nullopt;
  }
  // Guest instructions handed to the recompiler, counting retranslations.
  std::uint64_t translated() const {
    return translated_.load(std::memory_order_relaxed);
  }
  void set_exec_range(u32 lo, u32 hi) { exec_lo_ = lo; exec_hi_ = hi; }

  void InterpreterFallback(u32 pc, std::size_t num_instructions) override;
  void CallSVC(std::uint32_t swi) override;
  void ExceptionRaised(u32 pc, Dynarmic::A32::Exception exception) override;
  void AddTicks(std::uint64_t t) override {
    GuestThread& s = self();
    s.ticks -= std::min(s.ticks, t);
  }
  std::uint64_t GetTicksRemaining() override { return self().ticks; }

 private:
  friend class Args;

  // Counts callback-served accesses. The page table should serve every
  // mapped page directly, so a large number here means it is not being used
  // and every guest load and store is paying for a callback.
  mutable std::atomic<std::uint64_t> mem_callbacks_{0};
  mutable std::atomic<std::uint64_t> translated_{0};
  std::size_t code_cache_mb_ = 32;
  void guard(u32 addr, const char* how);
  GuestThread* make_thread();
  void thread_body(GuestThread* t, u32 entry, u32 arg);

  template <typename T>
  bool exclusive(u32 addr, T value, T expected) {
    T current{};
    std::memcpy(&current, mem_.host<std::uint8_t>(addr), sizeof(T));
    if (current != expected) return false;
    std::memcpy(mem_.host<std::uint8_t>(addr), &value, sizeof(T));
    return true;
  }

  Memory& mem_;
  ElfLoader& loader_;
  PageTable* page_table_ = nullptr;
  Dynarmic::ExclusiveMonitor* monitor_ = nullptr;

  std::mutex threads_lock_;
  std::vector<std::unique_ptr<GuestThread>> threads_;
  std::vector<std::thread> host_threads_;

  std::vector<ThunkFn> bound_;
  std::vector<bool> reported_;
  std::vector<bool> gl_thunk_;      // needs the context current on this thread
  // Incremented on every single import call from every guest thread, so a
  // mutex here would serialise the whole boundary. Relaxed atomics are
  // plenty for a diagnostic counter.
  std::vector<std::atomic<std::uint64_t>> calls_;
  std::mutex report_lock_;

  std::atomic<std::uint64_t> svc_total_{0};
  std::uint64_t svc_budget_ = 0;
  std::uint64_t tick_budget_ = 0;
  bool fatal_ = false;
  u32 return_magic_ = 0;
  u32 errno_addr_ = 0;
  u32 exec_lo_ = 0, exec_hi_ = 0;
  int null_reports_ = 0;
  int max_threads_ = 16;
  bool ieee_floats_ = false;
  std::atomic<bool> trace_{false};
  std::atomic<bool> call_log_{false};
  std::uint64_t call_log_limit_ = 60;   // 0 means every call, forever
  class AndroidGlue* glue_ = nullptr;
};

}  // namespace wb
