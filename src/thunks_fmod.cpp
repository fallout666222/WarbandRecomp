// FMOD Ex, answered locally - and, now, actually heard.
//
// The engine links against libfmodex.so and drives it the way a 2014 mobile
// title does: create a system, load every sound named in sounds.txt at
// startup, then play channels as the game runs. None of that can be forwarded
// - FMOD Ex is a closed 32-bit ARM library and there is no build of it for
// either target - so the whole interface is answered here, and the samples go
// to the mixer in audio_mix.cpp.
//
// Answering completely still matters as much as it did when this was silent.
// A stub that returns an error, or that leaves an out parameter untouched,
// does not merely lose the sound: the engine's loader treats a failed
// createSound as a fatal resource error and stops. So every call reports
// success and every out parameter is filled with something the engine can
// believe, whether or not there is a device behind it.
//
// Objects are integer tokens rather than pointers, the same trick the EGL and
// mutex layers use: an FMOD::Sound* is 4 bytes in the guest and cannot hold a
// host pointer. Tokens are tagged so a stray one is recognisable in a log.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio.h"
#include "env.h"

namespace wb {
namespace {

constexpr u32 kFmodOk = 0;

// Tags keep the three object kinds apart and out of the way of guest
// addresses, so a token that turns up somewhere it should not is obvious.
constexpr u32 kSystemTag = 0x60000000;
constexpr u32 kSoundTag = 0x61000000;
constexpr u32 kChannelTag = 0x62000000;

// FMOD_MODE, the few bits the engine sets.
constexpr u32 kModeLoopOff = 0x00000001;
constexpr u32 kModeLoopNormal = 0x00000002;
constexpr u32 kMode3D = 0x00000010;
constexpr u32 kModeCreateStream = 0x00000080;

struct Sound {
  std::string name;
  u32 clip = 0;                 // the mixer's id, 0 if it would not load
  u32 mode = 0;
  float min_distance = 1.0f;
  float max_distance = 10000.0f;
};

struct Channel {
  u32 sound = 0;
  u32 voice = 0;                // the mixer's id
  u32 user_data = 0;
  u32 mode = 0;
  float volume = 1.0f;
};

std::mutex g_lock;
std::vector<Sound> g_sounds;
std::vector<Channel> g_channels;

// Where the listener is, so a 3D sound can be made quieter with distance.
// FMOD would do the panning as well; the engine on a phone was getting little
// more than this out of it.
float g_listener[3] = {0, 0, 0};

u32 new_sound(const std::string& name, u32 mode) {
  const bool streaming = (mode & kModeCreateStream) != 0;
  const u32 clip = audio_load(name, streaming);
  std::lock_guard<std::mutex> lock(g_lock);
  g_sounds.push_back({name, clip, mode});
  return kSoundTag | static_cast<u32>(g_sounds.size());
}

Sound* sound_for(u32 token) {
  std::lock_guard<std::mutex> lock(g_lock);
  if ((token & 0xFF000000) != kSoundTag) return nullptr;
  const u32 i = token & 0x00FFFFFF;
  if (i == 0 || i > g_sounds.size()) return nullptr;
  return &g_sounds[i - 1];
}

u32 new_channel(u32 sound, u32 voice) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_channels.push_back({sound, voice});
  return kChannelTag | static_cast<u32>(g_channels.size());
}

Channel* channel_for(u32 token) {
  std::lock_guard<std::mutex> lock(g_lock);
  if ((token & 0xFF000000) != kChannelTag) return nullptr;
  const u32 i = token & 0x00FFFFFF;
  if (i == 0 || i > g_channels.size()) return nullptr;
  return &g_channels[i - 1];
}

// The mixer's voice for a channel token, or 0.
u32 voice_of(u32 token) {
  const Channel* c = channel_for(token);
  return c ? c->voice : 0;
}

// ------------------------------------------------------------------ system
//
// FMOD_System_Create is the only free function; everything else is a C++
// method, so `this` arrives in r0 and the real arguments follow.
void t_System_Create(Env& e) {
  Env::Args a(e);
  const u32 out = a.next32();
  if (out) e.mem().write32(out, kSystemTag | 1);
  e.ret(kFmodOk);
}

// Methods that take no argument the engine reads back.
void t_ok(Env& e) { e.ret(kFmodOk); }

// System::init is where the device is opened. Failing to open one is not an
// error the engine is told about: it would stop loading, and a silent game
// is better than no game.
void t_System_init(Env& e) {
  Env::Args a(e);
  a.next32();                          // this
  const int maxchannels = static_cast<int>(a.next32());
  static bool started = false;
  if (!started) {
    started = true;
    audio_voice_limit(maxchannels);
    audio_start();
  }
  e.ret(kFmodOk);
}

void t_System_close(Env& e) {
  audio_stop();
  e.ret(kFmodOk);
}

void t_getNumDrivers(Env& e) {
  Env::Args a(e);
  a.next32();                          // this
  const u32 out = a.next32();
  // One driver: zero would make the engine conclude there is no audio device
  // and take a different path through its setup.
  if (out) e.mem().write32(out, 1);
  e.ret(kFmodOk);
}

void t_getDriverInfo(Env& e) {
  Env::Args a(e);
  a.next32();                          // this
  a.next32();                          // index
  const u32 name = a.next32();
  const u32 namelen = static_cast<u32>(a.next32());
  const u32 guid = a.next32();
  if (name && namelen) {
    const char* text = "warband_nx";
    const u32 n = namelen - 1 < 10 ? namelen - 1 : 10;
    e.mem().copy_in(name, text, n);
    e.mem().write8(name + n, 0);
  }
  if (guid) e.mem().zero(guid, 16);    // FMOD_GUID
  e.ret(kFmodOk);
}

void t_getDriverCaps(Env& e) {
  Env::Args a(e);
  a.next32();                          // this
  a.next32();                          // id
  const u32 caps = a.next32();
  const u32 rate = a.next32();
  const u32 mode = a.next32();
  if (caps) e.mem().write32(caps, 0);
  if (rate) e.mem().write32(rate, kAudioRate);
  if (mode) e.mem().write32(mode, 3);  // FMOD_SPEAKERMODE_STEREO
  e.ret(kFmodOk);
}

void t_getChannelsPlaying(Env& e) {
  Env::Args a(e);
  a.next32();
  const u32 out = a.next32();
  if (out) e.mem().write32(out, static_cast<u32>(audio_channels_playing()));
  e.ret(kFmodOk);
}

// FMOD_VECTOR is three floats. Only the position is read; the engine's
// velocities are for a doppler effect nothing here produces.
void read_vector(Env& e, u32 ptr, float out[3]) {
  if (!ptr) return;
  for (int i = 0; i < 3; ++i) {
    const u32 bits = e.mem().read32(ptr + static_cast<u32>(i) * 4);
    std::memcpy(&out[i], &bits, 4);
  }
}

void t_set3DListenerAttributes(Env& e) {
  Env::Args a(e);
  a.next32();                          // this
  a.next32();                          // listener index
  const u32 pos = a.next32();
  std::lock_guard<std::mutex> lock(g_lock);
  read_vector(e, pos, g_listener);
  e.ret(kFmodOk);
}

// createSound and createStream have the same shape: a name, flags, an
// optional extended-info block, and somewhere to put the result. Which of the
// two it was decides whether the file is decoded now or as it plays, so the
// bit is folded into the mode rather than passed separately.
void create_sound(Env& e, bool streaming) {
  Env::Args a(e);
  a.next32();                          // this
  const u32 name_ptr = a.next32();
  u32 mode = a.next32();
  a.next32();                          // FMOD_CREATESOUNDEXINFO*
  const u32 out = a.next32();
  if (streaming) mode |= kModeCreateStream;
  const std::string name = name_ptr ? e.mem().str(name_ptr) : std::string();
  const u32 token = new_sound(name, mode);
  if (out) e.mem().write32(out, token);
  static int shown = 0;
  if (++shown <= 5)
    std::printf("[fmod] %s %s -> 0x%08X\n",
                streaming ? "createStream" : "createSound", name.c_str(),
                token);
  e.ret(kFmodOk);
}

void t_createSound(Env& e) { create_sound(e, false); }
void t_createStream(Env& e) { create_sound(e, true); }

void t_playSound(Env& e) {
  Env::Args a(e);
  a.next32();                          // this
  a.next32();                          // FMOD_CHANNELINDEX
  const u32 sound = a.next32();
  const bool paused = a.next32() != 0;
  const u32 out = a.next32();

  u32 clip = 0, mode = 0;
  if (const Sound* s = sound_for(sound)) {
    clip = s->clip;
    mode = s->mode;
  }
  const bool loop = (mode & kModeLoopNormal) != 0;
  const u32 voice = clip ? audio_play(clip, paused, loop) : 0;
  const u32 token = new_channel(sound, voice);
  if (Channel* c = channel_for(token)) c->mode = mode;
  if (out) e.mem().write32(out, token);
  e.ret(kFmodOk);
}

void t_getChannel(Env& e) {
  Env::Args a(e);
  a.next32();                          // this
  a.next32();                          // index
  const u32 out = a.next32();
  if (out) e.mem().write32(out, new_channel(0, 0));
  e.ret(kFmodOk);
}

// ------------------------------------------------------------------- sound
void t_Sound_getDefaults(Env& e) {
  Env::Args a(e);
  a.next32();                          // this
  const u32 freq = a.next32(), vol = a.next32(), pan = a.next32();
  const u32 pri = a.next32();
  const float f = static_cast<float>(kAudioRate), v = 1.0f, p = 0.0f;
  if (freq) e.mem().copy_in(freq, &f, 4);
  if (vol) e.mem().copy_in(vol, &v, 4);
  if (pan) e.mem().copy_in(pan, &p, 4);
  if (pri) e.mem().write32(pri, 128);
  e.ret(kFmodOk);
}

void t_Sound_setDefaults(Env& e) { e.ret(kFmodOk); }

void t_Sound_setMode(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const u32 mode = a.next32();
  if (Sound* s = sound_for(self)) s->mode = (s->mode & kModeCreateStream) | mode;
  e.ret(kFmodOk);
}

void t_Sound_set3DMinMaxDistance(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const float lo = a.nextf(), hi = a.nextf();
  if (Sound* s = sound_for(self)) {
    s->min_distance = lo;
    s->max_distance = hi;
  }
  e.ret(kFmodOk);
}

// The engine polls this after an asynchronous load and will wait forever if
// it never reads READY.
void t_Sound_getOpenState(Env& e) {
  Env::Args a(e);
  a.next32();                          // this
  const u32 state = a.next32();
  const u32 percent = a.next32();
  const u32 starving = a.next32();
  const u32 busy = a.next32();
  if (state) e.mem().write32(state, 0);      // FMOD_OPENSTATE_READY
  if (percent) e.mem().write32(percent, 100);
  if (starving) e.mem().write8(starving, 0);
  if (busy) e.mem().write8(busy, 0);
  e.ret(kFmodOk);
}

void t_Sound_getLength(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const u32 out = a.next32();
  const Sound* s = sound_for(self);
  const u32 ms = s && s->clip ? audio_length_ms(s->clip) : 1000;
  if (out) e.mem().write32(out, ms ? ms : 1000);
  e.ret(kFmodOk);
}

// ----------------------------------------------------------------- channel
void t_Channel_stop(Env& e) {
  Env::Args a(e);
  audio_channel_stop(voice_of(a.next32()));
  e.ret(kFmodOk);
}

void t_Channel_setPaused(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const bool paused = a.next32() != 0;
  audio_channel_pause(voice_of(self), paused);
  e.ret(kFmodOk);
}

void t_Channel_isPlaying(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const u32 out = a.next32();
  const bool playing = audio_channel_playing(voice_of(self));
  if (out) e.mem().write8(out, playing ? 1 : 0);
  e.ret(kFmodOk);
}

void t_Channel_getPosition(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const u32 out = a.next32();
  if (out) e.mem().write32(out, audio_channel_position_ms(voice_of(self)));
  e.ret(kFmodOk);
}

void t_Channel_setPosition(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const u32 ms = a.next32();
  audio_channel_set_position_ms(voice_of(self), ms);
  e.ret(kFmodOk);
}

void t_Channel_setFrequency(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const float hz = a.nextf();
  audio_channel_frequency(voice_of(self), hz);
  e.ret(kFmodOk);
}

void t_Channel_getIndex(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const u32 out = a.next32();
  if (out) e.mem().write32(out, static_cast<u32>(self & 0x00FFFFFF));
  e.ret(kFmodOk);
}

void t_Channel_getVolume(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const u32 out = a.next32();
  const Channel* c = channel_for(self);
  const float v = c ? c->volume : 1.0f;
  if (out) e.mem().copy_in(out, &v, 4);
  e.ret(kFmodOk);
}

void t_Channel_setVolume(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const float v = a.nextf();
  if (Channel* c = channel_for(self)) c->volume = v;
  audio_channel_volume(voice_of(self), v);
  e.ret(kFmodOk);
}

void t_Channel_getMode(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const u32 out = a.next32();
  const Channel* c = channel_for(self);
  if (out) e.mem().write32(out, c ? c->mode : 0);
  e.ret(kFmodOk);
}

void t_Channel_setMode(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const u32 mode = a.next32();
  if (Channel* c = channel_for(self)) c->mode = mode;
  audio_channel_loop(voice_of(self), (mode & kModeLoopNormal) != 0);
  e.ret(kFmodOk);
}

// Distance attenuation, from the sound's own min and max. Inside the minimum
// it is at full volume, past the maximum it is inaudible, and between the two
// it falls off with distance the way FMOD's linear-square rolloff does.
void t_Channel_set3DAttributes(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const u32 pos = a.next32();
  float p[3] = {0, 0, 0};
  read_vector(e, pos, p);

  float lo = 1.0f, hi = 10000.0f;
  const Channel* c = channel_for(self);
  if (c) {
    if (const Sound* s = sound_for(c->sound)) {
      lo = s->min_distance;
      hi = s->max_distance;
    }
  }
  float listener[3];
  {
    std::lock_guard<std::mutex> lock(g_lock);
    listener[0] = g_listener[0];
    listener[1] = g_listener[1];
    listener[2] = g_listener[2];
  }
  const float dx = p[0] - listener[0], dy = p[1] - listener[1],
              dz = p[2] - listener[2];
  const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
  float gain = 1.0f;
  if (hi > lo && distance > lo) {
    const float t = (distance - lo) / (hi - lo);
    gain = t >= 1.0f ? 0.0f : (1.0f - t) * (1.0f - t);
  }
  audio_channel_attenuation(voice_of(self), gain);
  e.ret(kFmodOk);
}

void t_Channel_getUserData(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const u32 out = a.next32();
  const Channel* c = channel_for(self);
  if (out) e.mem().write32(out, c ? c->user_data : 0);
  e.ret(kFmodOk);
}

void t_Channel_setUserData(Env& e) {
  Env::Args a(e);
  const u32 self = a.next32();
  const u32 data = a.next32();
  if (Channel* c = channel_for(self)) c->user_data = data;
  e.ret(kFmodOk);
}

}  // namespace

