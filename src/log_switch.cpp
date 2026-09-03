// Making the console say what happened.
//
// A Switch that shows "2144-0001" and nothing else is not debuggable. nxlink
// solves that, but it needs a PC listening on the same network at the moment
// the game runs, and in practice nobody has one running when the interesting
// crash happens. So everything printed also goes to a file on the SD card,
// which survives the process and can be read afterwards at leisure.
//
// The tee is installed as the process's standard-output device rather than by
// swapping stdout. newlib keeps one reent per thread, so assigning to stdout
// would only redirect the thread that did it, and the guest runs on eight of
// them; devoptab_list is process-wide and catches every one.
#if defined(WB_SWITCH)

#include <switch.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <sys/iosupport.h>
#include <sys/socket.h>
#include <unistd.h>

#include "crash.h"
#include "env.h"
#include "guest.h"
#include "log_switch.h"

// newlib's heap bounds. Declared here rather than inside the namespace: they
// are C symbols, and a C++ declaration in a namespace asks the linker for a
// mangled name that does not exist.
extern "C" char* fake_heap_start;
extern "C" char* fake_heap_end;

namespace wb {

// Whether the log is open yet. The constructor walk below is silent until it
// is, because the log is itself opened by one of the constructors it walks.
bool g_report_init = false;

namespace {

FILE* g_file = nullptr;
int g_sock = -1;
u64 g_last_flush = 0;
std::size_t g_pending = 0;

// Flushing after every line would mean an SD write per line, and there are
// tens of thousands of them during loading. Flushing never would mean losing
// the last few seconds - which is the part that matters - to a hard crash.
// A few kilobytes or a second, whichever comes first, keeps both.
constexpr std::size_t kFlushBytes = 4096;
constexpr u64 kFlushTicks = 19'200'000;    // one second, at 19.2 MHz

// Quiet by default.
//
// Loading writes tens of thousands of lines - a line per file opened, per
// texture, per buffer - and on a console every one of them is an SD card
// write on the path the game is waiting for. None of it is worth having while
// the game simply works, and all of it is worth having the moment it does
// not, so it is a switch rather than a deletion: --verbose in args.txt brings
// everything back.
//
// The list says what to drop rather than what to keep, so that a diagnostic
// added later is visible until somebody decides it is noise, instead of
// disappearing silently on the day it is needed.
bool g_verbose = false;

bool is_noise(const char* line, std::size_t len) {
  if (len < 6 || line[0] != '[') return false;
  static const char* const kNoisy[] = {
      "[fs  ]",   // a line per file opened, and the engine opens thousands
      "[init]",   // static constructors, one line each
      "[jni ]",   // every field and method the engine asks Java about
      "[tex ]", "[fbo ]", "[vbo ]", "[draw]", "[mtx ]", "[pos ]", "[chk ]",
      "[cond]",   // condition-variable traffic
      "[glc ]", "[ring]", "[prog]", "[dr->]", "[call]",
      "[inp ]",   // one line per touch and per button, all game long
  };
  for (const char* tag : kNoisy)
    if (std::memcmp(line, tag, 6) == 0) return true;
  return false;
}

// Lines arrive in whatever pieces printf hands over, so they are reassembled
// before being judged. Per thread rather than shared: the crash handler prints
// too, and a lock here would be a lock it could deadlock on.
thread_local char t_line[512];
thread_local std::size_t t_at = 0;

ssize_t tee_write_raw(const char* data, std::size_t len);

// Splits the incoming bytes into lines and passes on the ones worth keeping.
ssize_t tee_write_filtered(const char* data, std::size_t len) {
  for (std::size_t i = 0; i < len; ++i) {
    const char c = data[i];
    if (t_at < sizeof(t_line) - 1) t_line[t_at++] = c;
    if (c != 10) continue;
    if (!is_noise(t_line, t_at)) tee_write_raw(t_line, t_at);
    t_at = 0;
  }
  return static_cast<ssize_t>(len);
}

ssize_t tee_write(struct _reent*, void*, const char* data, std::size_t len) {
  if (!g_verbose) return tee_write_filtered(data, len);
  return tee_write_raw(data, len);
}

ssize_t tee_write_raw(const char* data, std::size_t len) {
  if (g_sock >= 0) {
    // Best effort: a host that has gone away must not stall the game.
    std::size_t sent = 0;
    while (sent < len) {
      const ssize_t n = send(g_sock, data + sent, len - sent, 0);
      if (n <= 0) { close(g_sock); g_sock = -1; break; }
      sent += static_cast<std::size_t>(n);
    }
  }
  if (g_file) {
    std::fwrite(data, 1, len, g_file);
    g_pending += len;
    const u64 now = armGetSystemTick();
    if (g_pending >= kFlushBytes || now - g_last_flush >= kFlushTicks) {
      std::fflush(g_file);
      g_pending = 0;
      g_last_flush = now;
    }
  }
  return static_cast<ssize_t>(len);
}

devoptab_t g_tee = {};

}  // namespace

void switch_log_set_verbose(bool on) {
  g_verbose = on;
  std::printf("[nx  ] logging %s\n", on ? "everything" : "the quiet set - "
              "put --verbose in args.txt for the rest");
}

void switch_log_buffered() {
  if (g_file) std::setvbuf(g_file, nullptr, _IOFBF, 32 * 1024);
}

void switch_log_flush() {
  if (g_file) {
    std::fflush(g_file);
    g_pending = 0;
  }
}

const char* switch_log_open(const char* path) {
  if (g_file) return "already open";

  g_file = std::fopen(path, "w");
  if (!g_file) {
    path = "sdmc:/warband.log";
    g_file = std::fopen(path, "w");
  }
  // Unbuffered to begin with. Buffering is a performance decision that only
  // makes sense once the game is running; during startup every line has to
  // survive the next instruction.
  if (g_file) std::setvbuf(g_file, nullptr, _IONBF, 0);
  g_last_flush = armGetSystemTick();

  g_tee.structSize = sizeof(devoptab_t);
  g_tee.name = "wblog";
  g_tee.write_r = tee_write;
  devoptab_list[STD_OUT] = &g_tee;
  devoptab_list[STD_ERR] = &g_tee;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);

