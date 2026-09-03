// Bring-up driver.
//
// Loads the engine, starts the JIT, runs the 95 static constructors, then
// enters android_main. Every import the engine touches prints itself once,
// so the first run produces the call order you implement against.
//
// Builds for the host PC by default; -DWB_PLATFORM=switch targets libnx.
#include <cstdio>
#include <chrono>
#include <atomic>
#include <exception>
#include <new>
#include <cstdlib>
#include <thread>
#include <memory>
#include <string>
#include <vector>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "android_glue.h"
#include "audio.h"
#include "crash.h"
#include "elf_loader.h"
#include "env.h"
#include "gamepad.h"
#include "gl_api.h"
#include "input.h"
#include "overlay.h"
#include "guest_code.h"
#include "guest.h"
#include "log_switch.h"

#if defined(WB_SWITCH)
#include <switch.h>

// Horizon fixes the heap once, before main, and libnx's default is far too
// small for a 2 GiB guest space. This takes everything the process is allowed
// minus a margin for the graphics driver and the JIT regions, which is the
// difference between the game loading and malloc failing on the first frame.
// It also means the .nro must be launched with title takeover: applet mode
// caps a process at about 448 MiB, and no amount of asking changes that.
//
// The guest space is taken off the front of the heap here, before newlib is
// told about the region at all. Asking malloc for it later does not work: a
// three-gigabyte heap will not serve a single two-gigabyte block, and the
// failure arrives as a plain null with nothing to say why. The heap is where
// the memory is, so the arena is simply the first part of it.
static char* carve_guest_space(char** base, std::size_t* size) {
  const std::size_t want = static_cast<std::size_t>(wb::layout::kSpaceSize);
  // Leave the host something to work in: the recompiler's code caches, the
  // graphics driver, the mixer and every host-side map live in what is left.
  constexpr std::size_t kHostFloor = 256u << 20;
  if (*size < want + kHostFloor) return nullptr;
  char* arena = *base;
  *base += want;
  *size -= want;
  return arena;
}

extern "C" void __libnx_initheap(void) {
  void* addr = nullptr;
  std::size_t size = 0;
  u64 available = 0, used = 0;

  svcGetInfo(&available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);

  // The loader usually hands the process a heap of its own choosing. Taking
  // it is not optional politeness: setting the heap size again behind the
  // loader's back is one of the ways an .nro dies before main with nothing but
  // an error code to show for it.
  if (envHasHeapOverride()) {
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    char* base = static_cast<char*>(envGetHeapOverrideAddr());
    std::size_t room = envGetHeapOverrideSize();
    wb::g_preallocated_space = carve_guest_space(&base, &room);
    fake_heap_start = base;
    fake_heap_end = base + room;
    return;
  }

  constexpr std::size_t kReserve = 32u << 20;    // driver and host objects
  if (available > used + kReserve)
    size = (available - used - kReserve) & ~std::size_t(0x1FFFFF);
  if (size == 0) size = 0x2000000 * 16;

  // Ask for less rather than giving up. This runs before main, before the
  // network log exists, so aborting here puts a bare error code on the screen
  // and nothing else - whereas a smaller heap still gets far enough to say
  // what went wrong.
  constexpr std::size_t kStep = 32u << 20;
  while (size >= kStep) {
    if (R_SUCCEEDED(svcSetHeapSize(&addr, size))) break;
    addr = nullptr;
    size -= kStep;
  }
  if (!addr)
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));

  extern char* fake_heap_start;
  extern char* fake_heap_end;
  char* base = static_cast<char*>(addr);
  wb::g_preallocated_space = carve_guest_space(&base, &size);
  fake_heap_start = base;
  fake_heap_end = base + size;
}
#endif

