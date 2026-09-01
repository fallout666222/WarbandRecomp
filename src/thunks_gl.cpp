// EGL and OpenGL ES 2.0.
//
// EGL is answered locally - there is no EGL underneath on either target that
// matches what the engine expects, and the whole surface is a dozen calls
// over opaque handles. Handles are small integers rather than pointers, which
// sidesteps the 64-bit-pointer-into-a-32-bit-word problem entirely.
//
// The GL calls go through g_gl, filled by the platform file. Most are pure
// scalars and pass straight through; the interesting ones are marked below.
// Two deserve attention:
//
//   glShaderSource        - a guest char** of guest char*, and the source is
//                           GLSL ES which desktop GL will not accept
//   glDrawElements and    - the last argument is a client pointer when no
//   glVertexAttribPointer   buffer is bound and a byte offset when one is, so
//                           the binding has to be shadowed to tell them apart

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "env.h"
#include "gl_api.h"
#include "overlay.h"

namespace wb {
namespace {

// --------------------------------------------------------------------- EGL
constexpr u32 kEglNoDisplay = 0;
constexpr u32 kEglDisplay = 0x1001;
constexpr u32 kEglConfig = 0x2001;
constexpr u32 kEglSurface = 0x3001;
constexpr u32 kEglContext = 0x4001;
constexpr u32 kEglTrue = 1;
constexpr u32 kEglFalse = 0;

int g_width = 1280;
int g_height = 720;
bool g_context_live = false;
u64 g_frames = 0;

// -------------------------------------------------- GL from several threads
//
// The engine calls GL from more than one guest thread: directx_thread draws,
// and Render3DEnvironment_thread creates and destroys meshes and textures as
// it loads. A GL call made with no context current is not an error the driver
// reports - it dereferences its own null thread state and takes the process
// with it - so every thread must have one.
//
// But handing every thread a working context is not more faithful than the
// device, it is less. On the device only one thread ever binds the EGL
// context, and a GLES call on a thread without one is silently ignored -
// rglMesh's destructor calling glDeleteBuffers from the loader thread simply
// does nothing there, and the engine releases the real resource later, on the
// render thread, through its own deferred queue. Give that thread a shared
// context and the deletes start landing, and the driver dies dereferencing an
// object the render thread is still drawing from.
//
// So ownership follows EGL, exactly as on the device: whoever last bound the
// context - or presented, which requires having bound it - owns GL, and
// everyone else's calls are dropped and counted.
std::mutex g_gl_lock;
std::atomic<int> g_gl_owner{0};    // guest thread id, 0 until a surface exists

// A frame grab asked for from outside, taken by whichever thread presents.
std::mutex g_shot_lock;
std::string g_shot_path;

// Checking glGetError after every call costs a pipeline flush each time, so
// it is off unless asked for. When something draws nothing and there is no
// crash to look at, it is the fastest way to find out why: the driver has
// usually already said so and nobody was listening.
bool g_check_errors = false;
thread_local const char* t_in_call = nullptr;

// Drops every draw call while still doing all the state changes and uploads.
// The graphics driver kills the process the moment the first scene frame is
// submitted, which makes the load impossible to study; with drawing off the
// run survives and the loading can be watched to its end.
bool g_draw = true;

// Waits for the GPU after every draw and names any that took long enough to
// matter. Submission is asynchronous, so a draw that overruns the driver's
// two-second watchdog kills the process at some arbitrary later point with
// nothing to point at; waiting turns "the process died" into "this draw, with
// these parameters, took 2.4 seconds".
bool g_draw_sync = false;

// Wait for the GPU every so many draws.
//
// The scene's first frame is the whole problem. Nothing in it is slow on its
// own - waiting after every single draw found not one call over fifty
// milliseconds - but the frame as a whole is more work than the graphics
// driver's two-second timeout will sit through, and when it fires the process
// is killed outright: no exception, no handler, nothing in the log.
//
// glFlush is not enough; it was tried, every 64 draws, and the driver still
// batched the frame into one submission and still killed us. glFinish is,
// because it makes the driver actually retire the work before more is queued.
// Doing that once per draw costs about fifty frames a second; once per 64
// costs almost nothing, and bounds how much can ever be outstanding.
int g_sync_every = 0;
std::atomic<unsigned> g_draws_since_sync{0};

// How much is in the frame the driver refuses to sit through. Counted rather
// than guessed: a hundred draws that each take twenty milliseconds is a
// different bug from a hundred thousand that take nothing.
std::atomic<unsigned> g_draws_this_frame{0};
std::atomic<unsigned long long> g_verts_this_frame{0};
std::atomic<unsigned> g_fbo_binds_this_frame{0};

void maybe_sync() {
  if (g_sync_every <= 0 || !g_gl.glFinish) return;
  if (g_draws_since_sync.fetch_add(1) + 1 < static_cast<unsigned>(g_sync_every))
    return;
  g_draws_since_sync.store(0);
  g_gl.glFinish();
}

// Shadowed only so that a report about a draw can say which program and which
// render target it belonged to. Reading them back from the driver would be a
// pipeline stall on every draw.
thread_local GLuint g_program = 0;
thread_local GLuint g_texture = 0;
thread_local GLuint g_renderbuffer = 0;
// Texture dimensions by name. A framebuffer whose colour attachment and depth
// attachment are different sizes is legal on the desktop and catastrophic:
// some drivers answer it by rendering in software.
std::unordered_map<GLuint, std::pair<int, int>> g_tex_size;
std::unordered_map<GLuint, std::pair<int, int>> g_rb_size;
std::mutex g_size_lock;

void note_tex_size(GLuint tex, int w, int h) {
  std::lock_guard<std::mutex> lock(g_size_lock);
  g_tex_size[tex] = {w, h};
}
void note_rb_size(GLuint rb, int w, int h) {
  std::lock_guard<std::mutex> lock(g_size_lock);
  g_rb_size[rb] = {w, h};
}
std::pair<int, int> tex_size(GLuint tex) {
  std::lock_guard<std::mutex> lock(g_size_lock);
  auto it = g_tex_size.find(tex);
  return it == g_tex_size.end() ? std::pair<int, int>{-1, -1} : it->second;
}
std::pair<int, int> rb_size(GLuint rb) {
  std::lock_guard<std::mutex> lock(g_size_lock);
  auto it = g_rb_size.find(rb);
  return it == g_rb_size.end() ? std::pair<int, int>{-1, -1} : it->second;
}
thread_local GLuint g_framebuffer = 0;

// The viewport in force, so a report about a draw can say how many pixels it
// could possibly have covered. Same reasoning as the shadowed program: asking
// the driver would stall the pipeline.
thread_local GLint g_viewport[4] = {0, 0, 0, 0};

// Where the frame's GPU time goes.
//
// The driver kills the process when a frame takes longer than two seconds,
// and waiting after every single draw showed no draw over fifty milliseconds
// - which rules out one monstrous call and leaves the sum. Whether four
// hundred draws at five milliseconds each or four at half a second is a
// completely different bug, so the time is measured per draw and added up per
// shader program, and the frame prints its own bill.
bool g_profile = false;

// Turns off the vertex attribute arrays a draw would read past the end of.
// On by default: it is a repair, not a diagnostic - see guard_draw. Declared
// here because the switch and the draw are in different parts of this file.
bool g_guard_draws = true;
// Keeps a copy of every vertex buffer so a draw can be described by what its
// triangles cover. Off by default: it doubles the memory the meshes take.
bool g_check_verts = false;
std::atomic<unsigned> g_stale_arrays{0};

// The parameters of the draw in progress, for the watchdog to print when one
// stops returning. A draw that never comes back leaves no other trace, and
// the watchdog runs on its own thread - so the program goes in a plain global
// rather than the shadowed, per-thread one.
std::mutex g_last_draw_lock;
std::string g_last_draw;
std::atomic<unsigned> g_last_program{0};

struct ProgramCost {
  double seconds = 0.0;
  unsigned draws = 0;
  unsigned long long verts = 0;
};
std::mutex g_cost_lock;
std::unordered_map<GLuint, ProgramCost> g_cost;
double g_frame_gpu = 0.0;
double g_worst_secs = 0.0;
GLuint g_worst_program = 0;
GLsizei g_worst_count = 0;
GLuint g_worst_fbo = 0;
GLint g_worst_vp[4] = {0, 0, 0, 0};

// Which shaders a program was built from, so the expensive one can be shown.
std::unordered_map<GLuint, std::vector<GLuint>> g_program_shaders;

// Keeps the translated source per shader so a failed compile can show what
// was actually handed to the driver, not what the engine wrote - and so the
// program that ate the frame can be printed rather than merely numbered.
std::unordered_map<GLuint, std::string> g_shader_source;

// Prints the shaders a program was linked from, once per program.
void show_program(GLuint program) {
  static std::unordered_map<GLuint, bool> said;
  if (said[program]) return;
  said[program] = true;
  auto it = g_program_shaders.find(program);
  if (it == g_program_shaders.end()) {
    std::printf("[prog] %u: no shaders recorded\n", program);
    return;
  }
  for (GLuint sh : it->second) {
    auto src = g_shader_source.find(sh);
    std::printf("[prog] program %u shader %u:\n%s\n", program, sh,
                src == g_shader_source.end() ? "(source not kept)"
                                             : src->second.c_str());
  }
  std::fflush(stdout);
}

// Prints the frame's bill when it is large enough to be the problem, and
// resets it either way. The order is by time, because the question is always
// which shader to look at first.
void report_frame_cost(double presented) {
  std::vector<std::pair<GLuint, ProgramCost>> rows;
  double total = 0.0, worst = 0.0;
  GLuint worst_program = 0;
  GLsizei worst_count = 0;
  GLuint worst_fbo = 0;
  GLint worst_vp[4] = {0, 0, 0, 0};
  {
    std::lock_guard<std::mutex> lock(g_cost_lock);
    total = g_frame_gpu;
    worst = g_worst_secs;
    worst_program = g_worst_program;
    worst_count = g_worst_count;
    worst_fbo = g_worst_fbo;
    for (int i = 0; i < 4; ++i) worst_vp[i] = g_worst_vp[i];
    rows.assign(g_cost.begin(), g_cost.end());
    g_cost.clear();
    g_frame_gpu = 0.0;
    g_worst_secs = 0.0;
  }
  static double loudest = 0.0;
  if (total < 0.05 && total < loudest) return;
  if (total > loudest) loudest = total;
  std::sort(rows.begin(), rows.end(),
            [](const std::pair<GLuint, ProgramCost>& a,
               const std::pair<GLuint, ProgramCost>& b) {
              return a.second.seconds > b.second.seconds;
            });
  std::printf("[cost] frame: %.3f s on the GPU across %zu programs, present "
              "%.3f s\n", total, rows.size(), presented);
  std::printf("[cost]   worst single draw %.4f s  program %u count %d fbo %u "
              "viewport %dx%d\n", worst, worst_program, worst_count, worst_fbo,
              worst_vp[2], worst_vp[3]);
  const std::size_t show = rows.size() < 8 ? rows.size() : 8;
  for (std::size_t i = 0; i < show; ++i)
    std::printf("[cost]   program %-5u %8.4f s  %5u draws  %10llu vertices  "
              "%.3f ms each\n", rows[i].first, rows[i].second.seconds,
                rows[i].second.draws, rows[i].second.verts,
                rows[i].second.draws
                    ? rows[i].second.seconds / rows[i].second.draws * 1000.0
                    : 0.0);
  std::fflush(stdout);
  // The one that cost the most is the one worth reading.
  if (!rows.empty() && rows[0].second.seconds > 0.2) show_program(rows[0].first);
}

void show_last_program() {
  show_program(g_last_program.load(std::memory_order_relaxed));
}

void note_draw_cost(double secs, GLsizei count) {
  std::lock_guard<std::mutex> lock(g_cost_lock);
  ProgramCost& c = g_cost[g_program];
  c.seconds += secs;
  c.draws += 1;
  c.verts += count > 0 ? static_cast<unsigned long long>(count) : 0ull;
  g_frame_gpu += secs;
  if (secs > g_worst_secs) {
    g_worst_secs = secs;
    g_worst_program = g_program;
    g_worst_count = count;
    g_worst_fbo = g_framebuffer;
    for (int i = 0; i < 4; ++i) g_worst_vp[i] = g_viewport[i];
  }
}

// Names every GL call before it is made, unbuffered. When the graphics driver
// takes the process down there is no handler and no unwind - the last line in
// the log is the only evidence of which call did it, and a call that is only
// logged after it returns leaves no line at all.
bool g_gl_trace = false;

// The last few hundred GL calls, and which one is in progress.
//
// The graphics driver's watchdog fires two seconds after a call stops making
// progress and takes the process with it - no exception, no handler, nothing
// flushed. Printing every call as it happens does capture that, but it costs
// fifty frames a second, which is enough never to reach the interesting part.
// So the names go into memory instead, and are printed only when gl_watchdog
// notices a call that has not returned.
constexpr int kRingSize = 512;
const char* g_ring[kRingSize] = {};
std::atomic<unsigned> g_ring_at{0};
std::atomic<const char*> g_in_call{nullptr};
std::atomic<double> g_call_since{0};
std::atomic<bool> g_watchdog_on{false};
std::atomic<bool> g_watchdog_stop{false};
std::thread g_watchdog;

double gl_now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void show_last_program();

void dump_ring(const char* why) {
  std::printf("[gl  ] %s. The last GL calls, newest first:\n", why);
  const unsigned end = g_ring_at.load();
  const unsigned first = end > kRingSize ? end - kRingSize : 0;
  for (unsigned i = end; i > first; --i) {
    const char* n = g_ring[(i - 1) % kRingSize];
    if (n) std::printf("[ring] %5u %s\n", end - i + 1, n);
  }
  std::fflush(stdout);
}

void claim_gl(const char* why) {
  const int me = current_thread_id();
  if (g_gl_owner.exchange(me) != me)
    std::printf("[gl  ] GL belongs to guest thread %d (%s)\n", me, why);
}

bool owns_gl() { return current_thread_id() == g_gl_owner.load(); }

// Takes the pending grab, if there is one, and clears it.
std::string take_shot_request() {
  std::lock_guard<std::mutex> lock(g_shot_lock);
  std::string path;
  path.swap(g_shot_path);
  return path;
}

}  // namespace

unsigned long long gl_frame_count() { return g_frames; }

void gl_request_shot(const char* path) {
  std::lock_guard<std::mutex> lock(g_shot_lock);
  g_shot_path = path ? path : "";
}

void gl_start_watchdog();

void gl_set_drawing(bool on) {
  gl_start_watchdog();
  g_draw = on;
  if (!on) std::printf("[gl  ] drawing disabled\n");
}

void gl_set_check_verts(bool on) {
  g_check_verts = on;
  if (on)
    std::printf("[gl  ] keeping vertex data to describe what each draw "
                "covers\n");
}

void gl_set_guard_draws(bool on) {
  g_guard_draws = on;
  if (!on)
    std::printf("[gl  ] out-of-range vertex arrays will be left alone\n");
}

unsigned gl_stale_arrays() {
  return g_stale_arrays.load(std::memory_order_relaxed);
}

void gl_set_profile(bool on) {
  gl_start_watchdog();
  g_profile = on;
  if (on)
    std::printf("[gl  ] measuring GPU time per draw and billing it to the "
              "shader program\n");
}

void gl_set_draw_sync(bool on) {
  g_draw_sync = on;
  if (!on) return;
  std::printf("[gl  ] waiting for the GPU after every draw\n");
}

void gl_set_sync_every(int draws) {
  g_sync_every = draws;
  if (draws > 0)
    std::printf("[gl  ] waiting for the GPU every %d draws\n", draws);
}

void gl_set_error_checking(bool on) {
  g_check_errors = on;
  std::printf("[gl  ] error checking %s\n", on ? "on" : "off");
}

void gl_set_call_trace(bool on) {
  g_gl_trace = on;
  if (on) std::printf("[gl  ] naming every GL call before it is made\n");
}

void gl_thunk_enter(const char* name) {
  if (!g_context_live) return;
  g_gl_lock.lock();
  g_ring[g_ring_at.fetch_add(1) % kRingSize] = name;
  g_in_call.store(name, std::memory_order_relaxed);
  g_call_since.store(gl_now(), std::memory_order_relaxed);
  if (g_gl_trace) {
    std::printf("[glc ] %s\n", name);
    std::fflush(stdout);
  }
  if (owns_gl()) {
    gl_make_current(true);
    t_in_call = name;
    return;
  }
  t_in_call = nullptr;
  // Not the GL thread. Name the first few: if something the engine genuinely
  // needs turns up here, this is where it will show.
  static std::mutex report_lock;
  static std::unordered_map<std::string, int> dropped;
  std::lock_guard<std::mutex> lock(report_lock);
  const std::string key = std::to_string(current_thread_id()) + " " + name;
  if (dropped[key]++ == 0 && dropped.size() <= 12)
    std::printf("[gl  ] %s from guest thread %d dropped: not the GL thread\n",
                name, current_thread_id());
}

void gl_thunk_leave() {
  if (!g_context_live) return;
  if (g_check_errors && t_in_call && g_gl.glGetError) {
    const GLenum err = g_gl.glGetError();
    if (err != 0) {
      static std::mutex report_lock;
      static std::unordered_map<std::string, int> seen;
      std::lock_guard<std::mutex> lock(report_lock);
      const std::string key = std::string(t_in_call) + "/" + std::to_string(err);
      if (seen[key]++ == 0 && seen.size() <= 40)
        std::printf("[gl  ] %s -> error 0x%04X\n", t_in_call, err);
    }
  }
  t_in_call = nullptr;
  g_in_call.store(nullptr, std::memory_order_relaxed);
  g_gl_lock.unlock();
}

// Watches for a GL call that has stopped returning. It has to notice inside
// the driver's two-second timeout, so it looks four times a second and speaks
// up after one - by the time the driver acts there is no process left to
// print anything.
//
// Kept joinable rather than detached, and not as a convenience: on Horizon
// `pthread_detach` fails, `std::thread::detach` turns that into a thrown
// system_error, and the temporary thread object is then destroyed while still
// joinable - which is a direct call to std::terminate, with no exception yet
// being handled, so the message says "no exception" and names nothing. A
// thread that is joined at the end has none of that.
void gl_start_watchdog() {
  if (g_watchdog_on.exchange(true)) return;
  g_watchdog = std::thread([] {
    bool said = false;
    while (!g_watchdog_stop.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      const char* name = g_in_call.load(std::memory_order_relaxed);
      if (!name) {
        said = false;
        continue;
      }
      const double waited =
          gl_now() - g_call_since.load(std::memory_order_relaxed);
      if (waited < 1.0 || said) continue;
      said = true;
      std::printf("[gl  ] %s has not returned after %.1f s\n", name, waited);
      {
        std::lock_guard<std::mutex> lock(g_last_draw_lock);
        if (!g_last_draw.empty())
          std::printf("[gl  ] the draw in flight was: %s\n",
                      g_last_draw.c_str());
      }
      show_last_program();
      dump_ring("the graphics driver is about to give up");
    }
  });
}

void gl_stop_watchdog() {
  g_watchdog_stop.store(true, std::memory_order_relaxed);
  if (g_watchdog.joinable()) g_watchdog.join();
}

namespace {

// RAII form, for the EGL entries that touch the context themselves.
struct GlScope {
  explicit GlScope(const char* name) { gl_thunk_enter(name); }
  ~GlScope() { gl_thunk_leave(); }
};

void t_eglGetDisplay(Env& e) { e.ret(kEglDisplay); }

void t_eglInitialize(Env& e) {
  Env::Args a(e);
  a.next32();
  u32 major = a.next32(), minor = a.next32();
  if (major) e.mem().write32(major, 1);
  if (minor) e.mem().write32(minor, 4);
  e.ret(kEglTrue);
}

void t_eglTerminate(Env& e) {
  gl_destroy_context();
  g_context_live = false;
  e.ret(kEglTrue);
}

void t_eglGetConfigs(Env& e) {
  Env::Args a(e);
  a.next32();
  u32 configs = a.next32();
  u32 size = a.next32();
  u32 num = a.next32();
  if (configs && size >= 1) e.mem().write32(configs, kEglConfig);
  if (num) e.mem().write32(num, 1);
  e.ret(kEglTrue);
}

// The engine asks about the one config we offer. Values describe a plain
// 32-bit RGBA window with a 24/8 depth-stencil, which is what the PC context
// is set up for and what the Switch will provide too.
void t_eglGetConfigAttrib(Env& e) {
  Env::Args a(e);
  a.next32();                      // display
  a.next32();                      // config
  u32 attrib = a.next32();
  u32 out = a.next32();
  u32 value = 0;
  switch (attrib) {
    case 0x3021: value = 8; break;         // EGL_RED_SIZE
    case 0x3022: value = 8; break;         // EGL_GREEN_SIZE
    case 0x3023: value = 8; break;         // EGL_BLUE_SIZE
    case 0x3024: value = 8; break;         // EGL_ALPHA_SIZE
    case 0x3025: value = 24; break;        // EGL_DEPTH_SIZE
    case 0x3026: value = 8; break;         // EGL_STENCIL_SIZE
    case 0x3020: value = 32; break;        // EGL_BUFFER_SIZE
    case 0x3028: value = kEglConfig; break;// EGL_CONFIG_ID
    case 0x3027: value = 0x3050; break;    // EGL_CONFIG_CAVEAT -> NONE
    case 0x3033: value = 0x0004; break;    // EGL_SURFACE_TYPE -> WINDOW_BIT
    case 0x3040: value = 0x0004; break;    // EGL_RENDERABLE_TYPE -> ES2_BIT
    case 0x3031: value = 0; break;         // EGL_SAMPLES
    case 0x3032: value = 0; break;         // EGL_SAMPLE_BUFFERS
    // ANativeWindow_setBuffersGeometry is called with whatever comes back
    // from EGL_NATIVE_VISUAL_ID, so it needs to be something sane.
    case 0x302E: value = 1; break;         // EGL_NATIVE_VISUAL_ID
    default: value = 0; break;
  }
  if (out) e.mem().write32(out, value);
  e.ret(kEglTrue);
}

void t_eglCreateContext(Env& e) { e.ret(kEglContext); }

void t_eglCreateWindowSurface(Env& e) {
  if (!gl_create_context(g_width, g_height)) {
    std::printf("[egl ] no GL context available\n");
    e.ret(0);
    return;
  }
  g_context_live = true;
  claim_gl("surface created");
  std::printf("[egl ] window surface %dx%d\n", g_width, g_height);
  e.ret(kEglSurface);
}

// This is what decides which guest thread may talk to GL at all, so it is the
// one EGL entry that matters. A thread that binds the context takes ownership
// and gets a host context of its own, sharing the objects the previous owner
// created; a thread that releases it hands ownership on at the next bind.
void t_eglMakeCurrent(Env& e) {
  Env::Args a(e);
  a.next32();                      // display
  u32 draw = a.next32();
  a.next32();                      // read
  u32 ctx = a.next32();
  const bool bind = draw != 0 && ctx != 0;
  if (!g_context_live) {
    e.ret(kEglFalse);
    return;
  }
  std::lock_guard<std::mutex> lock(g_gl_lock);
  if (bind) {
    claim_gl("eglMakeCurrent");
    gl_make_current(true);
  } else {
    // Whatever this thread made has to be visible to the next owner.
    gl_flush_for_share();
    gl_make_current(false);
  }
  e.ret(kEglTrue);
}

void t_eglQuerySurface(Env& e) {
  Env::Args a(e);
  a.next32();
  a.next32();
  u32 attrib = a.next32();
  u32 out = a.next32();
  u32 value = 0;
  if (attrib == 0x3057) value = static_cast<u32>(g_width);    // EGL_WIDTH
  if (attrib == 0x3056) value = static_cast<u32>(g_height);   // EGL_HEIGHT
  if (out) e.mem().write32(out, value);
  e.ret(kEglTrue);
}

void t_eglSwapBuffers(Env& e) {
  // Presenting requires a bound context, so whoever swaps is the GL thread
  // even if the engine never called eglMakeCurrent again after startup.
  claim_gl("eglSwapBuffers");
  GlScope scope("eglSwapBuffers");
  // Read the frame back before it is swapped away: after SwapBuffers the back
  // buffer's contents are undefined.
  // The counter goes on before the read-back, so a frame grab shows what the
  // screen showed.
  overlay_frame(static_cast<double>(guest_monotonic_ns()) * 1e-9);
  overlay_draw();
  const std::string shot = take_shot_request();
  if (!shot.empty()) gl_capture(shot.c_str());
  const unsigned draws = g_draws_this_frame.exchange(0);
  const unsigned long long verts = g_verts_this_frame.exchange(0);
  const unsigned binds = g_fbo_binds_this_frame.exchange(0);
  // Which framebuffer is bound when the frame is presented. Presenting with
  // one of the engine's own render targets still bound is not what the device
  // does, and it is the sort of thing a desktop driver answers slowly.
  if (g_framebuffer != 0) {
    static unsigned said = 0;
    if (++said <= 8)
      std::printf("[gl  ] presenting with framebuffer %u bound, not 0\n",
                  g_framebuffer);
  }
  const double before = gl_now();
  gl_present();
  const double presented = gl_now() - before;
  if (g_profile) report_frame_cost(presented);
  // A frame is worth naming when it took long enough to be near the driver's
  // patience, or when it is the first one after a quiet stretch - which is
  // what a scene appearing looks like.
  static unsigned worst_draws = 0;
  if (presented > 0.25 || draws > worst_draws * 2) {
    if (draws > worst_draws) worst_draws = draws;
    std::printf("[frame] %llu draws, %llu vertices, %u framebuffer binds, "
              "present took %.3f s\n", (unsigned long long)draws, verts,
                binds, presented);
    std::fflush(stdout);
  }
  if (++g_frames <= 5 || g_frames % 300 == 0)
    std::printf("[gl  ] t=%.1fs frame %llu, %.1f fps\n",
                static_cast<double>(guest_monotonic_ns()) * 1e-9,
                (unsigned long long)g_frames,
                static_cast<double>(overlay_fps()));
  e.ret(kEglTrue);
}

void t_eglDestroySurface(Env& e) { e.ret(kEglTrue); }
void t_eglDestroyContext(Env& e) { e.ret(kEglTrue); }

// ---------------------------------------------------------------- GL state
//
// Shadowed so the pointer-or-offset arguments can be told apart.
//
// Per thread, because buffer bindings are per context and each thread has its
// own. Sharing one copy meant the loader thread binding a vertex buffer made
// the render thread read its next client-array pointer as a byte offset - a
// wild fetch inside the driver, on a driver worker thread, with nothing of
// ours left on the stack to blame.
thread_local GLuint g_array_buffer = 0;
thread_local GLuint g_element_buffer = 0;
bool g_reported_missing = false;

// Called by every GL thunk. The second half is the important one: a call made
// with no context current on this thread does not return an error, it faults
// inside the driver, so dropping it is the only safe answer.
bool ready() {
  if (gl_have_context() && owns_gl() && gl_context_current()) return true;
  if (!g_reported_missing) {
    g_reported_missing = true;
    std::printf("[gl  ] calls are being dropped: no context on guest thread "
              "%d\n", current_thread_id());
  }
  return false;
}

// How many values glGetIntegerv/glGetFloatv writes for a given pname. Getting
// this wrong corrupts guest memory just past the destination.
int value_count(GLenum pname) {
  switch (pname) {
    case 0x0C22:  // GL_COLOR_CLEAR_VALUE
    case 0x0C10:  // GL_SCISSOR_BOX
    case 0x0BA2:  // GL_VIEWPORT
    case 0x0C23:  // GL_COLOR_WRITEMASK
      return 4;
    case 0x846D:  // GL_ALIASED_POINT_SIZE_RANGE
    case 0x846E:  // GL_ALIASED_LINE_WIDTH_RANGE
    case 0x0D33:  // GL_MAX_VIEWPORT_DIMS is 2
      return 2;
    default:
      return 1;
  }
}


// Pixel and buffer uploads hand a guest pointer straight to the driver, which
// then reads as many bytes as the arguments imply. If those arguments are
// wrong - or the data sits near the end of the arena - the read runs off the
// end of the allocation and the host dies with no explanation. Everything
// therefore goes through a staging copy that cannot overrun.
const void* staged(Env& e, u32 ptr, std::size_t bytes, const char* what) {
  if (!ptr || bytes == 0) return nullptr;
  const u32 room = e.mem().clamp(ptr, static_cast<u32>(bytes));
  if (room < bytes) {
    static int shown = 0;
    if (++shown <= 8)
      std::printf("[gl  ] %s wants %zu bytes at 0x%08X but only %u are "
              "mapped - upload skipped\n", what, bytes, ptr, room);
    return nullptr;
  }
  static thread_local std::vector<u8> staging;
  staging.resize(bytes);
  std::memcpy(staging.data(), e.mem().host<u8>(ptr), bytes);
  return staging.data();
}

// Bytes per pixel for the formats GLES2 accepts.
std::size_t pixel_size(GLenum format, GLenum type) {
  std::size_t components = 1;
  switch (format) {
    case 0x1907: components = 3; break;   // GL_RGB
    case 0x1908: components = 4; break;   // GL_RGBA
    case 0x190A: components = 2; break;   // GL_LUMINANCE_ALPHA
    case 0x1906: components = 1; break;   // GL_ALPHA
    case 0x1909: components = 1; break;   // GL_LUMINANCE
    default: components = 4; break;
  }
  switch (type) {
    case 0x1401: return components;       // GL_UNSIGNED_BYTE
    case 0x8363:                          // GL_UNSIGNED_SHORT_5_6_5
    case 0x8033:                          // GL_UNSIGNED_SHORT_4_4_4_4
    case 0x8034: return 2;                // GL_UNSIGNED_SHORT_5_5_5_1
    case 0x1403: return components * 2;   // GL_UNSIGNED_SHORT
    case 0x1405: return components * 4;   // GL_UNSIGNED_INT
    case 0x1406: return components * 4;   // GL_FLOAT
    default: return components;
  }
}

}  // namespace

u32 jni_screen_width() { return static_cast<u32>(g_width); }
u32 jni_screen_height() { return static_cast<u32>(g_height); }

// ------------------------------------------------------------------ thunks
namespace {

#define A Env::Args a(e)

void t_glActiveTexture(Env& e) { A; GLenum x = a.next32(); if (ready()) g_gl.glActiveTexture(x); e.ret(0); }
void t_glAttachShader(Env& e) {
  A;
  GLuint p = a.next32(), s = a.next32();
  {
    std::lock_guard<std::mutex> lock(g_cost_lock);
    g_program_shaders[p].push_back(s);
  }
  if (ready()) g_gl.glAttachShader(p, s);
  e.ret(0);
}

void t_glBindBuffer(Env& e) {
  A;
  GLenum target = a.next32();
  GLuint buffer = a.next32();
  if (target == 0x8892) g_array_buffer = buffer;     // GL_ARRAY_BUFFER
  if (target == 0x8893) g_element_buffer = buffer;   // GL_ELEMENT_ARRAY_BUFFER
  if (ready()) g_gl.glBindBuffer(target, buffer);
  e.ret(0);
}

// Render targets are worth narrating. The scene draws through a framebuffer
// object and the interface does not, which is exactly the difference between
// what appears on screen and what does not - so when the world is black,
// these six calls are where to look, and their arguments are the answer.
void fbo_trace(const char* what, GLenum a1, GLenum a2, GLenum a3, GLuint a4) {
  static int shown = 0;
  if (++shown > 60) return;
  std::printf("[fbo ] %-28s 0x%04X 0x%04X 0x%04X %u\n", what, a1, a2, a3, a4);
}

void t_glBindFramebuffer(Env& e) {
  A;
  GLenum t = a.next32();
  GLuint f = a.next32();
  fbo_trace("glBindFramebuffer", t, 0, 0, f);
  g_fbo_binds_this_frame.fetch_add(1, std::memory_order_relaxed);
  g_framebuffer = f;
  if (ready()) g_gl.glBindFramebuffer(t, f);
  e.ret(0);
}

void t_glBindRenderbuffer(Env& e) {
  A;
  GLenum t = a.next32();
  GLuint r = a.next32();
  fbo_trace("glBindRenderbuffer", t, 0, 0, r);
  g_renderbuffer = r;
  if (ready()) g_gl.glBindRenderbuffer(t, r);
  e.ret(0);
}
void t_glBindTexture(Env& e) { A; GLenum t = a.next32(); GLuint x = a.next32(); g_texture = x; if (ready()) g_gl.glBindTexture(t, x); e.ret(0); }
void t_glBlendEquation(Env& e) { A; GLenum x = a.next32(); if (ready()) g_gl.glBlendEquation(x); e.ret(0); }
void t_glBlendFunc(Env& e) { A; GLenum s = a.next32(), d = a.next32(); if (ready()) g_gl.glBlendFunc(s, d); e.ret(0); }

// What is actually in a vertex buffer.
//
// A draw with sane arguments can still hang the GPU if the vertices are
// nonsense: coordinates in the billions make triangles that cover the screen
// thousands of times over, and rasterising those does not finish inside the
// driver's two-second patience. So the range of the values is worth knowing,
// and it is one pass over the data we are already copying.
void describe_vertices(const float* v, std::size_t count) {
  static int shown = 0;
  if (++shown > 6) return;
  float lo = v[0], hi = v[0];
  int nans = 0, huge = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const float f = v[i];
    if (f != f) { ++nans; continue; }
    if (f < lo) lo = f;
    if (f > hi) hi = f;
    if (f > 1e6f || f < -1e6f) ++huge;
  }
  std::printf("[vbo ] %zu floats: %g .. %g, %d not-a-number, %d beyond a "
              "million\n", count, static_cast<double>(lo),
              static_cast<double>(hi), nans, huge);

