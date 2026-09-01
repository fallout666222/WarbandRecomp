// The NDK input path: looper, input queue, event accessors.
//
// native_app_glue's loop is
//
//     while ((ident = ALooper_pollAll(t, NULL, &events, &source)) >= 0)
//         source->process(app, source);
//
// so answering it needs the `source` pointer the glue registered - a pointer
// into a struct whose layout is NVIDIA's business, not ours. Rather than
// guess at that layout, the registration is *recorded*: the glue hands the
// pointer to ALooper_addFd and AInputQueue_attachLooper, and pollAll hands
// the same pointer back. The guest's own code then does the dispatching, and
// nothing here has to know what it points at.
//
// Events themselves are integer tokens. AInputEvent is opaque to the engine -
// it only ever passes the pointer back to the accessors below - so there is
// no object for it to look inside, and a token sidesteps the usual problem of
// a 64-bit host pointer in a 32-bit guest word.

#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "android_glue.h"
#include "env.h"
#include "input.h"

namespace wb {
namespace {

// NDK constants, spelled out rather than pulled from a header the Switch
// build does not have.
constexpr u32 kEventTypeKey = 1;
constexpr u32 kEventTypeMotion = 2;
constexpr u32 kLooperPollTimeout = static_cast<u32>(-3);

constexpr u32 kEventTag = 0x70000000;

std::mutex g_lock;
std::deque<InputEvent> g_queue;

// Events handed to the guest. It only holds one at a time - the handler reads
// the accessors and returns - so a small ring is enough and nothing has to be
// freed. Token indices wrap with it.
constexpr std::size_t kLiveSlots = 64;
std::array<InputEvent, kLiveSlots> g_live{};
std::size_t g_live_next = 0;

// What the glue registered with the looper. `data` is the android_poll_source
// it wants back; `ident` is how it tells its own sources apart.
struct Registration {
  bool valid = false;
  u32 ident = 0;
  u32 data = 0;
};
Registration g_input_source;
Registration g_cmd_source;

// The character carried by the event being dispatched right now, for the JNI
// side to read - see input_current_unicode.
u32 g_current_unicode = 0;

const InputEvent* event_for(u32 token) {
  if ((token & 0xFF000000) != kEventTag) return nullptr;
  const u32 i = token & 0x00FFFFFF;
  if (i == 0 || i > kLiveSlots) return nullptr;
  return &g_live[i - 1];
}

// Moves one queued event into the ring and returns its token, or 0 when
// there is nothing waiting.
u32 hand_out_next() {
  if (g_queue.empty()) return 0;
  g_live[g_live_next] = g_queue.front();
  g_queue.pop_front();
  const u32 token = kEventTag | static_cast<u32>(g_live_next + 1);
  g_live_next = (g_live_next + 1) % kLiveSlots;
  return token;
}

// ---------------------------------------------------------------- looper
void t_looper_prepare(Env& e) { e.ret(e.glue() ? e.glue()->looper() : 1); }

// int ALooper_addFd(ALooper*, int fd, int ident, int events,
//                   ALooper_callbackFunc, void* data)
void t_looper_add_fd(Env& e) {
  Env::Args a(e);
  a.next32();                        // looper
  a.next32();                        // fd
  const u32 ident = a.next32();
  a.next32();                        // events
  a.next32();                        // callback
  const u32 data = a.next32();
  std::lock_guard<std::mutex> lock(g_lock);
  g_cmd_source = {true, ident, data};
  std::printf("[inp ] looper fd registered: ident %u, source 0x%08X\n", ident,
              data);
  e.ret(1);
}

// int ALooper_pollAll(int timeoutMillis, int* outFd, int* outEvents,
//                     void** outData)
void t_looper_poll(Env& e) {
  Env::Args a(e);
  const int timeout = static_cast<int>(a.next32());
  const u32 out_fd = a.next32();
  const u32 out_events = a.next32();
  const u32 out_data = a.next32();

  Registration source;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_queue.empty() && g_input_source.valid) source = g_input_source;
  }

