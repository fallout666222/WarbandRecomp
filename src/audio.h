// Sound.
//
// The engine drives FMOD Ex, which is a closed 32-bit ARM library with no
// build for either target, so nothing can be forwarded: the API is answered
// on our side and the samples are mixed here. thunks_fmod.cpp is the shape of
// the engine's side of that boundary; this is the shape of ours.
//
// Two paths, because the assets are two kinds. The five hundred effects in
// Sounds/ are short and get decoded once and kept; the sixty tracks in music/
// are two hundred and forty megabytes between them and are decoded a block at
// a time while they play. FMOD makes the same distinction - createSound
// against createStream - so the engine has already told us which is which.
//
// The mixer is platform-independent and the output device is not: audio_mix
// fills a buffer, and the platform file hands that buffer to waveOut or to
// audren. Same split as the GL layer.
#pragma once

#include <string>

#include "guest.h"

namespace wb {

// What the mixer produces, and what the platform must accept: 16-bit stereo
// interleaved. The rate is fixed and everything is resampled to it, because
// the assets are a mixture of 44.1 and 22.05 kHz and the device only gets one
// rate.
// 48 kHz because that is the only rate Horizon's audio output offers, and one
// mixer rate for both platforms is one fewer thing that can differ. The assets
// are 44.1 and 22.05 kHz and were being resampled per voice either way.
constexpr int kAudioRate = 48000;
constexpr int kAudioChannels = 2;

// Opens the output device and starts the thread that feeds it. Returns false
// when there is no device, which is not fatal: the mixer keeps working and
// nobody hears it.
bool audio_start();
void audio_stop();

// Fills `frames` stereo frames. Called by the platform's feeding thread, and
// by nothing else. Always writes the whole buffer, silence included.
void audio_render(short* out, int frames);

// ------------------------------------------------------------- the mixer
//
// Sounds and channels are small integers rather than pointers, the same way
// the EGL and JNI layers work: an FMOD::Sound* is four bytes in the guest and
// cannot hold a host pointer.

// Loads a sound. `path` is the guest's own path, mapped through the file
// layer here rather than by the caller. Streaming sounds keep the file open
// and decode as they play. Returns 0 if it could not be read.
u32 audio_load(const std::string& path, bool streaming);

// How long the sound is, in milliseconds. The engine divides by this, so a
// sound that failed to load still answers something non-zero.
u32 audio_length_ms(u32 sound);

// Starts a sound on a new channel. Returns 0 if the sound is unknown.
u32 audio_play(u32 sound, bool paused, bool loop);

void audio_channel_stop(u32 channel);
void audio_channel_pause(u32 channel, bool paused);
bool audio_channel_playing(u32 channel);
void audio_channel_volume(u32 channel, float volume);
float audio_channel_get_volume(u32 channel);
// FMOD sets a channel's frequency in Hz; the ratio against the sound's own
// rate is the pitch.
void audio_channel_frequency(u32 channel, float hz);
void audio_channel_loop(u32 channel, bool loop);
u32 audio_channel_position_ms(u32 channel);
void audio_channel_set_position_ms(u32 channel, u32 ms);
// Distance attenuation for a 3D sound, worked out by the caller from the
// listener and the source. One number is enough: the engine's own panning is
// not something FMOD was doing much of on a phone.
void audio_channel_attenuation(u32 channel, float gain);

// How many channels are audible. The engine asks FMOD this and will not start
// new sounds if the answer is implausible.
int audio_channels_playing();

// Master volume, 0..1, for the platform layer to mute on focus loss.
void audio_master_volume(float volume);

// How many voices may sound at once. FMOD takes this as the first argument to
// System::init and enforces it by stealing the least important channel; the
// engine relies on that to keep a battle from turning into noise, and without
// it sixty voices arrive at the mixer and it clips solid.
void audio_voice_limit(int voices);

// Writes everything the mixer produces to a WAV file as well as to the
// speakers. There is no other way to check from here that a sound came out
// right - the device is not something a log can describe - and it is how a
// silent run gets told apart from a run nobody was listening to.
void audio_record(const char* path);
void audio_record_stop();

}  // namespace wb