  // The bit pattern says where a bad value came from, and that decides where
  // to look next. A NaN the hardware produced is 0x7FC00000; a repeated byte
  // or something that looks like an address is memory nobody ever wrote.
  if (nans == 0 && huge == 0) return;
  std::printf("[vbo ]   bad words:");
  int listed = 0;
  for (std::size_t i = 0; i < count && listed < 8; ++i) {
    const float f = v[i];
    if (f == f && f < 1e6f && f > -1e6f) continue;
    u32 bits;
    std::memcpy(&bits, &v[i], 4);
    std::printf(" [%zu]=0x%08X", i, bits);
    ++listed;
  }
  std::printf("\n");
}

// ------------------------------------------------------- what a draw reads
//
// A draw whose arguments are all legal can still hang the GPU, and the way it
// usually happens is an index that points past the end of the vertex buffer:
// the hardware fetches from an address that was never mapped, faults, and the
// driver's recovery takes the process with it. Nothing in GL reports that -
// the specification calls it undefined behaviour and the driver obliges.
//
// So the sizes are shadowed on the way in and checked before the draw. Index
// data is kept as well as measured, because the largest index in the range
// being drawn is the whole question.
std::mutex g_buffer_lock;
std::unordered_map<GLuint, std::size_t> g_buffer_bytes;
// Index data, kept so the largest index in a drawn range can be found, and
// the answer per range, because the same ranges are drawn every frame.
struct IndexBuffer {
  std::vector<unsigned char> data;
  std::unordered_map<unsigned long long, unsigned long long> biggest;
};
std::unordered_map<GLuint, IndexBuffer> g_index_data;

struct AttribState {
  bool enabled = false;
  GLuint buffer = 0;
  GLint size = 4;
  GLenum type = 0x1406;      // GL_FLOAT
  GLsizei stride = 0;
  u32 offset = 0;
};
constexpr int kMaxAttribs = 16;
thread_local AttribState t_attrib[kMaxAttribs];

std::size_t type_bytes(GLenum type) {
  switch (type) {
    case 0x1400: case 0x1401: return 1;   // GL_BYTE, GL_UNSIGNED_BYTE
    case 0x1402: case 0x1403: return 2;   // GL_SHORT, GL_UNSIGNED_SHORT
    case 0x1404: case 0x1405: return 4;   // GL_INT, GL_UNSIGNED_INT
    case 0x140B: return 2;                // GL_HALF_FLOAT
    case 0x1406: default: return 4;       // GL_FLOAT
  }
}

// What the draw's triangles actually cover.
//
// A draw of five hundred triangles that the GPU cannot finish in a second is
// not doing five hundred triangles' worth of work, so the question is what
// the vertices say. Positions in the billions, or not-a-number, produce
// triangles the rasteriser walks for a very long time - and nothing in GL
// reports that, because none of it is an error.
//
// Vertex data is kept for this and the bounding box is worked out once per
// drawn range, the same way the largest index is.
std::unordered_map<GLuint, std::vector<unsigned char>> g_vertex_data;

struct Box {
  float lo[3] = {0, 0, 0};
  float hi[3] = {0, 0, 0};
  int nan = 0;
  bool valid = false;
};
std::unordered_map<unsigned long long, Box> g_box;

// Called with g_buffer_lock already held, from note_buffer_size.
void note_vertex_data(GLuint name, std::size_t bytes, const void* data) {
  if (!g_check_verts || !name) return;
  std::vector<unsigned char>& copy = g_vertex_data[name];
  copy.assign(bytes, 0);
  if (data) std::memcpy(copy.data(), data, bytes);
  // Any box worked out from the old contents is meaningless now.
  g_box.clear();
}

// The bounding box of the positions this draw reads, or nothing if the data
// is not held. Assumes attribute 0 is the position, which is what every one
// of the engine's vertex shaders declares first.
void describe_positions(GLenum mode, GLsizei count, unsigned long long max_index) {
  if (!g_check_verts) return;
  const AttribState& at = t_attrib[0];
  if (!at.enabled || !at.buffer || at.type != 0x1406) return;   // GL_FLOAT
  const std::size_t element = 4 * static_cast<std::size_t>(at.size);
  const std::size_t stride = at.stride ? static_cast<std::size_t>(at.stride) : element;
  Box box;
  {
    std::lock_guard<std::mutex> lock(g_buffer_lock);
    auto vit = g_vertex_data.find(at.buffer);
    if (vit == g_vertex_data.end()) return;
    const unsigned long long key =
        (static_cast<unsigned long long>(at.buffer) << 40) ^
        (static_cast<unsigned long long>(at.offset) << 20) ^ max_index;
    auto cached = g_box.find(key);
    if (cached != g_box.end()) {
      box = cached->second;
    } else {
      const std::vector<unsigned char>& v = vit->second;
      for (unsigned long long i = 0; i <= max_index; ++i) {
        const std::size_t at_byte = at.offset + stride * i;
        if (at_byte + 12 > v.size()) break;
        float p[3];
        std::memcpy(p, v.data() + at_byte, 12);
        for (int c = 0; c < 3; ++c) {
          if (p[c] != p[c]) { ++box.nan; continue; }
          if (!box.valid || p[c] < box.lo[c]) box.lo[c] = p[c];
          if (!box.valid || p[c] > box.hi[c]) box.hi[c] = p[c];
        }
        box.valid = true;
      }
      g_box[key] = box;
    }
  }
  if (!box.valid) return;
  float widest = 0;
  for (int c = 0; c < 3; ++c) {
    const float w = box.hi[c] - box.lo[c];
    if (w > widest) widest = w;
  }
  const bool wild = box.nan > 0 || widest > 1e5f;
  static std::mutex report_lock;
  static int shown = 0;
  std::lock_guard<std::mutex> lock(report_lock);
  if (!wild && ++shown > 30) return;
  std::printf("[pos ] %s mode 0x%04X count %d program %u: x %g..%g  y %g..%g  "
              "z %g..%g%s\n", wild ? "WILD" : "    ", mode, count, g_program,
              box.lo[0], box.hi[0], box.lo[1], box.hi[1], box.lo[2], box.hi[2],
              box.nan ? "  with not-a-number" : "");
  std::fflush(stdout);
}

void note_buffer_size(GLenum target, std::size_t bytes, const void* data) {
  const GLuint name = target == 0x8893 ? g_element_buffer : g_array_buffer;
  if (!name) return;
  std::lock_guard<std::mutex> lock(g_buffer_lock);
  g_buffer_bytes[name] = bytes;
  if (target != 0x8893) {
    note_vertex_data(name, bytes, data);
    return;
  }
  IndexBuffer& ib = g_index_data[name];
  ib.data.assign(bytes, 0);
  ib.biggest.clear();
  if (data) std::memcpy(ib.data.data(), data, bytes);
}

// The parameters of the draw in progress, for the watchdog to print when one
// stops returning. A draw that never comes back leaves no other trace.
void remember_draw(const std::string& text) {
  std::lock_guard<std::mutex> lock(g_last_draw_lock);
  g_last_draw = text;
}

// Reports the first few draws that read outside a buffer, and stops them from
// doing it.
//
// The engine does not import glDisableVertexAttribArray at all: it enables an
// attribute array when a mesh has that attribute and never turns one off. So
// the arrays left over from the previous mesh stay enabled, still pointing at
// the previous mesh's buffer - and when the next mesh has more vertices than
// that buffer holds, the hardware fetches past its end, faults, and the
// driver's timeout recovery kills the process. That is what a "one frame then
// gone" scene load is.
//
// Turning the stale array off and giving the attribute a constant is what a
// driver that bounds-checked would do anyway, and the engine re-enables the
// array the next time it means to use it, so nothing correct is lost.
void guard_draw(GLenum mode, GLsizei count, GLenum index_type,
                u32 index_offset) {
  if (count <= 0) return;
  static std::mutex report_lock;
  static int reported = 0;
  unsigned long long max_index = 0;
  bool have_max = false;
  if (g_element_buffer) {
    std::lock_guard<std::mutex> lock(g_buffer_lock);
    auto it = g_index_data.find(g_element_buffer);
    if (it != g_index_data.end()) {
      IndexBuffer& ib = it->second;
      const std::size_t isize = type_bytes(index_type);
      const std::size_t need = index_offset + isize * static_cast<std::size_t>(count);
      if (need > ib.data.size()) {
        std::lock_guard<std::mutex> rl(report_lock);
        if (++reported <= 20)
          std::printf("[chk ] element buffer %u is %zu bytes but the draw "
                      "reads %zu (offset %u, %d indices of %zu)\n",
                      g_element_buffer, ib.data.size(), need, index_offset,
                      count, isize);
        return;
      }
      // The same range is drawn over and over, so the scan is done once.
      const unsigned long long key =
          (static_cast<unsigned long long>(index_offset) << 32) ^
          (static_cast<unsigned long long>(count) << 3) ^ isize;
      auto cached = ib.biggest.find(key);
      if (cached != ib.biggest.end()) {
        max_index = cached->second;
      } else {
        const unsigned char* base = ib.data.data() + index_offset;
        for (GLsizei i = 0; i < count; ++i) {
          unsigned long long v = 0;
          if (isize == 1) v = base[i];
          else if (isize == 2) { unsigned short sh; std::memcpy(&sh, base + i * 2, 2); v = sh; }
          else { unsigned int u; std::memcpy(&u, base + i * 4, 4); v = u; }
          if (v > max_index) max_index = v;
        }
        ib.biggest[key] = max_index;
      }
      have_max = true;
    }
  }
  if (!have_max) return;
  describe_positions(mode, count, max_index);
  for (int i = 0; i < kMaxAttribs; ++i) {
    AttribState& at = t_attrib[i];
    if (!at.enabled) continue;
    const std::size_t element = type_bytes(at.type) * static_cast<std::size_t>(at.size);
    const std::size_t stride = at.stride ? static_cast<std::size_t>(at.stride) : element;
    const std::size_t need = at.offset + stride * max_index + element;
    if (!at.buffer) {
      // A client array is read out of guest memory by us, not by the GPU, and
      // the arena is far larger than any mesh - so this one is only worth
      // saying out loud.
      std::lock_guard<std::mutex> rl(report_lock);
      if (++reported <= 20)
        std::printf("[chk ] attribute %d reads a client array at 0x%08X; "
                    "highest index %llu needs %zu bytes\n",
                    i, at.offset, max_index, need);
      continue;
    }
    std::size_t have = 0;
    {
      std::lock_guard<std::mutex> lock(g_buffer_lock);
      auto it = g_buffer_bytes.find(at.buffer);
      have = it == g_buffer_bytes.end() ? 0 : it->second;
    }
    if (need <= have) continue;
    {
      std::lock_guard<std::mutex> rl(report_lock);
      if (++reported <= 20)
        std::printf("[chk ] draw mode 0x%04X count %d program %u: attribute %d "
                    "would read %zu bytes of buffer %u which holds %zu (index "
                    "%llu, stride %zu, offset %u) - array turned off\n",
                    mode, count, g_program, i, need, at.buffer, have,
                    max_index, stride, at.offset);
      else if (reported == 21)
        std::printf("[chk ] further out-of-range attribute arrays will be "
                    "turned off without saying so\n");
    }
    g_stale_arrays.fetch_add(1, std::memory_order_relaxed);
    if (!g_guard_draws) continue;
    at.enabled = false;
    if (g_gl.glDisableVertexAttribArray) g_gl.glDisableVertexAttribArray(i);
    if (g_gl.glVertexAttrib4f) g_gl.glVertexAttrib4f(i, 0.0f, 0.0f, 0.0f, 1.0f);
  }
}

void t_glBufferData(Env& e) {
  A;
  GLenum target = a.next32();
  GLsizeiptr size = static_cast<GLsizeiptr>(static_cast<std::int32_t>(a.next32()));
  u32 data = a.next32();
  GLenum usage = a.next32();
  if (ready()) {
    const void* p = staged(e, data, static_cast<std::size_t>(size < 0 ? 0 : size),
                           "glBufferData");
    if (p && target == 0x8892 && size >= 4096)      // GL_ARRAY_BUFFER
      describe_vertices(static_cast<const float*>(p),
                        std::min<std::size_t>(static_cast<std::size_t>(size) / 4,
                                              1024));
    note_buffer_size(target, static_cast<std::size_t>(size < 0 ? 0 : size), p);
    g_gl.glBufferData(target, size, p, usage);
  }
  e.ret(0);
}

void t_glClear(Env& e) { A; GLbitfield m = a.next32(); if (ready()) g_gl.glClear(m); e.ret(0); }

void t_glClearColor(Env& e) {
  A;
  // softfp: the four floats arrive as bit patterns in r0-r3.
  GLclampf r = a.nextf(), g = a.nextf(), b = a.nextf(), al = a.nextf();
  if (ready()) g_gl.glClearColor(r, g, b, al);
  e.ret(0);
}

void t_glCompileShader(Env& e) {
  A;
  GLuint sh = a.next32();
  if (!ready()) { e.ret(0); return; }
  g_gl.glCompileShader(sh);

  GLint ok = 0;
  g_gl.glGetShaderiv(sh, 0x8B81, &ok);       // GL_COMPILE_STATUS
  if (ok) { e.ret(0); return; }

  static int shown = 0;
  GLint len = 0;
  g_gl.glGetShaderiv(sh, 0x8B84, &len);      // GL_INFO_LOG_LENGTH
  std::vector<char> log(len > 1 ? static_cast<std::size_t>(len) : 1, 0);
  if (len > 1) g_gl.glGetShaderInfoLog(sh, len, nullptr, log.data());
  std::printf("[gl  ] shader %u failed to compile:\n%s\n", sh, log.data());
  if (++shown <= 2) {
    auto it = g_shader_source.find(sh);
    if (it != g_shader_source.end())
      std::printf("[gl  ] ---- source as given to the driver ----\n%s\n"
              "[gl  ] ---------------------------------------\n",
                  it->second.c_str());
  }
  e.ret(0);
}

void t_glCompressedTexImage2D(Env& e) {
  A;
  GLenum target = a.next32();
  GLint level = a.next32();
  GLenum fmt = a.next32();
  GLsizei w = a.next32(), h = a.next32();
  GLint border = a.next32();
  GLsizei bytes = a.next32();
  u32 data = a.next32();
  if (ready()) {
    const void* p = staged(e, data, static_cast<std::size_t>(bytes < 0 ? 0 : bytes),
                           "glCompressedTexImage2D");
    g_gl.glCompressedTexImage2D(target, level, fmt, w, h, border, bytes, p);
  }
  e.ret(0);
}

void t_glCreateProgram(Env& e) { e.ret(ready() ? g_gl.glCreateProgram() : 0); }
void t_glCreateShader(Env& e) { A; GLenum t = a.next32(); e.ret(ready() ? g_gl.glCreateShader(t) : 0); }
void t_glCullFace(Env& e) { A; GLenum x = a.next32(); if (ready()) g_gl.glCullFace(x); e.ret(0); }

// The delete family takes an array of names to read.
template <void (*Fn)(GLsizei, const GLuint*)>
void delete_names(Env& e) {
  A;
  GLsizei n = a.next32();
  u32 names = a.next32();
  if (ready() && n > 0 && names) {
    std::vector<GLuint> v(static_cast<std::size_t>(n));
    for (GLsizei i = 0; i < n; ++i) v[i] = e.mem().read32(names + i * 4);
    Fn(n, v.data());
  }
  e.ret(0);
}

void t_glDeleteBuffers(Env& e) {
  A; GLsizei n = a.next32(); u32 p = a.next32();
  if (ready() && n > 0 && p) {
    std::vector<GLuint> v(n);
    for (GLsizei i = 0; i < n; ++i) v[i] = e.mem().read32(p + i * 4);
    g_gl.glDeleteBuffers(n, v.data());
  }
  e.ret(0);
}
void t_glDeleteFramebuffers(Env& e) {
  A; GLsizei n = a.next32(); u32 p = a.next32();
  if (ready() && n > 0 && p) {
    std::vector<GLuint> v(n);
    for (GLsizei i = 0; i < n; ++i) v[i] = e.mem().read32(p + i * 4);
    g_gl.glDeleteFramebuffers(n, v.data());
  }
  e.ret(0);
}
void t_glDeleteRenderbuffers(Env& e) {
  A; GLsizei n = a.next32(); u32 p = a.next32();
  if (ready() && n > 0 && p) {
    std::vector<GLuint> v(n);
    for (GLsizei i = 0; i < n; ++i) v[i] = e.mem().read32(p + i * 4);
    g_gl.glDeleteRenderbuffers(n, v.data());
  }
  e.ret(0);
}
void t_glDeleteTextures(Env& e) {
  A; GLsizei n = a.next32(); u32 p = a.next32();
  if (ready() && n > 0 && p) {
    std::vector<GLuint> v(n);
    for (GLsizei i = 0; i < n; ++i) v[i] = e.mem().read32(p + i * 4);
    g_gl.glDeleteTextures(n, v.data());
  }
  e.ret(0);
}

void t_glDeleteProgram(Env& e) { A; GLuint p = a.next32(); if (ready()) g_gl.glDeleteProgram(p); e.ret(0); }
void t_glDeleteShader(Env& e) { A; GLuint s = a.next32(); if (ready()) g_gl.glDeleteShader(s); e.ret(0); }
void t_glDepthFunc(Env& e) { A; GLenum x = a.next32(); if (ready()) g_gl.glDepthFunc(x); e.ret(0); }
void t_glDepthMask(Env& e) { A; GLboolean m = a.next32() ? 1 : 0; if (ready()) g_gl.glDepthMask(m); e.ret(0); }
void t_glDisable(Env& e) { A; GLenum x = a.next32(); if (ready()) g_gl.glDisable(x); e.ret(0); }
// A draw the GPU cannot finish inside two seconds gets the whole process
// killed by the driver's timeout recovery, with no exception and no chance to
// report - so the parameters of every draw are worth a look when that starts
// happening. A wild vertex count, or a client array pointing at the wrong
// place, produces triangles the size of the universe and rasterising them
// never ends.
double draw_clock() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Named before the draw is issued, so that the call which never returns still
// leaves a line behind.
void draw_about_to(const char* what, GLenum mode, GLsizei count) {
  char line[256];
  std::snprintf(line, sizeof(line),
                "%s mode 0x%04X count %d program=%u array=%u elements=%u "
                "fbo=%u viewport %dx%d",
                what, mode, count, g_program, g_array_buffer, g_element_buffer,
                g_framebuffer, g_viewport[2], g_viewport[3]);
  remember_draw(line);
  g_last_program.store(g_program, std::memory_order_relaxed);
  if (!g_draw_sync) return;
  std::printf("[dr->] %s\n", line);
  std::fflush(stdout);
}

// Called after the draw has been submitted, with the wall time the GPU took.
void draw_time(const char* what, GLenum mode, GLsizei count, double seconds) {
  if (seconds < 0.05) return;
  std::printf("[SLOW] %s mode 0x%04X count %d took %.3f s  program=%u "
              "array=%u elements=%u fbo=%u\n",
              what, mode, count, seconds, g_program, g_array_buffer,
              g_element_buffer, g_framebuffer);
  std::fflush(stdout);
}

void draw_trace(const char* what, GLenum mode, GLsizei count, u32 pointer) {
  static int shown = 0;
  static GLsizei biggest = 0;
  const bool notable = count > biggest * 4 && count > 4096;
  if (notable) biggest = count;
  if (++shown > 40 && !notable) return;
  std::printf("[draw] %-16s mode 0x%04X count %d ptr 0x%08X  array=%u "
              "elements=%u\n", what, mode, count, pointer, g_array_buffer,
              g_element_buffer);
}

void t_glDrawArrays(Env& e) {
  A;
  GLenum m = a.next32();
  GLint f = a.next32();
  GLsizei c = a.next32();
  draw_trace("glDrawArrays", m, c, static_cast<u32>(f));
  draw_about_to("glDrawArrays", m, c);
  guard_draw(m, c, 0, 0);
  g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
  g_verts_this_frame.fetch_add(c > 0 ? static_cast<unsigned>(c) : 0u,
                               std::memory_order_relaxed);
  if (ready() && g_draw) {
    const double t0 = draw_clock();
    g_gl.glDrawArrays(m, f, c);
    maybe_sync();
    if ((g_draw_sync || g_profile) && g_gl.glFinish) {
      g_gl.glFinish();
      const double took = draw_clock() - t0;
      if (g_profile) note_draw_cost(took, c);
      if (g_draw_sync) draw_time("glDrawArrays", m, c, took);
    }
  }
  e.ret(0);
}

void t_glDrawElements(Env& e) {
  A;
  GLenum mode = a.next32();
  GLsizei count = a.next32();
  GLenum type = a.next32();
  u32 indices = a.next32();
  draw_trace("glDrawElements", mode, count, indices);
  draw_about_to("glDrawElements", mode, count);
  guard_draw(mode, count, type, g_element_buffer ? indices : 0);
  g_draws_this_frame.fetch_add(1, std::memory_order_relaxed);
  g_verts_this_frame.fetch_add(count > 0 ? static_cast<unsigned>(count) : 0u,
                               std::memory_order_relaxed);
  if (ready()) {
    // With an element buffer bound the value is a byte offset into it, not a
    // pointer, and translating it would be wrong.
    const void* p = g_element_buffer
                        ? reinterpret_cast<const void*>(
                              static_cast<std::uintptr_t>(indices))
                        : (indices ? e.mem().host<void>(indices) : nullptr);
    if (g_draw) {
      const double t0 = draw_clock();
      g_gl.glDrawElements(mode, count, type, p);
      maybe_sync();
      if ((g_draw_sync || g_profile) && g_gl.glFinish) {
        g_gl.glFinish();
        const double took = draw_clock() - t0;
        if (g_profile) note_draw_cost(took, count);
        if (g_draw_sync) draw_time("glDrawElements", mode, count, took);
      }
    }
  }
  e.ret(0);
}

void t_glEnable(Env& e) { A; GLenum x = a.next32(); if (ready()) g_gl.glEnable(x); e.ret(0); }
void t_glEnableVertexAttribArray(Env& e) {
  A;
  GLuint i = a.next32();
  if (i < kMaxAttribs) t_attrib[i].enabled = true;
  if (ready()) g_gl.glEnableVertexAttribArray(i);
  e.ret(0);
}
void t_glFramebufferRenderbuffer(Env& e) {
  A;
  GLenum t = a.next32(), at = a.next32(), rt = a.next32();
  GLuint rb = a.next32();
  fbo_trace("glFramebufferRenderbuffer", t, at, rt, rb);
  {
    const auto rs = rb_size(rb);
    static std::mutex once_lock;
    static std::unordered_map<unsigned long long, bool> said;
    std::lock_guard<std::mutex> lock(once_lock);
    const unsigned long long key = (static_cast<unsigned long long>(at) << 32) | rb;
    if (!said[key]) {
      said[key] = true;
      std::printf("[fbo ] attachment 0x%04X <- renderbuffer %u (%dx%d)\n", at,
                  rb, rs.first, rs.second);
    }
  }
  if (ready()) g_gl.glFramebufferRenderbuffer(t, at, rt, rb);
  e.ret(0);
}

void t_glFramebufferTexture2D(Env& e) {
  A;
  GLenum t = a.next32(), at = a.next32(), tt = a.next32();
  GLuint tex = a.next32();
  GLint lv = a.next32();
  fbo_trace("glFramebufferTexture2D", t, at, tt, tex);
  {
    // Colour and depth of different sizes is legal on the desktop and ruinous
    // on some drivers, which answer it by rendering in software. Worth saying
    // out loud exactly once per pair.
    const auto ts = tex_size(tex);
    static std::mutex once_lock;
    static std::unordered_map<unsigned long long, bool> said;
    std::lock_guard<std::mutex> lock(once_lock);
    const unsigned long long key =
        (static_cast<unsigned long long>(g_framebuffer) << 32) | tex;
    if (!said[key]) {
      said[key] = true;
      std::printf("[fbo ] framebuffer %u colour <- texture %u (%dx%d)\n",
                  g_framebuffer, tex, ts.first, ts.second);
    }
  }
  if (ready()) g_gl.glFramebufferTexture2D(t, at, tt, tex, lv);
  e.ret(0);
}

// The gen family writes an array of new names back.
void gen_names(Env& e, void (*fn)(GLsizei, GLuint*)) {
  A;
  GLsizei n = a.next32();
  u32 out = a.next32();
  if (n <= 0 || !out) { e.ret(0); return; }
  std::vector<GLuint> v(static_cast<std::size_t>(n), 0);
  if (ready()) fn(n, v.data());
  for (GLsizei i = 0; i < n; ++i) e.mem().write32(out + i * 4, v[i]);
  e.ret(0);
}

void t_glGenBuffers(Env& e) { gen_names(e, g_gl.glGenBuffers); }
void t_glGenFramebuffers(Env& e) { gen_names(e, g_gl.glGenFramebuffers); }
void t_glGenRenderbuffers(Env& e) { gen_names(e, g_gl.glGenRenderbuffers); }
void t_glGenTextures(Env& e) { gen_names(e, g_gl.glGenTextures); }

void t_glGenerateMipmap(Env& e) { A; GLenum t = a.next32(); if (ready()) g_gl.glGenerateMipmap(t); e.ret(0); }

void t_glGetAttribLocation(Env& e) {
  A;
  GLuint p = a.next32();
  std::string name = e.mem().str(a.next32());
  e.ret(ready() ? static_cast<u32>(g_gl.glGetAttribLocation(p, name.c_str()))
                : 0xFFFFFFFF);
}

void t_glGetUniformLocation(Env& e) {
  A;
  GLuint p = a.next32();
  std::string name = e.mem().str(a.next32());
  e.ret(ready() ? static_cast<u32>(g_gl.glGetUniformLocation(p, name.c_str()))
                : 0xFFFFFFFF);
}

void t_glGetFloatv(Env& e) {
  A;
  GLenum pname = a.next32();
  u32 out = a.next32();
  int n = value_count(pname);
  std::vector<GLfloat> v(n, 0.0f);
  if (ready()) g_gl.glGetFloatv(pname, v.data());
  for (int i = 0; i < n && out; ++i) {
    u32 bits;
    std::memcpy(&bits, &v[i], 4);
    e.mem().write32(out + i * 4, bits);
  }
  e.ret(0);
}

void t_glGetIntegerv(Env& e) {
  A;
  GLenum pname = a.next32();
  u32 out = a.next32();
  int n = value_count(pname);
  std::vector<GLint> v(n, 0);
  if (ready()) g_gl.glGetIntegerv(pname, v.data());
  for (int i = 0; i < n && out; ++i)
    e.mem().write32(out + i * 4, static_cast<u32>(v[i]));
  e.ret(0);
}

void t_glGetProgramiv(Env& e) {
  A;
  GLuint p = a.next32();
  GLenum pname = a.next32();
  u32 out = a.next32();
  GLint v = 0;
  if (ready()) g_gl.glGetProgramiv(p, pname, &v);
  if (out) e.mem().write32(out, static_cast<u32>(v));
  e.ret(0);
}

void t_glGetShaderiv(Env& e) {
  A;
  GLuint s = a.next32();
  GLenum pname = a.next32();
  u32 out = a.next32();
  GLint v = 0;
  if (ready()) g_gl.glGetShaderiv(s, pname, &v);
  if (out) e.mem().write32(out, static_cast<u32>(v));
  e.ret(0);
}

void info_log(Env& e, void (*fn)(GLuint, GLsizei, GLsizei*, GLchar*),
              const char* what) {
  A;
  GLuint obj = a.next32();
  GLsizei cap = a.next32();
  u32 length_out = a.next32();
  u32 text_out = a.next32();
  std::vector<char> buf(cap > 0 ? static_cast<std::size_t>(cap) : 1, 0);
  GLsizei written = 0;
  if (ready() && cap > 0) fn(obj, cap, &written, buf.data());
  if (written > 0) {
    // Worth surfacing: a shader that will not compile is otherwise silent.
    std::printf("[gl  ] %s log: %.*s\n", what, static_cast<int>(written),
                buf.data());
  }
  if (length_out) e.mem().write32(length_out, static_cast<u32>(written));
  if (text_out && cap > 0)
    e.mem().copy_in(text_out, buf.data(),
                    static_cast<std::size_t>(written) + 1);
  e.ret(0);
}

void t_glGetProgramInfoLog(Env& e) { info_log(e, g_gl.glGetProgramInfoLog, "program"); }
void t_glGetShaderInfoLog(Env& e) { info_log(e, g_gl.glGetShaderInfoLog, "shader"); }

void t_glLinkProgram(Env& e) {
  A;
  GLuint p = a.next32();
  if (!ready()) { e.ret(0); return; }
  g_gl.glLinkProgram(p);

  // The engine reads the log with a small buffer, so what it prints is a
  // truncated first line. Report the whole thing here instead - a program
  // that will not link is why the renderer gives up.
  GLint ok = 0;
  g_gl.glGetProgramiv(p, 0x8B82, &ok);       // GL_LINK_STATUS
  if (!ok) {
    static int shown = 0;
    GLint len = 0;
    g_gl.glGetProgramiv(p, 0x8B84, &len);    // GL_INFO_LOG_LENGTH
    std::vector<char> log(len > 1 ? static_cast<std::size_t>(len) : 1, 0);
    if (len > 1) g_gl.glGetProgramInfoLog(p, len, nullptr, log.data());
    if (++shown <= 4)
      std::printf("[gl  ] program %u failed to link:\n%s\n", p, log.data());
    else if (shown == 5)
      std::printf("[gl  ] (further link failures not shown)\n");
  }
  e.ret(0);
}
void t_glPixelStorei(Env& e) { A; GLenum n = a.next32(); GLint v = a.next32(); if (ready()) g_gl.glPixelStorei(n, v); e.ret(0); }
void t_glRenderbufferStorage(Env& e) {
  A;
  GLenum t = a.next32(), f = a.next32();
  GLsizei w = a.next32(), h = a.next32();
  fbo_trace("glRenderbufferStorage", t, f, static_cast<GLenum>(w),
            static_cast<GLuint>(h));
  note_rb_size(g_renderbuffer, w, h);
  if (ready()) g_gl.glRenderbufferStorage(t, f, w, h);
  e.ret(0);
}
void t_glScissor(Env& e) { A; GLint x = a.next32(), y = a.next32(); GLsizei w = a.next32(), h = a.next32(); if (ready()) g_gl.glScissor(x, y, w, h); e.ret(0); }

// glShaderSource(shader, count, const GLchar* const* string, const GLint* len)
// The array and every string in it live in guest memory, and the source is
// GLSL ES that desktop GL will not compile as written.
// Gives every loop counter a starting value.
//
// The shipped shaders were run through an optimiser that emits
//
//     for (int j_13; j_13 < iLightPointCount; j_13++)
//
// with no initialiser. GLSL says that value is undefined, and the compiler on
// the device happened to make it zero. Desktop NVIDIA does not: the counter
// starts at whatever the register held, and when that is a large negative
// number the loop runs for billions of iterations, indexing a four-element
// array with it the whole way. The vertex shader never finishes, the driver's
// timeout recovery fires two seconds later, and the process is killed with no
// exception and nothing in the log.
//
// That is the scene load dying one frame in. Every scene vertex shader with a
// point-light loop has it - forty of the files in GLShadersOptimized - and
// the interface shaders do not, which is exactly the part that worked.
//
// Zero is not a guess: it is the only value that makes the loop mean what the
// rest of the shader assumes, and it is what the device produced.
std::string initialise_loop_counters(const std::string& source) {
  std::string out;
  out.reserve(source.size() + 64);
  std::size_t i = 0;
  int fixed = 0;
  while (i < source.size()) {
    // Find the next `for` that stands on its own.
    const std::size_t at = source.find("for", i);
    if (at == std::string::npos) break;
    const bool word_start = at == 0 || !(std::isalnum(static_cast<unsigned char>(source[at - 1])) ||
                                         source[at - 1] == '_');
    std::size_t p = at + 3;
    while (p < source.size() && std::isspace(static_cast<unsigned char>(source[p]))) ++p;
    if (!word_start || p >= source.size() || source[p] != '(') {
      out.append(source, i, at + 3 - i);
      i = at + 3;
      continue;
    }
    const std::size_t open = p;
    const std::size_t semi = source.find(';', open);
    if (semi == std::string::npos) break;
    const std::string init = source.substr(open + 1, semi - open - 1);
    // Only a declaration with no initialiser needs anything doing: two words
    // and nothing else.
    std::size_t a1 = init.find_first_not_of(" \t\r\n");
    bool declaration = false;
    std::string type;
    if (init.find('=') == std::string::npos && a1 != std::string::npos) {
      const std::size_t a2 = init.find_first_of(" \t\r\n", a1);
      if (a2 != std::string::npos) {
        type = init.substr(a1, a2 - a1);
        const std::size_t b1 = init.find_first_not_of(" \t\r\n", a2);
        if (b1 != std::string::npos &&
            init.find_first_of(" \t\r\n", b1) == std::string::npos &&
            (type == "int" || type == "float" || type == "lowp" ||
             type == "mediump" || type == "highp"))
          declaration = true;
      }
    }
    out.append(source, i, semi - i);
    if (declaration) {
      out += type == "float" ? " = 0.0" : " = 0";
      ++fixed;
    }
    i = semi;
  }
  out.append(source, i, std::string::npos);
  if (fixed) {
    static int said = 0;
    if (++said <= 8)
      std::printf("[gl  ] %d loop counter%s in this shader had no starting "
                  "value; set to zero\n", fixed, fixed == 1 ? "" : "s");
  }
  return out;
}

void t_glShaderSource(Env& e) {
  A;
  GLuint shader = a.next32();
  GLsizei count = a.next32();
  u32 strings = a.next32();
  u32 lengths = a.next32();
  {
    static int shown = 0;
    if (++shown <= 4)
      std::printf("[gl  ] glShaderSource(shader=%u count=%d strings=0x%08X "
              "lengths=0x%08X) first=0x%08X len=%d\n",
                  shader, count, strings, lengths,
                  strings ? e.mem().read32(strings) : 0,
                  lengths ? static_cast<int>(e.mem().read32(lengths)) : -1);
    if (shown <= 4 && strings) {
      u32 sp = e.mem().read32(strings);
      std::printf("[gl  ]   bytes at 0x%08X:", sp);
      for (int k = 0; k < 24; ++k) {
        u8 b = e.mem().read8(sp + k);
        std::printf(" %02X", b);
      }
      std::printf("\n");
    }
  }
  if (count <= 0 || !strings) { e.ret(0); return; }

  std::string combined;
  for (GLsizei i = 0; i < count; ++i) {
    u32 sp = e.mem().read32(strings + i * 4);
    if (!sp) continue;
    if (lengths) {
      std::int32_t len = static_cast<std::int32_t>(e.mem().read32(lengths + i * 4));
      if (len > 0) {
        std::vector<char> tmp(static_cast<std::size_t>(len));
        for (std::int32_t k = 0; k < len; ++k)
          tmp[k] = static_cast<char>(e.mem().read8(sp + k));
        combined.append(tmp.data(), static_cast<std::size_t>(len));
        continue;
      }
    }
    combined += e.mem().str(sp, 1 << 20);
  }

  // What the engine handed over, before translation - the engine assembles
  // this itself by reading the .glsl a character at a time, so it is worth
  // being able to see whether what arrived is the whole file.
  e.set_call_log(false);
  static int dumped = 0;
  if (dumped < 4) {
    char path[64];
    std::snprintf(path, sizeof(path), "shader_%d_raw.glsl", dumped);
    if (std::FILE* f = std::fopen(path, "wb")) {
      std::fwrite(combined.data(), 1, combined.size(), f);
      std::fclose(f);
    }
    std::printf("[gl  ] shader %u: %zu bytes from the engine -> %s\n", shader,
                combined.size(), path);
    ++dumped;
  }

  // Before anything platform-specific: this one is a bug in the shaders
  // themselves and bites any driver that does not happen to zero a register.
  combined = initialise_loop_counters(combined);
  if (gl_needs_glsl_translation()) combined = gl_translate_shader(combined);
  g_shader_source[shader] = combined;
  if (ready()) {
    const char* src = combined.c_str();
    GLint len = static_cast<GLint>(combined.size());
    g_gl.glShaderSource(shader, 1, &src, &len);
  }
  e.ret(0);
}

void t_glTexImage2D(Env& e) {
  A;
  GLenum target = a.next32();
  GLint level = a.next32();
  GLint internal = a.next32();
  GLsizei w = a.next32(), h = a.next32();
  GLint border = a.next32();
  GLenum fmt = a.next32(), type = a.next32();
  u32 pixels = a.next32();
  // Render targets arrive here with a null pixel pointer - the engine is
  // asking for storage, not uploading an image - and their size decides how
  // much work every later frame costs. One that came out wrong would be
  // invisible in the draw calls and fatal in the timing.
  if (level == 0) note_tex_size(g_texture, w, h);
  if (pixels == 0) {
    static int shown = 0;
    if (++shown <= 20)
      std::printf("[tex ] texture %u storage %dx%d level %d internal 0x%04X "
              "fmt 0x%04X type 0x%04X\n", g_texture, w, h, level, internal,
                  fmt, type);
  }
  if (ready()) {
    const std::size_t need = (w > 0 && h > 0)
                                 ? static_cast<std::size_t>(w) * h *
                                       pixel_size(fmt, type)
                                 : 0;
    const void* p = staged(e, pixels, need, "glTexImage2D");
    g_gl.glTexImage2D(target, level, internal, w, h, border, fmt, type, p);
  }
  e.ret(0);
}

void t_glTexParameterf(Env& e) { A; GLenum t = a.next32(), n = a.next32(); GLfloat v = a.nextf(); if (ready()) g_gl.glTexParameterf(t, n, v); e.ret(0); }
void t_glTexParameteri(Env& e) { A; GLenum t = a.next32(), n = a.next32(); GLint v = a.next32(); if (ready()) g_gl.glTexParameteri(t, n, v); e.ret(0); }
void t_glUniform1f(Env& e) { A; GLint l = a.next32(); GLfloat v = a.nextf(); if (ready()) g_gl.glUniform1f(l, v); e.ret(0); }
void t_glUniform1i(Env& e) { A; GLint l = a.next32(); GLint v = a.next32(); if (ready()) g_gl.glUniform1i(l, v); e.ret(0); }

void t_glUniform1iv(Env& e) {
  A;
  GLint loc = a.next32();
  GLsizei count = a.next32();
  u32 src = a.next32();
  if (ready() && count > 0 && src) {
    std::vector<GLint> v(count);
    for (GLsizei i = 0; i < count; ++i) v[i] = static_cast<GLint>(e.mem().read32(src + i * 4));
    g_gl.glUniform1iv(loc, count, v.data());
  }
  e.ret(0);
}

void t_glUniform4fv(Env& e) {
  A;
  GLint loc = a.next32();
  GLsizei count = a.next32();
  u32 src = a.next32();
  if (ready() && count > 0 && src) {
    std::vector<GLfloat> v(static_cast<std::size_t>(count) * 4);
    for (std::size_t i = 0; i < v.size(); ++i) {
      u32 bits = e.mem().read32(src + static_cast<u32>(i) * 4);
      std::memcpy(&v[i], &bits, 4);
    }
    g_gl.glUniform4fv(loc, count, v.data());
  }
  e.ret(0);
}

void t_glUniformMatrix4fv(Env& e) {
  A;
  GLint loc = a.next32();
  GLsizei count = a.next32();
  GLboolean transpose = a.next32() ? 1 : 0;
  u32 src = a.next32();
  if (ready() && count > 0 && src) {
    std::vector<GLfloat> v(static_cast<std::size_t>(count) * 16);
    for (std::size_t i = 0; i < v.size(); ++i) {
      u32 bits = e.mem().read32(src + static_cast<u32>(i) * 4);
      std::memcpy(&v[i], &bits, 4);
    }
    // A transform that has gone wrong projects the whole mesh across the
    // screen many times over, and rasterising that is what the driver's
    // two-second patience runs out on. The values themselves are the only
    // way to tell a broken matrix from a broken mesh, and by here the mesh
    // has already been ruled out.
    {
      static int shown = 0;
      bool bad = false;
      for (float f : v)
        if (f != f || f > 1e6f || f < -1e6f) bad = true;
      if (shown < 2 || (bad && shown < 10)) {
        ++shown;
        std::printf("[mtx ] %s", bad ? "suspect " : "");
        for (int i = 0; i < 16 && i < static_cast<int>(v.size()); ++i)
          std::printf(" %g", static_cast<double>(v[i]));
        std::printf("\n");
      }
    }
    g_gl.glUniformMatrix4fv(loc, count, transpose, v.data());
  }
  e.ret(0);
}

void t_glUseProgram(Env& e) { A; GLuint p = a.next32(); g_program = p; if (ready()) g_gl.glUseProgram(p); e.ret(0); }

void t_glVertexAttribPointer(Env& e) {
  A;
  GLuint index = a.next32();
  GLint size = a.next32();
  GLenum type = a.next32();
  GLboolean norm = a.next32() ? 1 : 0;
  GLsizei stride = a.next32();
  u32 pointer = a.next32();
  if (ready()) {
    // Same pointer-or-offset rule as glDrawElements, keyed on the array
    // buffer binding this time.
    const void* p = g_array_buffer
                        ? reinterpret_cast<const void*>(
                              static_cast<std::uintptr_t>(pointer))
                        : (pointer ? e.mem().host<void>(pointer) : nullptr);
    g_gl.glVertexAttribPointer(index, size, type, norm, stride, p);
  }
  if (index < kMaxAttribs) {
    AttribState& at = t_attrib[index];
    at.buffer = g_array_buffer;
    at.size = size;
    at.type = type;
    at.stride = stride;
    at.offset = pointer;
  }
  e.ret(0);
}

void t_glViewport(Env& e) {
  A;
  GLint x = a.next32(), y = a.next32();
  GLsizei w = a.next32(), h = a.next32();
  g_viewport[0] = x; g_viewport[1] = y; g_viewport[2] = w; g_viewport[3] = h;
  {
    // A viewport far larger than the window is the cheapest explanation there
    // is for a frame the driver will not sit through, so every distinct one
    // gets named once.
    static std::mutex once_lock;
    static std::unordered_map<unsigned long long, bool> said;
    std::lock_guard<std::mutex> lock(once_lock);
    const unsigned long long key =
        (static_cast<unsigned long long>(static_cast<unsigned>(w)) << 32) |
        static_cast<unsigned>(h);
    if (!said[key] && said.size() <= 24) {
      said[key] = true;
      std::printf("[vp  ] viewport %d,%d %dx%d\n", x, y, w, h);
    }
  }
  if (ready()) g_gl.glViewport(x, y, w, h);
  e.ret(0);
}

#undef A

}  // namespace

