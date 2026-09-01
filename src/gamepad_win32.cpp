// A gamepad on the PC, through XInput.
//
// This exists so the SHIELD control path can be tested without a Switch. The
// engine wants events shaped the way Android shapes them - buttons as key
// events with BUTTON_* keycodes, sticks as a motion event carrying axes - so
// that is what this produces, and the same events come out of the Switch's
// hid layer on the console.
//
// XInput is polled rather than pushed, so the interesting part is edges:
// sending a key event only when a button actually changes, and a motion event
// only when a stick has moved enough to matter. Sending them every frame
// would bury the engine in input it has already seen.

#if defined(_WIN32) && !defined(WB_SWITCH)

#include <windows.h>
#include <xinput.h>

#include <cmath>
#include <cstdio>

#include "gamepad.h"
#include "input.h"

namespace wb {
namespace {

// Below this a stick is treated as centred. XInput's own recommended
// deadzones are larger than a game like this wants, so it is deliberately
// small - the engine does its own filtering.
constexpr float kStickDeadzone = 0.18f;
constexpr float kTriggerThreshold = 0.12f;
constexpr float kAxisEpsilon = 0.02f;

struct ButtonMap {
  WORD xinput;
  int keycode;
};

// By label rather than by position. A Switch user pressing A expects the
// button marked A to confirm, and Android's BUTTON_A is its confirm button
// too; matching the letters keeps that true on both.
constexpr ButtonMap kButtons[] = {
    {XINPUT_GAMEPAD_A, kKeyButtonA},
    {XINPUT_GAMEPAD_B, kKeyButtonB},
    {XINPUT_GAMEPAD_X, kKeyButtonX},
    {XINPUT_GAMEPAD_Y, kKeyButtonY},
    {XINPUT_GAMEPAD_LEFT_SHOULDER, kKeyButtonL1},
    {XINPUT_GAMEPAD_RIGHT_SHOULDER, kKeyButtonR1},
    {XINPUT_GAMEPAD_LEFT_THUMB, kKeyButtonThumbL},
    {XINPUT_GAMEPAD_RIGHT_THUMB, kKeyButtonThumbR},
    {XINPUT_GAMEPAD_START, kKeyButtonStart},
    // Back, not Select. The engine's key handler compares against a fixed
    // list - 19..22 for the d-pad, 96..108 for the face and shoulder buttons,
    // 4 for Android's Back and 66 for Enter - and 109, BUTTON_SELECT, is not
    // in it. A pad whose Back button sends 109 has a Back button that does
    // nothing, which is exactly how it behaved.
    {XINPUT_GAMEPAD_BACK, kKeyBack},
    {XINPUT_GAMEPAD_DPAD_UP, kKeyDpadUp},
    {XINPUT_GAMEPAD_DPAD_DOWN, kKeyDpadDown},
    {XINPUT_GAMEPAD_DPAD_LEFT, kKeyDpadLeft},
    {XINPUT_GAMEPAD_DPAD_RIGHT, kKeyDpadRight},
};

WORD g_last_buttons = 0;
bool g_last_lt = false, g_last_rt = false;
float g_last_axis[kAxisSlots] = {};
bool g_seen = false;

float stick(SHORT raw) {
  const float v = static_cast<float>(raw) / 32767.0f;
  return std::fabs(v) < kStickDeadzone ? 0.0f : v;
}

void send_key(int keycode, bool down) {
  InputEvent ev;
  ev.kind = InputEvent::Kind::Key;
  ev.source = Source::Gamepad;
  ev.action = down ? 0 : 1;
  ev.key_code = keycode;
  input_push(ev);
}

}  // namespace

void gamepad_init(int, int) {}   // XInput needs no setting up

// The desktop has a keyboard, so the engine's request for an on-screen one is
// declined and the window's own key events do the work.
bool text_input_show(const char*, const char*) { return false; }

// Called once per watchdog tick. Cheap when nothing is plugged in: XInput
// caches the "no such device" answer.
void gamepad_poll() {
  XINPUT_STATE state{};
  if (XInputGetState(0, &state) != ERROR_SUCCESS) return;
  if (!g_seen) {
    g_seen = true;
    std::printf("[inp ] gamepad connected\n");
  }
  const XINPUT_GAMEPAD& pad = state.Gamepad;

  for (const ButtonMap& b : kButtons) {
    const bool now = (pad.wButtons & b.xinput) != 0;
    const bool before = (g_last_buttons & b.xinput) != 0;
    if (now != before) send_key(b.keycode, now);
  }
  g_last_buttons = pad.wButtons;

  // Triggers are analogue, but the engine also wants them as L2/R2 presses,
  // so they are reported both ways - as an axis and as a button crossing a
  // threshold.
  const float lt = static_cast<float>(pad.bLeftTrigger) / 255.0f;
  const float rt = static_cast<float>(pad.bRightTrigger) / 255.0f;
  const bool lt_down = lt > kTriggerThreshold;
  const bool rt_down = rt > kTriggerThreshold;
  if (lt_down != g_last_lt) send_key(kKeyButtonL2, lt_down);
  if (rt_down != g_last_rt) send_key(kKeyButtonR2, rt_down);
  g_last_lt = lt_down;
  g_last_rt = rt_down;

  float axis[kAxisSlots] = {};
  axis[0] = stick(pad.sThumbLX);
  axis[1] = -stick(pad.sThumbLY);      // Android's Y grows downward
  axis[2] = stick(pad.sThumbRX);
  axis[3] = -stick(pad.sThumbRY);
  axis[6] = lt;
  axis[7] = rt;

  bool moved = false;
  for (int i = 0; i < kAxisSlots; ++i)
    if (std::fabs(axis[i] - g_last_axis[i]) > kAxisEpsilon) moved = true;
  if (!moved) return;

  InputEvent ev;
  ev.kind = InputEvent::Kind::Motion;
  ev.source = Source::Joystick;
  ev.action = 2;                       // AMOTION_EVENT_ACTION_MOVE
  for (int i = 0; i < kAxisSlots; ++i) {
    ev.axis[i] = axis[i];
    g_last_axis[i] = axis[i];
  }
  input_push(ev);
}

}  // namespace wb

#endif  // _WIN32 && !WB_SWITCH
