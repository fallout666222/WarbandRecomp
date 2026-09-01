// pthread: real host threads, mutexes and condition variables.
//
// The engine hands the game to threads android_main spawns, so these cannot
// stay stubs. The awkward part is that a bionic pthread_mutex_t is four bytes
// and a host std::mutex is not, so the guest object holds a token indexing a
// host-side table - the same handle-width problem every opaque host object
// has here, just the first one that had to be solved.
//
// Tokens start at 1 so a zeroed guest object reads as "not initialised yet",
// which is what PTHREAD_MUTEX_INITIALIZER looks like in memory. Static
// initialisers are therefore handled by creating the object on first use.

#include <chrono>
#include <condition_variable>
#include <thread>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "env.h"

namespace wb {
namespace {

template <typename T>
class HandleTable {
 public:
  u32 create() {
    std::lock_guard<std::mutex> lock(lock_);
    items_.push_back(std::make_unique<T>());
    return static_cast<u32>(items_.size());   // 1-based
  }
  T* get(u32 token) {
    std::lock_guard<std::mutex> lock(lock_);
    if (token == 0 || token > items_.size()) return nullptr;
    return items_[token - 1].get();
  }
  std::size_t size() {
    std::lock_guard<std::mutex> lock(lock_);
    return items_.size();
  }

 private:
  std::mutex lock_;
  std::vector<std::unique_ptr<T>> items_;
};

// A pthread mutex, not a C++ one.
//
// std::recursive_mutex is the obvious choice and the wrong one: releasing it
// from a thread that did not take it is undefined, and it simply does not
// release. The engine hands locks across threads - one thread takes a lock,
// another signals completion and drops it - which is ordinary practice with
// raw pthread mutexes and deadlocks instantly against a C++ one.
//
// So ownership is tracked by hand: recursive for the owner, and release is
// permitted from anywhere.
class GuestMutex {
 public:
  void lock() {
    const std::thread::id me = std::this_thread::get_id();
    std::unique_lock<std::mutex> g(state_);
    if (held_ && owner_ == me) {
      ++depth_;
      return;
    }
    free_.wait(g, [&] { return !held_; });
    held_ = true;
    owner_ = me;
    depth_ = 1;
  }

  bool try_lock() {
    const std::thread::id me = std::this_thread::get_id();
    std::lock_guard<std::mutex> g(state_);
    if (held_ && owner_ != me) return false;
    if (held_) {
      ++depth_;
      return true;
    }
    held_ = true;
    owner_ = me;
    depth_ = 1;
    return true;
  }

  void unlock() {
    std::lock_guard<std::mutex> g(state_);
    if (!held_) return;             // releasing an unheld lock is a no-op
    if (depth_ > 1) {
      --depth_;
      return;
    }
    held_ = false;
    depth_ = 0;
    owner_ = std::thread::id();
    free_.notify_one();
  }

