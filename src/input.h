// Input events on their way from the host to the engine.
//
// This build of Warband is the NVIDIA SHIELD one, so the engine already knows
// about gamepads: it reads sticks through AMotionEvent_getAxisValue and takes
// buttons as key events carrying Android's BUTTON_* keycodes. That is a far
// better fit for a console than pretending to be a touchscreen, and it means
// the Switch's controllers need no new engine support at all - only events
// shaped the way Android shapes them.
//
// Touch stays as well. The Switch has a touchscreen in handheld mode, and on
// the PC the mouse is the only pointer there is.
//
// The platform layer pushes; the NDK thunks pop. Nothing else connects them,
// which is what lets the same thunks serve a window and a console.
#pragma once

#include "guest.h"

namespace wb {

// AINPUT_SOURCE_*, as Android numbers them.
enum class Source : u32 {
  Touchscreen = 0x00001002,
  Keyboard = 0x00000101,
  Gamepad = 0x00000401,
  Joystick = 0x01000010,
};

// AMOTION_EVENT_AXIS_*, which are sparse, and the dense slots we carry them
// in. axis_slot() maps one onto the other.
enum Axis {
  kAxisX = 0,          // left stick
  kAxisY = 1,
  kAxisZ = 11,         // right stick
  kAxisRz = 14,
  kAxisHatX = 15,      // d-pad, when it reports as an axis
  kAxisHatY = 16,
  kAxisLTrigger = 17,
  kAxisRTrigger = 18,
};
constexpr int kAxisSlots = 8;

// Android keycodes worth naming: these are what the engine binds controls to.
enum KeyCode {
  kKeyBack = 4,
  kKeyDpadUp = 19,
  kKeyDpadDown = 20,
  kKeyDpadLeft = 21,
  kKeyDpadRight = 22,
  kKeyDpadCenter = 23,
  kKeyEnter = 66,
  kKeyButtonA = 96,
  kKeyButtonB = 97,
  kKeyButtonX = 99,
  kKeyButtonY = 100,
  kKeyButtonL1 = 102,
  kKeyButtonR1 = 103,
  kKeyButtonL2 = 104,
  kKeyButtonR2 = 105,
  kKeyButtonThumbL = 106,
  kKeyButtonThumbR = 107,
  kKeyButtonStart = 108,
  kKeyButtonSelect = 109,
};

struct InputEvent {
  enum class Kind { Motion, Key };

  Kind kind = Kind::Motion;
  Source source = Source::Touchscreen;
  int action = 0;        // AMOTION_EVENT_ACTION_* or AKEY_EVENT_ACTION_*
  float x = 0, y = 0;    // touch only, in surface pixels
  int pointer_id = 0;
  int key_code = 0;      // key only, an Android keycode
  // The character the key produced, if any. Android carries this separately
  // from the keycode - the engine asks Java for it through
  // KeyEvent.getUnicodeChar - because a keycode says which key, not which
  // letter. Text entry needs both.
  u32 unicode = 0;
  // Stick and trigger positions, in the dense slots axis_slot() returns.
  float axis[kAxisSlots] = {};
};

// Where an Android axis constant lives in InputEvent::axis, or -1.
int axis_slot(u32 android_axis);

// Called from whichever thread the platform reads input on.
void input_push(const InputEvent& event);

// True while the engine has events waiting. The looper asks this to decide
// whether to report input available.
bool input_pending();

// Moves the next queued event into the guest-visible ring and returns its
// token, or 0 when the queue is empty. Used by the direct delivery path in
// AndroidGlue, which hands the token to the engine's own input handler.
u32 input_take_next();

// True when the next event waiting is a key rather than a touch or a stick.
// The delivery path paces keys and does not pace anything else - see
// AndroidGlue::pump_input.
bool input_next_is_key();

// The character carried by the event currently being dispatched. The engine
// asks for it through JNI rather than through the NDK, and JNI has no way to
// know which event it is being asked about - the call carries no reference to
// one - so it consults the event in flight.
u32 input_current_unicode();

}  // namespace wb