  if (!source.valid) {
    // Nothing to report. App commands are delivered by calling the engine's
    // own handler directly rather than through the looper, so a timeout here
    // is the normal answer.
    if (timeout > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
    e.ret(kLooperPollTimeout);
    return;
  }

  if (out_fd) e.mem().write32(out_fd, 0);
  if (out_events) e.mem().write32(out_events, 1);   // ALOOPER_EVENT_INPUT
  if (out_data) e.mem().write32(out_data, source.data);
  e.ret(source.ident);
}

// ------------------------------------------------------------ input queue
// void AInputQueue_attachLooper(AInputQueue*, ALooper*, int ident,
//                               ALooper_callbackFunc, void* data)
void t_input_attach_looper(Env& e) {
  Env::Args a(e);
  a.next32();                        // queue
  a.next32();                        // looper
  const u32 ident = a.next32();
  a.next32();                        // callback
  const u32 data = a.next32();
  {
    std::lock_guard<std::mutex> lock(g_lock);
    g_input_source = {true, ident, data};
  }
  std::printf("[inp ] input queue attached: ident %u, source 0x%08X\n", ident,
              data);
  e.ret(0);
}

void t_input_detach_looper(Env& e) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_input_source = {};
  e.ret(0);
}

// int32_t AInputQueue_getEvent(AInputQueue*, AInputEvent** outEvent)
void t_input_get_event(Env& e) {
  Env::Args a(e);
  a.next32();                        // queue
  const u32 out = a.next32();

  std::lock_guard<std::mutex> lock(g_lock);
  const u32 token = hand_out_next();
  if (out) e.mem().write32(out, token);
  e.ret(token ? 0u : static_cast<u32>(-1));
}

// Non-zero would mean "the system handled it, do not dispatch". Everything
// here is for the app.
void t_input_pre_dispatch(Env& e) { e.ret(0); }

void t_input_finish_event(Env& e) { e.ret(0); }

// -------------------------------------------------------------- accessors
void t_event_get_type(Env& e) {
  Env::Args a(e);
  std::lock_guard<std::mutex> lock(g_lock);
  const InputEvent* ev = event_for(a.next32());
  e.ret(!ev ? 0
            : ev->kind == InputEvent::Kind::Key ? kEventTypeKey
                                                : kEventTypeMotion);
}

// The source is how the engine tells a thumbstick from a finger and a
// gamepad button from a letter, so it has to be the event's own rather than
// guessed from its kind.
void t_event_get_source(Env& e) {
  Env::Args a(e);
  std::lock_guard<std::mutex> lock(g_lock);
  const InputEvent* ev = event_for(a.next32());
  e.ret(ev ? static_cast<u32>(ev->source) : 0);
}

void t_motion_get_action(Env& e) {
  Env::Args a(e);
  std::lock_guard<std::mutex> lock(g_lock);
  const InputEvent* ev = event_for(a.next32());
  e.ret(ev ? static_cast<u32>(ev->action) : 0);
}

void t_motion_get_x(Env& e) {
  Env::Args a(e);
  std::lock_guard<std::mutex> lock(g_lock);
  const InputEvent* ev = event_for(a.next32());
  e.retf(ev ? ev->x : 0.0f);
}

void t_motion_get_y(Env& e) {
  Env::Args a(e);
  std::lock_guard<std::mutex> lock(g_lock);
  const InputEvent* ev = event_for(a.next32());
  e.retf(ev ? ev->y : 0.0f);
}

void t_motion_pointer_count(Env& e) { e.ret(1); }

void t_motion_pointer_id(Env& e) {
  Env::Args a(e);
  std::lock_guard<std::mutex> lock(g_lock);
  const InputEvent* ev = event_for(a.next32());
  e.ret(ev ? static_cast<u32>(ev->pointer_id) : 0);
}

// float AMotionEvent_getAxisValue(const AInputEvent*, int32_t axis,
//                                 size_t pointer_index)
//
// For a touch this is another way of asking for x and y; for a stick it is
// the only way of asking at all.
void t_motion_axis_value(Env& e) {
  Env::Args a(e);
  const u32 token = a.next32();
  const u32 axis = a.next32();
  std::lock_guard<std::mutex> lock(g_lock);
  const InputEvent* ev = event_for(token);
  if (!ev) {
    e.retf(0.0f);
    return;
  }
  if (ev->source == Source::Touchscreen)
    e.retf(axis == kAxisX ? ev->x : axis == kAxisY ? ev->y : 0.0f);
  else {
    const int slot = axis_slot(axis);
    e.retf(slot < 0 ? 0.0f : ev->axis[slot]);
  }
}