  return g_file ? path : nullptr;
}

void switch_log_attach_nxlink() {
  // false, false: take the socket but leave the file descriptors alone. The
  // tee writes to it directly, so nxlink still works while the file is being
  // written as well. Sockets are not up until main has started them, which is
  // why this is not part of opening the log.
  if (g_sock >= 0) return;
  g_sock = nxlinkConnectToHost(false, false);
  if (g_sock >= 0) std::printf("[nx  ] an nxlink host is listening\n");
}

void switch_fatal(const char* fmt, ...) {
  // Straight to the log as well, in case the screen is never seen.
  va_list list;
  va_start(list, fmt);
  char text[1024];
  std::vsnprintf(text, sizeof(text), fmt, list);
  va_end(list);
  std::printf("[nx  ] cannot start: %s\n", text);
  switch_log_flush();

  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  consoleInit(nullptr);
  std::fputs("Mount & Blade: Warband\n\n", stdout);
  std::fputs(text, stdout);
  std::fputs("\n\nPress A to exit.\n", stdout);
  consoleUpdate(nullptr);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_A) break;
  }
  consoleExit(nullptr);
}

const char* switch_launch_problem() {
  static char msg[640];

  // Ask the machine, do not deduce from how it was started.
  //
  // The first version of this checked the applet type, three syscall hints
  // and the process handle. Every one of those is a proxy, and proxies are
  // wrong in both directions: an emulator ran the game happily with no
  // process handle at all - libnx had chosen the code-memory mechanism, which
  // does not need one - while the check refused to start. What the port
  // actually requires is two things, and both can simply be tried.

  // One: memory that can be written and then executed. jitCreate picks its
  // own mechanism, so this tests whichever one this machine will really use.
  Jit probe = {};
  const Result rc = jitCreate(&probe, 0x1000);
  if (R_FAILED(rc)) {
    std::snprintf(msg, sizeof(msg),
                  "This process may not generate code: jitCreate failed with\n"
                  "2%03d-%04d.\n\n"
                  "Launched from the album, homebrew gets neither memory nor\n"
                  "permission to compile. Hold R while starting any game in\n"
                  "hbmenu - that is \"title takeover\" - and run Warband from\n"
                  "the menu that appears.",
                  R_MODULE(rc), R_DESCRIPTION(rc));
    return msg;
  }
  jitClose(&probe);

  // Two: room for the guest. The engine's whole address space is one
  // allocation, and the host needs its own on top - the recompiler's caches,
  // the graphics driver, the decoded textures. Refusing here with the two
  // numbers side by side is worth more than a std::bad_alloc later.
  const std::size_t heap =
      static_cast<std::size_t>(fake_heap_end - fake_heap_start);
  constexpr std::size_t kHostNeeds = 192u << 20;
  const std::size_t need = layout::kSpaceSize + kHostNeeds;
  if (heap < need) {
    std::snprintf(msg, sizeof(msg),
                  "Not enough memory: this process was given %llu MiB, and the\n"
                  "guest address space alone is %u MiB.\n\n"
                  "Applet type is %d; an application is 0. Launched from the\n"
                  "album a process is capped at a few hundred megabytes, so\n"
                  "hold R while starting any game in hbmenu - that is \"title\n"
                  "takeover\" - and run Warband from the menu that appears.\n\n"
                  "If it was launched that way already, this build wants more\n"
                  "than the console has: rebuild with a smaller -DWB_GUEST_MB.",
                  (unsigned long long)(heap >> 20), layout::kSpaceSize >> 20,
                  (int)appletGetAppletType());
    return msg;
  }
  return nullptr;
}

