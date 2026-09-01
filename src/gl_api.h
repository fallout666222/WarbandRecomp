// The GL entry points the engine imports, as one table.
//
// The 60 GLES2 functions in the inventory are listed once, here, and that
// list generates the pointer types, the table members and the loader. The
// thunks call through the table, so the only platform-specific part is who
// fills it: WGL on the PC, switch-mesa's GLES2 on the console. Nothing in
// thunks_gl.cpp changes between the two.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace wb {

// GL types, spelled out so no platform GL header is needed here.
using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLclampf = float;
using GLchar = char;
using GLintptr = std::intptr_t;
using GLsizeiptr = std::ptrdiff_t;

// name, return type, parameter list
#define WB_GL_FUNCTIONS(X)                                                     \
  X(glActiveTexture, void, (GLenum))                                           \
  X(glAttachShader, void, (GLuint, GLuint))                                    \
  X(glBindBuffer, void, (GLenum, GLuint))                                      \
  X(glBindFramebuffer, void, (GLenum, GLuint))                                 \
  X(glBindRenderbuffer, void, (GLenum, GLuint))                                \
  X(glBindTexture, void, (GLenum, GLuint))                                     \
  X(glBlendEquation, void, (GLenum))                                           \
  X(glBlendFunc, void, (GLenum, GLenum))                                       \
  X(glBufferData, void, (GLenum, GLsizeiptr, const void*, GLenum))             \
  X(glClear, void, (GLbitfield))                                               \
  X(glClearColor, void, (GLclampf, GLclampf, GLclampf, GLclampf))              \
  X(glCompileShader, void, (GLuint))                                           \
  X(glCompressedTexImage2D, void,                                              \
    (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*))    \
  X(glCreateProgram, GLuint, ())                                               \
  X(glCreateShader, GLuint, (GLenum))                                          \
  X(glCullFace, void, (GLenum))                                                \
  X(glDeleteBuffers, void, (GLsizei, const GLuint*))                           \
  X(glDeleteFramebuffers, void, (GLsizei, const GLuint*))                      \
  X(glDeleteProgram, void, (GLuint))                                           \
  X(glDeleteRenderbuffers, void, (GLsizei, const GLuint*))                     \
  X(glDeleteShader, void, (GLuint))                                            \
  X(glDeleteTextures, void, (GLsizei, const GLuint*))                          \
  X(glDepthFunc, void, (GLenum))                                               \
  X(glDepthMask, void, (GLboolean))                                            \
  X(glDisable, void, (GLenum))                                                 \
  X(glDrawArrays, void, (GLenum, GLint, GLsizei))                              \
  X(glDrawElements, void, (GLenum, GLsizei, GLenum, const void*))              \
  X(glEnable, void, (GLenum))                                                  \
  X(glEnableVertexAttribArray, void, (GLuint))                                 \
  X(glFramebufferRenderbuffer, void, (GLenum, GLenum, GLenum, GLuint))         \
  X(glFramebufferTexture2D, void, (GLenum, GLenum, GLenum, GLuint, GLint))     \
  X(glGenBuffers, void, (GLsizei, GLuint*))                                    \
  X(glGenFramebuffers, void, (GLsizei, GLuint*))                               \
  X(glGenRenderbuffers, void, (GLsizei, GLuint*))                              \
  X(glGenTextures, void, (GLsizei, GLuint*))                                   \
  X(glGenerateMipmap, void, (GLenum))                                          \
  X(glGetAttribLocation, GLint, (GLuint, const GLchar*))                       \
  X(glGetFloatv, void, (GLenum, GLfloat*))                                     \
  X(glGetIntegerv, void, (GLenum, GLint*))                                     \
  X(glGetProgramInfoLog, void, (GLuint, GLsizei, GLsizei*, GLchar*))           \
  X(glGetProgramiv, void, (GLuint, GLenum, GLint*))                            \
  X(glGetShaderInfoLog, void, (GLuint, GLsizei, GLsizei*, GLchar*))            \
  X(glGetShaderiv, void, (GLuint, GLenum, GLint*))                             \
  X(glGetUniformLocation, GLint, (GLuint, const GLchar*))                      \
  X(glLinkProgram, void, (GLuint))                                             \
  X(glPixelStorei, void, (GLenum, GLint))                                      \
  X(glRenderbufferStorage, void, (GLenum, GLenum, GLsizei, GLsizei))           \
  X(glScissor, void, (GLint, GLint, GLsizei, GLsizei))                         \
  X(glShaderSource, void, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
  X(glTexImage2D, void,                                                        \
    (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,            \
     const void*))                                                             \
  X(glTexParameterf, void, (GLenum, GLenum, GLfloat))                          \
  X(glTexParameteri, void, (GLenum, GLenum, GLint))                            \
  X(glUniform1f, void, (GLint, GLfloat))                                       \
  X(glUniform1i, void, (GLint, GLint))                                         \
  X(glUniform1iv, void, (GLint, GLsizei, const GLint*))                        \
  X(glUniform4fv, void, (GLint, GLsizei, const GLfloat*))                      \
  X(glUniformMatrix4fv, void, (GLint, GLsizei, GLboolean, const GLfloat*))     \
  X(glUseProgram, void, (GLuint))                                              \
  X(glVertexAttribPointer, void,                                               \
    (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))                  \
  X(glViewport, void, (GLint, GLint, GLsizei, GLsizei))

struct GlApi {
#define WB_DECL(name, ret, args) ret(*name) args = nullptr;
  WB_GL_FUNCTIONS(WB_DECL)
#undef WB_DECL

  // Not imported by the engine, but the thunk layer needs them: shader source
  // has to be inspected after translation, and errors are worth reporting.
  GLenum (*glGetError)() = nullptr;
  const unsigned char* (*glGetString)(GLenum) = nullptr;
  // Shared contexts only see each other's objects after a flush.
  void (*glFlush)() = nullptr;
  // For --draw-sync: the only way to attribute a graphics-driver timeout to
  // the draw that caused it. Submission is asynchronous, so without waiting
  // for each one the process simply dies somewhere after the fact.
  void (*glFinish)() = nullptr;
  // For the frame grab. The engine never reads its own framebuffer back.
  void (*glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                       void*) = nullptr;
  // The frame counter enables a vertex array and has to put it back; the
  // engine only ever enables them, so this one is not in its import list.
  // It is also how a stale array that would read past the end of its buffer
  // gets turned off before it takes the graphics driver down - see
  // guard_draw. The constant that replaces it comes from glVertexAttrib4f,
  // which the engine does not import either.
  void (*glDisableVertexAttribArray)(GLuint) = nullptr;
  void (*glVertexAttrib4f)(GLuint, float, float, float, float) = nullptr;
};

extern GlApi g_gl;

// Brings up a real context of the given size and fills g_gl. Returns false if
// the platform could not provide one; the caller then keeps the stubs.
bool gl_create_context(int width, int height);
// Makes GL usable on the calling thread. A context belongs to one thread at
// a time, and the engine calls GL from several, so this gives each thread its
// own context sharing the primary one's objects rather than passing a single
// context around - which cannot work, since only the thread holding it can
// give it up.
bool gl_make_current(bool bind);
// Whether the calling thread has a context at all. A GL call without one does
// not fail, it faults inside the driver, so this is worth asserting.
bool gl_context_current();
// Publishes this thread's newly created objects to the other contexts.
void gl_flush_for_share();

// The drawable's size, which the frame grab needs and only the platform
// knows.
void gl_surface_size(int* w, int* h);

// Reads the back buffer and writes it out as a BMP. Grabbing the window from
// outside does not work - the OS paints a placeholder over a window whose
// thread is busy in the guest, and on the Switch there is no desktop to grab
// from at all - so the frame has to come from GL itself. Platform-independent:
// glReadPixels and a header.
bool gl_capture(const char* path);

// Asks for one frame to be written the next time the guest presents. Called
// from the watchdog thread, acted on by whichever thread swaps.
void gl_request_shot(const char* path);

// How many frames the guest has presented. The watchdog uses it to notice
// that the game has stopped drawing, which is the only outward sign of a
// hang - nothing faults, so nothing else reports one.
unsigned long long gl_frame_count();
void gl_present();          // swap buffers
// Services the window's message queue. The guest only swaps when it draws,
// so without this the window stops responding between frames and the desktop
// paints it over.
void gl_pump();
// True once the person running it has closed the window. The guest has no
// idea the window exists - it thinks it is an Android activity - so the close
// button cannot be handed to it, and without this the only way out of a run
// is the deadline or the task manager.
bool gl_close_requested();
void gl_destroy_context();
bool gl_have_context();

// Desktop GL will not accept GLSL ES source verbatim. The engine keeps its
// shaders as text in GLShaders/*.glsl, so they arrive here at glShaderSource
// and get patched on the way through.
bool gl_needs_glsl_translation();
std::string gl_translate_shader(const std::string& source);

}  // namespace wb
