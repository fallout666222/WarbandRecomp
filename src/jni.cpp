// Just enough JNI for the engine to get its answers.
//
// There is no Java here and there never will be, but the engine asks Java
// questions - most importantly where the OBB was mounted, which it fetches
// through the function table rather than through AStorageManager. So this is
// a tiny fake JVM: classes, methods and objects are integer tokens, and a
// call whose signature says it returns a String gets one back.
//
// Signatures are the trick. Recording the name and descriptor at GetMethodID
// time means the reply can be decided later from the descriptor alone -
// `()Ljava/lang/String;` gets the data root, an int-returning method gets 0 -
// without hard-coding which method of which class the engine happens to use.
//
// Every slot that is not implemented reports itself with its index, so the
// next thing the engine wants shows up by name on the first run.

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "android_glue.h"
#include "env.h"
#include "gamepad.h"
#include "input.h"

namespace wb {
namespace {

// JNINativeInterface slot numbers (index = byte offset / 4). The engine is
// C++, and jni.h's C++ wrappers forward varargs to the ...V forms, which is
// why CallVoidMethodV shows up rather than CallVoidMethod.
enum Slot : u32 {
  kGetVersion = 4,
  kFindClass = 6,
  kThrow = 13,
  kThrowNew = 14,
  kExceptionOccurred = 15,
  kExceptionDescribe = 16,
  kExceptionClear = 17,
  kPushLocalFrame = 19,
  kPopLocalFrame = 20,
  kNewGlobalRef = 21,
  kDeleteGlobalRef = 22,
  kDeleteLocalRef = 23,
  kIsSameObject = 24,
  kNewLocalRef = 25,
  kEnsureLocalCapacity = 26,
  kGetObjectClass = 31,
  kIsInstanceOf = 32,
  kGetMethodID = 33,
  kCallObjectMethod = 34,
  kCallObjectMethodV = 35,
  kCallObjectMethodA = 36,
  kCallBooleanMethod = 37,
  kCallBooleanMethodV = 38,
  kCallIntMethod = 49,
  kCallIntMethodV = 50,
  kCallVoidMethod = 61,
  kCallVoidMethodV = 62,
  kCallVoidMethodA = 63,
  kGetFieldID = 94,
  kGetObjectField = 95,
  kGetBooleanField = 96,
  kGetIntField = 100,
  kGetStaticMethodID = 113,
  kCallStaticObjectMethod = 114,
  kCallStaticObjectMethodV = 115,
  kCallStaticVoidMethod = 141 - 0,     // placeholder, see note below
  kGetStaticFieldID = 144,
  kGetStaticObjectField = 145,
  kNewStringUTF = 167,
  kGetStringUTFLength = 168,
  kGetStringUTFChars = 169,
  kReleaseStringUTFChars = 170,
  kGetArrayLength = 171,
  // Array reads. The engine fetches the controller's capabilities as int[]
  // and float[] fields and copies them out with the Region calls.
  kGetIntArrayElements = 187,
  kGetFloatArrayElements = 189,
  kReleaseIntArrayElements = 191,
  kReleaseFloatArrayElements = 193,
  kGetIntArrayRegion = 203,
  kGetFloatArrayRegion = 205,
  kExceptionCheck = 228,
};

// Object tokens. High bit patterns keep them clear of guest addresses, and a
// zero token stays null the way JNI expects.
constexpr u32 kObjectTag = 0x50000000;
constexpr u32 kMethodTag = 0x51000000;
constexpr u32 kFieldTag = 0x52000000;
constexpr u32 kClassTag = 0x53000000;

struct Member {
  std::string name;
  std::string signature;
};

std::mutex g_lock;
std::vector<Member> g_members;          // methods and fields share the space
std::vector<std::string> g_strings;     // jstring contents
std::unordered_map<u32, u32> g_string_bytes;   // token -> guest address
std::vector<bool> g_reported;

u32 add_member(const std::string& name, const std::string& sig, u32 tag) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_members.push_back({name, sig});
  return tag | static_cast<u32>(g_members.size());
}

// ------------------------------------------------------------- Java arrays
//
// The engine asks Java what the controller can do before it does anything
// else. `android_main` is barely started when it calls
//
//   NvGetGamepadAxes(env, activity, &count)      reads gamepadAxisIndices,
//                                                gamepadAxisMinVals and
//                                                gamepadAxisMaxVals
//   NvGetGamepadButtons(env, activity, &count)   reads gamepadButtonIndices
//   NvHasGamepadButton(19, ...)                  is there a d-pad up?
//   NvHasGamepadAxis(15, ...)  NvHasGamepadAxis(16, ...)   or a hat instead?
//
// and everything the engine later does with a stick or a button is gated on
// what those said. Answering null - which is what a field whose descriptor is
// not a String used to get - leaves it believing there is no controller at
// all, and no amount of correctly shaped input events changes that.
//
// The lists are searched by value, not by position: `NvHasGamepadAxis` takes
// the Android constant and looks for it. So the order here does not matter,
// only which constants are present.
constexpr u32 kArrayTag = 0x54000000;

std::vector<std::vector<u32>> g_arrays;   // raw words: ints, or float bits

u32 make_array(const std::vector<u32>& words) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_arrays.push_back(words);
  return kArrayTag | static_cast<u32>(g_arrays.size());
}

