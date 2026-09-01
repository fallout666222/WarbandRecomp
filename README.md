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

## Getting it running on a Switch

You need custom firmware (Atmosphère) and hbmenu. You also need, from your own
copy of the game:

- **`libMBExpMobile.so`** — out of the Android APK, from `lib/armeabi-v7a/`.
  An APK is a zip; any archiver will open it.
- **The game data** — out of the two `.obb` files that ship alongside the APK.
  They are jobb containers, which is to say FAT images with a footer:

  ```
  python tools/jobb_extract.py main.40.com.taleworlds.mbwarband.obb  gamedata
  python tools/jobb_extract.py patch.60.com.taleworlds.mbwarband.obb gamedata
  ```

  That gives about 795 MB in `gamedata/`.

Put it on the SD card like this:

```
/switch/warband/warband_nx.nro
/switch/warband/libMBExpMobile.so
/switch/warband/gamedata/          the extracted OBB tree
/switch/warband/args.txt           optional, see below
/switch/warband/user/              saves, created on first run
/switch/warband/warband.log        written every run
```

**Launch it with title takeover** — in hbmenu, hold **R** while opening a
game, then start warband_nx from the album. Not from the album applet on its
own: applet mode caps a process at about 448 MiB and the guest address space
alone is 2 GiB. Started the wrong way it says so in the log and stops.

Everything the port prints goes to `warband.log` next to the `.nro`. It is
quiet by default — loading writes tens of thousands of lines otherwise, and on
a console every one of them is an SD card write the game waits for.

### args.txt

hbmenu launches an `.nro` with no arguments, and every diagnostic in this
build is a command-line switch, so they are read from
`sdmc:/switch/warband/args.txt` instead — one per line, `#` starts a comment.
`switch/args.txt.example` lists them. The two worth knowing:

```
--verbose                       every log category, not just the quiet set
--shot=sdmc:/switch/warband/s   write s_01.bmp, s_02.bmp ... from the back buffer
```

## Building

Both targets build from the same source. The only files that differ are the
platform layer — `gl_win32.cpp` against `gl_switch.cpp`, `audio_win32.cpp`
against `audio_switch.cpp`, `gamepad_win32.cpp` against `gamepad_switch.cpp` —
and one header of oaknut's, replaced for the Switch so JIT pages come from
libnx.

You need, for either:

- **[dynarmic](https://github.com/yuzu-mirror/dynarmic)**, cloned somewhere.
  The upstream `merryhime/dynarmic` is gone; this mirror is the same code
  under the same 0BSD licence. No `--recursive` needed — it vendors its
  externals.
- **Boost headers**, 1.87 or thereabouts. Headers only, nothing to compile:
  dynarmic wants `icl` and `variant`.
- **Python 3**, for the OBB extractor and the dynarmic patch below.

Paths come from the environment, so nothing has to live where it lives here:

| variable | what it is | default |
| --- | --- | --- |
| `WB_DYNARMIC` | the dynarmic checkout | `H:\dwnld\dynarmic` |
| `WB_BOOST` | the Boost headers | `H:\dwnld\boost_1_87_0` |
| `WB_BUILD` | the Windows build tree | `H:\dwnld\wb_build` |
| `WB_SWITCH_BUILD` | the Switch build tree | `H:\dwnld\wb_switch` |
| `WB_DEVKITPRO` | where devkitPro is installed | found automatically |
| `VSROOT` | Visual Studio | 2022 Community |

Put the build tree somewhere without an `&` in the path. Several of the tools
in the chain quote carelessly, and `cmd` treats it as a command separator.

### Windows

Visual Studio 2022 with the **Desktop development with C++** workload —
CMake and Ninja come with it, and nothing needs to be on `PATH` beforehand.

```bat
configure.bat
build.bat
```

Then `run.bat` to play it, which also keeps a log. The mouse is a finger:
click to tap, drag to swipe. A plugged-in gamepad is read through XInput.
Closing the window ends the run.

### Switch

devkitPro with the **switch-dev** group, plus the GL stack and, into
devkitPro's own MSYS2, CMake and Ninja:

```bash
pacman -S switch-dev switch-mesa switch-libdrm_nouveau cmake ninja
```

```bat
configure_switch.bat
build_switch.bat
```

The `.nro` lands in the build tree. Copy it to the SD card as above.

Two things about that build are worth knowing before they surprise you.

**It must be devkitPro's own CMake, run from devkitPro's own shell.** Its
toolchain file refuses to run under any other CMake, and that CMake is an
MSYS2 program which reads `H:/x` as a relative path and glues it onto its
working directory. So `configure_switch.bat` translates every path with
`cygpath` and hands the job to `bash -lc`. If you build from the MSYS2 shell
yourself, none of that applies and the plain `cmake` invocation works.

**dynarmic needs a four-line patch**, applied for you by
`switch/patch_dynarmic.py` when you configure. Horizon hands out JIT memory as
two aliases of the same pages — one writable, one executable, at different
addresses — and dynarmic assumes a single address. oaknut, which it emits
through, already supports the split; the patch is which pointers get passed.
It is idempotent, and a patch rather than a fork because dynarmic is a
checkout, not part of this tree.

## When it goes wrong

The log is the tool. On the Switch it is `warband.log` beside the `.nro`; on
Windows `run.bat` writes one and prints its tail.

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
tools/                   the OBB extractor, disassembly and symbol helpers
NOTES.md                 how it works and what it cost to find out
```

## Legal

This runs a commercial game you must own, from files you must supply. The
repository contains no game code and no game data, and nothing here is
distributable together with either. The engine has no public source; what is
here is the layer underneath it.

dynarmic is 0BSD. `stb_vorbis` is public domain. The oaknut header under
`switch/` is a modified copy of oaknut's own, MIT, and says so at the top.