const ThunkEntry kGlTable[] = {
    {"eglGetDisplay", &t_eglGetDisplay},
    {"eglInitialize", &t_eglInitialize},
    {"eglTerminate", &t_eglTerminate},
    {"eglGetConfigs", &t_eglGetConfigs},
    {"eglGetConfigAttrib", &t_eglGetConfigAttrib},
    {"eglCreateContext", &t_eglCreateContext},
    {"eglCreateWindowSurface", &t_eglCreateWindowSurface},
    {"eglMakeCurrent", &t_eglMakeCurrent},
    {"eglQuerySurface", &t_eglQuerySurface},
    {"eglSwapBuffers", &t_eglSwapBuffers},
    {"eglDestroySurface", &t_eglDestroySurface},
    {"eglDestroyContext", &t_eglDestroyContext},

    {"glActiveTexture", &t_glActiveTexture},
    {"glAttachShader", &t_glAttachShader},
    {"glBindBuffer", &t_glBindBuffer},
    {"glBindFramebuffer", &t_glBindFramebuffer},
    {"glBindRenderbuffer", &t_glBindRenderbuffer},
    {"glBindTexture", &t_glBindTexture},
    {"glBlendEquation", &t_glBlendEquation},
    {"glBlendFunc", &t_glBlendFunc},
    {"glBufferData", &t_glBufferData},
    {"glClear", &t_glClear},
    {"glClearColor", &t_glClearColor},
    {"glCompileShader", &t_glCompileShader},
    {"glCompressedTexImage2D", &t_glCompressedTexImage2D},
    {"glCreateProgram", &t_glCreateProgram},
    {"glCreateShader", &t_glCreateShader},
    {"glCullFace", &t_glCullFace},
    {"glDeleteBuffers", &t_glDeleteBuffers},
    {"glDeleteFramebuffers", &t_glDeleteFramebuffers},
    {"glDeleteProgram", &t_glDeleteProgram},
    {"glDeleteRenderbuffers", &t_glDeleteRenderbuffers},
    {"glDeleteShader", &t_glDeleteShader},
    {"glDeleteTextures", &t_glDeleteTextures},
    {"glDepthFunc", &t_glDepthFunc},
    {"glDepthMask", &t_glDepthMask},
    {"glDisable", &t_glDisable},
    {"glDrawArrays", &t_glDrawArrays},
    {"glDrawElements", &t_glDrawElements},
    {"glEnable", &t_glEnable},
    {"glEnableVertexAttribArray", &t_glEnableVertexAttribArray},
    {"glFramebufferRenderbuffer", &t_glFramebufferRenderbuffer},
    {"glFramebufferTexture2D", &t_glFramebufferTexture2D},
    {"glGenBuffers", &t_glGenBuffers},
    {"glGenFramebuffers", &t_glGenFramebuffers},
    {"glGenRenderbuffers", &t_glGenRenderbuffers},
    {"glGenTextures", &t_glGenTextures},
    {"glGenerateMipmap", &t_glGenerateMipmap},
    {"glGetAttribLocation", &t_glGetAttribLocation},
    {"glGetFloatv", &t_glGetFloatv},
    {"glGetIntegerv", &t_glGetIntegerv},
    {"glGetProgramInfoLog", &t_glGetProgramInfoLog},
    {"glGetProgramiv", &t_glGetProgramiv},
    {"glGetShaderInfoLog", &t_glGetShaderInfoLog},
    {"glGetShaderiv", &t_glGetShaderiv},
    {"glGetUniformLocation", &t_glGetUniformLocation},
    {"glLinkProgram", &t_glLinkProgram},
    {"glPixelStorei", &t_glPixelStorei},
    {"glRenderbufferStorage", &t_glRenderbufferStorage},
    {"glScissor", &t_glScissor},
    {"glShaderSource", &t_glShaderSource},
    {"glTexImage2D", &t_glTexImage2D},
    {"glTexParameterf", &t_glTexParameterf},
    {"glTexParameteri", &t_glTexParameteri},
    {"glUniform1f", &t_glUniform1f},
    {"glUniform1i", &t_glUniform1i},
    {"glUniform1iv", &t_glUniform1iv},
    {"glUniform4fv", &t_glUniform4fv},
    {"glUniformMatrix4fv", &t_glUniformMatrix4fv},
    {"glUseProgram", &t_glUseProgram},
    {"glVertexAttribPointer", &t_glVertexAttribPointer},
    {"glViewport", &t_glViewport},
};

const std::size_t kGlTableSize = sizeof(kGlTable) / sizeof(kGlTable[0]);

}  // namespace wb
