// The printf family.
//
// These matter far more than logging: the engine builds resource paths,
// shader names and error text with them. An unimplemented vsnprintf returns
// zero and leaves the caller's buffer untouched, so the failure surfaces much
// later as a missing file or a technique that cannot be found.

#include <algorithm>
#include <map>
#include <mutex>
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

// sscanf, in full rather than in part.
//
// The version this replaces handled %d, %u, %x, %f and %s, skipped the length
// modifier, and stopped the whole scan the moment it met anything else. Three
// consequences, and the third is the one that cost a day:
//
//   %*d and %c aborted the scan, so everything after them kept whatever the
//   caller happened to have on the stack;
//   %hd wrote four bytes into a two-byte object;
//   %lf wrote a *float* into a double. The low half was a plausible number
//   and the high half was untouched memory, which as a double is nonsense.
//
// TinyXML's QueryDoubleAttribute is one line of sscanf with "%lf", and every
// float in the engine's XML goes through it - which is where the hit capsules
// in Data/skeleton_bodies.xml come from. Bodies ended up with capsules of
// nonsense size and arrows went straight through people, while shields, whose
// collision comes from the item and not from that file, kept working.
//
// So this one tracks assignment suppression, field width, and the length
// modifier, and knows how wide the thing it is writing into is.
void t_sscanf(Env& e) {
  Env::Args a(e);
  const std::string input = e.mem().str(a.next32(), 1 << 16);
  const std::string fmt = e.mem().str(a.next32(), 1 << 12);
  const char* const begin = input.c_str();
  const char* in = begin;
  int assigned = 0;
  bool matched = true;

  // -2 hh, -1 h, 0 none, 1 l, 2 ll or L.
  int length = 0;
  bool suppress = false;
  std::size_t width = 0;

  auto next_out = [&]() -> u32 { return suppress ? 0u : a.next32(); };

  auto store_int = [&](unsigned long long v) {
    const u32 dst = next_out();
    if (suppress) return;
    if (dst) {
      switch (length) {
        case -2: e.mem().write8(dst, static_cast<u8>(v)); break;
        case -1: e.mem().write16(dst, static_cast<u16>(v)); break;
        case 2: e.mem().write64(dst, v); break;
        default: e.mem().write32(dst, static_cast<u32>(v)); break;
      }
    }
    ++assigned;
  };

  auto store_float = [&](double v) {
    const u32 dst = next_out();
    if (suppress) return;
    if (dst) {
      if (length >= 1) {
        // A double, eight bytes, little-endian - which is what ARM's EABI
        // puts in memory whether or not there is a VFP unit.
        u64 bits;
        std::memcpy(&bits, &v, 8);
        e.mem().write64(dst, bits);
      } else {
        const float f = static_cast<float>(v);
        u32 bits;
        std::memcpy(&bits, &f, 4);
        e.mem().write32(dst, bits);
      }
    }
    ++assigned;
  };

  // The numeric conversions take a field width, so they are given a copy of
  // at most that many characters to chew on rather than the whole rest of the
  // input.
  auto bounded = [&](std::size_t limit) {
    return limit ? std::string(in, std::min(limit, std::strlen(in)))
                 : std::string(in);
  };

  for (std::size_t i = 0; i < fmt.size() && matched; ++i) {
    const char f = fmt[i];
    if (std::isspace(static_cast<unsigned char>(f))) {
      while (*in && std::isspace(static_cast<unsigned char>(*in))) ++in;
      continue;
    }
    if (f != '%') {
      if (*in != f) break;
      ++in;
      continue;
    }
    if (++i >= fmt.size()) break;
    if (fmt[i] == '%') {
      if (*in != '%') break;
      ++in;
      continue;
    }

    suppress = false;
    width = 0;
    length = 0;
    if (fmt[i] == '*') {
      suppress = true;
      ++i;
    }
    while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i])))
      width = width * 10 + static_cast<std::size_t>(fmt[i++] - '0');
    while (i < fmt.size() && std::strchr("hlLjzt", fmt[i])) {
      if (fmt[i] == 'h') length = length == -1 ? -2 : -1;
      else if (fmt[i] == 'l') length = length == 1 ? 2 : 1;
      else if (fmt[i] == 'L') length = 2;
      else length = 1;                       // j, z, t are all 32-bit here
      ++i;
    }
    if (i >= fmt.size()) break;
    const char conv = fmt[i];

    // Every conversion but %c, %[ and %n skips leading whitespace first.
    if (conv != 'c' && conv != '[' && conv != 'n')
      while (*in && std::isspace(static_cast<unsigned char>(*in))) ++in;

    char* end = nullptr;
    switch (conv) {
      case 'd':
      case 'i':
      case 'u':
      case 'o':
      case 'x':
      case 'X':
      case 'p': {
        const std::string text = bounded(width);
        const int base = conv == 'i' ? 0
                         : conv == 'o' ? 8
                         : (conv == 'x' || conv == 'X' || conv == 'p') ? 16
                                                                      : 10;
        const bool is_signed = conv == 'd' || conv == 'i';
        const unsigned long long v =
            is_signed ? static_cast<unsigned long long>(
                            std::strtoll(text.c_str(), &end, base))
                      : std::strtoull(text.c_str(), &end, base);
        if (end == text.c_str()) { matched = false; break; }
        in += end - text.c_str();
        store_int(v);
        break;
      }

      case 'f':
      case 'F':
      case 'e':
      case 'E':
      case 'g':
      case 'G':
      case 'a':
      case 'A': {
        const std::string text = bounded(width);
        const double v = std::strtod(text.c_str(), &end);
        if (end == text.c_str()) { matched = false; break; }
        in += end - text.c_str();
        store_float(v);
        break;
      }

      case 'c': {
        const std::size_t want = width ? width : 1;
        if (std::strlen(in) < want) { matched = false; break; }
        const u32 dst = next_out();
        if (!suppress) {
          if (dst) e.mem().copy_in(dst, in, want);   // no terminator, by design
          ++assigned;
        }
        in += want;
        break;
      }

      case 's': {
        const char* const from = in;
        while (*in && !std::isspace(static_cast<unsigned char>(*in)) &&
               (!width || static_cast<std::size_t>(in - from) < width))
          ++in;
        if (in == from) { matched = false; break; }
        const u32 dst = next_out();
        if (!suppress) {
          const std::string word(from, in);
          if (dst) e.mem().copy_in(dst, word.c_str(), word.size() + 1);
          ++assigned;
        }
        break;
      }

      case '[': {
        // %[abc] and %[^abc]: a set of characters to take, or to stop at.
        bool negate = false;
        std::size_t j = i + 1;
        if (j < fmt.size() && fmt[j] == '^') { negate = true; ++j; }
        std::string set;
        if (j < fmt.size() && fmt[j] == ']') set.push_back(fmt[j++]);
        while (j < fmt.size() && fmt[j] != ']') set.push_back(fmt[j++]);
        i = j;                                   // sits on the closing bracket
        const char* const from = in;
        while (*in && (set.find(*in) != std::string::npos) != negate &&
               (!width || static_cast<std::size_t>(in - from) < width))
          ++in;
        if (in == from) { matched = false; break; }
        const u32 dst = next_out();
        if (!suppress) {
          const std::string word(from, in);
          if (dst) e.mem().copy_in(dst, word.c_str(), word.size() + 1);
          ++assigned;
        }
        break;
      }

      case 'n': {
        // How far the scan has got. Not an assignment, so it is not counted.
        const u32 dst = next_out();
        if (!suppress && dst)
          e.mem().write32(dst, static_cast<u32>(in - begin));
        break;
      }

      default:
        // Something not implemented. Say so once rather than silently
        // returning a short count, which is how the last set of these went
        // unnoticed.
        static std::mutex once;
        static std::map<std::string, bool> said;
        {
          std::lock_guard<std::mutex> lock(once);
          const std::string key(1, conv);
          if (!said[key] && said.size() <= 16)
            std::printf("[fmt ] sscanf does not know %%%c, in \"%s\"\n", conv,
                        fmt.c_str());
          said[key] = true;
        }
        matched = false;
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
