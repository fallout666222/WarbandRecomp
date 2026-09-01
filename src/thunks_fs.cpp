// The filesystem the engine sees.
//
// Guest paths are Android paths - /sdcard/Android/obb/... and the OBB mount
// point the engine asked for - so every path is rewritten onto a real
// directory before it reaches the host. That mapping is the whole trick: the
// engine believes it is reading a mounted OBB, and it is really reading the
// extracted tree.
//
// File descriptors are small integers on both sides, so unlike EGL objects
// or mutexes they need no handle table - a host FILE* is kept in a vector
// indexed by the descriptor the guest was given.

#include <sys/stat.h>
#include <sys/types.h>
#if defined(_WIN32) && !defined(WB_SWITCH)
#include <direct.h>
#endif

// Asking the host filesystem whether something exists, and how big it is.
// MSVC spells the 64-bit form _stat64 and everyone else spells it stat, so
// the two names are unified here rather than at each of the six call sites.
#if defined(_WIN32) && !defined(WB_SWITCH)
using HostStat = struct _stat64;
inline int host_stat(const char* path, HostStat* out) {
  return ::_stat64(path, out);
}
#else
using HostStat = struct stat;
inline int host_stat(const char* path, HostStat* out) {
  return ::stat(path, out);
}
#endif

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "android_glue.h"
#include "env.h"