const ThunkEntry kFmodTable[] = {
    {"FMOD_System_Create", &t_System_Create},

    // System
    {"_ZN4FMOD6System4initEijPv", &t_System_init},
    {"_ZN4FMOD6System5closeEv", &t_System_close},
    {"_ZN4FMOD6System6updateEv", &t_ok},
    {"_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE", &t_ok},
    {"_ZN4FMOD6System13set3DSettingsEfff", &t_ok},
    {"_ZN4FMOD6System14setSpeakerModeE16FMOD_SPEAKERMODE", &t_ok},
    {"_ZN4FMOD6System16setDSPBufferSizeEji", &t_ok},
    {"_ZN4FMOD6System17setSoftwareFormatEi17FMOD_SOUND_FORMATii18FMOD_DSP_RESAMPLER",
     &t_ok},
    {"_ZN4FMOD6System19setHardwareChannelsEi", &t_ok},
    {"_ZN4FMOD6System23set3DListenerAttributesEiPK11FMOD_VECTORS3_S3_S3_",
     &t_set3DListenerAttributes},
    {"_ZN4FMOD6System13getNumDriversEPi", &t_getNumDrivers},
    {"_ZN4FMOD6System13getDriverInfoEiPciP9FMOD_GUID", &t_getDriverInfo},
    {"_ZN4FMOD6System13getDriverCapsEiPjPiP16FMOD_SPEAKERMODE", &t_getDriverCaps},
    {"_ZN4FMOD6System18getChannelsPlayingEPi", &t_getChannelsPlaying},
    {"_ZN4FMOD6System10getChannelEiPPNS_7ChannelE", &t_getChannel},
    {"_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE",
     &t_createSound},
    {"_ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE",
     &t_createStream},
    {"_ZN4FMOD6System9playSoundE17FMOD_CHANNELINDEXPNS_5SoundEbPPNS_7ChannelE",
     &t_playSound},

    // Sound
    {"_ZN4FMOD5Sound7releaseEv", &t_ok},
    {"_ZN4FMOD5Sound7setModeEj", &t_Sound_setMode},
    {"_ZN4FMOD5Sound11setDefaultsEfffi", &t_Sound_setDefaults},
    {"_ZN4FMOD5Sound11getDefaultsEPfS1_S1_Pi", &t_Sound_getDefaults},
    {"_ZN4FMOD5Sound19set3DMinMaxDistanceEff", &t_Sound_set3DMinMaxDistance},
    {"_ZN4FMOD5Sound12getOpenStateEP14FMOD_OPENSTATEPjPbS4_", &t_Sound_getOpenState},
    {"_ZN4FMOD5Sound9getLengthEPjj", &t_Sound_getLength},

    // Channel
    {"_ZN4FMOD7Channel4stopEv", &t_Channel_stop},
    {"_ZN4FMOD7Channel9setPausedEb", &t_Channel_setPaused},
    {"_ZN4FMOD7Channel11setPriorityEi", &t_ok},
    {"_ZN4FMOD7Channel11setPositionEjj", &t_Channel_setPosition},
    {"_ZN4FMOD7Channel12setFrequencyEf", &t_Channel_setFrequency},
    {"_ZN4FMOD7Channel15set3DAttributesEPK11FMOD_VECTORS3_",
     &t_Channel_set3DAttributes},
    {"_ZN4FMOD7Channel11setCallbackEPF11FMOD_RESULTP12FMOD_CHANNEL25FMOD_CHANNEL_CALLBACKTYPEPvS5_E",
     &t_ok},
    {"_ZN4FMOD7Channel9isPlayingEPb", &t_Channel_isPlaying},
    {"_ZN4FMOD7Channel11getPositionEPjj", &t_Channel_getPosition},
    {"_ZN4FMOD7Channel8getIndexEPi", &t_Channel_getIndex},
    {"_ZN4FMOD7Channel9getVolumeEPf", &t_Channel_getVolume},
    {"_ZN4FMOD7Channel9setVolumeEf", &t_Channel_setVolume},
    {"_ZN4FMOD7Channel7getModeEPj", &t_Channel_getMode},
    {"_ZN4FMOD7Channel7setModeEj", &t_Channel_setMode},
    {"_ZN4FMOD7Channel11getUserDataEPPv", &t_Channel_getUserData},
    {"_ZN4FMOD7Channel11setUserDataEPv", &t_Channel_setUserData},
};

const std::size_t kFmodTableSize = sizeof(kFmodTable) / sizeof(kFmodTable[0]);

}  // namespace wb
