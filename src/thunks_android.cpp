// The Android NDK surface: window, configuration, looper, input, storage,
// and the log.
//
// None of this is a translation layer - there is no Android underneath. Each
// function answers the question the engine is really asking: how big is the
// screen, what locale is this, where does the OBB live. The answers are ours
// to choose.
//
// __android_log_print is the exception and the most valuable one here: it is
// the engine talking. Its varargs are decoded rather than skipped, because
// the alternative is throwing away the only narration the engine offers.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "android_glue.h"
#include "guest_format.h"
#include "env.h"

namespace wb {
namespace {

// The window the engine will render into. Switch handheld is 1280x720; docked
// is 1920x1080. Starting at 720p keeps the first frame cheap.
constexpr u32 kWidth = 1280;
constexpr u32 kHeight = 720;

// ------------------------------------------------------------------- log
//
// __android_log_print(int prio, const char* tag, const char* fmt, ...).
// AAPCS puts prio, tag and fmt in r0-r2 and the first vararg in r3, so the
// same Args reader that serves every other thunk walks the list correctly -
// including the 8-byte alignment a double needs.
void t_log_print(Env& e) {
  Env::Args a(e);
  a.next32();                                  // priority
  std::string tag = e.mem().str(a.next32());
  std::string fmt = e.mem().str(a.next32(), 4096);
  RegArgs src(a);
  std::string msg = guest_format(e, src, fmt);
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
    msg.pop_back();
  std::printf("[game] %s: %s\n", tag.c_str(), msg.c_str());
  std::fflush(stdout);
  e.ret(0);
}

// ---------------------------------------------------------------- window
void t_window_width(Env& e) { e.ret(kWidth); }
void t_window_height(Env& e) { e.ret(kHeight); }

void t_window_set_geometry(Env& e) {
  Env::Args a(e);
  a.next32();
  u32 w = a.next32(), h = a.next32(), fmt = a.next32();
  std::printf("[andr] window geometry requested: %ux%u format %u\n", w, h, fmt);
  e.ret(0);
}

void t_activity_finish(Env& e) {
  std::printf("[andr] the engine asked to finish the activity\n");
  e.ret(0);
}

// --------------------------------------------------------- configuration
//
// A Tegra-era phone: English, 320 dpi, large screen, touch, no keyboard.
void t_config_new(Env& e) { e.ret(e.glue() ? e.glue()->config() : 1); }
void t_config_delete(Env& e) { e.ret(0); }
void t_config_from_asset_manager(Env& e) { e.ret(0); }

void t_config_language(Env& e) {
  Env::Args a(e);
  a.next32();
  u32 out = a.next32();
  if (out) {
    e.mem().write8(out, 'e');
    e.mem().write8(out + 1, 'n');
  }
  e.ret(0);
}

void t_config_country(Env& e) {
  Env::Args a(e);
  a.next32();
  u32 out = a.next32();
  if (out) {
    e.mem().write8(out, 'U');
    e.mem().write8(out + 1, 'S');
  }
  e.ret(0);
}

void t_config_density(Env& e) { e.ret(320); }
void t_config_sdk(Env& e) { e.ret(19); }          // Android 4.4
void t_config_orientation(Env& e) { e.ret(2); }   // landscape
void t_config_touchscreen(Env& e) { e.ret(3); }   // finger
void t_config_screen_size(Env& e) { e.ret(3); }   // large
void t_config_screen_long(Env& e) { e.ret(2); }   // long
void t_config_keyboard(Env& e) { e.ret(1); }      // nokeys
void t_config_navigation(Env& e) { e.ret(1); }    // nonav
void t_config_keys_hidden(Env& e) { e.ret(3); }   // soft
void t_config_nav_hidden(Env& e) { e.ret(2); }    // hidden
void t_config_ui_mode_type(Env& e) { e.ret(1); }  // normal
void t_config_ui_mode_night(Env& e) { e.ret(1); } // notnight
void t_config_mcc(Env& e) { e.ret(0); }
void t_config_mnc(Env& e) { e.ret(0); }

// The looper and the input queue live in thunks_input.cpp: they are one
// mechanism with the event accessors, not part of the window and storage
// surface here.

// --------------------------------------------------------------- storage
//
// The OBB is already extracted on disk, so "mounting" is answering with the
// directory it was extracted to.
void t_storage_new(Env& e) { e.ret(e.glue() ? e.glue()->storage() : 1); }
void t_storage_delete(Env& e) { e.ret(0); }

void t_storage_mount_obb(Env& e) {
  Env::Args a(e);
  a.next32();
  std::string path = e.mem().str(a.next32());
  std::printf("[andr] mount obb: %s\n", path.c_str());
  if (e.glue()) e.glue()->note_obb_mounted(path);
  e.ret(1);
}

void t_storage_is_mounted(Env& e) { e.ret(1); }

void t_storage_mounted_path(Env& e) {
  Env::Args a(e);
  a.next32();
  a.next32();                        // the .obb path being asked about
  e.ret(e.glue() ? e.glue()->data_root_addr() : 0);
}

void t_storage_unmount(Env& e) { e.ret(1); }

}  // namespace

const ThunkEntry kAndroidTable[] = {
    {"__android_log_print", &t_log_print},

    {"ANativeWindow_getWidth", &t_window_width},
    {"ANativeWindow_getHeight", &t_window_height},
    {"ANativeWindow_setBuffersGeometry", &t_window_set_geometry},
    {"ANativeActivity_finish", &t_activity_finish},

    {"AConfiguration_new", &t_config_new},
    {"AConfiguration_delete", &t_config_delete},
    {"AConfiguration_fromAssetManager", &t_config_from_asset_manager},
    {"AConfiguration_getLanguage", &t_config_language},
    {"AConfiguration_getCountry", &t_config_country},
    {"AConfiguration_getDensity", &t_config_density},
    {"AConfiguration_getSdkVersion", &t_config_sdk},
    {"AConfiguration_getOrientation", &t_config_orientation},
    {"AConfiguration_getTouchscreen", &t_config_touchscreen},
    {"AConfiguration_getScreenSize", &t_config_screen_size},
    {"AConfiguration_getScreenLong", &t_config_screen_long},
    {"AConfiguration_getKeyboard", &t_config_keyboard},
    {"AConfiguration_getNavigation", &t_config_navigation},
    {"AConfiguration_getKeysHidden", &t_config_keys_hidden},
    {"AConfiguration_getNavHidden", &t_config_nav_hidden},
    {"AConfiguration_getUiModeType", &t_config_ui_mode_type},
    {"AConfiguration_getUiModeNight", &t_config_ui_mode_night},
    {"AConfiguration_getMcc", &t_config_mcc},
    {"AConfiguration_getMnc", &t_config_mnc},

    {"AStorageManager_new", &t_storage_new},
    {"AStorageManager_delete", &t_storage_delete},
    {"AStorageManager_mountObb", &t_storage_mount_obb},
    {"AStorageManager_isObbMounted", &t_storage_is_mounted},
    {"AStorageManager_getMountedObbPath", &t_storage_mounted_path},
    {"AStorageManager_unmountObb", &t_storage_unmount},
};

const std::size_t kAndroidTableSize =
    sizeof(kAndroidTable) / sizeof(kAndroidTable[0]);

}  // namespace wb