namespace wb {
namespace {

std::string g_data_root;      // where the extracted OBB tree lives
std::string g_save_root;      // where anything the game writes goes

struct OpenFile {
  std::FILE* fp = nullptr;
  std::string path;
  bool trace = false;   // shader files: worth watching byte by byte
  u32 guest_file = 0;   // the guest-visible FILE object, if one was made
};

// A guest FILE* has to be a real object in guest memory, not a token: the
// engine's static libstdc++ reaches inside it for the descriptor - fileno()
// is a macro over `((FILE*)f)->_file` - and then calls read() with that. In
// 32-bit bionic _file is a short at offset 14.
constexpr u32 kFileFdOffset = 14;
constexpr u32 kFileFlagsOffset = 12;

std::mutex g_guest_files_lock;
std::vector<std::pair<u32, int>> g_guest_files;   // guest address -> fd
int fd_for_guest_file(u32 addr);

std::mutex g_files_lock;
std::vector<OpenFile> g_files;

// Descriptors 0-2 stay reserved for the standard streams.
constexpr int kFirstFd = 3;

int add_file(std::FILE* fp, const std::string& path) {
  std::lock_guard<std::mutex> lock(g_files_lock);
  for (std::size_t i = 0; i < g_files.size(); ++i) {
    if (!g_files[i].fp) {
      const u32 keep = g_files[i].guest_file;
      g_files[i] = {fp, path, path.find(".glsl") != std::string::npos, keep};
      return static_cast<int>(i) + kFirstFd;
    }
  }
  g_files.push_back({fp, path, path.find(".glsl") != std::string::npos});
  return static_cast<int>(g_files.size()) - 1 + kFirstFd;
}

// The guest FILE object belonging to a descriptor, if one was made. Reused
// when the descriptor is: otherwise every open leaks 84 bytes of the glue
// region and a long loading run exhausts it.
u32 guest_file_for(int fd) {
  std::lock_guard<std::mutex> lock(g_files_lock);
  std::size_t i = static_cast<std::size_t>(fd - kFirstFd);
  if (fd < kFirstFd || i >= g_files.size()) return 0;
  return g_files[i].guest_file;
}

void set_guest_file(int fd, u32 addr) {
  std::lock_guard<std::mutex> lock(g_files_lock);
  std::size_t i = static_cast<std::size_t>(fd - kFirstFd);
  if (fd < kFirstFd || i >= g_files.size()) return;
  g_files[i].guest_file = addr;
}

bool traced(int fd) {
  std::lock_guard<std::mutex> lock(g_files_lock);
  std::size_t i = static_cast<std::size_t>(fd - kFirstFd);
  if (fd < kFirstFd || i >= g_files.size()) return false;
  return g_files[i].trace;
}

std::FILE* file_for(int fd) {
  std::lock_guard<std::mutex> lock(g_files_lock);
  std::size_t i = static_cast<std::size_t>(fd - kFirstFd);
  if (fd < kFirstFd || i >= g_files.size()) return nullptr;
  return g_files[i].fp;
}

void drop_file(int fd) {
  std::lock_guard<std::mutex> lock(g_files_lock);
  std::size_t i = static_cast<std::size_t>(fd - kFirstFd);
  if (fd < kFirstFd || i >= g_files.size()) return;
  if (g_files[i].fp) std::fclose(g_files[i].fp);
  const u32 keep = g_files[i].guest_file;   // the object outlives the file
  g_files[i] = {};
  g_files[i].guest_file = keep;
}

// Creates every directory along a path except the last component. The engine
// makes saves in Savegames/<module>/ and does not create the directory
// itself, because on Android its private data directory already exists.
void ensure_parent(const std::string& path) {
  for (std::size_t i = 1; i < path.size(); ++i) {
    if (path[i] != '/' && path[i] != '\\') continue;
    const std::string dir = path.substr(0, i);
#if defined(_WIN32) && !defined(WB_SWITCH)
    ::_mkdir(dir.c_str());
#else
    ::mkdir(dir.c_str(), 0777);
#endif
  }
}

// Android path -> host path.
//
// The engine builds paths from the OBB mount point it was handed, so most
// arrive with that prefix. Anything already relative is taken as relative to
// the data root, which is how the engine's own "Data/foo.txt" style paths
// resolve. `writable` sends the result to the save tree instead.
std::string to_host(const std::string& guest, bool writable = false) {
  static const char* kPrefixes[] = {
      "/sdcard/Android/obb/com.taleworlds.mbwarband",
      "/sdcard/Android/data/com.taleworlds.mbwarband",
      "/data/data/com.taleworlds.mbwarband",
      "/sdcard",
  };
  // The engine sometimes joins a root it already has onto an already-absolute
  // path, producing ".../mbwarband/Modules/Native//sdcard/.../Data/font.xml".
  // Taking everything after the *last* prefix occurrence collapses that, and
  // is harmless for well-formed paths.
  std::string p = guest;
  for (const char* prefix : kPrefixes) {
    std::size_t n = std::strlen(prefix);
    std::size_t at = p.rfind(prefix);
    if (at != std::string::npos) {
      p = p.substr(at + n);
      break;
    }
  }
  while (!p.empty() && (p[0] == '/' || p[0] == '\\')) p.erase(0, 1);
  if (p.empty()) return g_data_root;

  // The engine builds some paths with a backslash - "Savegames/Native\sg00.sav"
  // is one it uses every time it looks for a save. Windows treats that as a
  // separator and Horizon does not, so it is normalised here rather than
  // becoming a filename with a backslash in it on the console.
  for (char& c : p)
    if (c == '\\') c = '/';

  // Anything the game writes goes into its own tree. On Android these paths
  // are the app's private data directory, which is writable and separate from
  // the read-only OBB; keeping that split means the extracted game data stays
  // exactly as it was extracted, and a save never lands among the assets.
  if (writable) {
    const std::string out = g_save_root + "/" + p;
    ensure_parent(out);
    return out;
  }

  std::string host = g_data_root + "/" + p;

  // Warband looks inside the active module first and falls back to the shared
  // trees. In the shipped OBB almost nothing is actually under the module, so
  // a miss here is normal rather than an error - resolve it instead of
  // letting the engine see a missing file.
  //
  //   Modules/Native/Resource/x.brf  ->  CommonRes/x.brf
  //   Modules/Native/languages/en/ui.csv -> languages/en/ui.csv
  // A file the game wrote earlier lives in the writable tree, and it will ask
  // for it back by the same Android path it wrote it under - so that tree is
  // searched first. It holds saves and settings only, so it is nearly always
  // a miss and costs one stat.
  HostStat st {};
  if (!g_save_root.empty()) {
    const std::string saved = g_save_root + "/" + p;
    if (host_stat(saved.c_str(), &st) == 0) return saved;
  }
  if (host_stat(host.c_str(), &st) == 0) return host;

  if (p.compare(0, 8, "Modules/") == 0) {
    const std::string marker = "/Resource/";
    std::size_t at = p.find(marker);
    if (at != std::string::npos) {
      std::string common =
          g_data_root + "/CommonRes/" + p.substr(at + marker.size());
      if (host_stat(common.c_str(), &st) == 0) return common;
    }
    // Anything else under the module: try the same relative path at the root.
    std::size_t slash = p.find('/', 8);
    if (slash != std::string::npos) {
      std::string shared = g_data_root + "/" + p.substr(slash + 1);
      if (host_stat(shared.c_str(), &st) == 0) return shared;
    }
  }
  return host;
}

// --------------------------------------------------------------------- open
constexpr u32 kO_WRONLY = 1, kO_RDWR = 2, kO_CREAT = 0100, kO_TRUNC = 01000,
              kO_APPEND = 02000;

void t_open(Env& e) {
  Env::Args a(e);
  std::string guest = e.mem().str(a.next32());
  u32 flags = a.next32();
  const bool writing =
      (flags & (kO_WRONLY | kO_RDWR | kO_CREAT | kO_TRUNC | kO_APPEND)) != 0;
  std::string host = to_host(guest, writing);

  const char* mode = "rb";
  if (flags & kO_APPEND) mode = "ab";
  else if (flags & kO_TRUNC) mode = "wb";
  else if ((flags & kO_RDWR) == kO_RDWR) mode = "r+b";
  else if (flags & kO_WRONLY) mode = "wb";
  if ((flags & kO_CREAT) && !(flags & kO_APPEND) && mode[0] == 'r') mode = "w+b";

  std::FILE* fp = std::fopen(host.c_str(), mode);
  if (!fp) {
    static int complaints = 0;
    if (++complaints <= 40)
      std::printf("[fs  ] missing: %s  (-> %s)\n", guest.c_str(), host.c_str());
    e.ret(0xFFFFFFFF);
    return;
  }
  int fd = add_file(fp, host);
  static int opened = 0;
  if (++opened <= 2000) std::printf("[fs  ] open %s -> fd %d\n", guest.c_str(), fd);
  e.ret(static_cast<u32>(fd));
}

void t_close(Env& e) {
  Env::Args a(e);
  drop_file(static_cast<int>(a.next32()));
  e.ret(0);
}

void t_read(Env& e) {
  Env::Args a(e);
  int fd = static_cast<int>(a.next32());
  u32 buf = a.next32();
  u32 count = e.mem().clamp(buf, a.next32());
  std::FILE* fp = file_for(fd);
  if (!fp || !count) {
    e.ret(0);
    return;
  }
  std::size_t got = std::fread(e.mem().host<u8>(buf), 1, count, fp);
  if (traced(fd)) {
    static int shown = 0;
    if (++shown <= 12)
      std::printf("[fs  ] .glsl read fd=%d count=%u -> %zu bytes\n", fd, count,
                  got);
  }
  e.ret(static_cast<u32>(got));
}

void t_write(Env& e) {
  Env::Args a(e);
  int fd = static_cast<int>(a.next32());
  u32 buf = a.next32();
  u32 count = e.mem().clamp(buf, a.next32());
  if (fd == 1 || fd == 2) {
    // The engine writing to stdout or stderr directly.
    std::string s(reinterpret_cast<const char*>(e.mem().host<u8>(buf)), count);
    std::printf("[game] %s", s.c_str());
    e.ret(count);
    return;
  }
  std::FILE* fp = file_for(fd);
  if (!fp || !count) {
    e.ret(0);
    return;
  }
  e.ret(static_cast<u32>(std::fwrite(e.mem().host<u8>(buf), 1, count, fp)));
}

void t_lseek(Env& e) {
  Env::Args a(e);
  int fd = static_cast<int>(a.next32());
  long off = static_cast<long>(static_cast<std::int32_t>(a.next32()));
  int whence = static_cast<int>(a.next32());
  std::FILE* fp = file_for(fd);
  if (!fp) {
    e.ret(0xFFFFFFFF);
    return;
  }
  std::fseek(fp, off, whence);
  e.ret(static_cast<u32>(std::ftell(fp)));
}

// struct stat as 32-bit bionic lays it out. Only the fields the engine reads
// are filled: the mode, so it can tell a directory from a file, and the size.
constexpr u32 kStatSize = 96;
constexpr u32 kStatModeOffset = 16;
constexpr u32 kStatSizeOffset = 48;

void fill_stat(Env& e, u32 out, const HostStat& st) {
  if (!out) return;
  e.mem().zero(out, kStatSize);
  e.mem().write32(out + kStatModeOffset, static_cast<u32>(st.st_mode));
  e.mem().write64(out + kStatSizeOffset, static_cast<u64>(st.st_size));
}

void t_stat(Env& e) {
  Env::Args a(e);
  std::string guest = e.mem().str(a.next32());
  u32 out = a.next32();
  HostStat st {};
  if (host_stat(to_host(guest).c_str(), &st) != 0) {
    e.ret(0xFFFFFFFF);
    return;
  }
  fill_stat(e, out, st);
  e.ret(0);
}

void t_fstat(Env& e) {
  Env::Args a(e);
  int fd = static_cast<int>(a.next32());
  u32 out = a.next32();
  std::FILE* fp = file_for(fd);
  if (!fp) {
    e.ret(0xFFFFFFFF);
    return;
  }
  long here = std::ftell(fp);
  std::fseek(fp, 0, SEEK_END);
  long size = std::ftell(fp);
  std::fseek(fp, here, SEEK_SET);
  HostStat st {};
  st.st_mode = 0100644;                 // regular file
  st.st_size = size;
  fill_stat(e, out, st);
  e.ret(0);
}

// The engine only makes directories for save games, so this lands in the
// writable tree - and to_host has already created the parents on the way.
void t_mkdir(Env& e) {
  Env::Args a(e);
  const std::string host = to_host(e.mem().str(a.next32()), true);
#if defined(_WIN32) && !defined(WB_SWITCH)
  ::_mkdir(host.c_str());
#else
  ::mkdir(host.c_str(), 0777);
#endif
  e.ret(0);
}

// remove and rename, which are how a save gets its name.
//
// Saving in Warband is not one write. The engine builds the whole file under
// a temporary name, verifies its checksum, and only then commits it:
//
//     write   Savegames/Native/new_game.sav
//     rename  sg00.sav -> last_savegame_backup.sav
//     rename  new_game.sav -> sg00.sav
//
// Both of these were answered with "success, did nothing", so every save was
// written correctly and then never given the name the load screen looks for.
// The engine said "Savegame succeded..." and meant it - the rename it asked
// for reported success - and Load Game listed nothing, because sg00.sav had
// never existed.
//
// Both paths map into the writable tree. That is not only where saves live,
// it is the guard: an engine that asked to delete something would otherwise
// be asking to delete the extracted game data, and there is no reason to
// give it that.
void t_remove(Env& e) {
  Env::Args a(e);
  const std::string guest = e.mem().str(a.next32());
  const std::string host = to_host(guest, true);
  const int rc = std::remove(host.c_str());
  static int shown = 0;
  if (++shown <= 8)
    std::printf("[fs  ] remove %s -> %s%s\n", guest.c_str(), host.c_str(),
                rc == 0 ? "" : " (not there)");
  e.ret(rc == 0 ? 0u : 0xFFFFFFFFu);
}

void t_rename(Env& e) {
  Env::Args a(e);
  const std::string from_guest = e.mem().str(a.next32());
  const std::string to_guest = e.mem().str(a.next32());
  const std::string from = to_host(from_guest, true);
  const std::string to = to_host(to_guest, true);

  // POSIX rename replaces the destination; Windows refuses if it exists, and
  // this has to behave the same on both or a save would commit on one and not
  // the other.
  std::remove(to.c_str());
  const int rc = std::rename(from.c_str(), to.c_str());
  static int shown = 0;
  if (++shown <= 8)
    std::printf("[fs  ] rename %s -> %s%s\n", from.c_str(), to.c_str(),
                rc == 0 ? "" : " FAILED");
  e.ret(rc == 0 ? 0u : 0xFFFFFFFFu);
}

void t_ok(Env& e) { e.ret(0); }
void t_minus_one(Env& e) { e.ret(0xFFFFFFFF); }

// ------------------------------------------------------------------- stdio
//
// A guest FILE* is a token, not a pointer: real files get 0x40000000 | fd,
// while stdin/stdout/stderr keep arriving as addresses inside __sF, because
// that is how bionic's macros expand. Both shapes resolve here.
u32 g_sF_base = 0;

void set_stdio_base(u32 addr) { g_sF_base = addr; }

int fd_for_guest_file(u32 addr) {
  std::lock_guard<std::mutex> lock(g_guest_files_lock);
  for (const auto& [a, fd] : g_guest_files)
    if (a == addr) return fd;
  return -1;
}

std::FILE* resolve_stream(u32 token) {
  if (!token) return nullptr;
  const int fd = fd_for_guest_file(token);
  if (fd >= 0) return file_for(fd);
  if (g_sF_base && token >= g_sF_base) {
    switch ((token - g_sF_base) / kBionicFileSize) {
      case 1: return stdout;
      case 2: return stderr;
      default: return nullptr;
    }
  }
  static int shown = 0;
  if (++shown <= 10)
    std::printf("[fs  ] unrecognised FILE* 0x%08X\n", token);
  return nullptr;
}

void t_fopen(Env& e) {
  Env::Args a(e);
  std::string guest = e.mem().str(a.next32());
  std::string mode = e.mem().str(a.next32());
  if (mode.find('b') == std::string::npos) mode += "b";
  const bool writing = mode.find('w') != std::string::npos ||
                       mode.find('a') != std::string::npos ||
                       mode.find('+') != std::string::npos;
  std::string host = to_host(guest, writing);
  std::FILE* fp = std::fopen(host.c_str(), mode.c_str());
  if (!fp) {
    static int complaints = 0;
    if (++complaints <= 40)
      std::printf("[fs  ] fopen missing: %s  (-> %s)\n", guest.c_str(),
                  host.c_str());
    e.ret(0);
    return;
  }
  int fd = add_file(fp, host);

  // The guest gets a real FILE object it can look inside. One per descriptor,
  // reused as descriptors are.
  u32 obj = guest_file_for(fd);
  if (!obj) {
    obj = e.glue() ? e.glue()->alloc_bytes(kBionicFileSize) : 0;
    if (!obj) {
      std::printf("[fs  ] out of room for guest FILE objects\n");
      e.ret(0);
      return;
    }
    set_guest_file(fd, obj);
    std::lock_guard<std::mutex> lock(g_guest_files_lock);
    g_guest_files.emplace_back(obj, fd);
  }
  e.mem().zero(obj, kBionicFileSize);
  e.mem().write16(obj + kFileFdOffset, static_cast<u16>(fd));
  e.mem().write16(obj + kFileFlagsOffset, 4);   // __SRD: open for reading
  static int opened = 0;
  if (++opened <= 2000)
    std::printf("[fs  ] fopen %s -> fd %d, FILE 0x%08X\n", guest.c_str(), fd,
                obj);
  e.ret(obj);
}

void t_fclose(Env& e) {
  Env::Args a(e);
  u32 obj = a.next32();
  int fd = fd_for_guest_file(obj);
  if (fd >= 0) drop_file(fd);
  e.ret(0);
}

void t_fread(Env& e) {
  Env::Args a(e);
  u32 buf = a.next32();
  u32 size = a.next32(), count = a.next32();
  u32 token = a.next32();
  std::FILE* fp = resolve_stream(token);
  u64 total = u64(size) * count;
  u32 safe = e.mem().clamp(buf, total > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                                      : static_cast<u32>(total));
  if (!fp || !safe || !size) {
    e.ret(0);
    return;
  }
  std::size_t got = std::fread(e.mem().host<u8>(buf), 1, safe, fp);
  static int shown = 0;
  const int tfd = fd_for_guest_file(token);
  const bool shader = tfd >= 0 && traced(tfd);
  if (++shown <= 20 || shader)
    std::printf("[fs  ] fread%s size=%u count=%u -> %zu bytes\n",
                shader ? " .glsl" : "", size, count, got);
  e.ret(static_cast<u32>(got / size));
}

void t_fwrite(Env& e) {
  Env::Args a(e);
  u32 buf = a.next32();
  u32 size = a.next32(), count = a.next32();
  u32 token = a.next32();
  std::FILE* fp = resolve_stream(token);
  u64 total = u64(size) * count;
  u32 safe = e.mem().clamp(buf, total > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                                      : static_cast<u32>(total));
  if (fp && safe) std::fwrite(e.mem().host<u8>(buf), 1, safe, fp);
  e.ret(count);
}

void t_fseek(Env& e) {
  Env::Args a(e);
  std::FILE* fp = resolve_stream(a.next32());
  long off = static_cast<long>(static_cast<std::int32_t>(a.next32()));
  int whence = static_cast<int>(a.next32());
  e.ret(fp ? static_cast<u32>(std::fseek(fp, off, whence)) : 0xFFFFFFFF);
}

void t_ftell(Env& e) {
  Env::Args a(e);
  std::FILE* fp = resolve_stream(a.next32());
  long at = fp ? std::ftell(fp) : -1;
  static int shown = 0;
  if (++shown <= 20) std::printf("[fs  ] ftell -> %ld\n", at);
  e.ret(static_cast<u32>(at));
}

void t_feof(Env& e) {
  Env::Args a(e);
  std::FILE* fp = resolve_stream(a.next32());
  e.ret(fp && std::feof(fp) ? 1 : 0);
}

void t_fflush(Env& e) {
  Env::Args a(e);
  std::FILE* fp = resolve_stream(a.next32());
  if (fp) std::fflush(fp);
  e.ret(0);
}

void t_fgetc(Env& e) {
  Env::Args a(e);
  u32 token = a.next32();
  std::FILE* fp = resolve_stream(token);
  int c = fp ? std::fgetc(fp) : -1;
  if (true) {
    static int shown = 0;
    if (++shown <= 0) std::printf("[fs  ] .glsl fgetc -> %d\n", c);
  }
  e.ret(static_cast<u32>(c));
}

void t_feof_logged(Env& e) {
  Env::Args a(e);
  u32 token = a.next32();
  std::FILE* fp = resolve_stream(token);
  int r = (fp && std::feof(fp)) ? 1 : 0;
  if (true) {
    static int shown = 0;
    if (++shown <= 0) std::printf("[fs  ] .glsl feof -> %d\n", r);
  }
  e.ret(static_cast<u32>(r));
}

void t_ungetc(Env& e) {
  Env::Args a(e);
  int c = static_cast<int>(a.next32());
  std::FILE* fp = resolve_stream(a.next32());
  e.ret(fp ? static_cast<u32>(std::ungetc(c, fp)) : 0xFFFFFFFF);
}

void t_fgets(Env& e) {
  Env::Args a(e);
  u32 buf = a.next32();
  u32 size = e.mem().clamp(buf, a.next32());
  std::FILE* fp = resolve_stream(a.next32());
  if (!fp || size < 2) {
    e.ret(0);
    return;
  }
  std::vector<char> tmp(size);
  if (!std::fgets(tmp.data(), static_cast<int>(size), fp)) {
    e.ret(0);
    return;
  }
  e.mem().copy_in(buf, tmp.data(), std::strlen(tmp.data()) + 1);
  e.ret(buf);
}

void t_fputs(Env& e) {
  Env::Args a(e);
  std::string s = e.mem().str(a.next32(), 1 << 16);
  std::FILE* fp = resolve_stream(a.next32());
  if (fp) std::fputs(s.c_str(), fp);
  e.ret(0);
}

void t_fputc(Env& e) {
  Env::Args a(e);
  u32 c = a.next32();
  std::FILE* fp = resolve_stream(a.next32());
  if (fp) std::fputc(static_cast<int>(c), fp);
  e.ret(c);
}

void t_setvbuf(Env& e) { e.ret(0); }

void t_getenv(Env& e) { e.ret(0); }
void t_geteuid(Env& e) { e.ret(1000); }
void t_realpath(Env& e) {
  Env::Args a(e);
  u32 in = a.next32();
  u32 out = a.next32();
  std::string p = e.mem().str(in);
  if (out) e.mem().copy_in(out, p.c_str(), p.size() + 1);
  e.ret(out ? out : in);
}

}  // namespace

// The audio layer opens files the engine named, and they arrive in the same
// Android shape as everything else: an OBB mount point the engine glued on,
// sometimes twice, over a tree that is not laid out the way it expects. All
// of that already lives here, so it is answered here.
std::string fs_host_path(const std::string& guest) {
  return to_host(guest, false);
}

void fs_set_stdio_base(u32 addr) { set_stdio_base(addr); }

void fs_set_data_root(const std::string& path) {
  g_data_root = path;
  while (!g_data_root.empty() &&
         (g_data_root.back() == '/' || g_data_root.back() == '\\'))
    g_data_root.pop_back();
  if (g_save_root.empty()) g_save_root = g_data_root + "/user";
  std::printf("[fs  ] data root: %s\n", g_data_root.c_str());
  std::printf("[fs  ] save root: %s\n", g_save_root.c_str());
}

void fs_set_save_root(const std::string& path) {
  g_save_root = path;
  while (!g_save_root.empty() &&
         (g_save_root.back() == '/' || g_save_root.back() == '\\'))
    g_save_root.pop_back();
}

const ThunkEntry kFsTable[] = {
    {"open", &t_open},
    {"close", &t_close},
    {"read", &t_read},
    {"write", &t_write},
    {"lseek", &t_lseek},
    {"stat", &t_stat},
    {"fstat", &t_fstat},
    {"mkdir", &t_mkdir},
    {"fcntl", &t_ok},
    {"ioctl", &t_minus_one},
    {"remove", &t_remove},
    {"rename", &t_rename},
    {"getenv", &t_getenv},
    {"geteuid", &t_geteuid},
    {"realpath", &t_realpath},

    {"fopen", &t_fopen},
    {"fclose", &t_fclose},
    {"fread", &t_fread},
    {"fwrite", &t_fwrite},
    {"fseek", &t_fseek},
    {"ftell", &t_ftell},
    {"feof", &t_feof_logged},
    {"fflush", &t_fflush},
    {"fgetc", &t_fgetc},
    {"getc", &t_fgetc},
    {"ungetc", &t_ungetc},
    {"fgets", &t_fgets},
    {"fputs", &t_fputs},
    {"fputc", &t_fputc},
    {"putc", &t_fputc},
    {"setvbuf", &t_setvbuf},
};

const std::size_t kFsTableSize = sizeof(kFsTable) / sizeof(kFsTable[0]);

}  // namespace wb
