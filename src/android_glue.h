#pragma once

#include <string>
#include <vector>

#include "env.h"
#include "guest.h"

namespace wb {

// SVC numbers at or above this are JNI table slots rather than imports.
// Import indices come from the loader and never approach it.
constexpr u32 kJniSvcBase = 0x00800000;

// JavaVM has its own, much smaller table (JNIInvokeInterface). Its slots get
// svc numbers above the JNIEnv ones so a single dispatcher can tell them
// apart. AttachCurrentThread is the one that matters: the render thread calls
// it, and an empty table means a jump to address zero.
constexpr u32 kVmSvcBase = 0x00800400;

class AndroidGlue {
 public:
  explicit AndroidGlue(Env& env);

  void set_data_root(std::string path) { data_root_ = std::move(path); }

  // Builds android_app and everything it points at. Returns the guest pointer
  // to pass as android_main's argument.
  u32 build();

  u32 app() const { return app_; }
  u32 config() const { return config_; }
  u32 looper() const { return looper_; }

  // Hands every queued input event to the engine's own handler. Called from
  // the watchdog thread, which is the same thread the lifecycle commands go
  // out on and is otherwise idle.
  void pump_input();
  u32 window() const { return window_; }
  u32 asset_manager() const { return asset_manager_; }
  u32 storage() const { return storage_; }
  u32 jni_env() const { return jni_env_; }
  u32 data_root_addr() const { return data_root_addr_; }
  void note_obb_mounted(const std::string& path) { last_obb_ = path; }
  const std::string& data_root() const { return data_root_; }
  // Scratch space in guest memory for things the glue has to materialise -
  // JNI string bytes, mainly.
  u32 alloc_bytes(std::size_t n);

  // Calls the engine's own onAppCmd, which is how native_app_glue delivers
  // lifecycle events. android_main registered it at app+0x04 before it
  // returned, and nothing else will ever call it here.
  void send_app_cmd(u32 cmd, const char* name);

  // NVIDIA's glue keeps a status bitfield at app+0x58 and android_main loops
  // on `nv_app_status_running`, which is bit 0. Nothing sets it until a
  // lifecycle command arrives, and the check happens first - so the loop
  // would exit before the app ever started. Bits: 1 running, 2 active,
  // 4 focused, 8 valid surface; all four together mean "interactable".
  void set_status(u32 bits);
  u32 status() const;
  void send_startup_sequence();

  // Called from Env when a JNI slot is invoked.
  void report_jni(u32 slot);

 private:
  u32 string(const char* text);

  Env& env_;
  BumpAllocator bump_;
  std::string data_root_ = "/sdcard/Android/obb/com.taleworlds.mbwarband";
  std::vector<bool> seen_;
  u32 app_ = 0, config_ = 0, looper_ = 0, window_ = 0, asset_manager_ = 0;
  u32 storage_ = 0, data_root_addr_ = 0, jni_env_ = 0;
  std::string last_obb_;
};

}  // namespace wb
