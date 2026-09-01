// The mixer: decoding, channels, and one buffer of samples at a time.
//
// Nothing here knows what the output device is. audio_render is called by the
// platform's feeding thread with however many frames it wants, and everything
// else is called from guest threads through the FMOD thunks - so one lock
// covers the channel list, and decoding for a stream happens inside it. That
// is affordable because the feeding thread runs a couple of buffers ahead;
// what it must never do is block on the guest, and it does not.

#include "audio.h"

#include "env.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis.c"

namespace wb {

namespace {

constexpr u32 kSoundBase = 0x70000000;
constexpr u32 kChannelBase = 0x71000000;

// A loaded sound. Effects hold their samples; a stream holds the file's bytes
// and every channel playing it decodes its own way through them.
struct Clip {
  std::string path;
  bool streaming = false;
  int rate = kAudioRate;
  int channels = 1;
  std::vector<short> pcm;          // interleaved, for effects
  std::vector<unsigned char> file; // the encoded bytes, for streams
  u32 length_ms = 0;
  bool decoded = false;
  bool broken = false;
};

struct Voice {
  u32 sound = 0;
  bool active = false;
  bool paused = false;
  bool loop = false;
  unsigned long long age = 0;      // when it started, for stealing
  double position = 0;             // in source frames
  double step = 1.0;               // source frames per output frame
  float volume = 1.0f;
  float attenuation = 1.0f;
  stb_vorbis* stream = nullptr;    // streaming only
  std::vector<short> block;        // what the stream has decoded but not used
  std::size_t block_at = 0;        // frames of `block` already consumed
  bool ended = false;
};

// The recorder. A WAV header with a length nobody knows yet, then samples;
// the length is written back when the file is closed, and left wrong if the
// process is killed - which still leaves a file most players will open.
std::FILE* g_record = nullptr;
u32 g_recorded_bytes = 0;

void write_le(std::FILE* f, u32 value, int bytes) {
  for (int i = 0; i < bytes; ++i)
    std::fputc(static_cast<int>((value >> (8 * i)) & 0xFF), f);
}

std::mutex g_lock;
std::vector<Clip> g_clips;
std::vector<Voice> g_voices;
float g_master = 1.0f;
int g_voice_limit = 32;
unsigned long long g_started = 0;      // for choosing which voice to steal

Clip* clip_for(u32 id) {
  if (id < kSoundBase) return nullptr;
  const u32 i = id - kSoundBase;
  return i < g_clips.size() ? &g_clips[i] : nullptr;
}

Voice* voice_for(u32 id) {
  if (id < kChannelBase) return nullptr;
  const u32 i = id - kChannelBase;
  return i < g_voices.size() ? &g_voices[i] : nullptr;
}

std::vector<unsigned char> read_file(const std::string& host) {
  std::vector<unsigned char> out;
  std::FILE* f = std::fopen(host.c_str(), "rb");
  if (!f) return out;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    out.resize(static_cast<std::size_t>(size));
    if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
  }
  std::fclose(f);
  return out;
}

// Microsoft's RIFF, enough of it for the fourteen files that use it. Anything
// unexpected is left to the vorbis decoder to reject.
bool decode_wav(const std::vector<unsigned char>& in, std::vector<short>& pcm,
                int& rate, int& channels) {
  if (in.size() < 44 || std::memcmp(in.data(), "RIFF", 4) != 0 ||
      std::memcmp(in.data() + 8, "WAVE", 4) != 0)
    return false;
  std::size_t at = 12;
  int bits = 16;
  bool have_format = false;
  while (at + 8 <= in.size()) {
    char id[5] = {};
    std::memcpy(id, in.data() + at, 4);
    u32 size;
    std::memcpy(&size, in.data() + at + 4, 4);
    const std::size_t body = at + 8;
    if (std::memcmp(id, "fmt ", 4) == 0 && body + 16 <= in.size()) {
      unsigned short ch, bps;
      u32 sr;
      std::memcpy(&ch, in.data() + body + 2, 2);
      std::memcpy(&sr, in.data() + body + 4, 4);
      std::memcpy(&bps, in.data() + body + 14, 2);
      channels = ch ? ch : 1;
      rate = sr ? static_cast<int>(sr) : kAudioRate;
      bits = bps;
      have_format = true;
    } else if (std::memcmp(id, "data", 4) == 0 && have_format) {
      const std::size_t bytes =
          std::min<std::size_t>(size, in.size() - body);
      if (bits == 16) {
        pcm.resize(bytes / 2);
        std::memcpy(pcm.data(), in.data() + body, pcm.size() * 2);
      } else if (bits == 8) {
        pcm.resize(bytes);
        for (std::size_t i = 0; i < bytes; ++i)
          pcm[i] = static_cast<short>((in[body + i] - 128) << 8);
      } else {
        return false;
      }
      return true;
    }
    at = body + size + (size & 1);
  }
  return false;
}

// Decodes a clip's samples the first time something plays it. Doing this at
// load would put five hundred files through the decoder before the main menu
// appears; doing it here spreads the cost over the first time each sound is
// heard, and most are never heard at all.
void ensure_decoded(Clip& c) {
  if (c.decoded || c.broken) return;
  c.decoded = true;
  if (c.file.empty()) {
    c.broken = true;
    return;
  }
  int rate = kAudioRate, channels = 1;
  if (decode_wav(c.file, c.pcm, rate, channels)) {
    c.rate = rate;
    c.channels = channels;
  } else {
    short* samples = nullptr;
    const int frames = stb_vorbis_decode_memory(
        c.file.data(), static_cast<int>(c.file.size()), &channels, &rate,
        &samples);
    if (frames <= 0 || !samples) {
      c.broken = true;
      std::printf("[snd ] cannot decode %s\n", c.path.c_str());
      return;
    }
    c.pcm.assign(samples, samples + static_cast<std::size_t>(frames) * channels);
    std::free(samples);
    c.rate = rate;
    c.channels = channels ? channels : 1;
  }
  if (c.channels < 1) c.channels = 1;
  const std::size_t frames = c.pcm.size() / static_cast<std::size_t>(c.channels);
  c.length_ms = static_cast<u32>(frames * 1000ull /
                                 static_cast<unsigned>(c.rate ? c.rate : kAudioRate));
  c.file.clear();
  c.file.shrink_to_fit();
}

// One sample from an effect, linearly interpolated, as a float pair.
void sample_clip(const Clip& c, double at, float& left, float& right) {
  const std::size_t frames = c.pcm.size() / static_cast<std::size_t>(c.channels);
  if (frames == 0) {
    left = right = 0;
    return;
  }
  const std::size_t i = static_cast<std::size_t>(at);
  const std::size_t j = i + 1 < frames ? i + 1 : i;
  const float t = static_cast<float>(at - static_cast<double>(i));
  const short* a = &c.pcm[i * static_cast<std::size_t>(c.channels)];
  const short* b = &c.pcm[j * static_cast<std::size_t>(c.channels)];
  const float l0 = a[0], l1 = b[0];
  left = l0 + (l1 - l0) * t;
  if (c.channels > 1) {
    const float r0 = a[1], r1 = b[1];
    right = r0 + (r1 - r0) * t;
  } else {
    right = left;
  }
}

// Keeps a streaming voice's decoded block ahead of where it is reading.
// Returns false when the file has run out and looping did not restart it.
bool stream_fill(Voice& v, const Clip& c, std::size_t frames_wanted) {
  const int ch = c.channels ? c.channels : 1;
  while ((v.block.size() / static_cast<std::size_t>(ch)) - v.block_at <
         frames_wanted) {
    short buffer[4096];
    const int got = stb_vorbis_get_samples_short_interleaved(
        v.stream, ch, buffer, static_cast<int>(sizeof(buffer) / sizeof(short)));
    if (got <= 0) {
      if (!v.loop) return false;
      stb_vorbis_seek_start(v.stream);
      continue;
    }
    // Drop what has already been played rather than growing forever.
    if (v.block_at > 0) {
      v.block.erase(v.block.begin(),
                    v.block.begin() +
                        static_cast<long>(v.block_at * static_cast<std::size_t>(ch)));
      v.block_at = 0;
    }
    v.block.insert(v.block.end(), buffer,
                   buffer + static_cast<std::size_t>(got) * static_cast<std::size_t>(ch));
  }
  return true;
}

}  // namespace

