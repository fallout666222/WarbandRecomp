// A real GL context on the PC, through WGL and nothing else.
//
// No windowing library: opengl32.dll ships with Windows, and everything past
// GL 1.1 comes from wglGetProcAddress. That keeps the PC build dependency-free
// and, more importantly, keeps the GL thunk layer identical to the one the
// Switch build will use - only this file is replaced there, by switch-mesa's
// EGL and GLES2.
//
// The window is visible on purpose: the point of the exercise is to see a
// frame.

#if defined(_WIN32) && !defined(WB_SWITCH)

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "gl_api.h"
#include "input.h"

namespace wb {

GlApi g_gl;

namespace {

HWND g_window = nullptr;
HDC g_dc = nullptr;
int g_client_width = 0, g_client_height = 0;
HGLRC g_rc = nullptr;          // the primary context, made on the first thread
bool g_ready = false;
std::atomic<bool> g_closed{false};

// One context per host thread, all sharing the primary one's objects.
//
// The engine calls GL from several threads - the render thread draws while
// the environment thread creates and destroys meshes - and a context can only
// be current on one thread at a time. Passing one context back and forth
// deadlocks in practice: wglMakeCurrent fails with ERROR_BUSY while the other
// thread still holds it, and a GL call with no context current does not
// return an error, it faults inside the driver.
//
// Shared contexts remove the handover entirely. Texture and buffer names are
// common to all of them, which is exactly what the engine assumes. EGL does
// the same thing with the share_context argument, so the Switch build keeps
// this shape.
// wglShareLists refuses while either context is current, and by the time a
// second thread turns up the primary has been current on the first one for a
// long time. So the whole group is created and joined up front, before
// anything is made current, and threads take one each as they appear.
constexpr int kSpareContexts = 7;
thread_local HGLRC t_rc = nullptr;
HGLRC g_spare[kSpareContexts] = {};
int g_spare_next = 0;
std::mutex g_spare_lock;
std::atomic<int> g_contexts{0};

void* load(const char* name) {
  void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
  // wglGetProcAddress only knows about GL 1.2 and later; the rest live as
  // ordinary exports of opengl32.dll. It also has a handful of "error"
  // returns that are not null.
  if (p == nullptr || p == reinterpret_cast<void*>(1) ||
      p == reinterpret_cast<void*>(2) || p == reinterpret_cast<void*>(3) ||
      p == reinterpret_cast<void*>(-1)) {
    static HMODULE gl = LoadLibraryA("opengl32.dll");
    p = gl ? reinterpret_cast<void*>(GetProcAddress(gl, name)) : nullptr;
  }
  return p;
}

// Warband's Android build is driven by touch, so the mouse becomes a finger:
// a press is a pointer going down, a drag is it moving, a release lifts it.
// That is a truer fit than pretending to be a mouse, because the engine has
// no mouse code left - the desktop input path is not in this binary.
bool pointer_down(WPARAM w) { return (w & MK_LBUTTON) != 0; }

// What character this key produces right now, given the layout and the
// modifiers currently held. Text entry needs it: a keycode says which key was
// pressed, not which letter it stands for.
u32 character_for(WPARAM vk) {
  BYTE keyboard[256];
  if (!GetKeyboardState(keyboard)) return 0;
  const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
  wchar_t out[8] = {};
  const int n = ToUnicode(static_cast<UINT>(vk), scan, keyboard, out, 8, 0);
  if (n <= 0) return 0;
  return static_cast<u32>(out[0]);
}

// Android keycodes for the few keys worth having during bring-up.
int android_keycode(WPARAM vk) {
  switch (vk) {
    case VK_ESCAPE: return 4;      // KEYCODE_BACK, which closes menus
    case VK_RETURN: return 66;     // KEYCODE_ENTER
    case VK_SPACE:  return 62;
    case VK_UP:     return 19;
    case VK_DOWN:   return 20;
    case VK_LEFT:   return 21;
    case VK_RIGHT:  return 22;
    case VK_TAB:    return 61;
    default:
      if (vk >= 'A' && vk <= 'Z') return 29 + static_cast<int>(vk - 'A');
      if (vk >= '0' && vk <= '9') return 7 + static_cast<int>(vk - '0');
      return 0;
  }
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  InputEvent ev;
  switch (msg) {
    case WM_CLOSE:
      // The guest cannot be told: it believes it is an Android activity and
      // has no quit path that does not go through the Java side. So the
      // close button is recorded here and the run loop shuts everything down
      // the same way the deadline does.
      g_closed.store(true);
      std::printf("[gl  ] window closed - shutting down\n");
      std::fflush(stdout);
      return 0;

    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP: {
      if (msg == WM_MOUSEMOVE && !pointer_down(w)) break;
      ev.kind = InputEvent::Kind::Motion;
      ev.source = Source::Touchscreen;
      ev.action = msg == WM_LBUTTONDOWN ? 0 : msg == WM_LBUTTONUP ? 1 : 2;
      ev.x = static_cast<float>(static_cast<short>(LOWORD(l)));
      ev.y = static_cast<float>(static_cast<short>(HIWORD(l)));
      if (msg == WM_LBUTTONDOWN) SetCapture(h);
      if (msg == WM_LBUTTONUP) ReleaseCapture();
      input_push(ev);
      return 0;
    }

    case WM_KEYDOWN:
    case WM_KEYUP: {
      const int code = android_keycode(w);
      if (!code) break;
      ev.kind = InputEvent::Kind::Key;
      ev.source = Source::Keyboard;
      ev.action = msg == WM_KEYDOWN ? 0 : 1;
      ev.key_code = code;
      // Windows decides which character a key produces from the layout and
      // the modifier state, and tells us in a separate WM_CHAR. Asking it
      // here rather than waiting for that message keeps the character on the
      // same event as the keycode, which is how Android delivers it.
      ev.unicode = character_for(w);
      input_push(ev);
      return 0;
    }

    default:
      break;
  }
  return DefWindowProcA(h, msg, w, l);
}

}  // namespace

bool gl_create_context(int width, int height) {
  if (g_ready) return true;

  WNDCLASSA wc = {};
  wc.style = CS_OWNDC;
  wc.lpfnWndProc = proc;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "warband_nx";
  RegisterClassA(&wc);

  RECT r = {0, 0, width, height};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  g_window = CreateWindowExA(0, "warband_nx", "Mount & Blade: Warband",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                             CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
  if (!g_window) {
    std::printf("[gl  ] CreateWindow failed\n");
    return false;
  }

  g_client_width = width;
  g_client_height = height;
  g_dc = GetDC(g_window);
  PIXELFORMATDESCRIPTOR pfd = {};
  pfd.nSize = sizeof(pfd);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  pfd.cDepthBits = 24;
  pfd.cStencilBits = 8;
  int fmt = ChoosePixelFormat(g_dc, &pfd);
  if (!fmt || !SetPixelFormat(g_dc, fmt, &pfd)) {
    std::printf("[gl  ] no suitable pixel format\n");
    return false;
  }

  g_rc = wglCreateContext(g_dc);
  if (!g_rc) {
    std::printf("[gl  ] wglCreateContext failed\n");
    return false;
  }
  // Join the group now, while none of them is current on any thread.
  int shared = 0;
  for (int i = 0; i < kSpareContexts; ++i) {
    HGLRC rc = wglCreateContext(g_dc);
    if (!rc) break;
    if (!wglShareLists(g_rc, rc)) {
      std::printf("[gl  ] wglShareLists failed (error %lu)\n", GetLastError());
      wglDeleteContext(rc);
      break;
    }
    g_spare[shared++] = rc;
  }
  std::printf("[gl  ] %d contexts, sharing one set of objects\n", shared + 1);

  if (!wglMakeCurrent(g_dc, g_rc)) {
    std::printf("[gl  ] wglMakeCurrent failed on the first thread (error %lu)\n",
                GetLastError());
    return false;
  }
  t_rc = g_rc;
  g_contexts.store(1);

#define WB_LOAD(name, ret, args)                                    \
  g_gl.name = reinterpret_cast<ret(*) args>(load(#name));           \
  if (!g_gl.name) missing += #name " ";
  std::string missing;
  WB_GL_FUNCTIONS(WB_LOAD)
#undef WB_LOAD
  g_gl.glGetError = reinterpret_cast<GLenum (*)()>(load("glGetError"));
  g_gl.glGetString =
      reinterpret_cast<const unsigned char* (*)(GLenum)>(load("glGetString"));
  g_gl.glFlush = reinterpret_cast<void (*)()>(load("glFlush"));
  g_gl.glFinish = reinterpret_cast<void (*)()>(load("glFinish"));
  g_gl.glReadPixels = reinterpret_cast<void (*)(GLint, GLint, GLsizei, GLsizei,
                                                GLenum, GLenum, void*)>(
      load("glReadPixels"));
  g_gl.glDisableVertexAttribArray =
      reinterpret_cast<void (*)(GLuint)>(load("glDisableVertexAttribArray"));
  g_gl.glVertexAttrib4f =
      reinterpret_cast<void (*)(GLuint, float, float, float, float)>(
          load("glVertexAttrib4f"));

  const char* vendor = g_gl.glGetString
                           ? reinterpret_cast<const char*>(g_gl.glGetString(0x1F00))
                           : "?";
  const char* version = g_gl.glGetString
                            ? reinterpret_cast<const char*>(g_gl.glGetString(0x1F02))
                            : "?";
  std::printf("[gl  ] %s, GL %s\n", vendor ? vendor : "?",
              version ? version : "?");
  if (!missing.empty())
    std::printf("[gl  ] entry points not found: %s\n", missing.c_str());

  g_ready = true;
  return true;
}

bool gl_make_current(bool bind) {
  if (!g_ready) return false;
  if (!bind) {
    wglMakeCurrent(nullptr, nullptr);
    return true;
  }
  if (t_rc && wglGetCurrentContext() == t_rc) return true;

  if (!t_rc) {
    // First GL call on this thread: take one from the group made at startup.
    std::lock_guard<std::mutex> lock(g_spare_lock);
    if (g_spare_next >= kSpareContexts || !g_spare[g_spare_next]) {
      static bool said = false;
      if (!said) {
        said = true;
        std::printf("[gl  ] more GL threads than contexts; calls from this one "
                    "will be dropped\n");
      }
      return false;
    }
    t_rc = g_spare[g_spare_next++];
    std::printf("[gl  ] context %d taken by a new thread\n",
                g_contexts.fetch_add(1) + 1);
  }
  if (wglMakeCurrent(g_dc, t_rc)) return true;
  std::printf("[gl  ] wglMakeCurrent failed on this thread (error %lu)\n",
              GetLastError());
  return false;
}

bool gl_context_current() { return wglGetCurrentContext() != nullptr; }

void gl_surface_size(int* w, int* h) {
  if (w) *w = g_client_width;
  if (h) *h = g_client_height;
}

// Objects one context creates only become visible to the others once it has
// flushed, so this runs when the context is handed on.
void gl_flush_for_share() {
  if (g_ready && g_contexts.load() > 1 && g_gl.glFlush) g_gl.glFlush();
}

void gl_pump() {
  if (!g_ready) return;
  MSG msg;
  while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
}

void gl_present() {
  if (!g_ready) return;
  SwapBuffers(g_dc);
  gl_pump();
}

void gl_destroy_context() {
  if (!g_ready) return;
  wglMakeCurrent(nullptr, nullptr);
  wglDeleteContext(g_rc);
  ReleaseDC(g_window, g_dc);
  DestroyWindow(g_window);
  g_ready = false;
}

bool gl_have_context() { return g_ready; }

bool gl_close_requested() { return g_closed.load(); }

// Desktop GL rejects GLSL ES source: it does not know `precision` qualifiers
// and defaults to GLSL 1.10, where `in`/`out` do not exist either. The engine
// writes plain GLSL ES 1.00, so prepending a version and neutralising the
// precision keywords is enough. The Switch build, talking to a real GLES2
// driver, does none of this.
bool gl_needs_glsl_translation() { return true; }

std::string gl_translate_shader(const std::string& source) {
  std::string out =
      "#version 120\n"
      "#define lowp\n"
      "#define mediump\n"
      "#define highp\n";
  // A bare `precision mediump float;` statement is a syntax error rather than
  // a macro use, so those lines are dropped outright.
  std::size_t i = 0;
  while (i < source.size()) {
    std::size_t eol = source.find('\n', i);
    if (eol == std::string::npos) eol = source.size();
    std::string line = source.substr(i, eol - i);
    std::size_t first = line.find_first_not_of(" \t\r");
    bool drop = first != std::string::npos &&
                line.compare(first, 9, "precision") == 0;
    if (!drop) {
      out += line;
      out.push_back('\n');
    }
    i = eol + 1;
  }
  return out;
}

}  // namespace wb

#endif  // _WIN32 && !WB_SWITCH
