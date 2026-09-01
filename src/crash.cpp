#include "crash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "elf_loader.h"
#include "env.h"
#include "guest.h"

namespace wb {
namespace {

Env* g_env = nullptr;

// What the guest was doing when the host died. This is the half of the
// report that actually names the cause: a host fault during a thunk is
// almost always the guest having passed something impossible.
void report_guest() {
  if (!g_env) return;
  GuestThread* t = current_guest_thread();
  if (!t || !t->jit) {
    std::printf("[fault] no guest thread on this host thread\n");
    return;
  }
  const auto& r = t->jit->Regs();
  std::printf("[fault] guest thread %d  pc %08X  %s\n", t->id, r[15],
              g_env->loader().symbolize(r[15]).c_str());
  std::printf("[fault] guest lr %08X  %s\n", r[14],
              g_env->loader().symbolize(r[14]).c_str());
  std::printf("[fault] guest r0-r3 %08X %08X %08X %08X  sp %08X\n", r[0], r[1],
              r[2], r[3], r[13]);
}

}  // namespace
}  // namespace wb

#if defined(_WIN32)

#include <windows.h>
// dbghelp.h must follow windows.h.
#include <dbghelp.h>
#include <tlhelp32.h>

namespace wb {
namespace {

const char* fault_name(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned access";
    default: return "exception";
  }
}

// Symbol lookup wants a scratch buffer with the name appended to the struct.
void name_address(HANDLE proc, DWORD64 addr, char* out, std::size_t out_size) {
  alignas(SYMBOL_INFO) char scratch[sizeof(SYMBOL_INFO) + 512] = {};
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(scratch);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 511;
  DWORD64 disp = 0;
  if (SymFromAddr(proc, addr, &disp, sym)) {
    IMAGEHLP_LINE64 line = {sizeof(line)};
    DWORD line_disp = 0;
    if (SymGetLineFromAddr64(proc, addr, &line_disp, &line)) {
      const char* file = std::strrchr(line.FileName, '\\');
      std::snprintf(out, out_size, "%s+0x%llX  (%s:%lu)", sym->Name,
                    (unsigned long long)disp, file ? file + 1 : line.FileName,
                    line.LineNumber);
    } else {
      std::snprintf(out, out_size, "%s+0x%llX", sym->Name,
                    (unsigned long long)disp);
    }
    return;
  }
  // No symbols: the module name still separates "our bug" from "the driver".
  HMODULE mod = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(addr), &mod) &&
      mod) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(mod, path, MAX_PATH);
    const char* base = std::strrchr(path, '\\');
    std::snprintf(out, out_size, "%s+0x%llX", base ? base + 1 : path,
                  (unsigned long long)(addr - reinterpret_cast<DWORD64>(mod)));
    return;
  }
  std::snprintf(out, out_size, "?");
}