namespace {

// Open the log before anything else in the process runs.
//
// A crash inside a static constructor - and this binary has a great many,
// most of them the recompiler's - happens before main and would otherwise
// leave nothing at all behind, which is exactly the "it just shows an error
// code" report that cannot be acted on. Priority 101 is the earliest a user
// constructor may ask for, so this one runs first and every later failure has
// somewhere to be written down.
// What std::terminate has to say before the process ends.
//
// An uncaught exception during static initialisation is the likeliest way
// this binary dies before main: it does not fault, so no exception handler
// runs, and the default terminate calls abort, which on Horizon is an error
// code on the screen and nothing else.
void say_why_it_gave_up() {
  const char* what = "no exception";
  if (std::exception_ptr p = std::current_exception()) {
    try {
      std::rethrow_exception(p);
    } catch (const std::exception& ex) {
      what = ex.what();
    } catch (...) {
      what = "an exception that is not a std::exception";
    }
  }
  std::printf("[nx  ] terminate called: %s\n", what);

  // "no exception" means somebody called terminate outright - a noexcept
  // function that threw, a joinable std::thread destroyed, a pure virtual
  // call - and the message on its own names none of them. The return
  // addresses do, once they are put through addr2line, and the handler's own
  // address is what turns them into file offsets.
  std::printf("[nx  ] terminate handler at %p (subtract to get a file "
              "offset)\n", (void*)&say_why_it_gave_up);
  // Walked by hand rather than with __builtin_return_address(n), which is
  // documented as unreliable past the first frame. AArch64 keeps the frame
  // pointer in x29 and the chain is {previous fp, return address}; the checks
  // are what keep a bad chain from turning a diagnosis into a second crash.
  const std::uintptr_t start = (std::uintptr_t)__builtin_frame_address(0);
  std::uintptr_t fp = start;
  for (int depth = 0; depth < 12; ++depth) {
    if (fp == 0 || (fp & 15) != 0 || fp < start || fp - start > (64u << 20))
      break;
    const std::uintptr_t next = *(const std::uintptr_t*)fp;
    const std::uintptr_t ret = *(const std::uintptr_t*)(fp + 8);
    if (ret == 0) break;
    std::printf("[nx  ]   #%d  %p\n", depth, (void*)ret);
    if (next <= fp) break;
    fp = next;
  }
  switch_log_flush();
  std::_Exit(1);
}

__attribute__((constructor(101))) void open_the_log_first() {
  const char* where = switch_log_open("sdmc:/switch/warband/warband.log");
  std::set_terminate(&say_why_it_gave_up);
  std::printf("[nx  ] process started, log at %s\n", where ? where : "nowhere");

  u64 total = 0, used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  std::printf("[nx  ] %llu MiB total, %llu MiB used, %lld MiB heap, "
              "applet type %d, heap override %s\n",
              (unsigned long long)(total >> 20),
              (unsigned long long)(used >> 20),
              (long long)((fake_heap_end - fake_heap_start) >> 20),
              (int)appletGetAppletType(),
              envHasHeapOverride() ? "taken from the loader" : "ours");
  std::printf("[nx  ] static constructors running; this one is at %p\n",
              (void*)&open_the_log_first);
  g_report_init = true;
}

__attribute__((destructor(101))) void note_the_end() {
  std::printf("[nx  ] process finished\n");
  switch_log_flush();
}

}  // namespace

}  // namespace wb

