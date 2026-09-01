# warband_nx: the engineering log

How the port works, and what it cost to find out. `README.md` is the short
version - what it is, how to build it, how to install it. This is everything
underneath: the decisions, the measurements, and the four or five days where
the answer was nothing like the guess.

It is written in the order things were learned rather than tidied afterwards,
because the order is half the value: the wrong hypotheses are recorded next to
the evidence that killed them.

**Where it stands:** the game runs on hardware - main menu, sound, controller
- and on Windows through the tutorial ground at 60 fps.

## Why a JIT at all

The usual Android→Switch route (the `*_nx` ports collected in
[ChanseyIsTheBest/SwitchPorts](https://github.com/ChanseyIsTheBest/SwitchPorts))
loads the original **arm64-v8a** `.so` natively inside a minimal Android
environment. Warband has no 64-bit build and never did — the Android version
is from March 2014 and left the Play Store before Google's 64-bit mandate.
So the binary has to be translated, not loaded.

## Status

The engine boots, mounts its data, loads every shader, mesh and texture,
reaches `CD3DApplication::Render3DEnvironment` - its main loop - and draws
the Warband main menu at around 25 frames a second on the PC host.

| Piece | State |
|---|---|
| Loader, guest space, thunk dispatch | works |
| libc, allocator, stdio, printf family, maths | works |
| Guest threads, mutexes, condition variables | works |
| Android glue, lifecycle, NVIDIA status bits | works |
| Mini-JNI, including the JavaVM table | works |
| Filesystem over the extracted OBB | works |
| EGL, GL context, 60 GLES2 thunks | works |
| Shader compile and link | works |
| Main loop, main menu on screen | works |
| Input: touch and keys, straight to the engine's handler | works |
| Gamepad: XInput on the PC, hid on the Switch | works |
| On-screen frame counter | works |
| Software keyboard (Switch swkbd) | written, untested |
| New Game through to character creation | works |
| A 3D scene | loads far too slowly, then the driver's timeout kills it |
| FMOD Ex | answered locally, silent |
| Guest-side clock (removes 393M crossings) | works |
| Host crash reporter with guest context | works |
| Switch platform layer (EGL, W^X JIT memory) | builds; `.nro` produced |
| Switch build run on hardware | error 2144-0001, not yet diagnosed |
| Input (hid), audio through audren | not started |
| ARM EHABI unwinder, qsort, setjmp | not implemented (17 imports) |

### Five things worth knowing before touching this again

**The engine already supports a gamepad, because it is the SHIELD build.**
There is no need to invent a control scheme: it reads `gamepadAxisIndices`
and `gamepadButtonIndices` from Java, takes buttons as key events carrying
Android's `BUTTON_*` keycodes, and reads sticks through
`AMotionEvent_getAxisValue`. Feeding it events in that shape is all a
controller needs, on either platform. Buttons map by label rather than by
position - Switch A to `BUTTON_A` - so the button marked A confirms, which is
what both a Switch and an Android user expect; mapping by position would put
confirm on B.

**GL belongs to one guest thread, and the others must be ignored.** The
engine calls GL from more than one thread: `directx_thread` draws, and
`Render3DEnvironment_thread` destroys meshes as it loads, because
`rglMesh::~rglMesh` calls `glDeleteBuffers` wherever it happens to run. On
the device those stray calls do nothing - only one thread ever binds the EGL
context, and a GLES call without one is silently ignored. Emulating that
faithfully is the fix. Giving every thread a working shared context is *less*
faithful and kills the process: the deletes start landing on objects the
render thread is still drawing from, and the driver dies on its own worker
thread with nothing of ours on the stack.

**Loading a campaign costs 456 MiB and 556 000 live allocations.** The guest
space started at 512 MiB and ran out partway through "Loading Setting Data" -
not a leak on either side, just what the campaign costs: a million calls to
`operator new` during the load, fewer than half of them ever freed. It is now
2 GiB, which rules out applet mode on the Switch.

The allocator behind it had to be rewritten too. First-fit over one block
chain is fine for a few thousand blocks and quadratic for half a million:
allocation walked the heap from the bottom and free walked all of it to
coalesce, so the engine read `scenes.txt` and then simply stayed inside
`malloc` and never came out. Segregated free lists with boundary tags fixed
it. The symptom to recognise is a guest that stops retiring instructions
without crashing - the sampling profiler goes silent because nothing is
executing at all.

**Reading the clock was two thirds of the render thread.** The frame limiter
polls `clock_gettime`; that came to 393 million calls in a four-minute run,
each one a trap out of the recompiler. A clock cannot be computed in guest
code but it can be read there, so a host thread now publishes the time into a
page of guest memory and the import resolves to a fifteen-instruction seqlock
read. Nothing crosses the boundary and the frame rate roughly doubled. This
is what Linux calls a vDSO, and it is the general answer for any import that
is hot and cheap.

**A guest FILE\* has to be a real object in guest memory.** Returning an
opaque token works right up until the engine's statically linked libstdc++
reaches inside it: `fileno()` is a macro over `((FILE*)f)->_file`, and the
filebuf then calls `read()` with whatever that gave. Shaders arrived as
zero-byte strings for exactly this reason - the file opened, the read went to
a descriptor that did not exist, and an empty shader "compiled" fine and then
failed to link with `must write to gl_Position`. The FILE object now lives in
guest memory with the descriptor at offset 14, as 32-bit bionic lays it out.

**Anything hot and cheap belongs guest-side.** `__aeabi_idiv` was called 2.2
million times a run - more than every file, string and memory operation
combined - to do one division. It, the other EABI division helpers and the
three clock calls are now a few ARM instructions each, living in guest memory
and resolved by the loader before it reaches for a trampoline. Between them
that removed about 396 million boundary crossings. `guest_code.cpp` is where
the next one goes.

### The scene load: solved

Choosing Play Tutorial and then Continue used to put one frame on the screen
and then lose the process without a word. The Windows event log named the
executioner:

    NVIDIA OpenGL Driver: A TDR has been detected. The application must
    close. Error code: 7 (warband_nx.exe 64bit)

Timeout Detection and Recovery - the GPU did not finish inside the driver's
two-second limit, so the driver reset it and Windows killed the application
instantly, past every handler. That is why there was no fault report, no
terminate handler, no atexit hook, and why the address sanitizer had nothing
to say: no memory was corrupted, there was simply a command the GPU could not
complete. **Look in the event log first when a process dies silently.**

The command was a vertex shader that never returns. The shipped shaders were
run through an optimiser that writes loops like this:

```glsl
for (int j_13; j_13 < iLightPointCount; j_13++) {
  int tmpvar_15 = iLightIndices[j_13];
  ...
}
```

The counter has no initialiser. GLSL leaves that value undefined, and the
compiler on the device happened to start it at zero. Desktop NVIDIA starts it
at whatever the register held: when that is a large negative number the loop
runs for billions of iterations, indexing a four-element array with it the
whole way, and the draw never finishes. Forty of the files in
`GLShadersOptimized` have it - every scene vertex shader with a point-light
loop - and none of the interface shaders do, which is exactly why the menu
was fine and the first scene frame was not.

`initialise_loop_counters` in `thunks_gl.cpp` gives every such counter a zero
on the way through `glShaderSource`, before any platform translation, so the
Switch build gets it too. With that, the tutorial ground draws and holds 60
fps.

Finding it took building the diagnostics to ask the right question, and they
are worth keeping:

- `--profile` waits for the GPU after every draw and bills the time to the
  shader program, printing the frame's total. It turns "the process died" into
  "this draw, with these parameters, took 1.2 seconds" - and the watchdog then
  prints the draw's arguments and the program's source.
- `--check-verts` keeps a copy of every vertex buffer and reports the
  bounding box of the positions each draw reads, which is how "the geometry is
  nonsense" gets ruled out rather than assumed.

### The other thing that was wrong: stale vertex attribute arrays

The engine does not import `glDisableVertexAttribArray` at all. It enables an
attribute array when a mesh has that attribute and never turns one off, so the
arrays left over from the previous mesh stay enabled, still pointing into the
previous mesh's buffer. When the next mesh has more vertices than that buffer
holds, the hardware fetches past its end:

    draw mode 0x0004 count 4908 program 681: attribute 4 would read 104680
    bytes of buffer 2438 which holds 22400 (index 1635, stride 64, offset 32)

Undefined behaviour, no GL error, and on a stricter driver a fault. `guard_draw`
shadows every buffer size and the attribute table, works out the largest index
in the range being drawn, and turns off any array that would overrun it,
giving the attribute a constant instead. On by default; `--no-draw-guard`
leaves it alone, and the game does survive without it on this driver - keep it
for Horizon, where an out-of-range fetch is far less likely to be forgiven.

### Where it stops now

The tutorial ground renders and runs, but the scene is dark - a starfield in
the sky and a faint horizon. Whether that is the scene's own time of day or
lighting arriving wrong is the next question.

### Things only running it could have found

- The image region was sized 39.0 MiB for a 39.3 MiB image.
- `ExclusiveMonitor` was built for one processor; it indexes by
  `processor_id`, so a second guest thread corrupted the host heap.
- `pthread_cond_timedwait` returning 0 instead of `ETIMEDOUT` turns every
  `while (!ready) wait()` into a spin.
- The JNIEnv is at `app+0x38`, and the NVIDIA status bitfield at `app+0x58`;
  `android_main` loops on bit 0 and checks it before any lifecycle command
  can arrive.
- `android_main` is a loop, not a launcher - it needs its own thread.
- `vsnprintf` was never implemented, so every formatted string the engine
  built was uninitialised memory. Paths and error text both.
- The whole maths library was unbound: every `sqrtf`, `sinf` and `powf`
  returned zero.
- Warband looks in the active module first and falls back to the shared
  trees, and it sometimes joins a root onto an already-absolute path. Both
  are handled in `to_host`.

## How it works

The engine is mapped into one flat 2 GiB host allocation, so translating a
guest pointer is a single add. Every import is bound to one of three things:

1. **The guest libc**, when the symbol is pure computation — `memcpy`,
   `strtod`, `sinf`, `malloc`, the ARM EABI helpers, the EHABI unwinder.
   156 of the 377 imports belong here and never cross the boundary.
2. **An 8-byte trampoline** — `svc #index ; bx lr` — for the 221 imports that
   genuinely need the host. The JIT traps the `svc` and calls a host function
   that reads its own arguments out of the guest registers.
3. **Guest storage**, for the four imported data objects (`__sF`, `_ctype_`,
   `_tolower_tab_`, `__stack_chk_guard`). Their sizes are not declared in the
   ELF, so the guest libc has to agree with the loader about them.

Guest memory is reached through a **page table** rather than the read/write
callbacks: one entry per 4 KiB page, each pointing straight into the flat
allocation. The callbacks stay as the fallback for unmapped pages, which is
what makes the null guard work.

`malloc` has to be guest-side: a host pointer is 64 bits and will not fit in
a guest word. For the same reason every opaque host handle — `EGLDisplay`,
`FMOD::Sound*`, `pthread_mutex_t` — needs a handle table that hands the guest
a 32-bit token. That table is not written yet.

## Facts the code depends on

Established by `../port_analysis/loader_ref.py` and `armattrs.py`, both run
against `libMBExpMobile.so`:

- Two `PT_LOAD` segments: `r-x` 3 714 364 bytes, `rw-` 35 908 + 37 438 968
  zero-fill. 39.3 MiB of guest image.
- 6587 relocations, four kinds only: `R_ARM_RELATIVE` 6200,
  `R_ARM_JUMP_SLOT` 369, `R_ARM_GLOB_DAT` 13, `R_ARM_ABS32` 5.
- No `PT_TLS`, no `DT_TEXTREL`. `DT_SYMBOLIC` is set.
- 4786 dynsym entries: 377 imports, 4406 exports. Not stripped, so any engine
  function can be hooked or replaced.
- `android_main` at image + 0x2360E8; 95 `init_array` constructors run first.
- **softfp**: `.ARM.attributes` has no `Tag_ABI_VFP_args`, so float and double
  arguments arrive in core registers, not VFP. Thunks read a float as a
  32-bit pattern.
- `THUMB_ISA_use 2` and `Advanced_SIMD_arch 1`: mixed ARM/Thumb-2 with NEON.

## Backend coverage: checked, and it holds

The worry was that dynarmic's AArch64 backend is younger than its x86-64 one
and might not cover A32 NEON. Measured against the actual checkout:

- The arm64 backend has an `EmitIR` specialisation for **all 724** IR opcodes,
  and `backend/arm64/` contains `a32_address_space`, `a32_core`,
  `a32_interface`, `emit_arm64_a32*` — A32 guest on AArch64 host is a
  first-class path, not an afterthought. It is what Android builds of 3DS
  emulators run on.
- **54** of those are pure stubs: body is `(void)` casts plus
  `ASSERT_FALSE("Unimplemented")`. Another 62 functions merely contain a
  defensive `ASSERT` on an unexpected branch and work fine — do not confuse
  the two when grepping.
- Of the 54, the A32 front end can name **24**. Every one is a size variant
  ARMv7 cannot produce: 18 are FP16 arithmetic (ARMv7 has no half-precision
  maths, only VCVT conversion), 5 need 64- or 128-bit vector elements, and
  `VectorSignExtend64` is unreachable by construction — both call sites
  compute `esize * 2` into a 128-bit result, so the source is at most 32.

**Nothing an ARMv7 binary can encode lands on a stub.** That removes the
single largest technical risk from the plan. The remaining unknown is
throughput, not correctness.

One thing that is *not* true: `arch_version` does not gate ARMv8 A32
instructions in the decoder. `v8_VRINT*` and friends translate regardless;
`ArchVersion()` is only consulted for BX/BLX-era behaviour. The protection
here comes from the binary being compiled ARMv7-A, not from the setting.

## Sound

FMOD Ex is a closed 32-bit ARM library with no build for either target, so
none of it can be forwarded. `thunks_fmod.cpp` answers the API and the samples
go to a mixer of our own: `audio_mix.cpp` is platform-independent,
`audio_win32.cpp` feeds waveOut, and `audio_switch.cpp` is where audren will
go. The decoder is a vendored `stb_vorbis.c`, which is public domain and
builds for both.

The assets split the same way FMOD's own API does. The five hundred effects in
`Sounds/` are decoded whole the first time something plays them and kept -
decoding all of them at load would put five hundred files through the decoder
before the main menu appeared, and most are never heard. The sixty tracks in
`music/` are two hundred and forty megabytes between them and are decoded a
block at a time as they play, which is what `createStream` asks for.

Two things about it are not obvious:

- **`System::init`'s first argument is the channel limit** and it has to be
  honoured. The engine asks for a hundred and relies on FMOD stealing the
  least important channel when they run out; without that, sixty voices reach
  the mixer at once and it clips solid. Voices are stolen oldest-one-shot
  first, and a looping sound - which is ambience the engine expects to still
  be there - is never taken.
- **The mix needs a limiter.** Thirty voices of full-scale samples add up to
  far more than a sample can hold. The gain follows the loudest recent peak,
  ducks at once and recovers slowly.

`--rec=FILE.wav` writes everything the mixer produced. It is the only way to
tell a silent run from a run nobody was listening to, and the log carries a
`peak N of 32767 across M voices` line every second for the same reason.

## The controller

The engine already knows about gamepads - this is the SHIELD build - and it
asks Java what the controller can do in the first few instructions of
`android_main`:

```
NvGetGamepadAxes(env, activity, &count)     gamepadAxisIndices   [I
                                            gamepadAxisMinVals   [F
                                            gamepadAxisMaxVals   [F
NvGetGamepadButtons(env, activity, &count)  gamepadButtonIndices [I
NvHasGamepadButton(19, ...)                 is there a d-pad?
NvHasGamepadAxis(15, ...) NvHasGamepadAxis(16, ...)   or a hat?
```

Those fields used to come back null, because the JNI layer only answered
fields whose descriptor said String - and a null answer means the engine
believes there is no controller at all, whatever events arrive afterwards.
`jni.cpp` now holds real Java arrays, with `GetArrayLength`,
`GetIntArrayRegion`, `GetFloatArrayRegion` and the `Get*ArrayElements` pair.
The lists are searched by value rather than by position, so what matters is
which Android constants are in them, not their order. The d-pad is offered as
buttons and not as hat axes; offering both has the engine hear one press
twice.

**`BUTTON_SELECT` is not a button as far as this engine is concerned.** Its
key handler compares against 19..22 for the d-pad, 96..108 for the face and
shoulder buttons, 4 for Android's Back and 66 for Enter. 109 is not in the
list, so a pad whose Back button sends `BUTTON_SELECT` has a Back button that
does nothing. It sends keycode 4.

The pad is polled from the main loop, which is the input loop as much as the
watch loop: nothing the platform reads reaches the engine until `pump_input`
runs. It ticks every five milliseconds rather than every fifty, because fifty
was three frames of lag on every button, stick and click.

## Numbers in the interface were addresses

"Distance: 38528128 yards", "38527640 men" - the numbers on screen were guest
addresses. Warband's script machine works in 64-bit registers and prints them
with a bare `"%lld"`, and the formatter dropped the length modifier and read
32 bits.

That is worse than losing half a number. For `snprintf(buf, size, "%lld", v)`
AAPCS aligns the 64-bit argument to eight: `r0` is the buffer, `r1` the size,
`r2` the format, **`r3` is skipped**, and the value goes on the stack. Reading
four bytes returns `r3` - whatever the caller happened to leave there, which
is usually a pointer. `guest_format.cpp` reads the length modifier separately
now and takes a 64-bit argument for `ll`, `q` and `j`. Not for `l`: a `long`
on ARM32 is four bytes, and widening it would break every `%ld` in the engine.

## Building for the PC

The upstream `merryhime/dynarmic` repository is gone (404). Use the mirror
fork, which is the same 0BSD code with the A32 front end intact:

```bash
git clone https://github.com/yuzu-mirror/dynarmic
cmake -B build -DDYNARMIC_DIR=/path/to/dynarmic
cmake --build build
./build/warband_nx path/to/libMBExpMobile.so
```

It vendors its externals directly rather than as submodules, so `--recursive`
does nothing. CMake looks for it beside this project and one level up before
falling back to `-DDYNARMIC_DIR`.

The mirror is frozen at 2024-03-05, the day yuzu shut down. That is fine
here: we need a stable A32 front end, not new features. Actively maintained
alternatives, if one is ever needed, live inside emulator projects that use
A32 in production — `azahar-emu/dynarmic` and `Vita3K/dynarmic` are both
0BSD and both emulate 32-bit ARM guests on AArch64 hosts.

**Watch the path.** This project sits under a directory containing a space
and an `&`. Plenty of Windows build tooling mishandles that; if the build
misbehaves in strange ways, move the tree somewhere plainer before debugging
anything else.

Useful switches while running it:

```
--data=DIR          the extracted OBB tree
--seconds=N         how long to let the guest run
--shot=NAME         write NAME_01.bmp, NAME_02.bmp ... from the GL back buffer
--shot-every=N      seconds between those grabs (default 30)
--tap=X,Y@SEC       a finger at X,Y that many seconds in; repeatable
--trace-ticks=N     sample the guest pc every N instructions, with symbols
--svcs=0            no cap on import calls
--profile           wait for the GPU after each draw, bill it per program
--check-verts       describe the box each draw's positions cover
--no-draw-guard     leave out-of-range vertex arrays enabled
--rec=FILE.wav      write everything the mixer produced
--press=CODE@SEC    a controller button that many seconds in; repeatable
```

The frame grab is worth knowing about: the desktop paints a placeholder over
a window whose thread is busy inside the guest, so a screenshot taken from
outside shows a blank rectangle even while the game is drawing correctly.
`--shot` reads the back buffer instead, and works the same way on the Switch,
where there is no desktop to grab from at all.

## Building for the Switch

Needs devkitPro with the **switch-dev** group, plus `switch-mesa` and
`switch-libdrm_nouveau` for the GL stack, and `cmake` and `ninja` installed
into devkitPro's own MSYS2:

```bash
pacman -S switch-mesa switch-libdrm_nouveau cmake ninja
```

Then:

```bash
configure_switch.bat
```

Four things about that build are not obvious, and all four cost time:

- **It has to be devkitPro's CMake.** `dkp-initialize-path.cmake` aborts with
  "CMake must be installed and launched from msys2" the moment
  `CMAKE_HOST_WIN32` is true, so Visual Studio's copy cannot be used at all.
  That CMake is linked against `msys-*.dll`, so devkitPro's `msys2/usr/bin`
  has to be on PATH or it will not start — and that failure is a missing-DLL
  line on stderr, which looks nothing like a CMake error and is easy to read
  as a configure that worked.
- **The MSYS2 tree may be older than the packages it installs.** Installing
  `cmake` pulled one that wanted `msys-jsoncpp-27.dll` while the tree still
  had 26. `pacman -Su` fixes it.
- **CMake 4 rejects `cmake_minimum_required(VERSION 3.0)`,** which dynarmic's
  vendored robin-map still declares, so `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`
  is required rather than advisory.
- **Precompiled headers have to be off.** The compiler is a native Windows
  binary driven by an MSYS2 CMake; the toolchain file translates the paths it
  controls, but CMake bakes an MSYS path into the PCH and the compiler cannot
  open it.

And one that is a genuine collision rather than a build-system quirk: libnx
defines `BIT(n)` as a macro, and oaknut has an assembler mnemonic of that
name, so every oaknut header parsed after `switch.h` fails. Our copy of
`code_block.hpp` undefines it right after the include, which is the only
place the two ever meet.

On the SD card:

```
/switch/warband/warband_nx.nro
/switch/warband/libMBExpMobile.so     the engine, taken from the APK
/switch/warband/gamedata/             the extracted OBB tree, 795 MB
```

`build_switch.bat` writes the `.nro` to `H:\dwnld\wb_switch`, and
`H:\dwnld\wb_sdcard` holds it beside the engine ready to copy; `gamedata`
goes next to them.

Three things about Horizon shape the build, and all three are handled.

**W^X.** A page is never writable and executable at once, and there is no
`mmap`. JIT memory comes from libnx's `Jit` object, which moves a region
between the two states on request — which is exactly oaknut's
`protect()`/`unprotect()` pair, the one dynarmic already calls when
`DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT` is on. `switch/oaknut_override/` holds a
copy of oaknut's `code_block.hpp` with a Horizon branch, placed ahead of the
vendored one for the Switch build only.

libnx picks the backing mechanism inside `jitCreate`, and on a current console
with Atmosphère it picks the code-memory syscalls - **whether or not the .nro
was launched with title takeover**. Those map the same physical pages twice, at
two different addresses, both live at once: one writable, one executable. The
other mechanism, `svcSetProcessMemoryPermission`, gives a single region whose
permissions flip. Both have to work, and the first is the common one.

dynarmic assumes a single address - it builds its generator as
`code(mem.ptr(), mem.ptr())` - so everything it emits is branched to at the
address it was written to, which under code memory is memory nothing may
execute. oaknut already supports the split: its generator takes a write
pointer and an execute pointer and computes every label and relocation against
the second. `switch/patch_dynarmic.py` changes the four places that pass those
pointers, and `configure_switch.bat` runs it; it is idempotent, and it is a
patch rather than a fork because dynarmic is a checkout, not part of this tree.

There are four places, and the fourth is the one that costs a day: **the
AArch64 spin lock assembles itself into a 4 KiB block of its own during static
initialisation**, before main, and takes its two function pointers out of it
the same wrong way. Miss it and everything else works right up until the first
guest constructor touches the exclusive monitor, which branches to the
writable alias and takes an instruction abort at offset zero of a block
nothing else ever mentions.

Two more things about the split are easy to get wrong:

- **`protect()` and `unprotect()` must do nothing when the two aliases
  differ.** There is no transition to make - both are permanently mapped with
  the permissions they need - and libnx's transition for that type flushes the
  whole region's caches, which would be thirty-two megabytes of cache
  maintenance around every basic block.
- **Cache maintenance is split too.** The dirty lines are at the address that
  was written and the fetch happens at the address that will be branched to,
  so `invalidate` cleans the data cache through the writable alias and
  invalidates the instruction cache through the executable one.

**Memory.** The guest space is 2 GiB in one allocation, plus a 32 MiB code
cache per guest thread. That needs application memory, so the `.nro` must be
launched with title takeover (hold R on a game in hbmenu) rather than in
applet mode's ~448 MiB.

Title takeover gets the process about 3.1 GiB, all of it handed to newlib as
the heap - and **a three-gigabyte newlib heap will not serve a single
two-gigabyte `malloc`.** It comes back null, with nothing to say why, which
reads exactly like not having the memory at all. So the arena is taken off the
front of the heap region in `__libnx_initheap`, before newlib is told where
the heap is; `g_preallocated_space` is what `Memory` then uses. Kernel heap
pages arrive zeroed, which is the guarantee `calloc` would otherwise have cost
seconds of memset to provide.

**Reading a `[FAULT]` block.** The dump prints the handler's own runtime
address, and `__libnx_exception_handler` is in the `.elf`'s symbol table, so
the difference between them is where the image was loaded:

```bash
aarch64-none-elf-nm warband_nx.elf | grep __libnx_exception_handler
```

Subtract that from the runtime address in the log to get the load base, take
it off `pc` or `lr`, and `aarch64-none-elf-addr2line -f -C -e warband_nx.elf`
turns the result into a function and a line. The same arithmetic works on the
`[init] N/24 at 0x...` lines, which is how a constructor that dies before main
gets a name.

**No console.** The console owns the framebuffer and so does the game, so
logging goes over the network with `nxlinkStdio()`; run `nxlink -s` on the
PC. If nothing is listening it falls back to the on-screen console, which is
only useful for a boot that fails early.

## What the first run gives you

Every stubbed import prints itself the first time the engine calls it. That
trace is the work queue: it shows which of the 221 the engine actually
touches before it needs a frame, and in what order. Implement against the
trace rather than against the alphabet.

## Layout

```
src/guest.h            address-space map and flat memory
src/elf_loader.{h,cpp} maps the image, applies relocations, binds imports
src/env.{h,cpp}        dynarmic callbacks, svc dispatch, AAPCS arg reader
src/thunks_generated.cpp   221 stubs + the dispatch table  (generated)
tools/gen_thunks.py    regenerates the above from inventory.json
```

Analysis lives in `../port_analysis/`: `imports.md` is the per-symbol
reference, `loader_ref.py` is the Python loader the C++ one was checked
against.

## Legal

Runs a commercial game binary you must supply yourself, and the engine has
no public source. Nothing here is distributable with game code or assets.
