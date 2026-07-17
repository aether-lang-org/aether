/* std.audio — playback substrate (v1).
 *
 * The device/decode layer is C by necessity — the runtime-stays-C hard
 * line (docs/cross-references/audio.md §6): a real audio backend pulls
 * samples on a realtime thread that must not allocate or block, so no
 * Aether-emitted code can run there. This header is the FFI surface the
 * `std.audio` module binds; the implementation carries a self-contained
 * WAV decoder and a NULL (silent, deterministic) backend that is the
 * default and the CI/test path. A real device backend (ALSA/CoreAudio/
 * WASAPI, or vendored miniaudio) slots in behind the same surface without
 * changing the Aether API. */
#ifndef AETHER_AUDIO_H
#define AETHER_AUDIO_H

#include <stdint.h>

/* Engine lifecycle. open() picks a backend (null unless a real one is
 * compiled in and available); returns 1 on success, 0 on failure. */
int  aether_audio_open(void);
void aether_audio_close(void);
/* 1 if the active backend is the null (silent) backend — lets tests and
 * headless callers assert deterministic behaviour. */
int  aether_audio_is_null_backend(void);

/* Decode `length` bytes of `data` (a WAV byte string) into a new sound
 * handle. Returns the handle (opaque pointer) or NULL on failure; the
 * failure reason is available via aether_audio_last_error(). The caller
 * owns the handle until aether_audio_unload(). */
void* aether_audio_load_wav(const char* data, int length);
const char* aether_audio_last_error(void);
void  aether_audio_unload(void* sound);

/* Transport. play() starts/resumes; pause() halts keeping position;
 * stop() halts and rewinds to 0. Each returns 1 on success, 0 on a null
 * handle. */
int aether_audio_play(void* sound);
int aether_audio_pause(void* sound);
int aether_audio_stop(void* sound);
int aether_audio_is_playing(void* sound);

/* Volume in [0.0, 1.0] (clamped). */
int    aether_audio_set_volume(void* sound, double v);
double aether_audio_get_volume(void* sound);

/* Position / duration in FRAMES and in NANOSECONDS (for Aether's
 * Duration). seek() clamps to [0, total_frames]. */
int64_t aether_audio_position_ns(void* sound);
int64_t aether_audio_duration_ns(void* sound);
int     aether_audio_seek_ns(void* sound, int64_t ns);

int aether_audio_channels(void* sound);
int aether_audio_sample_rate(void* sound);

#endif /* AETHER_AUDIO_H */