void walk_stack(CONTEXT* ctx, HANDLE thread = GetCurrentThread()) {
  HANDLE proc = GetCurrentProcess();

  // StackWalk64 modifies the context it walks, so give it a copy.
  CONTEXT walk = *ctx;
  STACKFRAME64 frame = {};
#if defined(_M_X64)
  const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
  frame.AddrPC.Offset = walk.Rip;
  frame.AddrFrame.Offset = walk.Rbp;
  frame.AddrStack.Offset = walk.Rsp;
#elif defined(_M_ARM64)
  const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
  frame.AddrPC.Offset = walk.Pc;
  frame.AddrFrame.Offset = walk.Fp;
  frame.AddrStack.Offset = walk.Sp;
#else
  const DWORD machine = IMAGE_FILE_MACHINE_I386;
  frame.AddrPC.Offset = walk.Eip;
  frame.AddrFrame.Offset = walk.Ebp;
  frame.AddrStack.Offset = walk.Esp;
#endif
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Mode = AddrModeFlat;

  char name[640];
  int depth = 0;
  for (; depth < 32; ++depth) {
    if (!StackWalk64(machine, proc, thread, &frame, &walk, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
      break;
    if (frame.AddrPC.Offset == 0) break;
    name_address(proc, frame.AddrPC.Offset, name, sizeof(name));
    std::printf("[fault]   #%-2d %016llX  %s\n", depth,
                (unsigned long long)frame.AddrPC.Offset, name);
  }
  if (depth >= 3) return;

  // StackWalk64 needs unwind information, and generated code has none - so it
  // stops at the first JIT frame and the interesting caller is never named.
  // Reading the raw stack and reporting every slot that lands in a loaded
  // module recovers it: noisier than a real backtrace, but it says who called.
  std::printf("[fault] no usable unwind information; scanning the stack:\n");
#if defined(_M_X64)
  auto sp = reinterpret_cast<const std::uintptr_t*>(ctx->Rsp);
#elif defined(_M_ARM64)
  auto sp = reinterpret_cast<const std::uintptr_t*>(ctx->Sp);
#else
  auto sp = reinterpret_cast<const std::uintptr_t*>(ctx->Esp);
#endif
  int shown = 0;
  for (int i = 0; i < 4096 && shown < 24; ++i) {
    std::uintptr_t value = 0;
    if (IsBadReadPtr(sp + i, sizeof(value))) break;
    value = sp[i];
    if (value < 0x10000) continue;
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(value), &mod) ||
        !mod)
      continue;
    name_address(proc, value, name, sizeof(name));
    std::printf("[fault]   sp+%-5d %016llX  %s\n", i * 8,
                (unsigned long long)value, name);
    ++shown;
  }
}

LONG WINAPI on_fault(EXCEPTION_POINTERS* info) {
  // A fault inside the handler must not loop, and a fault the process turns
  // out to survive should not print a hundred times.
  static LONG reports = 0;
  if (InterlockedIncrement(&reports) > 3) return EXCEPTION_CONTINUE_SEARCH;

  // Whatever the run had printed is still in the buffer; it is the last thing
  // the engine did before dying and is worth more than the report.
  std::fflush(stdout);

  const DWORD code = info->ExceptionRecord->ExceptionCode;
  const auto pc =
      (std::uintptr_t)info->ExceptionRecord->ExceptionAddress;
  char where[640];
  name_address(GetCurrentProcess(), pc, where, sizeof(where));
  std::printf("\n[fault] %s (0x%08lX) on host thread %lu at %016llX  %s\n",
              fault_name(code), (unsigned long)code, GetCurrentThreadId(),
              (unsigned long long)pc, where);

  if (code == EXCEPTION_ACCESS_VIOLATION ||
      code == EXCEPTION_IN_PAGE_ERROR) {
    const ULONG_PTR* p = info->ExceptionRecord->ExceptionInformation;
    const char* how = p[0] == 0 ? "reading" : p[0] == 1 ? "writing"
                                                        : "executing";
    const auto addr = static_cast<std::uintptr_t>(p[1]);
    std::printf("[fault] %s %016llX\n", how, (unsigned long long)addr);

    // The useful question: was it a guest pointer that had run off the end?
    if (g_env) {
      const auto base =
          reinterpret_cast<std::uintptr_t>(g_env->mem().base());
      const std::uintptr_t offset = addr - base;
      // One space's worth past the end still points at the arena's neighbour,
      // which is exactly the overrun case worth naming.
      if (offset < std::uintptr_t(layout::kSpaceSize) * 2) {
        std::printf("[fault] that is guest address 0x%08llX%s\n",
                    (unsigned long long)offset,
                    offset >= layout::kSpaceSize ? "  (past the end of the "
                                                   "guest space)"
                                                 : "");
      }
    }
  }

  report_guest();
  std::printf("[fault] host stack:\n");
  walk_stack(info->ContextRecord);
  std::fflush(stdout);
  return EXCEPTION_EXECUTE_HANDLER;   // die, but having said why
}

}  // namespace

