// The frame-rate counter, drawn on top of the game.
//
// Printing it to the log is not the same thing: what you want to know while
// playing is whether a particular scene is smooth, and that means seeing the
// number over the scene. So this draws it - through the same GL table the
// engine uses, which is why it works identically on the desktop and on the
// console.
//
// Two constraints shape it. The digits come from a 3x5 bitmap font compiled
// in, because there is no font to load and no text renderer to load it with.
// And every piece of GL state it touches is put back afterwards: the engine
// leaves the pipeline configured the way its next draw call expects, and a
// counter that quietly unbound its vertex buffer would corrupt the frame
// after it rather than its own.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gl_api.h"
#include "overlay.h"

namespace wb {
namespace {

// GL constants, spelled out so no platform header is needed.
constexpr GLenum kArrayBuffer = 0x8892;
constexpr GLenum kStreamDraw = 0x88E0;
constexpr GLenum kFragmentShader = 0x8B30;
constexpr GLenum kVertexShader = 0x8B31;
constexpr GLenum kFloat = 0x1406;
constexpr GLenum kTriangles = 0x0004;
constexpr GLenum kBlend = 0x0BE2;
constexpr GLenum kDepthTest = 0x0B71;
constexpr GLenum kCullFace = 0x0B44;
constexpr GLenum kScissorTest = 0x0C11;
constexpr GLenum kSrcAlpha = 0x0302;
constexpr GLenum kOneMinusSrcAlpha = 0x0303;
constexpr GLenum kCompileStatus = 0x8B81;
constexpr GLenum kLinkStatus = 0x8B82;
constexpr GLenum kCurrentProgram = 0x8B8D;
constexpr GLenum kArrayBufferBinding = 0x8894;
constexpr GLenum kViewport = 0x0BA2;

// A 3x5 font, one bit per pixel, top row first. Digits, a colon and a few
// letters - enough for "60 fps" and a label.
struct Glyph {
  char c;
  unsigned char rows[5];   // low three bits used, leftmost pixel is bit 2
};

constexpr Glyph kFont[] = {
    {'0', {0b111, 0b101, 0b101, 0b101, 0b111}},
    {'1', {0b010, 0b110, 0b010, 0b010, 0b111}},
    {'2', {0b111, 0b001, 0b111, 0b100, 0b111}},
    {'3', {0b111, 0b001, 0b011, 0b001, 0b111}},
    {'4', {0b101, 0b101, 0b111, 0b001, 0b001}},
    {'5', {0b111, 0b100, 0b111, 0b001, 0b111}},
    {'6', {0b111, 0b100, 0b111, 0b101, 0b111}},
    {'7', {0b111, 0b001, 0b010, 0b010, 0b010}},
    {'8', {0b111, 0b101, 0b111, 0b101, 0b111}},
    {'9', {0b111, 0b101, 0b111, 0b001, 0b111}},
    {'.', {0b000, 0b000, 0b000, 0b000, 0b010}},
    {'f', {0b011, 0b100, 0b110, 0b100, 0b100}},
    {'p', {0b110, 0b101, 0b110, 0b100, 0b100}},
    {'s', {0b011, 0b100, 0b010, 0b001, 0b110}},
    {' ', {0b000, 0b000, 0b000, 0b000, 0b000}},
};

const Glyph* glyph_for(char c) {
  for (const Glyph& g : kFont)
    if (g.c == c) return &g;
  return nullptr;
}

const char* kVertexSource =
    "attribute vec2 pos;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";

// Drawn twice: once offset and dark, once bright, so the digits stay legible
// over whatever the game put behind them.
const char* kFragmentSource =
    "uniform vec4 tint;\n"
    "void main() { gl_FragColor = tint; }\n";

GLuint g_program = 0;
GLuint g_buffer = 0;
GLint g_pos_attrib = -1;
GLint g_tint_uniform = -1;
bool g_failed = false;

// Frame timing, in the units the platform gives us.
double g_last_report = 0;
int g_frames = 0;
float g_fps = 0;

GLuint compile(GLenum type, const char* source) {
  const GLuint shader = g_gl.glCreateShader(type);
  const GLint length = static_cast<GLint>(std::strlen(source));
  g_gl.glShaderSource(shader, 1, &source, &length);
  g_gl.glCompileShader(shader);
  GLint ok = 0;
  g_gl.glGetShaderiv(shader, kCompileStatus, &ok);
  if (!ok) {
    char log[512] = {};
    g_gl.glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
    std::printf("[hud ] shader failed: %s\n", log);
    return 0;
  }
  return shader;
}

bool build() {
  if (g_program) return true;
  if (g_failed) return false;
  if (!g_gl.glCreateProgram || !g_gl.glGenBuffers) {
    g_failed = true;
    return false;
  }

  const GLuint vs = compile(kVertexShader, kVertexSource);
  const GLuint fs = compile(kFragmentShader, kFragmentSource);
  if (!vs || !fs) {
    g_failed = true;
    return false;
  }
  g_program = g_gl.glCreateProgram();
  g_gl.glAttachShader(g_program, vs);
  g_gl.glAttachShader(g_program, fs);
  g_gl.glLinkProgram(g_program);
  GLint ok = 0;
  g_gl.glGetProgramiv(g_program, kLinkStatus, &ok);
  if (!ok) {
    char log[512] = {};
    g_gl.glGetProgramInfoLog(g_program, sizeof(log) - 1, nullptr, log);
    std::printf("[hud ] link failed: %s\n", log);
    g_program = 0;
    g_failed = true;
    return false;
  }
  g_gl.glDeleteShader(vs);
  g_gl.glDeleteShader(fs);

  g_pos_attrib = g_gl.glGetAttribLocation(g_program, "pos");
  g_tint_uniform = g_gl.glGetUniformLocation(g_program, "tint");
  g_gl.glGenBuffers(1, &g_buffer);
  std::printf("[hud ] frame counter ready\n");
  return true;
}

// Appends two triangles covering one pixel of the font, in clip space.
void add_quad(std::vector<GLfloat>& out, float x, float y, float w, float h) {
  const float x0 = x, x1 = x + w, y0 = y, y1 = y - h;
  const float quad[12] = {x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1};
  out.insert(out.end(), quad, quad + 12);
}

void build_text(std::vector<GLfloat>& out, const std::string& text, float left,
                float top, float pixel_w, float pixel_h) {
  float pen = left;
  for (char c : text) {
    const Glyph* g = glyph_for(c);
    if (g) {
      for (int row = 0; row < 5; ++row)
        for (int col = 0; col < 3; ++col)
          if (g->rows[row] & (1 << (2 - col)))
            add_quad(out, pen + col * pixel_w, top - row * pixel_h, pixel_w,
                     pixel_h);
    }
    pen += 4 * pixel_w;
  }
}

}  // namespace

void overlay_frame(double now_seconds) {
  if (g_last_report == 0) g_last_report = now_seconds;
  ++g_frames;
  const double elapsed = now_seconds - g_last_report;
  if (elapsed >= 0.5) {
    g_fps = static_cast<float>(g_frames / elapsed);
    g_frames = 0;
    g_last_report = now_seconds;
  }
}

float overlay_fps() { return g_fps; }

bool g_visible = true;

void overlay_set_visible(bool on) { g_visible = on; }

void overlay_draw() {
  if (!g_visible || !build()) return;

  char text[32];
  std::snprintf(text, sizeof(text), "%.0f fps", static_cast<double>(g_fps));

  int width = 1280, height = 720;
  gl_surface_size(&width, &height);
  // Six screen pixels per font pixel, in the top-left corner, in clip space.
  const float px = 6.0f * 2.0f / static_cast<float>(width);
  const float py = 6.0f * 2.0f / static_cast<float>(height);
  const float left = -1.0f + 8.0f * 2.0f / static_cast<float>(width);
  const float top = 1.0f - 8.0f * 2.0f / static_cast<float>(height);

  std::vector<GLfloat> verts;
  verts.reserve(text[0] ? 512 : 0);
  build_text(verts, text, left, top, px, py);
  if (verts.empty()) return;

  // Everything below is restored before returning: the engine's next draw
  // call assumes the state it left behind.
  GLint old_program = 0, old_buffer = 0;
  g_gl.glGetIntegerv(kCurrentProgram, &old_program);
  g_gl.glGetIntegerv(kArrayBufferBinding, &old_buffer);
  GLint old_viewport[4] = {};
  g_gl.glGetIntegerv(kViewport, old_viewport);

  g_gl.glDisable(kDepthTest);
  g_gl.glDisable(kCullFace);
  g_gl.glDisable(kScissorTest);
  g_gl.glEnable(kBlend);
  g_gl.glBlendFunc(kSrcAlpha, kOneMinusSrcAlpha);
  g_gl.glViewport(0, 0, width, height);

  g_gl.glUseProgram(g_program);
  g_gl.glBindBuffer(kArrayBuffer, g_buffer);
  g_gl.glBufferData(kArrayBuffer,
                    static_cast<GLsizeiptr>(verts.size() * sizeof(GLfloat)),
                    verts.data(), kStreamDraw);
  g_gl.glEnableVertexAttribArray(static_cast<GLuint>(g_pos_attrib));
  g_gl.glVertexAttribPointer(static_cast<GLuint>(g_pos_attrib), 2, kFloat, 0,
                             0, nullptr);

  const GLsizei count = static_cast<GLsizei>(verts.size() / 2);
  // A dark pass one pixel down and right, then the bright one on top.
  const GLfloat shadow[4] = {0.0f, 0.0f, 0.0f, 0.7f};
  const GLfloat bright[4] = {1.0f, 0.95f, 0.4f, 1.0f};

  std::vector<GLfloat> shifted(verts);
  for (std::size_t i = 0; i < shifted.size(); i += 2) {
    shifted[i] += px * 0.5f;
    shifted[i + 1] -= py * 0.5f;
  }
  g_gl.glBufferData(kArrayBuffer,
                    static_cast<GLsizeiptr>(shifted.size() * sizeof(GLfloat)),
                    shifted.data(), kStreamDraw);
  g_gl.glUniform4fv(g_tint_uniform, 1, shadow);
  g_gl.glDrawArrays(kTriangles, 0, count);

  g_gl.glBufferData(kArrayBuffer,
                    static_cast<GLsizeiptr>(verts.size() * sizeof(GLfloat)),
                    verts.data(), kStreamDraw);
  g_gl.glUniform4fv(g_tint_uniform, 1, bright);
  g_gl.glDrawArrays(kTriangles, 0, count);

  g_gl.glDisableVertexAttribArray(static_cast<GLuint>(g_pos_attrib));
  g_gl.glBindBuffer(kArrayBuffer, static_cast<GLuint>(old_buffer));
  g_gl.glUseProgram(static_cast<GLuint>(old_program));
  g_gl.glDisable(kBlend);
  g_gl.glViewport(old_viewport[0], old_viewport[1], old_viewport[2],
                  old_viewport[3]);
}

}  // namespace wb
