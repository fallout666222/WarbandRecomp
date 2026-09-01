// A real GLES2 context on the Switch, through libnx and EGL.
//
// This is the counterpart of gl_win32.cpp and the only file that differs
// between the two targets. Everything above it - the sixty GL thunks, the
// EGL emulation the engine talks to, the frame grab - is shared, which is the
// whole point of routing GL through a table.
//
// Two things are easier here than on the PC:
//
//   * the engine's shaders are GLSL ES and this is a GLES2 driver, so the
//     translation the desktop build needs is not wanted at all;
//   * the entry points are ordinary symbols rather than something fished out
//     of wglGetProcAddress, so the table is filled by taking their addresses.
//
// And one is harder: there is exactly one drawable, and EGL will not let two
// contexts share it. That suits the engine, which binds the context on its
// render thread and keeps it there - see the ownership rules in thunks_gl.cpp
// - so a context per thread is neither needed nor offered.

#if defined(WB_SWITCH)

#include <switch.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "gl_api.h"

namespace wb {

GlApi g_gl;

namespace {

EGLDisplay g_display = EGL_NO_DISPLAY;
EGLContext g_context = EGL_NO_CONTEXT;
EGLSurface g_surface = EGL_NO_SURFACE;
NWindow* g_window = nullptr;
int g_width = 0, g_height = 0;
bool g_ready = false;

void fill_table() {
  // Signatures are spelled the same way on both sides, but GLES2 types come
  // from the platform headers and ours from gl_api.h, so each pointer is cast
  // through its own declared type rather than assigned directly.
#define WB_BIND(name, ret, args) \
  g_gl.name = reinterpret_cast<ret(*) args>(&::name);
  WB_GL_FUNCTIONS(WB_BIND)
#undef WB_BIND
  g_gl.glGetError = reinterpret_cast<GLenum (*)()>(&::glGetError);
  g_gl.glGetString =
      reinterpret_cast<const unsigned char* (*)(GLenum)>(&::glGetString);
  g_gl.glFlush = reinterpret_cast<void (*)()>(&::glFlush);
  g_gl.glFinish = reinterpret_cast<void (*)()>(&::glFinish);
  g_gl.glReadPixels = reinterpret_cast<void (*)(GLint, GLint, GLsizei, GLsizei,
                                                GLenum, GLenum, void*)>(
      &::glReadPixels);
  g_gl.glDisableVertexAttribArray =
      reinterpret_cast<void (*)(GLuint)>(&::glDisableVertexAttribArray);
  g_gl.glVertexAttrib4f =
      reinterpret_cast<void (*)(GLuint, float, float, float, float)>(
          &::glVertexAttrib4f);
}

}  // namespace

bool gl_create_context(int width, int height) {
  if (g_ready) return true;

  g_window = nwindowGetDefault();
  if (!g_window) {
    std::printf("[gl  ] no default window\n");
    return false;
  }
  // Docked output is 1920x1080 and handheld is 1280x720. Asking for one size
  // and letting the compositor scale keeps the engine's idea of the screen
  // fixed, which matters because it was told that size through JNI before the
  // surface existed.
  nwindowSetDimensions(g_window, static_cast<u32>(width),
                       static_cast<u32>(height));
  g_width = width;
  g_height = height;

  g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (g_display == EGL_NO_DISPLAY) {
    std::printf("[gl  ] eglGetDisplay failed (0x%X)\n", eglGetError());
    return false;
  }
  if (!eglInitialize(g_display, nullptr, nullptr)) {
    std::printf("[gl  ] eglInitialize failed (0x%X)\n", eglGetError());
    return false;
  }
  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    std::printf("[gl  ] eglBindAPI failed (0x%X)\n", eglGetError());
    return false;
  }

  // The same format the engine asks our emulated EGL for, so what it believes
  // about the framebuffer is true.
  static const EGLint config_attrs[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
      EGL_RED_SIZE,        8,
      EGL_GREEN_SIZE,      8,
      EGL_BLUE_SIZE,       8,
      EGL_ALPHA_SIZE,      8,
      EGL_DEPTH_SIZE,      24,
      EGL_STENCIL_SIZE,    8,
      EGL_NONE};
  EGLConfig config;
  EGLint config_count = 0;
  if (!eglChooseConfig(g_display, config_attrs, &config, 1, &config_count) ||
      config_count == 0) {
    std::printf("[gl  ] no matching EGL config\n");
    eglTerminate(g_display);
    g_display = EGL_NO_DISPLAY;
    return false;
  }

  g_surface = eglCreateWindowSurface(g_display, config, g_window, nullptr);
  if (g_surface == EGL_NO_SURFACE) {
    std::printf("[gl  ] eglCreateWindowSurface failed (0x%X)\n", eglGetError());
    eglTerminate(g_display);
    g_display = EGL_NO_DISPLAY;
    return false;
  }

  static const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                         EGL_NONE};
  g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT,
                               context_attrs);
  if (g_context == EGL_NO_CONTEXT) {
    std::printf("[gl  ] eglCreateContext failed (0x%X)\n", eglGetError());
    eglDestroySurface(g_display, g_surface);
    eglTerminate(g_display);
    g_display = EGL_NO_DISPLAY;
    return false;
  }
  eglMakeCurrent(g_display, g_surface, g_surface, g_context);

  fill_table();
  std::printf("[gl  ] %s, %s\n",
              reinterpret_cast<const char*>(::glGetString(GL_VENDOR)),
              reinterpret_cast<const char*>(::glGetString(GL_VERSION)));

  g_ready = true;
  return true;
}

// EGL, unlike WGL, has no way for one thread to take a context another thread
// still holds - so this only ever binds on the thread that owns GL, which is
// the rule thunks_gl.cpp enforces anyway.
bool gl_make_current(bool bind) {
  if (!g_ready) return false;
  if (!bind)
    return eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                          EGL_NO_CONTEXT) == EGL_TRUE;
  if (eglGetCurrentContext() == g_context) return true;
  if (eglMakeCurrent(g_display, g_surface, g_surface, g_context) == EGL_TRUE)
    return true;
  std::printf("[gl  ] eglMakeCurrent failed on this thread (0x%X)\n",
              eglGetError());
  return false;
}

bool gl_context_current() {
  return g_ready && eglGetCurrentContext() != EGL_NO_CONTEXT;
}

// One context, so nothing to publish between them.
void gl_flush_for_share() {}

void gl_surface_size(int* w, int* h) {
  if (w) *w = g_width;
  if (h) *h = g_height;
}

void gl_present() {
  if (!g_ready) return;
  eglSwapBuffers(g_display, g_surface);
}

// There is no window manager to answer to.
void gl_pump() {}

void gl_destroy_context() {
  if (!g_ready) return;
  eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(g_display, g_context);
  eglDestroySurface(g_display, g_surface);
  eglTerminate(g_display);
  g_display = EGL_NO_DISPLAY;
  g_ready = false;
}

// Horizon has no window and no close button; leaving is the Home menu's
// business, and libnx handles that above us.
bool gl_close_requested() { return false; }

bool gl_have_context() { return g_ready; }

// The engine writes GLSL ES 1.00 and this is a GLSL ES 1.00 driver, so the
// source goes through untouched. The desktop build is the odd one out.
bool gl_needs_glsl_translation() { return false; }

std::string gl_translate_shader(const std::string& source) { return source; }

}  // namespace wb

#endif  // WB_SWITCH
