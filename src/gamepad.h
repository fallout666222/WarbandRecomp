// The platform's controller, polled.
//
// Both implementations turn what they read into the same Android-shaped
// events - see input.h - so nothing above this line knows whether it is
// talking to XInput or to hid.
#pragma once

namespace wb {

// Switch only; the PC picks the pad up on the first poll. The surface size is
// needed to place touches, which arrive in panel coordinates.
void gamepad_init(int surface_w, int surface_h);

// Called from the watchdog tick. Cheap when nothing is connected.
void gamepad_poll();

// Asks the platform for a line of text and delivers it as key events, the way
// an Android IME would. The engine requests this through JNI when it focuses
// a text field - a character name, a server address - and on a console it is
// the only way to type at all. Returns false if the platform has no such
// thing, which on the desktop it does not: there is a real keyboard.
bool text_input_show(const char* prompt, const char* initial);

}  // namespace wb
