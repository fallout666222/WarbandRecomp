// Maths, wide characters, time and the odds and ends.
//
// These were classified GUEST in the inventory - pure computation that a
// guest-side libc would own - and then never implemented, so every one of
// them was quietly returning zero. For a renderer that means every square
// root, sine and cosine was zero, which is not a subtle failure mode once
// anything tries to draw.
//
// Floats arrive and leave as bit patterns in core registers, because the
// engine is built softfp; Env::Args::nextf and Env::retf hide that.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <string>

#include "android_glue.h"
#include "env.h"

namespace wb {
namespace {

// ------------------------------------------------------------------- float
#define WB_MATH1(name, expr)                     \
  void t_##name(Env& e) {                        \
    Env::Args a(e);                              \
    float x = a.nextf();                         \
    e.retf(static_cast<float>(expr));            \
  }

WB_MATH1(sinf, std::sin(x))
WB_MATH1(cosf, std::cos(x))
WB_MATH1(tanf, std::tan(x))
WB_MATH1(asinf, std::asin(x))
WB_MATH1(acosf, std::acos(x))
WB_MATH1(atanf, std::atan(x))
WB_MATH1(expf, std::exp(x))
WB_MATH1(logf, std::log(x))
WB_MATH1(sqrtf, std::sqrt(x))
WB_MATH1(ceilf, std::ceil(x))
WB_MATH1(floorf, std::floor(x))
#undef WB_MATH1

void t_atan2f(Env& e) {
  Env::Args a(e);
  float y = a.nextf(), x = a.nextf();
  e.retf(std::atan2(y, x));
}

void t_powf(Env& e) {
  Env::Args a(e);
  float x = a.nextf(), y = a.nextf();
  // Nearly six million of these during loading, and every single one with
  // the exponent 2.2: this is gamma, sRGB to linear, done on the processor a
  // sample at a time. It is the biggest thing left crossing the boundary by a
  // wide margin, and the fact that the exponent never varies is what makes it
  // answerable in guest code later - see NOTES.md.
  e.retf(std::pow(x, y));
}

void t_fmodf(Env& e) {
  Env::Args a(e);
  float x = a.nextf(), y = a.nextf();
  e.retf(std::fmod(x, y));
}

void t_sqrt(Env& e) {
  Env::Args a(e);
  double x = a.nextd();
  double r = std::sqrt(x);
  u64 bits;
  std::memcpy(&bits, &r, 8);
  e.ret64(bits);
}

// ------------------------------------------------------------------ string
void t_memrchr(Env& e) {
  Env::Args a(e);
  u32 p = a.next32();
  int c = static_cast<int>(a.next32()) & 0xFF;
  u32 n = e.mem().clamp(p, a.next32());
  for (u32 i = n; i-- > 0;)
    if (e.mem().read8(p + i) == c) { e.ret(p + i); return; }
  e.ret(0);
}

void t_strcoll(Env& e) {
  Env::Args a(e);
  std::string x = e.mem().str(a.next32()), y = e.mem().str(a.next32());
  e.ret(static_cast<u32>(x.compare(y)));
}

void t_strxfrm(Env& e) {
  Env::Args a(e);
  u32 dst = a.next32();
  std::string src = e.mem().str(a.next32());
  u32 n = a.next32();
  if (dst && n > 0) {
    u32 room = e.mem().clamp(dst, n);
    u32 copy = static_cast<u32>(src.size());
    if (copy > room - 1) copy = room - 1;
    e.mem().copy_in(dst, src.data(), copy);
    e.mem().write8(dst + copy, 0);
  }
  e.ret(static_cast<u32>(src.size()));
}

// A single shared buffer, as strerror is allowed to use static storage.
u32 g_strerror_buf = 0;

void t_strerror(Env& e) {
  Env::Args a(e);
  int err = static_cast<int>(a.next32());
  if (!g_strerror_buf && e.glue()) g_strerror_buf = e.glue()->alloc_bytes(128);
  if (!g_strerror_buf) { e.ret(0); return; }
  char buf[128];
  std::snprintf(buf, sizeof(buf), "error %d", err);
  e.mem().copy_in(g_strerror_buf, buf, std::strlen(buf) + 1);
  e.ret(g_strerror_buf);
}

void t_strerror_r(Env& e) {
  Env::Args a(e);
  int err = static_cast<int>(a.next32());
  u32 dst = a.next32();
  u32 n = a.next32();
  char buf[128];
  std::snprintf(buf, sizeof(buf), "error %d", err);
  if (dst && n) {
    u32 room = e.mem().clamp(dst, n);
    u32 copy = static_cast<u32>(std::strlen(buf));
    if (copy > room - 1) copy = room - 1;
    e.mem().copy_in(dst, buf, copy);
    e.mem().write8(dst + copy, 0);
  }
  e.ret(0);
}

// strtok keeps state between calls; one static cursor matches the contract.
u32 g_strtok_cursor = 0;

void tokenise(Env& e, u32 str, const std::string& delims, u32* cursor) {
  u32 at = str ? str : *cursor;
  if (!at) { e.ret(0); return; }
  auto is_delim = [&](u8 c) {
    return delims.find(static_cast<char>(c)) != std::string::npos;
  };
  while (e.mem().read8(at) && is_delim(e.mem().read8(at))) ++at;
  if (!e.mem().read8(at)) { *cursor = 0; e.ret(0); return; }
  u32 begin = at;
  while (e.mem().read8(at) && !is_delim(e.mem().read8(at))) ++at;
  if (e.mem().read8(at)) {
    e.mem().write8(at, 0);
    *cursor = at + 1;
  } else {
    *cursor = 0;
  }
  e.ret(begin);
}

void t_strtok(Env& e) {
  Env::Args a(e);
  u32 str = a.next32();
  std::string delims = e.mem().str(a.next32());
  tokenise(e, str, delims, &g_strtok_cursor);
}

void t_strtok_r(Env& e) {
  Env::Args a(e);
  u32 str = a.next32();
  std::string delims = e.mem().str(a.next32());
  u32 save = a.next32();
  u32 cursor = save ? e.mem().read32(save) : 0;
  tokenise(e, str, delims, &cursor);
  if (save) e.mem().write32(save, cursor);
}

void t_strtoll(Env& e) {
  Env::Args a(e);
  u32 s = a.next32(), endp = a.next32();
  int base = static_cast<int>(a.next32());
  std::string text = e.mem().str(s);
  char* end = nullptr;
  long long v = std::strtoll(text.c_str(), &end, base);
  if (endp) e.mem().write32(endp, s + static_cast<u32>(end - text.c_str()));
  e.ret64(static_cast<u64>(v));
}

void t_basename(Env& e) {
  Env::Args a(e);
  u32 p = a.next32();
  std::string s = e.mem().str(p);
  std::size_t at = s.find_last_of("/\\");
  e.ret(at == std::string::npos ? p : p + static_cast<u32>(at) + 1);
}

// ------------------------------------------------------------- wide chars
void t_wmemcpy(Env& e) {
  Env::Args a(e);
  u32 d = a.next32(), s = a.next32(), n = a.next32() * 4;
  n = std::min(e.mem().clamp(d, n), e.mem().clamp(s, n));
  if (n) std::memcpy(e.mem().host<u8>(d), e.mem().host<u8>(s), n);
  e.ret(d);
}

void t_wmemmove(Env& e) {
  Env::Args a(e);
  u32 d = a.next32(), s = a.next32(), n = a.next32() * 4;
  n = std::min(e.mem().clamp(d, n), e.mem().clamp(s, n));
  if (n) std::memmove(e.mem().host<u8>(d), e.mem().host<u8>(s), n);
  e.ret(d);
}

void t_wmemset(Env& e) {
  Env::Args a(e);
  u32 d = a.next32(), v = a.next32(), n = a.next32();
  for (u32 i = 0; i < n; ++i) e.mem().write32(d + i * 4, v);
  e.ret(d);
}

void t_wmemcmp(Env& e) {
  Env::Args a(e);
  u32 p = a.next32(), q = a.next32(), n = a.next32();
  for (u32 i = 0; i < n; ++i) {
    u32 x = e.mem().read32(p + i * 4), y = e.mem().read32(q + i * 4);
    if (x != y) { e.ret(static_cast<u32>(x < y ? -1 : 1)); return; }
  }
  e.ret(0);
}

void t_wmemchr(Env& e) {
  Env::Args a(e);
  u32 p = a.next32(), v = a.next32(), n = a.next32();
  for (u32 i = 0; i < n; ++i)
    if (e.mem().read32(p + i * 4) == v) { e.ret(p + i * 4); return; }
  e.ret(0);
}

void t_wcscoll(Env& e) {
  Env::Args a(e);
  u32 p = a.next32(), q = a.next32();
  for (u32 i = 0;; ++i) {
    u32 x = e.mem().read32(p + i * 4), y = e.mem().read32(q + i * 4);
    if (x != y) { e.ret(static_cast<u32>(x < y ? -1 : 1)); return; }
    if (!x) break;
  }
  e.ret(0);
}

void t_wcsxfrm(Env& e) {
  Env::Args a(e);
  u32 d = a.next32(), s = a.next32(), n = a.next32();
  u32 i = 0;
  for (; i + 1 < n; ++i) {
    u32 c = e.mem().read32(s + i * 4);
    e.mem().write32(d + i * 4, c);
    if (!c) break;
  }
  e.ret(i);
}

void t_mbrtowc(Env& e) {
  Env::Args a(e);
  u32 wc = a.next32(), src = a.next32(), n = a.next32();
  a.next32();                       // mbstate: single-byte only here
  if (!src || !n) { e.ret(0); return; }
  u8 c = e.mem().read8(src);
  if (wc) e.mem().write32(wc, c);
  e.ret(c ? 1u : 0u);
}

void t_wcrtomb(Env& e) {
  Env::Args a(e);
  u32 dst = a.next32(), wc = a.next32();
  if (dst) e.mem().write8(dst, static_cast<u8>(wc & 0xFF));
  e.ret(1);
}

void t_wide_eof(Env& e) { e.ret(0xFFFFFFFF); }   // getwc / ungetwc
void t_putwc(Env& e) {
  Env::Args a(e);
  e.ret(a.next32());
}

// -------------------------------------------------------------------- time
void t_difftime(Env& e) {
  Env::Args a(e);
  std::int32_t later = static_cast<std::int32_t>(a.next32());
  std::int32_t earlier = static_cast<std::int32_t>(a.next32());
  double d = static_cast<double>(later - earlier);
  u64 bits;
  std::memcpy(&bits, &d, 8);
  e.ret64(bits);
}

// struct tm as 32-bit bionic lays it out: nine ints, then gmtoff and zone.
void t_gmtime_r(Env& e) {
  Env::Args a(e);
  u32 tp = a.next32(), out = a.next32();
  std::time_t t = tp ? static_cast<std::time_t>(e.mem().read32(tp)) : 0;
  std::tm tm {};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  if (out) {
    const int f[9] = {tm.tm_sec,  tm.tm_min,  tm.tm_hour,
                      tm.tm_mday, tm.tm_mon,  tm.tm_year,
                      tm.tm_wday, tm.tm_yday, tm.tm_isdst};
    for (int i = 0; i < 9; ++i)
      e.mem().write32(out + i * 4, static_cast<u32>(f[i]));
    e.mem().write32(out + 36, 0);   // tm_gmtoff
    e.mem().write32(out + 40, 0);   // tm_zone
  }
  e.ret(out);
}

void t_strftime(Env& e) {
  Env::Args a(e);
  u32 dst = a.next32(), cap = a.next32();
  std::string fmt = e.mem().str(a.next32());
  u32 tmp = a.next32();
  std::tm tm {};
  if (tmp) {
    tm.tm_sec = static_cast<int>(e.mem().read32(tmp));
    tm.tm_min = static_cast<int>(e.mem().read32(tmp + 4));
    tm.tm_hour = static_cast<int>(e.mem().read32(tmp + 8));
    tm.tm_mday = static_cast<int>(e.mem().read32(tmp + 12));
    tm.tm_mon = static_cast<int>(e.mem().read32(tmp + 16));
    tm.tm_year = static_cast<int>(e.mem().read32(tmp + 20));
    tm.tm_wday = static_cast<int>(e.mem().read32(tmp + 24));
    tm.tm_yday = static_cast<int>(e.mem().read32(tmp + 28));
  }
  char buf[512];
  std::size_t n = std::strftime(buf, sizeof(buf), fmt.c_str(), &tm);
  if (dst && cap) {
    u32 room = e.mem().clamp(dst, cap);
    if (n > room - 1) n = room - 1;
    e.mem().copy_in(dst, buf, n);
    e.mem().write8(dst + static_cast<u32>(n), 0);
  }
  e.ret(static_cast<u32>(n));
}

void t_wcsftime(Env& e) { e.ret(0); }
void t_fdopen(Env& e) { e.ret(0); }
void t_raise(Env& e) { e.ret(0); }

}  // namespace

