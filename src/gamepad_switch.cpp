// The Switch's own controllers, through libnx hid.
//
// Joy-Cons, a Pro Controller and the handheld console itself all arrive
// through one PadState, and all of them come out of here as the events an
// NVIDIA SHIELD controller would have produced - which is what this build of
// the engine was written for. The touchscreen goes through the same queue as
// a finger, because in handheld mode it is one.
//
// Buttons are mapped by label: pressing A confirms, because the button marked
// A is Android's confirm button too. Mapping by position would put confirm on
// B, which is right for someone holding an Xbox pad and wrong for everyone
// holding a Switch.

#if defined(WB_SWITCH)

#include <switch.h>

#include <cmath>
#include <cstdio>

#include "gamepad.h"
#include "input.h"

namespace wb {
namespace {

constexpr float kStickDeadzone = 0.18f;
constexpr float kAxisEpsilon = 0.02f;

struct ButtonMap {
  u64 button;
  int keycode;
};

constexpr ButtonMap kButtons[] = {
    {HidNpadButton_A, kKeyButtonA},
    {HidNpadButton_B, kKeyButtonB},
    {HidNpadButton_X, kKeyButtonX},
    {HidNpadButton_Y, kKeyButtonY},
    {HidNpadButton_L, kKeyButtonL1},
    {HidNpadButton_R, kKeyButtonR1},
    {HidNpadButton_ZL, kKeyButtonL2},
    {HidNpadButton_ZR, kKeyButtonR2},
    {HidNpadButton_StickL, kKeyButtonThumbL},
    {HidNpadButton_StickR, kKeyButtonThumbR},
    {HidNpadButton_Plus, kKeyButtonStart},
    // Back, not Select. The engine's key handler compares against a fixed
    // list - 19..22 for the d-pad, 96..108 for the face and shoulder buttons,
    // 4 for Android's Back and 66 for Enter - and 109, BUTTON_SELECT, is not
    // in it. A pad whose Back button sends 109 has a Back button that does
    // nothing, which is exactly how it behaved.
    {HidNpadButton_Minus, kKeyBack},
    {HidNpadButton_Up, kKeyDpadUp},
    {HidNpadButton_Down, kKeyDpadDown},
    {HidNpadButton_Left, kKeyDpadLeft},
    {HidNpadButton_Right, kKeyDpadRight},
};

PadState g_pad;
bool g_ready = false;
float g_last_axis[kAxisSlots] = {};

// Touch state, so a finger produces down / move / up rather than a stream of
// identical presses.
bool g_touching = false;
float g_touch_x = 0, g_touch_y = 0;

int g_surface_w = 1280, g_surface_h = 720;

float stick(s32 raw) {
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

// A keycode for a character, where Android has one. The unicode value is
// carried separately and is what actually ends up in the text field; the
// keycode only has to be plausible.
int android_keycode_for(char c) {
  if (c >= 'a' && c <= 'z') return 29 + (c - 'a');
  if (c >= 'A' && c <= 'Z') return 29 + (c - 'A');
  if (c >= '0' && c <= '9') return 7 + (c - '0');
  if (c == ' ') return 62;
  return 0;
}

void send_touch(int action, float x, float y) {
  InputEvent ev;
  ev.kind = InputEvent::Kind::Motion;
  ev.source = Source::Touchscreen;
  ev.action = action;
  ev.x = x;
  ev.y = y;
  input_push(ev);
}

void poll_touch() {
  HidTouchScreenState touch{};
  if (!hidGetTouchScreenStates(&touch, 1) || touch.count == 0) {
    if (g_touching) {
      g_touching = false;
      send_touch(1, g_touch_x, g_touch_y);       // ACTION_UP
    }
    return;
  }

  // The panel is 1280x720 and so is the surface we asked the compositor for,
  // but scale anyway: docked output and the emulated surface need not agree.
  const float x = static_cast<float>(touch.touches[0].x) *
                  static_cast<float>(g_surface_w) / 1280.0f;
  const float y = static_cast<float>(touch.touches[0].y) *
                  static_cast<float>(g_surface_h) / 720.0f;
  if (!g_touching) {
    g_touching = true;
    send_touch(0, x, y);                          // ACTION_DOWN
  } else if (std::fabs(x - g_touch_x) > 1.0f || std::fabs(y - g_touch_y) > 1.0f) {
    send_touch(2, x, y);                          // ACTION_MOVE
  }
  g_touch_x = x;
  g_touch_y = y;
}

}  // namespace

void gamepad_init(int surface_w, int surface_h) {
  g_surface_w = surface_w;
  g_surface_h = surface_h;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);
  hidInitializeTouchScreen();
  g_ready = true;
  std::printf("[inp ] hid ready: pad and touchscreen\n");
}

// Horizon's own on-screen keyboard. It is an applet: it takes over the
// display, the game stops rendering while it is up, and control comes back
// with the finished string rather than a character at a time.
//
// The engine wants characters, not a string, because on Android an IME
// delivers key events - so the result is replayed as key events, which is
// also what makes the existing text-entry path work unchanged.
bool text_input_show(const char* prompt, const char* initial) {
  SwkbdConfig kbd;
  if (R_FAILED(swkbdCreate(&kbd, 0))) {
    std::printf("[inp ] swkbdCreate failed\n");
    return false;
  }
  swkbdConfigMakePresetDefault(&kbd);
  if (prompt && *prompt) swkbdConfigSetGuideText(&kbd, prompt);
  if (initial && *initial) swkbdConfigSetInitialText(&kbd, initial);
  swkbdConfigSetStringLenMax(&kbd, 64);

  char text[128] = {};
  const Result rc = swkbdShow(&kbd, text, sizeof(text));
  swkbdClose(&kbd);
  if (R_FAILED(rc)) return false;

  std::printf("[inp ] keyboard returned \"%s\"\n", text);
  for (const char* c = text; *c; ++c) {
    InputEvent ev;
    ev.kind = InputEvent::Kind::Key;
    ev.source = Source::Keyboard;
    ev.key_code = android_keycode_for(*c);
    ev.unicode = static_cast<u32>(static_cast<unsigned char>(*c));
    ev.action = 0;
    input_push(ev);
    ev.action = 1;
    input_push(ev);
  }
  // The engine closes the field on Enter, which is what leaving the keyboard
  // means here.
  InputEvent done;
  done.kind = InputEvent::Kind::Key;
  done.source = Source::Keyboard;
  done.key_code = kKeyEnter;
  done.action = 0;
  input_push(done);
  done.action = 1;
  input_push(done);
  return true;
}

void gamepad_poll() {
  if (!g_ready) return;
  padUpdate(&g_pad);

  const u64 down = padGetButtonsDown(&g_pad);
  const u64 up = padGetButtonsUp(&g_pad);
  for (const ButtonMap& b : kButtons) {
    if (down & b.button) send_key(b.keycode, true);
    if (up & b.button) send_key(b.keycode, false);
  }

  const HidAnalogStickState left = padGetStickPos(&g_pad, 0);
  const HidAnalogStickState right = padGetStickPos(&g_pad, 1);

  float axis[kAxisSlots] = {};
  axis[0] = stick(left.x);
  axis[1] = -stick(left.y);            // Android's Y grows downward
  axis[2] = stick(right.x);
  axis[3] = -stick(right.y);
  // ZL and ZR are digital on every Switch controller, so the analogue axes
  // follow the buttons rather than the other way round.
  const u64 held = padGetButtons(&g_pad);
  axis[6] = (held & HidNpadButton_ZL) ? 1.0f : 0.0f;
  axis[7] = (held & HidNpadButton_ZR) ? 1.0f : 0.0f;

  bool moved = false;
  for (int i = 0; i < kAxisSlots; ++i)
    if (std::fabs(axis[i] - g_last_axis[i]) > kAxisEpsilon) moved = true;
  if (moved) {
    InputEvent ev;
    ev.kind = InputEvent::Kind::Motion;
    ev.source = Source::Joystick;
    ev.action = 2;                     // AMOTION_EVENT_ACTION_MOVE
    for (int i = 0; i < kAxisSlots; ++i) {
      ev.axis[i] = axis[i];
      g_last_axis[i] = axis[i];
    }
    input_push(ev);
  }

  poll_touch();
}

}  // namespace wb

#endif  // WB_SWITCH