namespace {

struct Options {
  std::string so_path = "libMBExpMobile.so";
  std::uint64_t ticks = 500'000'000;   // per Run(), 0 = unlimited
  std::uint64_t svcs = 2'000'000;      // import calls, 0 = unlimited
  bool skip_main = false;              // stop after the constructors
  bool trace = false;                  // sample the pc every budget
  std::uint64_t trace_ticks = 200;     // sampling interval, once tracing
  int seconds = 5;                     // how long to let the game threads run
  int threads = 16;                    // cap on guest threads, for bisecting
  std::string data;                    // extracted OBB tree
  std::string save;                    // where saves and settings are written
  std::string shot;                    // frame-grab basename
  std::string record;                  // where to write the mixed sound
  int shot_every = 30;                 // seconds between grabs
  int calls = 25;                      // how much of the import histogram
  bool gl_errors = false;              // check glGetError after every GL call
  // Leave the floating-point unit in strict IEEE mode instead of the
  // flush-to-zero the device uses. Only for telling the two apart.
  bool ieee_floats = false;
  // The frame counter, off on the console.
  //
  // Its fragment shader has no precision qualifier, which desktop GL accepts
  // and GLES rejects outright - so on the Switch it compiles to nothing and
  // the log fills with the failure. Rather than carry a second shader for one
  // number, it is simply off there; --overlay turns it back on for anyone who
  // wants to fix it.
#if defined(WB_SWITCH)
  bool overlay = false;
#else
  bool overlay = true;
#endif
  bool drawing = true;                 // issue draw calls at all
  bool draw_sync = false;              // wait for the GPU after every draw
  bool profile = false;                // time every draw, bill it per program
  bool guard_draws = true;             // turn off arrays a draw would overrun
  bool check_verts = false;            // describe what each draw's box covers
  bool verbose = false;                // every log category, not just the quiet set
  bool gl_trace = false;               // name every GL call before it is made
  int sync_every = 64;                 // wait for the GPU every N draws
  int code_cache_mb = 32;              // recompiler room per guest thread
  bool log_calls = false;              // name every import call as it happens
  int log_calls_from = 0;              // seconds to wait before starting
  int trace_from = -1;                 // seconds to wait before sampling
  // Synthetic taps, "x,y@seconds". Driving the menu without a hand on the
  // mouse is what makes an input change checkable in a scripted run - and it
  // is the only way to test input at all on a console with no keyboard.
  struct Tap { float x, y; int at; };
  std::vector<Tap> taps;
  // A controller button, pressed on the same clock. The pad is the input path
  // this build of the engine was written for, and it is the one that cannot
  // be tested by clicking - so it gets the same treatment as the finger.
  struct Press { int code; int at; };
  std::vector<Press> presses;
  // A whole string, delivered the way the Switch's software keyboard delivers
  // one: all at once. That burst is the thing worth being able to reproduce -
  // it is what made a typed name come out one letter long.
  std::string typed;
  int typed_at = 0;
};

Options parse(int argc, char** argv) {
  Options o;
  bool positional_taken = false;
  // Which switches a log was made with is half of what the log means, and it
  // is not recoverable afterwards - so it goes in the first line.
  std::printf("[args]");
  for (int i = 1; i < argc; ++i) std::printf(" %s", argv[i]);
  std::printf("\n");
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--ticks=", 0) == 0) {
      o.ticks = std::strtoull(a.c_str() + 8, nullptr, 0);
    } else if (a.rfind("--svcs=", 0) == 0) {
      o.svcs = std::strtoull(a.c_str() + 7, nullptr, 0);
    } else if (a.rfind("--seconds=", 0) == 0) {
      o.seconds = std::atoi(a.c_str() + 10);
    } else if (a.rfind("--threads=", 0) == 0) {
      o.threads = std::atoi(a.c_str() + 10);
    } else if (a.rfind("--data=", 0) == 0) {
      o.data = a.substr(7);
    } else if (a.rfind("--save=", 0) == 0) {
      o.save = a.substr(7);
    } else if (a.rfind("--rec=", 0) == 0) {
      o.record = a.substr(6);
    } else if (a.rfind("--shot=", 0) == 0) {
      o.shot = a.substr(7);
    } else if (a.rfind("--shot-every=", 0) == 0) {
      o.shot_every = std::atoi(a.c_str() + 13);
    } else if (a.rfind("--calls=", 0) == 0) {
      o.calls = std::atoi(a.c_str() + 8);
    } else if (a.rfind("--type=", 0) == 0) {
      const std::size_t at = a.rfind('@');
      if (at != std::string::npos && at > 7) {
        o.typed = a.substr(7, at - 7);
        o.typed_at = std::atoi(a.c_str() + at + 1);
      } else {
        std::printf("[opt ] cannot read text from %s\n", a.c_str());
      }
    } else if (a.rfind("--press=", 0) == 0) {
      int code = 0, at = 0;
      if (std::sscanf(a.c_str() + 8, "%d@%d", &code, &at) == 2)
        o.presses.push_back({code, at});
      else
        std::printf("[opt ] cannot read a button from %s\n", a.c_str());
    } else if (a.rfind("--tap=", 0) == 0) {
      float x = 0, y = 0;
      int at = 0;
      if (std::sscanf(a.c_str() + 6, "%f,%f@%d", &x, &y, &at) == 3)
        o.taps.push_back({x, y, at});
      else
        std::printf("[opt ] cannot read a tap from %s\n", a.c_str());
    } else if (a.rfind("--log-calls", 0) == 0) {
      o.log_calls = true;
      if (a.size() > 12) o.log_calls_from = std::atoi(a.c_str() + 12);
    } else if (a == "--no-draw") {
      o.drawing = false;
    } else if (a == "--draw-sync") {
      o.draw_sync = true;
    } else if (a == "--profile") {
      o.profile = true;
    } else if (a == "--no-draw-guard") {
      o.guard_draws = false;
    } else if (a == "--check-verts") {
      o.check_verts = true;
    } else if (a == "--verbose") {
      o.verbose = true;
    } else if (a.rfind("--sync-every=", 0) == 0) {
      o.sync_every = std::atoi(a.c_str() + 13);
    } else if (a == "--gl-trace") {
      o.gl_trace = true;
    } else if (a == "--no-overlay") {
      o.overlay = false;
    } else if (a == "--overlay") {
      o.overlay = true;
    } else if (a == "--ieee") {
      o.ieee_floats = true;
    } else if (a == "--gl-errors") {
      o.gl_errors = true;
    } else if (a == "--trace") {
      o.trace = true;
    } else if (a.rfind("--code-cache=", 0) == 0) {
      o.code_cache_mb = std::atoi(a.c_str() + 13);
    } else if (a.rfind("--trace-from=", 0) == 0) {
      o.trace = true;
      o.trace_from = std::atoi(a.c_str() + 13);
    } else if (a.rfind("--trace-ticks=", 0) == 0) {
      o.trace = true;
      o.trace_ticks = std::strtoull(a.c_str() + 14, nullptr, 0);
    } else if (a == "--no-main") {
      o.skip_main = true;
    } else if (!a.empty() && a[0] != '-' && !positional_taken) {
      o.so_path = a;
      positional_taken = true;
    } else {
      std::printf("[opt ] ignoring argument: %s\n", a.c_str());
    }
  }
  return o;
}

