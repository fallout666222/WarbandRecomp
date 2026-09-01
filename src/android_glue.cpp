// A minimal Android environment for the engine to start against.
//
// android_main receives a struct android_app*. Passing null gets as far as
// the first JNI call and then jumps to address zero, because the engine reads
// app->activity->env and calls through it. So the glue has to provide:
//
//   android_app -> ANativeActivity -> JNIEnv -> a table of ~250 functions
//
// Every JNI slot points at its own svc trampoline, so a call that matters
// announces itself by name-and-index instead of crashing. Opaque handles
// (AConfiguration, ALooper, ANativeWindow) are non-null tokens the matching
// thunks recognise.
//
// Field offsets follow the 32-bit NDK layout of the era. The engine carries
// its own copy of native_app_glue, so the early fields - activity, config,
// looper, window - are the ones that have to be right.

#include <cstdio>
#include <cstring>

#include "android_glue.h"

#include "input.h"

namespace wb {
namespace {

constexpr u32 kSvcAl = 0xEF000000;
constexpr u32 kBxLr = 0xE12FFF1E;

// The glue borrows the region reserved for a future guest libc image. When
// that image arrives, move this somewhere else.
constexpr u32 kGlueBase = layout::kLibcBase;
constexpr u32 kGlueSize = 0x10000;

// JNINativeInterface has grown over the years; 250 slots covers every version
// the engine could have been built against.
constexpr u32 kJniSlots = 250;

}  // namespace

AndroidGlue::AndroidGlue(Env& env) : env_(env), bump_(kGlueBase, kGlueBase + kGlueSize) {}

u32 AndroidGlue::string(const char* text) {
  u32 n = static_cast<u32>(std::strlen(text)) + 1;
  u32 p = bump_.alloc(n, 4);
  env_.mem().copy_in(p, text, n);
  return p;
}

u32 AndroidGlue::build() {
  Memory& m = env_.mem();

  // ---- JNI function table, one trampoline per slot
  BumpAllocator tramp(env_.loader().trampoline_next(),
                      layout::kTrampolines + layout::kTrampSize);
  u32 table = bump_.alloc(kJniSlots * 4, 16);
  for (u32 i = 0; i < kJniSlots; ++i) {
    u32 stub = tramp.alloc(8, 8);
    if (!stub) {
      std::printf("[glue] out of trampoline space at JNI slot %u\n", i);
      break;
    }
    m.write32(stub, kSvcAl | ((kJniSvcBase + i) & 0x00FFFFFF));
    m.write32(stub + 4, kBxLr);
    m.write32(table + i * 4, stub);
  }
  // JNIEnv is a pointer to a pointer to the table.
  u32 jni_env = bump_.alloc(4, 4);
  m.write32(jni_env, table);

  jni_env_ = jni_env;

  // JavaVM has the same shape and needs the same treatment - an empty table
  // is a jump to zero the moment a thread attaches itself.
  constexpr u32 kVmSlots = 16;
  u32 vm_table = bump_.alloc(kVmSlots * 4, 16);
  for (u32 i = 0; i < kVmSlots; ++i) {
    u32 stub = tramp.alloc(8, 8);
    if (!stub) break;
    m.write32(stub, kSvcAl | ((kVmSvcBase + i) & 0x00FFFFFF));
    m.write32(stub + 4, kBxLr);
    m.write32(vm_table + i * 4, stub);
  }
  u32 java_vm = bump_.alloc(4, 4);
  m.write32(java_vm, vm_table);

  // ---- opaque handles the A*_ thunks hand back
  config_ = bump_.alloc(64, 16);
  looper_ = bump_.alloc(64, 16);
  window_ = bump_.alloc(64, 16);
  asset_manager_ = bump_.alloc(64, 16);
  storage_ = bump_.alloc(64, 16);
  data_root_addr_ = string(data_root_.c_str());

  // ---- ANativeActivity
  u32 activity = bump_.alloc(0x28, 16);
  m.zero(activity, 0x28);
  m.write32(activity + 0x00, 0);            // callbacks
  m.write32(activity + 0x04, java_vm);      // vm
  m.write32(activity + 0x08, jni_env);      // env  <- the null deref that was
  m.write32(activity + 0x0C, 0x1000);       // clazz: a non-null jobject token
  m.write32(activity + 0x10, string("/data/data/com.taleworlds.mbwarband"));
  m.write32(activity + 0x14, data_root_addr_);
  m.write32(activity + 0x18, 19);           // sdkVersion: 4.4, matching 2014
  m.write32(activity + 0x1C, 0);            // instance
  m.write32(activity + 0x20, asset_manager_);
  m.write32(activity + 0x24, data_root_addr_);   // obbPath

  // ---- android_app
  //
  // Not quite the stock native_app_glue layout. android_main's prologue reads
  //     ldr r0, [r0, #56]     ; 0x38 -> JNIEnv*
  //     ldr r1, [r5, #60]     ; 0x3C -> jobject
  // and passes both straight to NvGetGamepadAxes. In the stock struct those
  // offsets are activityState and destroyRequested; here they hold NVIDIA's
  // cached thread environment, which pushes the two state fields to 0x40 and
  // 0x44. Everything below 0x38 does match stock - the engine writes
  // onAppCmd at 0x04 and onInputEvent at 0x08 a few instructions earlier.
  u32 app = bump_.alloc(0x100, 16);
  m.zero(app, 0x100);
  m.write32(app + 0x0C, activity);
  m.write32(app + 0x10, config_);
  m.write32(app + 0x1C, looper_);
  m.write32(app + 0x24, window_);
  m.write32(app + 0x38, jni_env);   // appThreadEnv
  m.write32(app + 0x3C, 0x1000);    // appThreadThis, a non-null jobject token
  m.write32(app + 0x40, 0);         // activityState
  m.write32(app + 0x44, 0);         // destroyRequested

  app_ = app;
  std::printf("[glue] android_app 0x%08X, activity 0x%08X, JNIEnv 0x%08X "
              "(%u slots)\n",
              app, activity, jni_env, kJniSlots);
  return app;
}

// native_app_glue command codes. The engine's onAppCmd switches on these.
namespace cmd {
constexpr u32 kInitWindow = 1;
constexpr u32 kWindowResized = 3;
constexpr u32 kWindowRedrawNeeded = 4;
constexpr u32 kContentRectChanged = 5;
constexpr u32 kGainedFocus = 6;
constexpr u32 kConfigChanged = 8;
constexpr u32 kStart = 10;
constexpr u32 kResume = 11;
}  // namespace cmd

void AndroidGlue::send_app_cmd(u32 command, const char* name) {
  const u32 handler = env_.mem().read32(app_ + 0x04);   // app->onAppCmd
  if (!handler) {
    std::printf("[glue] no onAppCmd registered; cannot deliver %s\n", name);
    return;
  }
  std::printf("[glue] -> %s\n", name);
  std::fflush(stdout);
  env_.call(handler, {app_, command});
}

void AndroidGlue::send_startup_sequence() {
  // The order a real activity would see: started, resumed, then a window,
  // then focus. The engine's renderer is waiting on the window one.
  send_app_cmd(cmd::kStart, "APP_CMD_START");
  send_app_cmd(cmd::kResume, "APP_CMD_RESUME");
  send_app_cmd(cmd::kInitWindow, "APP_CMD_INIT_WINDOW");
  send_app_cmd(cmd::kWindowResized, "APP_CMD_WINDOW_RESIZED");
  send_app_cmd(cmd::kContentRectChanged, "APP_CMD_CONTENT_RECT_CHANGED");
  send_app_cmd(cmd::kConfigChanged, "APP_CMD_CONFIG_CHANGED");
  send_app_cmd(cmd::kGainedFocus, "APP_CMD_GAINED_FOCUS");
  send_app_cmd(cmd::kWindowRedrawNeeded, "APP_CMD_WINDOW_REDRAW_NEEDED");
}

// android_app lays its callbacks out userData, onAppCmd, onInputEvent, so the
// handler the engine registered - android_handle_input - is one word past the
// command handler we already use.
constexpr u32 kOnInputEventOffset = 0x08;

void AndroidGlue::pump_input() {
  const u32 handler = env_.mem().read32(app_ + kOnInputEventOffset);
  static bool announced = false;
  if (!announced) {
    announced = true;
    std::printf("[inp ] app->onInputEvent = 0x%08X (%s)\n", handler,
                env_.loader().symbolize(handler).c_str());
  }
  if (!handler) return;
  for (int guard = 0; guard < 64; ++guard) {
    const u32 event = input_take_next();
    if (!event) return;
    const u32 handled = env_.call(handler, {app_, event});
    static int shown = 0;
    if (++shown <= 8)
      std::printf("[inp ] delivered event 0x%08X -> %u\n", event, handled);
  }
}

constexpr u32 kStatusOffset = 0x58;

void AndroidGlue::set_status(u32 bits) {
  env_.mem().write32(app_ + kStatusOffset, bits);
  std::printf(
      "[glue] app status = 0x%X (running%s active%s focused%s surface%s)\n",
      bits, (bits & 1) ? "+" : "-", (bits & 2) ? "+" : "-",
      (bits & 4) ? "+" : "-", (bits & 8) ? "+" : "-");
}

u32 AndroidGlue::status() const {
  return env_.mem().read32(app_ + kStatusOffset);
}

u32 AndroidGlue::alloc_bytes(std::size_t n) {
  return bump_.alloc(static_cast<u32>(n), 4);
}

void AndroidGlue::report_jni(u32 slot) {
  if (slot >= seen_.size()) seen_.resize(slot + 1, false);
  if (seen_[slot]) return;
  seen_[slot] = true;
  std::printf("[jni ] slot %u called (returning 0)\n", slot);
}

}  // namespace wb