u32 audio_load(const std::string& path, bool streaming) {
  const std::string host = fs_host_path(path);
  std::vector<unsigned char> bytes = read_file(host);
  if (bytes.empty()) {
    static int shown = 0;
    if (++shown <= 8)
      std::printf("[snd ] %s is not there (%s)\n", path.c_str(), host.c_str());
    return 0;
  }

  std::lock_guard<std::mutex> lock(g_lock);
  g_clips.emplace_back();
  Clip& c = g_clips.back();
  c.path = path;
  c.streaming = streaming;
  if (streaming) {
    // A stream is not decoded now and not decoded whole ever; the length has
    // to come out of the header, which stb_vorbis will read on its own.
    int error = 0;
    stb_vorbis* probe = stb_vorbis_open_memory(
        bytes.data(), static_cast<int>(bytes.size()), &error, nullptr);
    if (probe) {
      const stb_vorbis_info info = stb_vorbis_get_info(probe);
      c.rate = static_cast<int>(info.sample_rate);
      c.channels = info.channels ? info.channels : 1;
      const unsigned frames = stb_vorbis_stream_length_in_samples(probe);
      c.length_ms = static_cast<u32>(static_cast<unsigned long long>(frames) *
                                     1000ull / (c.rate ? c.rate : kAudioRate));
      stb_vorbis_close(probe);
    } else {
      c.broken = true;
    }
    c.file = std::move(bytes);
    c.decoded = true;             // nothing more to do up front
  } else {
    c.file = std::move(bytes);
  }
  return kSoundBase + static_cast<u32>(g_clips.size() - 1);
}

