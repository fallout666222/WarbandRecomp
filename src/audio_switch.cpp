// Sound out of the Switch, through audout.
//
// audout rather than audren: audren is the renderer with a mix graph, voices,
// effects and memory pools, and every one of those is something the mixer next
// door already does. What is wanted here is a hole to push finished stereo
// samples into, and that is exactly what audout is - initialise, start, hand
// it buffers, take them back when they have played.
//
// It is fixed at 48 kHz, two channels, signed 16-bit, which is why the mixer
// runs at that rate on both platforms. The assets are 44.1 and 22.05 kHz and
// were being resampled per voice regardless.
//
// The buffers have to be page-aligned and their size rounded up to a page;
// that is a kernel requirement, not a preference, and audout rejects anything
// else. `data_size` stays the real number of bytes.

#if defined(WB_SWITCH)

#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "audio.h"

namespace wb {
namespace {

// About twenty-one milliseconds each. Four of them is enough slack for the
// recompiler to stall briefly without the sound tearing, and little enough
// that a footstep still lands with the foot.
constexpr int kBufferFrames = 1024;
constexpr int kBuffers = 4;

AudioOutBuffer g_buffers[kBuffers];
void* g_memory[kBuffers] = {};
std::thread g_feeder;
std::atomic<bool> g_running{false};
bool g_open = false;

// Fills whatever has finished playing and hands it straight back. The mixer is
// called from here and nowhere else.
void feed() {
  while (g_running.load(std::memory_order_relaxed)) {
    AudioOutBuffer* done = nullptr;
    u32 count = 0;
    // A timeout rather than a wait forever, so that shutdown does not need to
    // interrupt a blocked thread.
    if (R_FAILED(audoutWaitPlayFinish(&done, &count, 100'000'000ULL)))
      continue;
    if (!done) continue;
    audio_render(static_cast<short*>(done->buffer), kBufferFrames);
    audoutAppendAudioOutBuffer(done);
  }
}

}  // namespace

bool audio_start() {
  if (g_open) return true;

  Result rc = audoutInitialize();
  if (R_FAILED(rc)) {
    std::printf("[snd ] audoutInitialize failed: 2%03d-%04d\n", R_MODULE(rc),
                R_DESCRIPTION(rc));
    return false;
  }
  rc = audoutStartAudioOut();
  if (R_FAILED(rc)) {
    std::printf("[snd ] audoutStartAudioOut failed: 2%03d-%04d\n", R_MODULE(rc),
                R_DESCRIPTION(rc));
    audoutExit();
    return false;
  }

  // The device decides the format; say so if it is not what the mixer was
  // built for, because the result would be the right sound at the wrong pitch
  // and that is hard to recognise as a bug.
  const u32 rate = audoutGetSampleRate();
  const u32 channels = audoutGetChannelCount();
  if (rate != static_cast<u32>(kAudioRate) ||
      channels != static_cast<u32>(kAudioChannels))
    std::printf("[snd ] the device wants %u Hz and %u channels, the mixer "
                "produces %d and %d - everything will play at the wrong "
                "speed\n", rate, channels, kAudioRate, kAudioChannels);

  const std::size_t bytes =
      static_cast<std::size_t>(kBufferFrames) * kAudioChannels * 2;
  const std::size_t rounded = (bytes + 0xFFF) & ~std::size_t(0xFFF);
  for (int i = 0; i < kBuffers; ++i) {
    g_memory[i] = std::aligned_alloc(0x1000, rounded);
    if (!g_memory[i]) {
      std::printf("[snd ] no room for the output buffers\n");
      audoutStopAudioOut();
      audoutExit();
      return false;
    }
    std::memset(g_memory[i], 0, rounded);
    g_buffers[i].next = nullptr;
    g_buffers[i].buffer = g_memory[i];
    g_buffers[i].buffer_size = rounded;
    g_buffers[i].data_size = bytes;
    g_buffers[i].data_offset = 0;
    // Queued silent: the feeding thread takes them back as they play and
    // fills them from then on.
    audoutAppendAudioOutBuffer(&g_buffers[i]);
  }

  g_running.store(true);
  g_feeder = std::thread(feed);
  g_open = true;
  std::printf("[snd ] audout: %u Hz, %u channels, %d frames a buffer\n", rate,
              channels, kBufferFrames);
  return true;
}

void audio_stop() {
  if (!g_open) return;
  g_running.store(false);
  // Joinable, never detached: Horizon has no working pthread_detach, and
  // destroying a joinable std::thread is a straight call to terminate.
  if (g_feeder.joinable()) g_feeder.join();
  audoutStopAudioOut();
  audoutExit();
  for (int i = 0; i < kBuffers; ++i) {
    std::free(g_memory[i]);
    g_memory[i] = nullptr;
  }
  g_open = false;
}

}  // namespace wb

#endif  // WB_SWITCH
