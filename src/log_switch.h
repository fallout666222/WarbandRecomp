// The Switch log: everything printed goes to the SD card as well as to
// nxlink, and a CPU fault writes itself there before the process ends.
#pragma once

namespace wb {

// Opens the log and installs it as the process's standard output. Returns the
// path actually used, or null if the SD card could not be written at all.
const char* switch_log_open(const char* path);
void switch_log_flush();

// Whether the chatty categories reach the log at all. Off by default: loading
// writes tens of thousands of lines, and each is an SD write the game waits
// for. `--verbose` turns them back on.
void switch_log_set_verbose(bool on);
// Start buffering the log. Every line is written straight through until this
// is called, because during startup a lost line is the whole answer.
void switch_log_buffered();
// Also send the log to a listening nxlink host, if there is one. Needs the
// socket layer, so it happens later than opening the log itself.
void switch_log_attach_nxlink();

// Puts a message on the console's own screen and waits for A.
//
// Anything that goes wrong before the graphics context exists has nowhere else
// to go: the game has not drawn yet, so the framebuffer is free, and an error
// the player can read beats a four-digit code every time. After EGL is up this
// must not be called - the console and the game cannot both own the screen.
void switch_fatal(const char* fmt, ...);

// Whether this process was launched in a way that can actually run the game:
// enough memory, and the kernel calls the recompiler needs. Returns null when
// it can, or a description of what is missing.
const char* switch_launch_problem();

}  // namespace wb