u32 audio_length_ms(u32 sound) {
  std::lock_guard<std::mutex> lock(g_lock);
  Clip* c = clip_for(sound);
  if (!c) return 1000;
  if (!c->streaming) ensure_decoded(*c);
  return c->length_ms ? c->length_ms : 1000;
}

u32 audio_play(u32 sound, bool paused, bool loop) {
  std::lock_guard<std::mutex> lock(g_lock);
  Clip* c = clip_for(sound);
  if (!c) return 0;
  if (!c->streaming) {
    ensure_decoded(*c);
    if (c->broken) return 0;
  }

  // Reuse a finished slot before making another; the engine starts thousands
  // of sounds over a session and never frees a channel.
  std::size_t slot = g_voices.size();
  int live = 0;
  for (std::size_t i = 0; i < g_voices.size(); ++i) {
    if (g_voices[i].active) {
      ++live;
      continue;
    }
    if (slot == g_voices.size()) slot = i;
  }
  // At the limit, take the oldest one-shot. A looping sound is ambience the
  // engine expects to still be there, so it is left alone; if everything is a
  // loop the new sound is dropped rather than cutting one of them off.
  if (slot == g_voices.size() && live >= g_voice_limit) {
    unsigned long long oldest = ~0ull;
    for (std::size_t i = 0; i < g_voices.size(); ++i) {
      if (!g_voices[i].active || g_voices[i].loop) continue;
      if (g_voices[i].age < oldest) {
        oldest = g_voices[i].age;
        slot = i;
      }
    }
    if (slot == g_voices.size()) return 0;
  }
  if (slot == g_voices.size()) g_voices.emplace_back();
  Voice& v = g_voices[slot];

  if (v.stream) {
    stb_vorbis_close(v.stream);
    v.stream = nullptr;
  }
  v = Voice();
  v.sound = sound;
  v.age = ++g_started;
  v.active = true;
  v.paused = paused;
  v.loop = loop;
  v.step = static_cast<double>(c->rate ? c->rate : kAudioRate) / kAudioRate;
  if (c->streaming) {
    int error = 0;
    v.stream = stb_vorbis_open_memory(c->file.data(),
                                      static_cast<int>(c->file.size()), &error,
                                      nullptr);
    if (!v.stream) {
      v.active = false;
      return 0;
    }
  }
  return kChannelBase + static_cast<u32>(slot);
}

void audio_channel_stop(u32 channel) {
  std::lock_guard<std::mutex> lock(g_lock);
  Voice* v = voice_for(channel);
  if (!v) return;
  if (v->stream) {
    stb_vorbis_close(v->stream);
    v->stream = nullptr;
  }
  v->active = false;
}