void t_key_get_action(Env& e) {
  Env::Args a(e);
  std::lock_guard<std::mutex> lock(g_lock);
  const InputEvent* ev = event_for(a.next32());
  e.ret(ev ? static_cast<u32>(ev->action) : 0);
}

void t_key_get_key_code(Env& e) {
  Env::Args a(e);
  std::lock_guard<std::mutex> lock(g_lock);
  const InputEvent* ev = event_for(a.next32());
  e.ret(ev ? static_cast<u32>(ev->key_code) : 0);
}

}  // namespace

int axis_slot(u32 android_axis) {
  switch (android_axis) {
    case kAxisX: return 0;
    case kAxisY: return 1;
    case kAxisZ: return 2;
    case kAxisRz: return 3;
    case kAxisHatX: return 4;
    case kAxisHatY: return 5;
    case kAxisLTrigger: return 6;
    case kAxisRTrigger: return 7;
    default: return -1;
  }
}

void input_push(const InputEvent& event) {
  std::lock_guard<std::mutex> lock(g_lock);
  // A backlog means the engine is not draining; dropping the oldest keeps a
  // stall from turning into unbounded memory and a minute of replayed clicks.
  if (g_queue.size() > 256) g_queue.pop_front();
  g_queue.push_back(event);
  static int shown = 0;
  if (++shown <= 10)
    std::printf("[inp ] %s action %d at %.0f,%.0f\n",
                event.kind == InputEvent::Kind::Key ? "key" : "touch",
                event.action, event.x, event.y);
}

bool input_pending() {
  std::lock_guard<std::mutex> lock(g_lock);
  return !g_queue.empty();
}

// The engine never attaches an input queue to the looper, because we call
// android_main directly and so its glue's entry function - the part that
// would have set the queue up - never runs. It does register a handler at
// app->onInputEvent, though, and that is what the glue would eventually have
// called. So events are delivered straight to it, exactly the way lifecycle
// commands are.
u32 input_take_next() {
  std::lock_guard<std::mutex> lock(g_lock);
  const u32 token = hand_out_next();
  if (token) {
    const InputEvent* ev = event_for(token);
    g_current_unicode = ev ? ev->unicode : 0;
  }
  return token;
}

u32 input_current_unicode() {
  std::lock_guard<std::mutex> lock(g_lock);
  return g_current_unicode;
}

const ThunkEntry kInputTable[] = {
    {"ALooper_prepare", &t_looper_prepare},
    {"ALooper_pollAll", &t_looper_poll},
    {"ALooper_addFd", &t_looper_add_fd},

    {"AInputQueue_attachLooper", &t_input_attach_looper},
    {"AInputQueue_detachLooper", &t_input_detach_looper},
    {"AInputQueue_getEvent", &t_input_get_event},
    {"AInputQueue_preDispatchEvent", &t_input_pre_dispatch},
    {"AInputQueue_finishEvent", &t_input_finish_event},

    {"AInputEvent_getType", &t_event_get_type},
    {"AInputEvent_getSource", &t_event_get_source},

    {"AMotionEvent_getAction", &t_motion_get_action},
    {"AMotionEvent_getX", &t_motion_get_x},
    {"AMotionEvent_getY", &t_motion_get_y},
    {"AMotionEvent_getPointerCount", &t_motion_pointer_count},
    {"AMotionEvent_getPointerId", &t_motion_pointer_id},
    {"AMotionEvent_getAxisValue", &t_motion_axis_value},

    {"AKeyEvent_getAction", &t_key_get_action},
    {"AKeyEvent_getKeyCode", &t_key_get_key_code},
};

const std::size_t kInputTableSize = sizeof(kInputTable) / sizeof(kInputTable[0]);

}  // namespace wb