extern "C" {

// Running the static constructors ourselves, so that the one that dies can be
// named.
//
// newlib's __libc_init_array walks the same three arrays and says nothing.
// Replacing it costs nothing at runtime and turns "it stops somewhere during
// startup" into an address, which the .elf's symbol table can turn into a
// function and a file. The addresses are printed only once the log exists -
// the log is opened by one of these constructors, and the ones before it are
// the C++ runtime's own.
extern void (*__preinit_array_start[])(void) __attribute__((weak));
extern void (*__preinit_array_end[])(void) __attribute__((weak));
extern void (*__init_array_start[])(void) __attribute__((weak));
extern void (*__init_array_end[])(void) __attribute__((weak));
extern void _init(void);

void __libc_init_array(void) {
  std::size_t n = __preinit_array_end - __preinit_array_start;
  for (std::size_t i = 0; i < n; i++) __preinit_array_start[i]();
  _init();
  n = __init_array_end - __init_array_start;
  for (std::size_t i = 0; i < n; i++) {
    if (wb::g_report_init)
      std::printf("[init] %zu/%zu at %p\n", i + 1, n,
                  (void*)__init_array_start[i]);
    __init_array_start[i]();
  }
  if (wb::g_report_init) std::printf("[init] all %zu constructors done\n", n);
}

// A page of stack for the handler, because the faulting thread's own stack is
// exactly what cannot be trusted after a stack overflow - the most likely way
// for a recompiler to die.
alignas(16) u8 __nx_exception_stack[0x2000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
  static const char* kDesc[] = {"instruction abort", "other", "misaligned pc",
                                "misaligned sp", "trap", "?", "SError"};
  const unsigned d = ctx->error_desc;
  const char* what = (d >= 0x100 && d <= 0x106) ? kDesc[d - 0x100]
                     : (d == 0x301)             ? "bad SVC"
                                                : "unknown";

  std::printf("\n[FAULT] %s (0x%X)\n", what, d);
  std::printf("[FAULT] pc  0x%016llX   lr  0x%016llX\n",
              (unsigned long long)ctx->pc.x, (unsigned long long)ctx->lr.x);
  std::printf("[FAULT] sp  0x%016llX   far 0x%016llX  esr 0x%08X\n",
              (unsigned long long)ctx->sp.x, (unsigned long long)ctx->far.x,
              ctx->esr);
  // The .nro is loaded wherever Horizon puts it, so a raw pc means nothing on
  // its own. Printing a known function's address gives the slide, and the pc
  // minus that slide is what the .elf's symbol table can name.
  std::printf("[FAULT] handler at 0x%016llX (subtract to get a file offset)\n",
              (unsigned long long)(uintptr_t)&__libnx_exception_handler);
  // There are 29 of them, so the last row is short. Walking off the end of
  // the array to keep the loop tidy would mean reading someone else's memory
  // inside a crash handler, which is the worst place in the program for it.
  for (int i = 0; i < 29; ++i) {
    if (i % 4 == 0) std::printf("[FAULT] ");
    std::printf("x%-2d %016llX%s", i, (unsigned long long)ctx->cpu_gprs[i].x,
                (i % 4 == 3 || i == 28) ? "\n" : "  ");
  }

  // What the emulated CPU was doing, if this thread was running one. That is
  // the line that actually names the bug: a host address in the recompiler
  // says nothing, a guest pc points at a function in the engine.
  if (wb::GuestThread* t = wb::current_guest_thread()) {
    if (t->jit) {
      const auto& r = t->jit->Regs();
      std::printf("[FAULT] guest thread %d: pc 0x%08X  lr 0x%08X  sp 0x%08X\n",
                  t->id, r[15], r[14], r[13]);
    }
  }

  wb::switch_log_flush();
  // Returning from here resumes the faulting instruction, which faults again
  // for ever. Ending the process leaves the log written and closed.
  svcExitProcess();
}

}  // extern "C"

#endif  // WB_SWITCH