const std::vector<u32>* array_for(u32 token) {
  if ((token & 0xFF000000) != kArrayTag) return nullptr;
  std::lock_guard<std::mutex> lock(g_lock);
  u32 i = token & 0x00FFFFFF;
  if (i == 0 || i > g_arrays.size()) return nullptr;
  return &g_arrays[i - 1];
}

u32 as_bits(float f) {
  u32 bits;
  std::memcpy(&bits, &f, 4);
  return bits;
}

// What a controller of the shape this build was written for reports.
//
// The d-pad is offered as buttons and not as a hat: the platform layers send
// it as key events, and offering both would have the engine listening for the
// same press twice.
u32 gamepad_field(const std::string& name) {
  static std::mutex once;
  static std::unordered_map<std::string, u32> made;
  std::lock_guard<std::mutex> lock(once);
  auto it = made.find(name);
  if (it != made.end()) return it->second;

  u32 token = 0;
  if (name == "gamepadAxisIndices") {
    token = make_array({kAxisX, kAxisY, kAxisZ, kAxisRz,
                        kAxisLTrigger, kAxisRTrigger});
  } else if (name == "gamepadAxisMinVals") {
    token = make_array({as_bits(-1.0f), as_bits(-1.0f), as_bits(-1.0f),
                        as_bits(-1.0f), as_bits(0.0f), as_bits(0.0f)});
  } else if (name == "gamepadAxisMaxVals") {
    token = make_array({as_bits(1.0f), as_bits(1.0f), as_bits(1.0f),
                        as_bits(1.0f), as_bits(1.0f), as_bits(1.0f)});
  } else if (name == "gamepadButtonIndices") {
    token = make_array({kKeyButtonA, kKeyButtonB, kKeyButtonX, kKeyButtonY,
                        kKeyButtonL1, kKeyButtonR1, kKeyButtonL2, kKeyButtonR2,
                        kKeyButtonThumbL, kKeyButtonThumbR, kKeyButtonStart,
                        kKeyButtonSelect, kKeyDpadUp, kKeyDpadDown,
                        kKeyDpadLeft, kKeyDpadRight, kKeyDpadCenter});
  }
  if (token) {
    made[name] = token;
    std::printf("[jni ] %s -> an array of %zu\n", name.c_str(),
                array_for(token)->size());
  }
  return token;
}

const Member* member(u32 token) {
  std::lock_guard<std::mutex> lock(g_lock);
  u32 i = token & 0x00FFFFFF;
  if (i == 0 || i > g_members.size()) return nullptr;
  return &g_members[i - 1];
}

// A jstring is a token; its bytes are materialised in guest memory on demand,
// because GetStringUTFChars has to hand back something the guest can read.
u32 make_string(Env& e, const std::string& text) {
  u32 token;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    g_strings.push_back(text);
    token = kObjectTag | static_cast<u32>(g_strings.size());
  }
  (void)e;
  return token;
}

const std::string* string_for(u32 token) {
  std::lock_guard<std::mutex> lock(g_lock);
  if ((token & 0xFF000000) != kObjectTag) return nullptr;
  u32 i = token & 0x00FFFFFF;
  if (i == 0 || i > g_strings.size()) return nullptr;
  return &g_strings[i - 1];
}

bool returns_string(const std::string& sig) {
  // Methods: "(...)Ljava/lang/String;". Fields: "Ljava/lang/String;".
  const std::string want = "Ljava/lang/String;";
  return sig.size() >= want.size() &&
         sig.compare(sig.size() - want.size(), want.size(), want) == 0;
}

bool returns_int_like(const std::string& sig) {
  if (sig.empty()) return false;
  char last = sig.back();
  return last == 'I' || last == 'Z' || last == 'J' || last == 'B' ||
         last == 'S' || last == 'C';
}

void report_once(u32 slot, const char* what) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (slot >= g_reported.size()) g_reported.resize(slot + 1, false);
  if (g_reported[slot]) return;
  g_reported[slot] = true;
  std::printf("[jni ] slot %u %s\n", slot, what);
}

}  // namespace