 private:
  std::mutex state_;
  std::condition_variable free_;
  bool held_ = false;
  int depth_ = 0;
  std::thread::id owner_;
};

struct GuestCond {
  std::condition_variable_any cv;
};

HandleTable<GuestMutex> g_mutexes;
HandleTable<GuestCond> g_conds;

// Creating on first use covers both pthread_mutex_init and the static
// PTHREAD_MUTEX_INITIALIZER, which is simply a zeroed object.
GuestMutex* mutex_for(Env& e, u32 addr) {
  if (!addr) return nullptr;
  u32 token = e.mem().read32(addr);
  if (!token) {
    token = g_mutexes.create();
    e.mem().write32(addr, token);
  }
  return g_mutexes.get(token);
}

GuestCond* cond_for(Env& e, u32 addr) {
  if (!addr) return nullptr;
  u32 token = e.mem().read32(addr);
  if (!token) {
    token = g_conds.create();
    e.mem().write32(addr, token);
  }
  return g_conds.get(token);
}

void who(Env& e, const char* what);

// ---------------------------------------------------------------- threads
void t_pthread_create(Env& e) {
  Env::Args a(e);
  u32 out = a.next32();
  a.next32();                       // attr: detach state is not interesting
  u32 entry = a.next32();
  u32 arg = a.next32();
  u32 id = e.spawn(entry, arg);
  if (!id) {
    e.ret(11);                      // EAGAIN
    return;
  }
  if (out) e.mem().write32(out, id);
  e.ret(0);
}

// A distinct, non-zero id per guest thread. neosmart and the engine both
// compare these, so handing every thread the same value would make them
// think they are each other.
void t_pthread_self(Env& e) { e.ret(static_cast<u32>(current_thread_id())); }

void t_pthread_join(Env& e) {
  Env::Args a(e);
  a.next32();
  u32 retval = a.next32();
  // Threads here outlive the call; joining individually would need per-thread
  // futures. Nothing in the engine's startup path depends on it yet.
  if (retval) e.mem().write32(retval, 0);
  e.ret(0);
}

void t_zero(Env& e) { e.ret(0); }

// ----------------------------------------------------------------- mutexes
void t_mutex_init(Env& e) {
  Env::Args a(e);
  u32 m = a.next32();
  if (m) e.mem().write32(m, 0);     // drop any old token, recreate on first use
  mutex_for(e, m);
  e.ret(0);
}

void t_mutex_lock(Env& e) {
  Env::Args a(e);
  GuestMutex* m = mutex_for(e, a.next32());
  if (m) {
    // Take it without blocking first. If that fails the lock is contended,
    // and naming the caller is the difference between "a thread is stuck"
    // and knowing which lock and which function.
    if (!m->try_lock()) {
      who(e, "mutex_lock(wait)");
      m->lock();
    }
  }
  e.ret(0);
}

void t_mutex_trylock(Env& e) {
  Env::Args a(e);
  GuestMutex* m = mutex_for(e, a.next32());
  e.ret(m && m->try_lock() ? 0u : 16u);   // EBUSY
}

void t_mutex_unlock(Env& e) {
  Env::Args a(e);
  GuestMutex* m = mutex_for(e, a.next32());
  if (m) m->unlock();
  e.ret(0);
}

// Names the engine code on either side of a wait. Which function blocks and
// which one wakes it is the whole question when threads park.
void who(Env& e, const char* what) {
  static std::mutex lock;
  static std::unordered_map<std::string, int> seen;
  const u32 lr = e.jit()->Regs()[14];
  std::string site = e.loader().symbolize(lr);
  std::lock_guard<std::mutex> g(lock);
  std::string key = std::string(what) + " " + site;
  if (seen[key]++ < 3)
    std::printf("[cond] t%d %-14s from %s\n", current_thread_id(),
                what, site.c_str());
}

// ------------------------------------------------------------- conditions
void t_cond_init(Env& e) {
  Env::Args a(e);
  u32 c = a.next32();
  if (c) e.mem().write32(c, 0);
  cond_for(e, c);
  e.ret(0);
}

void t_cond_wait(Env& e) {
  who(e, "cond_wait");
  Env::Args a(e);
  GuestCond* c = cond_for(e, a.next32());
  GuestMutex* m = mutex_for(e, a.next32());
  if (c && m) c->cv.wait(*m);
  e.ret(0);
}

// pthread_cond_timedwait takes an *absolute* deadline, and its return value
// carries the whole meaning: 0 says "you were signalled", ETIMEDOUT says "the
// deadline passed". Returning 0 unconditionally tells every caller its
// predicate came true, which turns `while (!ready) wait()` into a spin and
// makes an event that never fires look like one that fires constantly.
constexpr u32 kETimedOut = 110;   // Linux ETIMEDOUT, which is what bionic uses

void t_cond_timedwait(Env& e) {
  who(e, "cond_timedwait");
  Env::Args a(e);
  GuestCond* c = cond_for(e, a.next32());
  GuestMutex* m = mutex_for(e, a.next32());
  u32 ts = a.next32();
  if (!c || !m) {
    e.ret(kETimedOut);
    return;
  }

  // struct timespec on 32-bit bionic: tv_sec then tv_nsec, both 32-bit. The
  // guest built it from clock_gettime, so it is on the same base as
  // guest_monotonic_ns().
  std::int64_t deadline = 0;
  if (ts) {
    std::int64_t sec = static_cast<std::int32_t>(e.mem().read32(ts));
    std::int64_t nsec = static_cast<std::int32_t>(e.mem().read32(ts + 4));
    deadline = sec * 1'000'000'000LL + nsec;
  }
  const std::int64_t now = guest_monotonic_ns();
  if (ts && deadline <= now) {
    e.ret(kETimedOut);
    return;
  }

  // Cap a single sleep so a wildly distant deadline - neosmart asks for about
  // 49 days when it means "forever" - still lets the thread notice a stop.
  std::int64_t remaining = ts ? deadline - now : 50'000'000LL;
  const std::int64_t kSlice = 1'000'000LL;   // 1 ms
  const bool capped = remaining > kSlice;
  if (capped) remaining = kSlice;

  const auto status = c->cv.wait_for(*m, std::chrono::nanoseconds(remaining));
  if (status == std::cv_status::no_timeout) {
    e.ret(0);
    return;
  }
  // A capped slice expiring is not the caller's deadline expiring; report a
  // spurious wakeup so the caller re-checks its predicate and waits again.
  e.ret(capped ? 0u : kETimedOut);
}

void t_cond_signal(Env& e) {
  who(e, "cond_signal");
  Env::Args a(e);
  GuestCond* c = cond_for(e, a.next32());
  if (c) c->cv.notify_one();
  e.ret(0);
}

void t_cond_broadcast(Env& e) {
  who(e, "cond_broadcast");
  Env::Args a(e);
  GuestCond* c = cond_for(e, a.next32());
  if (c) c->cv.notify_all();
  e.ret(0);
}

// -------------------------------------------------------- thread-local keys
std::mutex g_key_lock;
u32 g_next_key = 1;
// One slot per (key, host thread). Guest TLS is per guest thread, and guest
// threads map one-to-one onto host threads here.
thread_local std::unordered_map<u32, u32> t_tls;

void t_key_create(Env& e) {
  Env::Args a(e);
  u32 out = a.next32();
  a.next32();                       // destructor: never run
  u32 key;
  {
    std::lock_guard<std::mutex> lock(g_key_lock);
    key = g_next_key++;
  }
  if (out) e.mem().write32(out, key);
  e.ret(0);
}

void t_getspecific(Env& e) {
  Env::Args a(e);
  auto it = t_tls.find(a.next32());
  e.ret(it == t_tls.end() ? 0 : it->second);
}

void t_setspecific(Env& e) {
  Env::Args a(e);
  u32 key = a.next32();
  t_tls[key] = a.next32();
  e.ret(0);
}

}  // namespace

const ThunkEntry kThreadTable[] = {
    {"pthread_create", &t_pthread_create},
    {"pthread_join", &t_pthread_join},
    {"pthread_self", &t_pthread_self},
    {"pthread_detach", &t_zero},
    {"pthread_kill", &t_zero},
    {"pthread_attr_init", &t_zero},
    {"pthread_attr_setdetachstate", &t_zero},
    {"pthread_mutexattr_init", &t_zero},
    {"pthread_mutexattr_settype", &t_zero},

    {"pthread_mutex_init", &t_mutex_init},
    {"pthread_mutex_destroy", &t_zero},
    {"pthread_mutex_lock", &t_mutex_lock},
    {"pthread_mutex_trylock", &t_mutex_trylock},
    {"pthread_mutex_unlock", &t_mutex_unlock},

    {"pthread_cond_init", &t_cond_init},
    {"pthread_cond_destroy", &t_zero},
    {"pthread_cond_wait", &t_cond_wait},
    {"pthread_cond_timedwait", &t_cond_timedwait},
    {"pthread_cond_signal", &t_cond_signal},
    {"pthread_cond_broadcast", &t_cond_broadcast},

    {"pthread_key_create", &t_key_create},
    {"pthread_key_delete", &t_zero},
    {"pthread_getspecific", &t_getspecific},
    {"pthread_setspecific", &t_setspecific},
};

const std::size_t kThreadTableSize =
    sizeof(kThreadTable) / sizeof(kThreadTable[0]);

}  // namespace wb
