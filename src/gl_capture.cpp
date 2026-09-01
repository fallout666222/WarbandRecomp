// Writing a frame out, on either target.
//
// Grabbing the window from outside is not an option: the desktop paints a
// placeholder over a window whose thread is busy inside the guest, and on the
// Switch there is no desktop at all. So the frame comes from GL - one
// glReadPixels and a header - which also makes a run checkable without anyone
// watching it.
//
// BMP because it is a header and then the pixels. No encoder, no dependency,
// and GL hands rows back bottom-up, which is the order BMP already wants.

#include <cstdio>
#include <cstdint>
#include <vector>

#include "gl_api.h"

namespace wb {
namespace {

void put16(std::FILE* f, unsigned v) {
  const unsigned char b[2] = {static_cast<unsigned char>(v & 0xFF),
                              static_cast<unsigned char>(v >> 8)};
  std::fwrite(b, 1, 2, f);
}

void put32(std::FILE* f, std::uint32_t v) {
  const unsigned char b[4] = {static_cast<unsigned char>(v & 0xFF),
                              static_cast<unsigned char>(v >> 8),
                              static_cast<unsigned char>(v >> 16),
                              static_cast<unsigned char>(v >> 24)};
  std::fwrite(b, 1, 4, f);
}

}  // namespace

bool gl_capture(const char* path) {
  if (!g_gl.glReadPixels) return false;
  int w = 0, h = 0;
  gl_surface_size(&w, &h);
  if (w <= 0 || h <= 0) return false;

  std::vector<unsigned char> rgba(static_cast<std::size_t>(w) * h * 4);
  g_gl.glReadPixels(0, 0, w, h, 0x1908 /*GL_RGBA*/,
                    0x1401 /*GL_UNSIGNED_BYTE*/, rgba.data());

  const int stride = (w * 3 + 3) & ~3;      // BMP rows are 4-byte aligned
  const std::uint32_t pixels = static_cast<std::uint32_t>(stride) * h;
  const std::uint32_t offset = 14 + 40;

  std::FILE* f = std::fopen(path, "wb");
  if (!f) {
    std::printf("[gl  ] cannot write %s\n", path);
    return false;
  }
  std::fwrite("BM", 1, 2, f);
  put32(f, offset + pixels);
  put16(f, 0);
  put16(f, 0);
  put32(f, offset);
  put32(f, 40);                              // BITMAPINFOHEADER
  put32(f, static_cast<std::uint32_t>(w));
  put32(f, static_cast<std::uint32_t>(h));
  put16(f, 1);
  put16(f, 24);
  put32(f, 0);
  put32(f, pixels);
  put32(f, 2835);                            // 72 dpi, in pixels per metre
  put32(f, 2835);
  put32(f, 0);
  put32(f, 0);

  std::vector<unsigned char> row(static_cast<std::size_t>(stride), 0);
  for (int y = 0; y < h; ++y) {
    const unsigned char* src =
        rgba.data() + static_cast<std::size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      row[x * 3 + 0] = src[x * 4 + 2];       // BMP is BGR
      row[x * 3 + 1] = src[x * 4 + 1];
      row[x * 3 + 2] = src[x * 4 + 0];
    }
    std::fwrite(row.data(), 1, static_cast<std::size_t>(stride), f);
  }
  std::fclose(f);
  std::printf("[gl  ] wrote %s (%dx%d)\n", path, w, h);
  return true;
}

}  // namespace wb