int run(const Options& opt) {
  const std::string& so_path = opt.so_path;
  wb::Memory mem;
  wb::ElfLoader loader(mem);

  // Guest-side routines get first refusal on every import. Right now that is
  // integer division, which was 2.2 million boundary crossings a run.
  wb::GuestCode guest_code(mem);
  guest_code.build();
  guest_code.start_clock();
  loader.set_guest_resolver(
      [&guest_code](const std::string& name, wb::u32* out) {
        return guest_code.resolve(name, out);
      });

  std::string error;
  if (!loader.load(so_path, &error)) {
    std::printf("[load] failed: %s\n", error.c_str());
    return 1;
  }

  std::printf("[load] image at 0x%08X, %u trampolines, %zu constructors\n",
              wb::layout::kImageBase, loader.thunk_count(),
              loader.init_array().size());
  for (const auto& [name, d] : loader.data_imports())
    std::printf("[load] data import %-20s 0x%08X (%u bytes)\n", name.c_str(),
                d.addr, d.size);

  const wb::u32 entry = loader.android_main();
  if (!entry) {
    std::printf("[load] android_main not found\n");
    return 1;
  }
  std::printf("[load] android_main at 0x%08X\n", entry);

  wb::Env env(mem, loader);
  // From here on a host fault can be explained rather than just fatal.
  wb::crash_install(&env);
  env.set_exec_range(loader.exec_begin(), loader.exec_end());
  std::printf("[load] executable segment 0x%08X..0x%08X\n",
              loader.exec_begin(), loader.exec_end());
  auto page_table = wb::make_page_table(mem);
  // ldrex/strex reservations. The monitor indexes its tables by processor_id,
  // so it must be sized for every guest thread that can ever exist - sizing
  // it for one and then spawning more writes past the end of its arrays and
  // corrupts the host heap.
  Dynarmic::ExclusiveMonitor monitor(static_cast<std::size_t>(opt.threads));

  // Each guest thread gets its own Jit built from the same settings; Env owns
  // them. ARMv7 rather than the v8 default, little-endian always, memory
  // through the page table, cycle counting on so a runaway loop is catchable,
  // and a code cache sized by --code-cache: dynarmic discards every block it
  // holds when that fills, so too small a cache turns into a recompile storm
  // that is indistinguishable from the guest simply being slow.
  env.set_ieee_floats(opt.ieee_floats);
  env.set_code_cache_mb(static_cast<std::size_t>(opt.code_cache_mb));
  env.start(page_table.get(), &monitor);
  env.bind_thunks();
  guest_code.self_test(env);
  wb::init_libc_data(env, loader.data_imports());
  if (!opt.save.empty()) wb::fs_set_save_root(opt.save);
  wb::fs_set_data_root(opt.data.empty()
                           ? std::string("../Mount & Blade - Warband/gamedata")
                           : opt.data);

  env.set_max_threads(opt.threads);
  env.set_svc_budget(opt.svcs);
  env.set_tick_budget(opt.ticks);
  std::printf("[run ] budgets: %llu ticks per run, %llu import calls\n",
              (unsigned long long)opt.ticks, (unsigned long long)opt.svcs);

  std::printf("[run ] running %zu constructors\n", loader.init_array().size());
  int index = 0;
  for (wb::u32 slot : loader.init_array()) {
    wb::u32 fn = mem.read32(slot);
    if (!fn || fn == 0xFFFFFFFF) continue;
    env.call(fn);
    ++index;
    if (env.failed()) {
      std::printf("[run ] stopped in constructor %d of %zu\n", index,
                  loader.init_array().size());
      env.dump_calls(15);
      return 1;
    }
  }
  std::printf("[run ] %d constructors completed\n", index);

  if (opt.skip_main) {
    env.dump_calls(25);
    return 0;
  }

  // Tracing starts here: the constructors are known-good, and sampling them
  // at a useful rate would bury the interesting part.
  if (opt.trace) {
    // The budget is what makes sampling possible - Run() has to come back for
    // the pc to be readable at all - so it is set now either way. Whether the
    // samples are printed is a separate switch, because the interesting phase
    // usually starts minutes in and the menu before it would bury it.
    env.set_tick_budget(opt.trace_ticks);
    if (opt.trace_from < 0) env.set_trace(true);
  }

  // Breadcrumbs. Between the constructors and android_main there is a stretch
  // that prints nothing of its own, and on a console a death in it looks like
  // a death anywhere: the last line in the log is the one before the stretch.
  auto step = [](const char* what) {
    std::printf("[run ] %s\n", what);
    std::fflush(stdout);
  };

  if (opt.gl_errors) wb::gl_set_error_checking(true);
  step("host side: overlay");
  wb::overlay_set_visible(opt.overlay);
  step("host side: drawing and the GL watchdog thread");
  wb::gl_set_drawing(opt.drawing);
  step("host side: the watchdog is up");
  wb::gl_set_draw_sync(opt.draw_sync);
  wb::gl_set_call_trace(opt.gl_trace);
  wb::gl_set_sync_every(opt.sync_every);
  wb::gl_set_profile(opt.profile);
  wb::gl_set_check_verts(opt.check_verts);
  wb::gl_set_guard_draws(opt.guard_draws);
  if (!opt.record.empty()) wb::audio_record(opt.record.c_str());

  step("host side: done; building the android_app");
  wb::AndroidGlue glue(env);
  env.set_glue(&glue);
  const wb::u32 app = glue.build();
  step("android_app built");

  // android_main loops on nv_app_status_running, which reads bit 0 of the
  // status word - and it checks before any lifecycle command could arrive.
  // Starting at "running, active, focused, valid surface" is what a real
  // activity would already have established by this point.
  glue.set_status(0xF);

  // native_app_glue runs android_main on its own thread and pumps events from
  // another; do the same, or the loop would own the only thread here and
  // there would be nobody left to deliver commands to it.
  std::printf("[run ] starting android_main on its own guest thread\n");
  std::fflush(stdout);
  env.spawn(entry, app);

  // Wait for it to register its command handler before sending anything.
  for (int i = 0; i < 200 && mem.read32(app + 0x04) == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (mem.read32(app + 0x04) == 0)
    std::printf("[run ] android_main never registered onAppCmd\n");

  // The controller can only place touches once the surface size is known,
  // and the surface exists by the time the startup commands have gone out.
  wb::gamepad_init(static_cast<int>(wb::jni_screen_width()),
                   static_cast<int>(wb::jni_screen_height()));

  glue.send_startup_sequence();

  std::printf("[run ] %d guest threads running, watching for %d seconds\n",
              env.live_threads(), opt.seconds);
  std::fflush(stdout);
  // A grab every so often, rather than one at a chosen moment: booting takes
  // minutes under the recompiler, and a strip of frames says where it got to
  // far better than one picture taken at a guess.
  // Both the grabs and the synthetic taps are scheduled against the wall
  // clock rather than against loop iterations. A tick is nominally 50 ms, but
  // pumping the window and polling the pad take time too, and under a
  // sanitizer they take a great deal of it - so counting ticks quietly means
  // something different in every build.
  const auto started = std::chrono::steady_clock::now();
  auto elapsed_seconds = [&started] {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - started)
        .count();
  };
  int shot_index = 0;
  bool logging_calls = false;
  bool sampling = false;
  // Noticing that the game has stopped drawing.
  //
  // On its own thread, and that is the whole point. A hang produces no fault,
  // so no handler runs, and the guest stops retiring instructions so the
  // sampling profiler is quiet too - the frame counter is the only thing left
  // that still says anything. But the watchdog loop below cannot do the
  // checking, because delivering input runs guest code, and if the guest is
  // stuck holding a lock then the watchdog stops with it and never gets round
  // to noticing. A thread that does nothing but count survives that.
  std::atomic<bool> watching{true};
  std::thread stall_watch([&env, &watching] {
    std::printf("[run ] stall watchdog running\n");
    unsigned long long last = 0;
    int still = 0;
    bool reported = false;
    while (watching.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      const unsigned long long now_frames = wb::gl_frame_count();
      if (now_frames != last) {
        last = now_frames;
        still = 0;
        reported = false;
        continue;
      }
      if (now_frames == 0 || reported) continue;
      if (++still < 3) continue;
      reported = true;
      std::printf("[hang] no frame presented for %d seconds\n", still);
      env.dump_threads();
      wb::crash_dump_all_threads("the guest has stopped drawing");
    }
  });
  long long next_shot = opt.shot_every;
  std::vector<bool> tapped(opt.taps.size(), false);
  // Zero, so press and release land in the same iteration. That is not a
  // guess: a real mouse click through the window procedure arrives exactly
  // that way - Windows has both messages queued by the time the loop pumps -
  // and a real click is the one thing known to work.
  // How often this loop comes round. It is the input loop as much as the
  // watch loop: nothing the platform reads reaches the engine until
  // pump_input runs, so the tick is the latency of every button, stick and
  // click. Fifty milliseconds was fine for watching a load and is three
  // frames of lag to play through.
  constexpr int kTickMs = 5;
  constexpr int kFingerHeldTicks = 100 / kTickMs;
  struct Release { float x, y; int tick; };
  std::vector<Release> release_at;
  struct ReleasePress { int code; int tick; };
  std::vector<ReleasePress> release_press;
  std::vector<bool> pressed(opt.presses.size(), false);
  bool text_sent = false;
  // Counted in ticks this loop would be counting its own sleeps, and the
  // work between them is not free - a hundred and twenty seconds of ticks
  // took five minutes. The deadline is wall time.
  for (int i = 0; elapsed_seconds() < opt.seconds && env.live_threads() > 1 &&
                  !wb::gl_close_requested();
       ++i) {
    // Keep the window alive while the guest works: it only swaps when it
    // draws, and it draws rarely during startup.
    wb::gl_pump();
    // The pad is polled, not pushed, so this is where it gets read. Cheap
    // when nothing is plugged in.
    wb::gamepad_poll();
    const long long now = elapsed_seconds();
    // Turned on late on purpose: naming every import from the start buries
    // the loading phase in millions of lines, and the interesting part is
    // whatever happens just before the process dies.
    if (opt.trace && opt.trace_from >= 0 && !sampling && now >= opt.trace_from) {
      sampling = true;
      std::printf("[run ] sampling the guest from here\n");
      env.set_trace(true);
    }
    if (opt.log_calls && !logging_calls && now >= opt.log_calls_from) {
      logging_calls = true;
      std::printf("[run ] logging every import call from here\n");
      env.set_call_log(true, 0);
    }
    if (!opt.typed.empty() && !text_sent && now >= opt.typed_at) {
      text_sent = true;
      std::printf("[run ] typing \"%s\" the way a software keyboard would - "
                  "every character at once\n", opt.typed.c_str());
      for (char c : opt.typed) {
        wb::InputEvent ev;
        ev.kind = wb::InputEvent::Kind::Key;
        ev.source = wb::Source::Keyboard;
        // Android's keycodes for the characters a name can contain; anything
        // else goes as a character with no key, which is what an IME does for
        // symbols it has no key for.
        if (c >= 'a' && c <= 'z') ev.key_code = 29 + (c - 'a');
        else if (c >= 'A' && c <= 'Z') ev.key_code = 29 + (c - 'A');
        else if (c >= '0' && c <= '9') ev.key_code = 7 + (c - '0');
        else if (c == ' ') ev.key_code = 62;
        ev.unicode = static_cast<wb::u32>(static_cast<unsigned char>(c));
        ev.action = 0;
        wb::input_push(ev);
        ev.action = 1;
        wb::input_push(ev);
      }
    }
    for (std::size_t b = 0; b < opt.presses.size(); ++b) {
      if (pressed[b] || now < opt.presses[b].at) continue;
      pressed[b] = true;
      std::printf("[run ] gamepad button %d\n", opt.presses[b].code);
      wb::InputEvent down;
      down.kind = wb::InputEvent::Kind::Key;
      down.source = wb::Source::Gamepad;
      down.action = 0;
      down.key_code = opt.presses[b].code;
      wb::input_push(down);
      release_press.push_back({opt.presses[b].code, i + kFingerHeldTicks});
    }
    for (std::size_t r = 0; r < release_press.size();) {
      if (i < release_press[r].tick) {
        ++r;
        continue;
      }
      wb::InputEvent up;
      up.kind = wb::InputEvent::Kind::Key;
      up.source = wb::Source::Gamepad;
      up.action = 1;
      up.key_code = release_press[r].code;
      wb::input_push(up);
      release_press.erase(release_press.begin() + static_cast<long>(r));
    }
    for (std::size_t t = 0; t < opt.taps.size(); ++t) {
      const auto& tap = opt.taps[t];
      if (tapped[t] || now < tap.at) continue;
      tapped[t] = true;
      std::printf("[run ] finger down at %.0f,%.0f\n", tap.x, tap.y);
      wb::InputEvent down;
      down.kind = wb::InputEvent::Kind::Motion;
      down.action = 0;                 // AMOTION_EVENT_ACTION_DOWN
      down.x = tap.x;
      down.y = tap.y;
      wb::input_push(down);
      // The release is deliberately left for a later iteration. Pushing both
      // halves at once means no frame of the engine ever sees a finger on the
      // screen - it polls a state, not a queue - and the tap does nothing. A
      // real finger stays down for a tenth of a second, so this one does too.
      release_at.push_back({tap.x, tap.y, i + kFingerHeldTicks});
    }
    for (std::size_t r = 0; r < release_at.size();) {
      if (i < release_at[r].tick) {
        // A finger resting on glass still reports, every frame, that it is
        // there. The engine collapses the queued events into one "is it down"
        // flag per frame, so a gap with nothing in it is a frame with the
        // finger lifted.
        wb::InputEvent move;
        move.kind = wb::InputEvent::Kind::Motion;
        move.action = 2;               // ACTION_MOVE
        move.x = release_at[r].x;
        move.y = release_at[r].y;
        wb::input_push(move);
        ++r;
        continue;
      }
      std::printf("[run ] finger up at %.0f,%.0f\n", release_at[r].x,
                  release_at[r].y);
      wb::InputEvent up;
      up.kind = wb::InputEvent::Kind::Motion;
      up.action = 1;                   // ACTION_UP
      up.x = release_at[r].x;
      up.y = release_at[r].y;
      wb::input_push(up);
      release_at.erase(release_at.begin() + static_cast<long>(r));
    }

    glue.pump_input();
    if (!opt.shot.empty() && opt.shot_every > 0 && now >= next_shot) {
      next_shot = now + opt.shot_every;
      char path[512];
      std::snprintf(path, sizeof(path), "%s_%02d.bmp", opt.shot.c_str(),
                    ++shot_index);
      wb::gl_request_shot(path);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
  }

  watching.store(false);
  if (stall_watch.joinable()) stall_watch.join();

  env.dump_threads();
  env.dump_calls(opt.calls);
  env.stop_all();
  env.join_all();
  // The clock thread writes into the guest arena, so it has to stop before
  // anything releases it, and the audio device's feeding thread reads the
  // mixer. Both outlive main otherwise, and both fault on the way out.
  guest_code.stop_clock();
  wb::gl_stop_watchdog();
  wb::audio_stop();
  wb::audio_record_stop();
  return 0;
}

}  // namespace