// JNIInvokeInterface slots.
enum VmSlot : u32 {
  kDestroyJavaVM = 3,
  kAttachCurrentThread = 4,
  kDetachCurrentThread = 5,
  kGetEnv = 6,
  kAttachCurrentThreadAsDaemon = 7,
};

// Returns the value to leave in r0.
u32 jni_dispatch(Env& e, u32 slot) {
  Env::Args a(e);
  const u32 env_ptr = a.next32();   // JNIEnv* or JavaVM*, always first
  (void)env_ptr;

  // The JavaVM table lives above the JNIEnv one.
  if (slot >= kVmSvcBase - kJniSvcBase) {
    const u32 vm_slot = slot - (kVmSvcBase - kJniSvcBase);
    switch (vm_slot) {
      case kAttachCurrentThread:
      case kAttachCurrentThreadAsDaemon:
      case kGetEnv: {
        // Every guest thread shares one environment here; there is no real
        // JVM to have per-thread state in.
        u32 out = a.next32();
        if (out && e.glue()) e.mem().write32(out, e.glue()->jni_env());
        std::printf("[jni ] %s -> env 0x%08X\n",
                    vm_slot == kGetEnv ? "GetEnv" : "AttachCurrentThread",
                    e.glue() ? e.glue()->jni_env() : 0);
        return 0;                   // JNI_OK
      }
      case kDetachCurrentThread:
      case kDestroyJavaVM:
        return 0;
      default:
        report_once(kVmSvcBase + vm_slot, "JavaVM slot not implemented");
        return 0;
    }
  }

  switch (slot) {
    case kGetVersion:
      return 0x00010006;            // JNI_VERSION_1_6

    case kFindClass: {
      std::string name = e.mem().str(a.next32());
      std::printf("[jni ] FindClass %s\n", name.c_str());
      return kClassTag | 1;
    }

    case kGetObjectClass:
      return kClassTag | 1;

    case kGetMethodID:
    case kGetStaticMethodID: {
      a.next32();                   // class
      std::string name = e.mem().str(a.next32());
      std::string sig = e.mem().str(a.next32());
      u32 id = add_member(name, sig, kMethodTag);
      std::printf("[jni ] GetMethodID %s %s -> 0x%08X\n", name.c_str(),
                  sig.c_str(), id);
      return id;
    }

    case kGetFieldID:
    case kGetStaticFieldID: {
      a.next32();                   // class
      std::string name = e.mem().str(a.next32());
      std::string sig = e.mem().str(a.next32());
      u32 id = add_member(name, sig, kFieldTag);
      // Naming the caller turns a field the engine asks for into a place in
      // the engine to go and read. The binary keeps its symbols, so this is
      // the fastest route from "it wants gamepadAxisIndices" to what it does
      // with the answer.
      std::printf("[jni ] GetFieldID %s %s -> 0x%08X, asked from %s\n",
                  name.c_str(), sig.c_str(), id,
                  e.loader().symbolize(e.jit()->Regs()[14]).c_str());
      return id;
    }

    // An object-returning call or field read. If the descriptor says String,
    // answer with the data root - that is the question the engine is really
    // asking, and answering it is what unblocks the OBB wait.
    case kCallObjectMethod:
    case kCallObjectMethodV:
    case kCallObjectMethodA:
    case kCallStaticObjectMethod:
    case kCallStaticObjectMethodV:
    case kGetObjectField:
    case kGetStaticObjectField: {
      a.next32();                   // object or class
      u32 id = a.next32();
      const Member* m = member(id);
      if (m) {
        const u32 array = gamepad_field(m->name);
        if (array) return array;
      }
      if (m && returns_string(m->signature)) {
        const std::string path =
            e.glue() ? e.glue()->data_root() : std::string();
        std::printf("[jni ] %s -> \"%s\"\n", m->name.c_str(), path.c_str());
        return make_string(e, path);
      }
      if (m)
        std::printf("[jni ] %s (%s) -> null\n", m->name.c_str(),
                    m->signature.c_str());
      return 0;
    }

    case kCallBooleanMethod:
    case kCallBooleanMethodV:
      return 1;                     // "yes" is the useful answer here

    // Integer answers. A few of these are load-bearing: the engine reads the
    // screen size from Java and would otherwise size its viewport, and divide
    // its aspect ratio, by zero.
    case kCallIntMethod:
    case kCallIntMethodV:
    case kGetIntField:
    case kGetBooleanField: {
      a.next32();
      const Member* m = member(a.next32());
      if (!m) return 0;
      u32 value = 0;
      if (m->name == "screenWidth") value = jni_screen_width();
      else if (m->name == "screenHeight") value = jni_screen_height();
      else if (m->name == "displayRotation") value = 0;
      // Text entry. Android carries the character on the KeyEvent and the
      // engine fetches it through Java, so the answer is whichever event is
      // being dispatched right now - the call itself does not say which.
      else if (m->name.find("nicodeChar") != std::string::npos)
        value = input_current_unicode();
      std::printf("[jni ] %s (%s) -> %u\n", m->name.c_str(),
                  m->signature.c_str(), value);
      return value;
    }

    case kCallVoidMethod:
    case kCallVoidMethodV:
    case kCallVoidMethodA: {
      a.next32();
      const Member* m = member(a.next32());
      if (!m) return 0;
      // The engine focuses a text field and asks Java to raise the keyboard.
      // On a console that is the only way to type, so the request is real
      // work rather than something to note and ignore.
      if (m->name == "show_soft_keyboard") {
        std::printf("[jni ] the engine wants the keyboard\n");
        text_input_show("Enter text", "");
        return 0;
      }
      report_once(slot, ("void call, first was " + m->name).c_str());
      return 0;
    }

    case kGetArrayLength: {
      const std::vector<u32>* v = array_for(a.next32());
      return v ? static_cast<u32>(v->size()) : 0;
    }

    // Get<Type>ArrayRegion(array, start, len, buffer) - a straight copy into
    // guest memory. Ints and floats are the same four bytes here; the caller
    // decided which they are when it chose the call.
    case kGetIntArrayRegion:
    case kGetFloatArrayRegion: {
      const std::vector<u32>* v = array_for(a.next32());
      const u32 start = a.next32();
      const u32 len = a.next32();
      const u32 out = a.next32();
      if (!v || !out) return 0;
      for (u32 i = 0; i < len; ++i) {
        const std::size_t at = start + i;
        e.mem().write32(out + i * 4, at < v->size() ? (*v)[at] : 0);
      }
      return 0;
    }

    // Get<Type>ArrayElements(array, isCopy) - the same data, but the guest
    // wants a pointer to it. It gets a copy that is never freed; there are
    // four of these in the whole run.
    case kGetIntArrayElements:
    case kGetFloatArrayElements: {
      const u32 token = a.next32();
      const u32 is_copy = a.next32();
      const std::vector<u32>* v = array_for(token);
      if (!v) return 0;
      std::lock_guard<std::mutex> lock(g_lock);
      auto it = g_string_bytes.find(token);
      if (it != g_string_bytes.end()) return it->second;
      u32 addr = e.glue() ? e.glue()->alloc_bytes(v->size() * 4 + 4) : 0;
      if (!addr) return 0;
      for (std::size_t i = 0; i < v->size(); ++i)
        e.mem().write32(addr + static_cast<u32>(i) * 4, (*v)[i]);
      if (is_copy) e.mem().write8(is_copy, 1);
      g_string_bytes[token] = addr;
      return addr;
    }

    case kReleaseIntArrayElements:
    case kReleaseFloatArrayElements:
      return 0;                     // the copy stays; nothing owns it

    case kNewStringUTF: {
      u32 chars = a.next32();
      return chars ? make_string(e, e.mem().str(chars, 1 << 16)) : 0;
    }

    case kGetStringUTFLength: {
      const std::string* s = string_for(a.next32());
      return s ? static_cast<u32>(s->size()) : 0;
    }

    // The guest needs real bytes it can read, so the string is materialised
    // into guest memory the first time it is asked for and kept there.
    case kGetStringUTFChars: {
      u32 token = a.next32();
      const std::string* s = string_for(token);
      if (!s) return 0;
      std::lock_guard<std::mutex> lock(g_lock);
      auto it = g_string_bytes.find(token);
      if (it != g_string_bytes.end()) return it->second;
      u32 addr = e.glue() ? e.glue()->alloc_bytes(s->size() + 1) : 0;
      if (!addr) return 0;
      e.mem().copy_in(addr, s->c_str(), s->size() + 1);
      g_string_bytes[token] = addr;
      return addr;
    }

    case kReleaseStringUTFChars:
      return 0;                     // the bytes stay; nothing owns them

    case kNewGlobalRef:
    case kNewLocalRef:
      return a.next32();            // references are the object itself here

    case kDeleteGlobalRef:
    case kDeleteLocalRef:
    case kExceptionClear:
    case kExceptionDescribe:
    case kEnsureLocalCapacity:
    case kPushLocalFrame:
      return 0;

    case kPopLocalFrame:
      return a.next32();

    case kExceptionOccurred:
    case kExceptionCheck:
      return 0;                     // nothing ever throws

    case kIsSameObject: {
      u32 x = a.next32(), y = a.next32();
      return x == y ? 1u : 0u;
    }

    case kIsInstanceOf:
      return 1;

    default:
      report_once(slot, "not implemented, returning 0");
      return 0;
  }
}

}  // namespace wb
