#include "guest_format.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wb {
namespace {

// The length modifier in a conversion, and whether it means the argument is
// 64 bits wide on the guest.
//
// This is the whole of the difference between "Distance: 47 yards" and
// "Distance: 38528128 yards". Warband's script machine works in 64-bit
// registers and prints them with %lld; on ARM32 a long long variadic argument
// is eight bytes, aligned to eight - it takes an even/odd register pair, or an
// aligned slot on the stack. Reading four bytes for it returns half a number
// and leaves every argument after it one word out of step, so the second
// number in a line comes out as whatever was next in memory, which is usually
// an address.
//
// `long` is not one of these: it is four bytes on ARM32, the same as int.
bool is_64_bit(const std::string& length) {
  return length == "ll" || length == "q" || length == "j";
}

}  // namespace

std::string guest_format(Env& e, ArgSource& args, const std::string& fmt) {
  std::string out;
  char buf[1024];

  for (std::size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%') {
      out.push_back(fmt[i]);
      continue;
    }
    if (i + 1 >= fmt.size()) break;

    // Flags, width and precision are kept so the output is laid out the way
    // the engine asked; the length modifier is read separately, because it
    // decides how much to consume rather than how to print.
    const std::size_t start = i++;
    std::string head;                        // everything before the length
    int stars = 0;                           // width or precision from an
    while (i < fmt.size() && std::strchr("-+ #0123456789.*", fmt[i])) {
      if (fmt[i] == '*') ++stars;            // argument, not from the format
      head.push_back(fmt[i++]);
    }
    // "%*d" and "%.*f" take the width, or the precision, as an argument of
    // their own, ahead of the value. Collecting the star into the spec and
    // not consuming that argument leaves the host printf reading the value as
    // a width and everything after it one place out of step - the same shape
    // of fault that made a long long print as an address.
    std::string sized = head;
    for (int star = 0; star < stars; ++star) {
      const int n = static_cast<int>(args.next32());
      const std::size_t at = sized.find('*');
      if (at == std::string::npos) break;
      sized = sized.substr(0, at) + std::to_string(n) + sized.substr(at + 1);
    }
    head = sized;
    std::string length;
    while (i < fmt.size() && std::strchr("hlLqjzt", fmt[i]))
      length.push_back(fmt[i++]);
    if (i >= fmt.size()) break;
    const char conv = fmt[i];
    const std::string spec = fmt.substr(start, i - start + 1);
    const bool wide = is_64_bit(length);

    switch (conv) {
      case '%':
        out.push_back('%');
        break;

      case 'd':
      case 'i':
        if (wide) {
          const std::string clean = "%" + head + "lld";
          std::snprintf(buf, sizeof(buf), clean.c_str(),
                        static_cast<long long>(args.next64()));
        } else {
          const std::string clean = "%" + head + "d";
          std::snprintf(buf, sizeof(buf), clean.c_str(),
                        static_cast<int>(args.next32()));
        }
        out += buf;
        break;

      case 'u':
      case 'x':
      case 'X':
      case 'o':
        if (wide) {
          const std::string clean = "%" + head + "ll" + conv;
          std::snprintf(buf, sizeof(buf), clean.c_str(),
                        static_cast<unsigned long long>(args.next64()));
        } else {
          const std::string clean = "%" + head + conv;
          std::snprintf(buf, sizeof(buf), clean.c_str(), args.next32());
        }
        out += buf;
        break;

      case 'c':
        out.push_back(static_cast<char>(args.next32()));
        break;

      case 'p':
        std::snprintf(buf, sizeof(buf), "0x%08X", args.next32());
        out += buf;
        break;

      case 'f':
      case 'F':
      case 'g':
      case 'G':
      case 'e':
      case 'E': {
        // Even %f promotes to double in a variadic call, so this is always a
        // 64-bit read - and always 8-byte aligned.
        double d = 0.0;
        const u64 bits = args.next64();
        std::memcpy(&d, &bits, 8);
        const std::string clean = "%" + head + conv;
        std::snprintf(buf, sizeof(buf), clean.c_str(), d);
        out += buf;
        break;
      }

      case 's': {
        const u32 p = args.next32();
        // Width and precision matter here - the engine pads names into
        // columns - so the host does the laying out, over a copy that cannot
        // run off the end of the arena.
        const std::string text = p ? e.mem().str(p, 1 << 16) : std::string("(null)");
        if (head.empty()) {
          out += text;
        } else {
          const std::string clean = "%" + head + "s";
          std::vector<char> wide_buf(text.size() + 1024);
          std::snprintf(wide_buf.data(), wide_buf.size(), clean.c_str(),
                        text.c_str());
          out += wide_buf.data();
        }
        break;
      }

      default:
        out += spec;
        break;
    }
  }
  return out;
}

}  // namespace wb
