// The on-screen frame counter.
//
// Drawn through the same GL table the engine uses, so it appears on the
// desktop and on the console with no per-platform code.
#pragma once

namespace wb {

// Counts a presented frame. `now_seconds` is any monotonic clock.
void overlay_frame(double now_seconds);

// The current rate, for anyone who wants it in a log or a window title.
float overlay_fps();

// Draws the counter. Called with the context current, just before the swap;
// leaves every piece of GL state as it found it.
void overlay_draw();

// Turns the drawing off while still counting. The counter is the newest GL
// code in the frame, so when the frame misbehaves it has to be possible to
// take it out of the picture and see whether the misbehaviour goes with it.
void overlay_set_visible(bool on);

}  // namespace wb
