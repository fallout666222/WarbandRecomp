// The printf family.
//
// These matter far more than logging: the engine builds resource paths,
// shader names and error text with them. An unimplemented vsnprintf returns
// zero and leaves the caller's buffer untouched, so the failure surfaces much
// later as a missing file or a technique that cannot be found.

#include <cstdio>
#include <cstring>
#include <string>

#include "guest_format.h"

namespace wb {
namespace {

// Writes the result into a guest buffer, truncating to `cap` including the
// terminator, and returns what printf would return: the length the output
// *would* have had.
u32 emit(Env& e, u32 dst, u32 cap, const std::string& text) {
  if (dst && cap > 0) {
    u32 room = e.mem().clamp(dst, cap);
    if (room > 0) {
      u32 n = static_cast<u32>(text.size());
      if (n > room - 1) n = room - 1;
      e.mem().copy_in(dst, text.data(), n);
      e.mem().write8(dst + n, 0);
    }
  }
  return static_cast<u32>(text.size());
}

void t_snprintf(Env& e) {
  Env::Args a(e);
  u32 dst = a.next32();
  u32 cap = a.next32();
  std::string fmt = e.mem().str(a.next32(), 1 << 16);
  RegArgs src(a);
  e.ret(emit(e, dst, cap, guest_format(e, src, fmt)));
}

void t_sprintf(Env& e) {
  Env::Args a(e);
  u32 dst = a.next32();
  std::string fmt = e.mem().str(a.next32(), 1 << 16);
  RegArgs src(a);
  // No bound was given, so the only limit is the arena itself.
  e.ret(emit(e, dst, 1u << 20, guest_format(e, src, fmt)));
}

void t_vsnprintf(Env& e) {
  Env::Args a(e);
  u32 dst = a.next32();
  u32 cap = a.next32();
  std::string fmt = e.mem().str(a.next32(), 1 << 16);
  VaListArgs src(e.mem(), a.next32());
  e.ret(emit(e, dst, cap, guest_format(e, src, fmt)));
}

void t_vsprintf(Env& e) {
  Env::Args a(e);
  u32 dst = a.next32();
  std::string fmt = e.mem().str(a.next32(), 1 << 16);
  VaListArgs src(e.mem(), a.next32());
  e.ret(emit(e, dst, 1u << 20, guest_format(e, src, fmt)));
}

void t_printf(Env& e) {
  Env::Args a(e);
  std::string fmt = e.mem().str(a.next32(), 1 << 16);
  RegArgs src(a);
  std::string text = guest_format(e, src, fmt);
  std::printf("[game] %s", text.c_str());
  e.ret(static_cast<u32>(text.size()));
}

void t_fprintf(Env& e) {
  Env::Args a(e);
  a.next32();                       // stream: stdout and stderr both go here
  std::string fmt = e.mem().str(a.next32(), 1 << 16);
  RegArgs src(a);
  std::string text = guest_format(e, src, fmt);
  std::printf("[game] %s", text.c_str());
  e.ret(static_cast<u32>(text.size()));
}

// sscanf is used for parsing a handful of config values. Only the conversions
// the engine actually uses are supported; anything else stops the scan, which
// is what a real sscanf would do on a mismatch anyway.
void t_sscanf(Env& e) {
  Env::Args a(e);
  std::string input = e.mem().str(a.next32(), 1 << 16);
  std::string fmt = e.mem().str(a.next32(), 1 << 12);
  const char* in = input.c_str();
  int assigned = 0;

  for (std::size_t i = 0; i < fmt.size(); ++i) {
    if (std::isspace(static_cast<unsigned char>(fmt[i]))) {
      while (*in && std::isspace(static_cast<unsigned char>(*in))) ++in;
      continue;
    }
    if (fmt[i] != '%') {
      if (*in != fmt[i]) break;
      ++in;
      continue;
    }
    if (++i >= fmt.size()) break;
    while (i < fmt.size() && std::strchr("0123456789hlL", fmt[i])) ++i;
    if (i >= fmt.size()) break;

    char* end = nullptr;
    switch (fmt[i]) {
      case 'd':
      case 'i': {
        long v = std::strtol(in, &end, 10);
        if (end == in) { i = fmt.size(); break; }
        e.mem().write32(a.next32(), static_cast<u32>(v));
        in = end;
        ++assigned;
        break;
      }
      case 'u':
      case 'x': {
        unsigned long v = std::strtoul(in, &end, fmt[i] == 'x' ? 16 : 10);
        if (end == in) { i = fmt.size(); break; }
        e.mem().write32(a.next32(), static_cast<u32>(v));
        in = end;
        ++assigned;
        break;
      }
      case 'f':
      case 'g': {
        double v = std::strtod(in, &end);
        if (end == in) { i = fmt.size(); break; }
        float f = static_cast<float>(v);
        u32 bits;
        std::memcpy(&bits, &f, 4);
        e.mem().write32(a.next32(), bits);
        in = end;
        ++assigned;
        break;
      }
      case 's': {
        while (*in && std::isspace(static_cast<unsigned char>(*in))) ++in;
        const char* begin = in;
        while (*in && !std::isspace(static_cast<unsigned char>(*in))) ++in;
        u32 dst = a.next32();
        std::string word(begin, in);
        if (dst) {
          e.mem().copy_in(dst, word.c_str(), word.size() + 1);
          ++assigned;
        }
        break;
      }
      default:
        i = fmt.size();
        break;
    }
  }
  e.ret(static_cast<u32>(assigned));
}

}  // namespace

const ThunkEntry kPrintfTable[] = {
    {"snprintf", &t_snprintf},
    {"sprintf", &t_sprintf},
    {"vsnprintf", &t_vsnprintf},
    {"vsprintf", &t_vsprintf},
    {"printf", &t_printf},
    {"fprintf", &t_fprintf},
    {"sscanf", &t_sscanf},
};

const std::size_t kPrintfTableSize =
    sizeof(kPrintfTable) / sizeof(kPrintfTable[0]);

}  // namespace wb