void audio_channel_pause(u32 channel, bool paused) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (Voice* v = voice_for(channel)) v->paused = paused;
}

bool audio_channel_playing(u32 channel) {
  std::lock_guard<std::mutex> lock(g_lock);
  Voice* v = voice_for(channel);
  return v && v->active && !v->ended;
}

void audio_channel_volume(u32 channel, float volume) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (Voice* v = voice_for(channel))
    v->volume = volume < 0 ? 0 : (volume > 4 ? 4 : volume);
}

float audio_channel_get_volume(u32 channel) {
  std::lock_guard<std::mutex> lock(g_lock);
  Voice* v = voice_for(channel);
  return v ? v->volume : 1.0f;
}

void audio_channel_frequency(u32 channel, float hz) {
  std::lock_guard<std::mutex> lock(g_lock);
  Voice* v = voice_for(channel);
  if (!v) return;
  Clip* c = clip_for(v->sound);
  if (!c || hz <= 0) return;
  v->step = static_cast<double>(hz) / kAudioRate;
}

void audio_channel_loop(u32 channel, bool loop) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (Voice* v = voice_for(channel)) v->loop = loop;
}

u32 audio_channel_position_ms(u32 channel) {
  std::lock_guard<std::mutex> lock(g_lock);
  Voice* v = voice_for(channel);
  if (!v) return 0;
  Clip* c = clip_for(v->sound);
  const int rate = c && c->rate ? c->rate : kAudioRate;
  return static_cast<u32>(v->position * 1000.0 / rate);
}

void audio_channel_set_position_ms(u32 channel, u32 ms) {
  std::lock_guard<std::mutex> lock(g_lock);
  Voice* v = voice_for(channel);
  if (!v) return;
  Clip* c = clip_for(v->sound);
  const int rate = c && c->rate ? c->rate : kAudioRate;
  v->position = static_cast<double>(ms) * rate / 1000.0;
  if (v->stream) {
    stb_vorbis_seek(v->stream, static_cast<unsigned>(v->position));
    v->block.clear();
    v->block_at = 0;
    v->position = 0;
  }
}

void audio_channel_attenuation(u32 channel, float gain) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (Voice* v = voice_for(channel))
    v->attenuation = gain < 0 ? 0 : (gain > 1 ? 1 : gain);
}

int audio_channels_playing() {
  std::lock_guard<std::mutex> lock(g_lock);
  int n = 0;
  for (const Voice& v : g_voices)
    if (v.active && !v.paused) ++n;
  return n;
}

void audio_master_volume(float volume) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_master = volume < 0 ? 0 : (volume > 1 ? 1 : volume);
}

void audio_voice_limit(int voices) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (voices > 0) g_voice_limit = voices > 128 ? 128 : voices;
  std::printf("[snd ] at most %d voices at once\n", g_voice_limit);
}

void audio_record(const char* path) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (g_record) return;
  g_record = std::fopen(path, "wb");
  if (!g_record) {
    std::printf("[snd ] cannot write %s\n", path);
    return;
  }
  std::fwrite("RIFF", 1, 4, g_record);
  write_le(g_record, 0, 4);                    // size, filled in on close
  std::fwrite("WAVEfmt ", 1, 8, g_record);
  write_le(g_record, 16, 4);                   // fmt chunk size
  write_le(g_record, 1, 2);                    // PCM
  write_le(g_record, kAudioChannels, 2);
  write_le(g_record, kAudioRate, 4);
  write_le(g_record, kAudioRate * kAudioChannels * 2, 4);
  write_le(g_record, kAudioChannels * 2, 2);   // block align
  write_le(g_record, 16, 2);                   // bits
  std::fwrite("data", 1, 4, g_record);
  write_le(g_record, 0, 4);                    // data size, filled in on close
  g_recorded_bytes = 0;
  std::printf("[snd ] recording the mix to %s\n", path);
}

void audio_record_stop() {
  std::lock_guard<std::mutex> lock(g_lock);
  if (!g_record) return;
  std::fseek(g_record, 4, SEEK_SET);
  write_le(g_record, 36 + g_recorded_bytes, 4);
  std::fseek(g_record, 40, SEEK_SET);
  write_le(g_record, g_recorded_bytes, 4);
  std::fclose(g_record);
  g_record = nullptr;
  std::printf("[snd ] %u bytes of sound written\n", g_recorded_bytes);
}