// A crash that reports nothing is worse than a crash. Three things cause
// that, and all three are fixable here:
//
//   - a block-buffered stdout loses whatever it was holding when the process
//     dies, which is exactly the part that says what went wrong;
//   - a stack overflow leaves no stack for the handler to run on, so the
//     report never happens and the process simply vanishes;
//   - an uncaught exception goes through std::terminate, which does not call
//     the exception filter at all.
void report_terminate() {
  std::fflush(stdout);
  std::printf("\n[fault] std::terminate: an exception went uncaught\n");
  report_guest();
  std::fflush(stdout);
  std::_Exit(3);
}

void report_exit() {
  std::printf("[run ] process exiting\n");
  std::fflush(stdout);
}

// Where every thread in the process currently is.
//
// A hang is harder to diagnose than a crash: nothing faults, so no handler
// runs, and the sampling profiler goes quiet because the guest is not
// executing either. What is left is to walk up to each thread and ask. That
// is what a debugger would do, and there is no reason a program cannot do it
// to itself.
//
// Threads are suspended one at a time and resumed immediately. Suspending a
// thread that holds the C runtime's lock while this one calls printf would
// deadlock, so nothing is printed until the thread is running again.
void crash_dump_all_threads(const char* why) {
  std::printf("\n[hang] %s - where every thread is:\n", why);
  std::fflush(stdout);

  const DWORD self = GetCurrentThreadId();
  const DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    std::printf("[hang] cannot enumerate threads\n");
    return;
  }

  THREADENTRY32 entry = {sizeof(entry)};
  for (BOOL ok = Thread32First(snap, &entry); ok;
       ok = Thread32Next(snap, &entry)) {
    if (entry.th32OwnerProcessID != pid) continue;
    if (entry.th32ThreadID == self) continue;

    HANDLE thread = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
        FALSE, entry.th32ThreadID);
    if (!thread) continue;

    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    bool have = false;
    if (SuspendThread(thread) != (DWORD)-1) {
      have = GetThreadContext(thread, &ctx) != 0;
      ResumeThread(thread);
    }
    if (have) {
      std::printf("[hang] thread %lu:\n", entry.th32ThreadID);
      walk_stack(&ctx, thread);
      std::fflush(stdout);
    }
    CloseHandle(thread);
  }
  CloseHandle(snap);
  std::printf("[hang] end of thread dump\n");
  std::fflush(stdout);
}

void crash_arm_thread() {
  // Both of these are per-thread, which is easy to miss and expensive to
  // miss. The stack reservation is what lets the handler run at all when the
  // stack is what overflowed. And in the Microsoft runtime set_terminate
  // installs a handler for the calling thread only - so a guest thread
  // reaching std::terminate would go to the default one, which is abort(),
  // which in a release build is __fastfail: instant death, no message, no
  // handler, nothing in the log at all.
  ULONG guarantee = 64 * 1024;
  SetThreadStackGuarantee(&guarantee);
  std::set_terminate(&report_terminate);
}

void crash_install(Env* env) {
  g_env = env;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::set_terminate(&report_terminate);
  std::atexit(&report_exit);
  // Leave the handler somewhere to stand when the stack is what overflowed.
  crash_arm_thread();
  // Load symbols now rather than inside the handler: a crashed process is a
  // bad place to be doing first-time initialisation.
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  SetUnhandledExceptionFilter(&on_fault);
  // The filter only runs if nothing else claims the exception, and a fault
  // inside a driver thread can die before it gets there. The vectored handler
  // sees every fault first-chance, so it is the one that reliably reports.
  // It never terminates - a fault something else genuinely handles should
  // leave a note and nothing more.
  AddVectoredExceptionHandler(0, [](EXCEPTION_POINTERS* info) -> LONG {
    switch (info->ExceptionRecord->ExceptionCode) {
      case EXCEPTION_ACCESS_VIOLATION:
      case EXCEPTION_ILLEGAL_INSTRUCTION:
      case EXCEPTION_STACK_OVERFLOW:
      case EXCEPTION_PRIV_INSTRUCTION:
        on_fault(info);
        break;
      default:
        break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
  });
}

}  // namespace wb

#else   // not Windows

namespace wb {
void crash_install(Env* env) { g_env = env; }
void crash_arm_thread() {}
void crash_dump_all_threads(const char*) {}
}  // namespace wb

#endif
