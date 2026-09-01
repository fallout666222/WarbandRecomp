// Sound out of the PC, through waveOut.
//
// waveOut rather than WASAPI or XAudio2 on purpose: it ships with Windows,
// needs no COM, no SDK headers beyond mmsystem.h and no initialisation
// ceremony, and the whole of it is four calls. This is the platform half of
// the audio layer - the mixer next door does the work, and on the Switch the
// same mixer feeds audren instead.
//
// A ring of small buffers rather than one big one: latency is the buffer
// size, and a game wants a footstep to land when the foot does. Four buffers
// of 1024 frames is about ninety milliseconds of slack with twenty-three
// milliseconds of lag.

#if defined(_WIN32) && !defined(WB_SWITCH)

#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "audio.h"

namespace wb {
namespace {

constexpr int kBufferFrames = 1024;
constexpr int kBuffers = 4;

HWAVEOUT g_device = nullptr;
WAVEHDR g_headers[kBuffers] = {};
std::vector<short> g_blocks[kBuffers];
HANDLE g_free = nullptr;           // set by waveOut when a buffer comes back
std::thread g_feeder;
std::atomic<bool> g_running{false};

void CALLBACK on_done(HWAVEOUT, UINT message, DWORD_PTR, DWORD_PTR, DWORD_PTR) {
  if (message == WOM_DONE && g_free) SetEvent(g_free);
}

// Fills whichever buffers have come back and hands them straight to the
// device again. The mixer is called from here and nowhere else.
void feed() {
  while (g_running.load()) {
    bool idle = true;
    for (int i = 0; i < kBuffers; ++i) {
      if (!(g_headers[i].dwFlags & WHDR_DONE)) continue;
      waveOutUnprepareHeader(g_device, &g_headers[i], sizeof(WAVEHDR));
      audio_render(g_blocks[i].data(), kBufferFrames);
      g_headers[i].dwFlags = 0;
      g_headers[i].lpData = reinterpret_cast<LPSTR>(g_blocks[i].data());
      g_headers[i].dwBufferLength =
          static_cast<DWORD>(g_blocks[i].size() * sizeof(short));
      waveOutPrepareHeader(g_device, &g_headers[i], sizeof(WAVEHDR));
      waveOutWrite(g_device, &g_headers[i], sizeof(WAVEHDR));
      idle = false;
    }
    // Wait for the device rather than spinning; the timeout is only there so
    // shutdown does not have to interrupt the wait.
    if (idle) WaitForSingleObject(g_free, 20);
  }
}

}  // namespace

bool audio_start() {
  if (g_device) return true;

  WAVEFORMATEX format = {};
  format.wFormatTag = WAVE_FORMAT_PCM;
  format.nChannels = kAudioChannels;
  format.nSamplesPerSec = kAudioRate;
  format.wBitsPerSample = 16;
  format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
  format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

  g_free = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  const MMRESULT r =
      waveOutOpen(&g_device, WAVE_MAPPER, &format,
                  reinterpret_cast<DWORD_PTR>(&on_done), 0, CALLBACK_FUNCTION);
  if (r != MMSYSERR_NOERROR) {
    std::printf("[snd ] no output device (waveOutOpen returned %u)\n", r);
    g_device = nullptr;
    return false;
  }

  for (int i = 0; i < kBuffers; ++i) {
    g_blocks[i].assign(kBufferFrames * kAudioChannels, 0);
    g_headers[i] = {};
    g_headers[i].dwFlags = WHDR_DONE;      // so the feeder claims it at once
  }
  g_running.store(true);
  g_feeder = std::thread(feed);
  std::printf("[snd ] %d Hz stereo, %d frames a buffer\n", kAudioRate,
              kBufferFrames);
  return true;
}

void audio_stop() {
  if (!g_device) return;
  g_running.store(false);
  if (g_free) SetEvent(g_free);
  if (g_feeder.joinable()) g_feeder.join();
  waveOutReset(g_device);
  for (int i = 0; i < kBuffers; ++i)
    waveOutUnprepareHeader(g_device, &g_headers[i], sizeof(WAVEHDR));
  waveOutClose(g_device);
  g_device = nullptr;
  if (g_free) {
    CloseHandle(g_free);
    g_free = nullptr;
  }
}

}  // namespace wb

#endif  // _WIN32 && !WB_SWITCH