void audio_render(short* out, int frames) {
  static std::vector<float> mix;
  mix.assign(static_cast<std::size_t>(frames) * 2, 0.0f);

  {
    std::lock_guard<std::mutex> lock(g_lock);
    const float master = g_master;
    for (Voice& v : g_voices) {
      if (!v.active || v.paused) continue;
      Clip* c = clip_for(v.sound);
      if (!c) {
        v.active = false;
        continue;
      }
      const float gain = v.volume * v.attenuation * master;

      if (v.stream) {
        const int ch = c->channels ? c->channels : 1;
        // How far through the source this buffer will walk.
        const std::size_t need =
            static_cast<std::size_t>(v.step * frames) + 2;
        if (!stream_fill(v, *c, need)) {
          v.ended = true;
          v.active = false;
          stb_vorbis_close(v.stream);
          v.stream = nullptr;
          continue;
        }
        double at = 0;
        const std::size_t have = v.block.size() / static_cast<std::size_t>(ch);
        for (int i = 0; i < frames; ++i) {
          const std::size_t k = v.block_at + static_cast<std::size_t>(at);
          if (k >= have) break;
          const short* s = &v.block[k * static_cast<std::size_t>(ch)];
          const float l = s[0], r = ch > 1 ? s[1] : s[0];
          mix[static_cast<std::size_t>(i) * 2] += l * gain;
          mix[static_cast<std::size_t>(i) * 2 + 1] += r * gain;
          at += v.step;
        }
        v.block_at += static_cast<std::size_t>(at);
        v.position += at;
        continue;
      }

      const std::size_t total =
          c->pcm.size() / static_cast<std::size_t>(c->channels ? c->channels : 1);
      if (total == 0) {
        v.active = false;
        continue;
      }
      for (int i = 0; i < frames; ++i) {
        if (v.position >= static_cast<double>(total)) {
          if (!v.loop) {
            v.ended = true;
            v.active = false;
            break;
          }
          v.position -= static_cast<double>(total);
        }
        float l = 0, r = 0;
        sample_clip(*c, v.position, l, r);
        mix[static_cast<std::size_t>(i) * 2] += l * gain;
        mix[static_cast<std::size_t>(i) * 2 + 1] += r * gain;
        v.position += v.step;
      }
    }
  }

  // Thirty voices of full-scale samples add up to far more than a sample can
  // hold, and hard clipping sounds like tearing paper. This is a limiter with
  // a memory: the gain follows the loudest recent peak, comes down fast and
  // goes back up slowly, so a sudden crash of noise ducks the mix instead of
  // shredding it.
  static float gain = 1.0f;
  float loudest_f = 0;
  for (int i = 0; i < frames * 2; ++i) {
    const float m = std::fabs(mix[static_cast<std::size_t>(i)]);
    if (m > loudest_f) loudest_f = m;
  }
  const float wanted = loudest_f > 32000.0f ? 32000.0f / loudest_f : 1.0f;
  if (wanted < gain) gain = wanted;                 // duck at once
  else gain += (wanted - gain) * 0.02f;             // recover gently

  short peak = 0;
  for (int i = 0; i < frames * 2; ++i) {
    float s = mix[static_cast<std::size_t>(i)] * gain;
    if (s > 32767.0f) s = 32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    out[i] = static_cast<short>(s);
    const short mag = out[i] < 0 ? static_cast<short>(-out[i]) : out[i];
    if (mag > peak) peak = mag;
  }

  // Once a second, how loud it got. Silence that should not be silent is the
  // one audio bug a log can catch on its own.
  static int since = 0;
  static short loudest = 0;
  if (peak > loudest) loudest = peak;
  since += frames;
  if (since >= kAudioRate) {
    since = 0;
    if (loudest > 0)
      std::printf("[snd ] peak %d of 32767 across %d voices\n", loudest,
                  audio_channels_playing());
    loudest = 0;
  }

  std::lock_guard<std::mutex> lock(g_lock);
  if (g_record) {
    std::fwrite(out, 2, static_cast<std::size_t>(frames) * 2, g_record);
    g_recorded_bytes += static_cast<u32>(frames) * 4;
  }
}

}  // namespace wb
