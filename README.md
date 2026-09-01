# warband_nx

Mount & Blade: Warband on the Nintendo Switch, and on Windows, by
recompiling the game's own ARM32 code as it runs.

There is no Warband source. What there is, is the 2014 Android release built
for the NVIDIA SHIELD: one 32-bit ARM shared object, `libMBExpMobile.so`, and
about 800 MB of data. No 64-bit build of it was ever made, so it cannot simply
be loaded and run on an AArch64 console. Instead this project translates its
instructions to AArch64 as they execute — [dynarmic](https://github.com/yuzu-mirror/dynarmic)
does the recompiling — and answers everything the engine asks its operating
system for: EGL and OpenGL ES, the NDK, JNI, libc, threads, files, input, and
FMOD.

**Nothing here is the game.** No engine code, no assets, not one byte of
either. You supply those from a copy you own, and without them this builds and
runs and shows nothing.

## What works

On the Switch: the game boots, the main menu draws, and the sound plays. The
recompiler runs the engine's own code, GL goes through switch-mesa's nouveau
driver, audio through `audout`, and the Joy-Cons arrive as the SHIELD
controller the engine was written for.

On Windows the same source builds against WGL and waveOut, which is where
almost all of the work happened: the main menu at 60 fps, character creation,
and the tutorial ground — trees, terrain, the player, the HUD.

Loading is slow, and honestly so: every one of the engine's instructions is
translated the first time it runs, and a Tegra X1 is not a fast machine to do
that on.

Not done: an unwinder for the engine's exceptions, `qsort`, `setjmp`, and a
long tail of polish. `NOTES.md` is the engineering log — what was found, what
it cost, and why each piece is the shape it is.

## What you need

- **Windows**, with Visual Studio 2022 and the *Desktop development with C++*
  workload. CMake and Ninja come with it.
- **devkitPro** for the Switch build, with the `switch-dev` group, plus the GL
  stack and CMake and Ninja inside its own MSYS2:

  ```bash
  pacman -S switch-dev switch-mesa switch-libdrm_nouveau cmake ninja
  ```

- **git** and **Python 3** on `PATH`.
- **Your copy of the game**: the Android APK and its two `.obb` files. The APK
  carries the engine; the OBBs carry the data. Nothing here downloads either.

## Setup

Clone this repository, drop the APK and the two `.obb` files into the folder,
and run:

```bat
setup.bat
```

It clones dynarmic, downloads the Boost headers, takes `libMBExpMobile.so` out
of the APK and unpacks both OBBs. Everything lands inside this folder — the
project does not reach outside itself:

```
warband_nx/
  external/dynarmic/     cloned by setup.bat
  external/boost/        downloaded by setup.bat, headers only
  game/libMBExpMobile.so out of the APK
  game/gamedata/         the OBBs unpacked, about 795 MB
  game/saves/            written as you play
  build/                 the Windows build
  build_switch/          the Switch build
  sdcard/switch/warband/ assembled by stage_sd.bat, ready to copy
```

If you already have dynarmic or Boost somewhere, point at them instead and
that step is skipped:

```bat
set WB_DYNARMIC=C:\src\dynarmic
set WB_BOOST=C:\src\boost
```

Every path the build uses works the same way — `WB_BUILD`, `WB_SWITCH_BUILD`,
`WB_DEVKITPRO`, `WB_SO`, `WB_DATA`, `WB_SAVE`, `VSROOT`. Unset, they all point
inside this folder.

## Building

```bat
configure.bat
build.bat
```

`run.bat` then plays it and keeps a log. The mouse is a finger: click to tap,
drag to swipe. A plugged-in gamepad is read through XInput. Closing the window
ends the run.

For the Switch:

```bat
configure_switch.bat
build_switch.bat
stage_sd.bat
```

`stage_sd.bat` assembles `sdcard\switch\warband\` — the `.nro`, the engine, the
data and an `args.txt`. Copy the `switch` folder inside it to the root of your
SD card.

**Launch it with title takeover** — in hbmenu, hold **R** while opening a game,
then start warband_nx from the album. Not from the album applet on its own:
applet mode caps a process at about 448 MiB and the guest address space alone
is 2 GiB. Started the wrong way it says so in the log and stops.

Two things about the Switch build are worth knowing before they surprise you.

**It must be devkitPro's own CMake, run from devkitPro's own shell.** Its
toolchain file refuses to run under any other CMake, and that CMake is an
MSYS2 program which reads `C:/x` as a relative path and glues it onto its
working directory. So `configure_switch.bat` translates every path with
`cygpath` and hands the job to `bash -lc`. If you build from the MSYS2 shell
yourself, none of that applies and a plain `cmake` invocation works.

**dynarmic needs a four-line patch**, applied for you by
`switch/patch_dynarmic.py` when you configure. Horizon hands out JIT memory as
two aliases of the same pages — one writable, one executable, at different
addresses — and dynarmic assumes a single address. oaknut, which it emits
through, already supports the split; the patch is which pointers get passed.
It is idempotent, and a patch rather than a fork because dynarmic is a
checkout, not part of this tree.

## Running it

Everything the port prints goes to a log: `warband.log` beside the `.nro` on
the console, and inside `build\` on Windows. It is quiet by default — loading
writes tens of thousands of lines otherwise, and on a console every one of
them is an SD card write the game waits for.

hbmenu launches an `.nro` with no arguments, and every diagnostic in this build
is a command-line switch, so they are read from
`sdmc:/switch/warband/args.txt` instead — one per line, `#` starts a comment.
`switch/args.txt.example` lists them all. The two worth knowing:

```
--verbose                       every log category, not just the quiet set
--shot=sdmc:/switch/warband/s   write s_01.bmp, s_02.bmp ... from the back buffer
```

## When it goes wrong

- **A run that ends with nothing in the log** is usually the graphics driver.
  On Windows, look in the Event Viewer for a TDR before looking anywhere else
  — the driver's timeout kills the process instantly, past every handler, and
  leaves nothing behind.
- **A `[FAULT]` block** prints the guest program counter symbolically and the
  host registers. It also prints the handler's own runtime address, and
  `__libnx_exception_handler` is in the `.elf`'s symbol table — subtract one
  from the other for the load base, take that off any address in the dump, and
  `aarch64-none-elf-addr2line -f -C -e warband_nx.elf` turns it into a
  function and a line.
- **A run that stops early** with `budget exhausted` hit a deliberate cap;
  `--svcs=0` removes it. It exists to bisect a hang: run to N import calls and
  read the histogram of what the engine was doing.

## Layout

```
src/guest.h              address-space map and flat memory
src/elf_loader.{h,cpp}   maps the image, applies relocations, binds imports
src/env.{h,cpp}          dynarmic callbacks, svc dispatch, AAPCS arg reader
src/thunks_*.cpp         the boundary: GL, libc, files, threads, input, FMOD
src/audio_*.cpp          the mixer, and one file per platform for the device
src/gl_*.cpp             the GL layer, and one file per platform for the context
src/guest_code.cpp       imports answered with ARM32 code instead of a thunk
switch/                  the oaknut override, the dynarmic patch, args template
tools/                   the OBB extractor, the guest assembler, symbol helpers
NOTES.md                 how it works and what it cost to find out
```

## Legal

This runs a commercial game you must own, from files you must supply. The
repository contains no game code and no game data, and nothing here is
distributable together with either. The engine has no public source; what is
here is the layer underneath it.

dynarmic is 0BSD. `stb_vorbis` is public domain. The oaknut header under
`switch/` is a modified copy of oaknut's own, MIT, and says so at the top.