#if defined(WB_SWITCH)
// Switches, from a file, because hbmenu launches an .nro with no arguments and
// every diagnostic in this build is a command-line switch. One per line or all
// on one line; blank lines and anything after a # are ignored.
std::vector<std::string> extra_arguments(const char* path) {
  std::vector<std::string> out;
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return out;
  std::string text;
  char buf[512];
  while (std::size_t n = std::fread(buf, 1, sizeof(buf), f))
    text.append(buf, n);
  std::fclose(f);
  text.push_back(10);            // so the last word is closed like the rest
  std::string word;
  bool comment = false;
  for (char c : text) {
    if (c == 10 || c == 13) comment = false;      // newline ends a comment
    if (c == '#') comment = true;
    if (comment) continue;
    if (c == ' ' || c == 9 || c == 10 || c == 13) {
      if (!word.empty()) out.push_back(word);
      word.clear();
      continue;
    }
    word.push_back(c);
  }
  if (!out.empty())
    std::printf("[args] %zu from %s\n", out.size(), path);
  return out;
}
#endif

int main(int argc, char** argv) {
  std::vector<std::string> args(argv, argv + argc);
#if defined(WB_SWITCH)
  for (std::string& a : extra_arguments("sdmc:/switch/warband/args.txt"))
    args.push_back(std::move(a));
#endif
  std::vector<char*> argp;
  argp.reserve(args.size());
  for (std::string& a : args) argp.push_back(a.data());
  Options opt = parse(static_cast<int>(argp.size()), argp.data());

#if defined(WB_SWITCH)
  // Logging goes to the SD card, and to nxlink as well when a host happens to
  // be listening. Not to a console: the console owns the framebuffer, and the
  // framebuffer is what the game needs, so printing to it and then handing it
  // to EGL gives you one or the other. A file gives both, and it is still
  // there after the console has shown an error code and gone back to the home
  // menu - which is the only moment the log is ever wanted.
  // The log is already open - a constructor did it before main - so this only
  // adds the network side for whoever happens to be listening.
  if (opt.verbose) wb::switch_log_set_verbose(true);
  socketInitializeDefault();
  wb::switch_log_attach_nxlink();
  std::printf("[nx  ] main() reached\n");
  wb::switch_log_flush();

  // Before anything else, and before a single byte is allocated: is this
  // process even allowed to run the game? Getting this wrong is the whole
  // reason the console shows a code instead of a picture, and the answer is
  // knowable in four kernel queries.
  if (const char* problem = wb::switch_launch_problem()) {
    wb::switch_fatal("%s", problem);
    wb::switch_log_flush();
    socketExit();
    return 1;
  }

  // Say how much memory this process was given before anything tries to use
  // it. Launched from the album applet a process gets about 448 MiB and the
  // guest space cannot fit; with title takeover it gets around 3.2 GiB and
  // can. Those two numbers are the first thing to look at when the console
  // shows an error code instead of a game.
  {
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    std::printf("[nx  ] applet type %d, heap override %s\n",
                (int)appletGetAppletType(),
                envHasHeapOverride() ? "yes" : "no");
    std::printf("[nx  ] memory: %llu MiB total, %llu MiB used, %lld MiB left "
                "for the host, guest space %u MiB %s\n",
                (unsigned long long)(total >> 20),
                (unsigned long long)(used >> 20),
                (long long)((fake_heap_end - fake_heap_start) >> 20),
                wb::layout::kSpaceSize >> 20,
                wb::g_preallocated_space
                    ? "carved out of the heap before newlib saw it"
                    : "NOT carved - the heap was too small");
    if (total >> 20 < (wb::layout::kSpaceSize >> 20) + 256u)
      std::printf("[nx  ] that is not enough. Launch with title takeover - "
                  "hold R on a game in hbmenu - or rebuild with a smaller "
                  "-DWB_GUEST_MB.\n");
    std::fflush(stdout);
  }

  // No romfs: the engine and its 800 MB of data live on the SD card next to
  // the .nro, because nothing that large belongs inside the executable.
  if (opt.so_path == "libMBExpMobile.so")
    opt.so_path = "sdmc:/switch/warband/libMBExpMobile.so";
  if (opt.data.empty()) opt.data = "sdmc:/switch/warband/gamedata";
  // Saves go beside the data rather than inside it, so replacing the game
  // data does not take the saves with it.
  if (opt.save.empty()) opt.save = "sdmc:/switch/warband/user";
  // Nothing is watching the clock here; the game runs until it is closed.
  if (opt.seconds == 5) opt.seconds = 24 * 60 * 60;
  // The import-call cap exists to bisect a hang: run to N calls, see where it
  // got. Every PC run turns it off with --svcs=0, and the .nro is launched
  // with no arguments at all - so on a console the default has to be the one
  // that plays the game. Two million calls is about as far as the main menu's
  // meshes.
  if (opt.svcs == 2'000'000) opt.svcs = 0;
  // Waiting for the GPU every 64 draws is a desktop-driver workaround - it
  // was how a frame was kept inside Windows' two-second timeout. Horizon has
  // no such timeout, and the wait is pure stall.
  if (opt.sync_every == 64) opt.sync_every = 0;
#endif

  // A failure here would otherwise be an uncaught exception, which on Horizon
  // means abort() and a bare error code on screen with nothing to read.
  int rc = 1;
  try {
    rc = run(opt);
  } catch (const std::bad_alloc&) {
    std::printf("[run ] out of host memory: could not reserve the %u MiB "
                "guest space.\n", wb::layout::kSpaceSize >> 20);
    std::fflush(stdout);
  } catch (const std::exception& ex) {
    std::printf("[run ] stopped: %s\n", ex.what());
    std::fflush(stdout);
  }

#if defined(WB_SWITCH)
  std::printf("\nPress + to exit.\n");
  wb::switch_log_flush();
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
  }
  wb::switch_log_flush();
  socketExit();
#endif
  return rc;
}