const ThunkEntry kMathTable[] = {
    {"sinf", &t_sinf},     {"cosf", &t_cosf},       {"tanf", &t_tanf},
    {"asinf", &t_asinf},   {"acosf", &t_acosf},     {"atanf", &t_atanf},
    {"atan2f", &t_atan2f}, {"expf", &t_expf},       {"logf", &t_logf},
    {"powf", &t_powf},     {"sqrtf", &t_sqrtf},     {"sqrt", &t_sqrt},
    {"ceilf", &t_ceilf},   {"floorf", &t_floorf},   {"fmodf", &t_fmodf},

    {"memrchr", &t_memrchr},       {"strcoll", &t_strcoll},
    {"strxfrm", &t_strxfrm},       {"strerror", &t_strerror},
    {"strerror_r", &t_strerror_r}, {"strtok", &t_strtok},
    {"strtok_r", &t_strtok_r},     {"strtoll", &t_strtoll},
    {"basename", &t_basename},

    {"wmemcpy", &t_wmemcpy},   {"wmemmove", &t_wmemmove},
    {"wmemset", &t_wmemset},   {"wmemcmp", &t_wmemcmp},
    {"wmemchr", &t_wmemchr},   {"wcscoll", &t_wcscoll},
    {"wcsxfrm", &t_wcsxfrm},   {"mbrtowc", &t_mbrtowc},
    {"wcrtomb", &t_wcrtomb},   {"getwc", &t_wide_eof},
    {"ungetwc", &t_wide_eof},  {"putwc", &t_putwc},

    {"difftime", &t_difftime}, {"gmtime_r", &t_gmtime_r},
    {"strftime", &t_strftime}, {"wcsftime", &t_wcsftime},
    {"fdopen", &t_fdopen},     {"raise", &t_raise},
};

const std::size_t kMathTableSize = sizeof(kMathTable) / sizeof(kMathTable[0]);

}  // namespace wb
