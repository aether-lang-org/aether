/* std.audio playback substrate — see aether_audio.h.
 *
 * v1 carries a self-contained WAV (PCM / IEEE-float) decoder and a NULL
 * backend: playback advances the play cursor against the monotonic clock
 * rather than driving a device, so every transport/seek/position behaviour
 * is exercised deterministically and headlessly (no sound card, valgrind-
 * clean). This is the CI path and the default. A real device backend
 * (ALSA/CoreAudio/WASAPI or vendored miniaudio) replaces the cursor-advance
 * with a data callback without touching the FFI surface or the Aether API.
 *
 * All buffers go through the caps allocator so resource accounting balances
 * exactly like every other std/ C module. */
#include "aether_audio.h"
#include "../../runtime/aether_resource_caps.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Monotonic nanoseconds — the null backend's clock. */
static int64_t audio_now_ns(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/* ---- engine ------------------------------------------------------------ */

static int g_audio_open = 0;
static const char* g_audio_err = "";

int aether_audio_open(void) {
    g_audio_open = 1;
    g_audio_err = "";
    return 1;
}

void aether_audio_close(void) {
    g_audio_open = 0;
}

int aether_audio_is_null_backend(void) {
    /* v1 ships only the null backend. */
    return 1;
}

const char* aether_audio_last_error(void) {
    return g_audio_err;
}

/* ---- sound handle ------------------------------------------------------ */

typedef struct {
    float*  samples;        /* interleaved f32, channels*frames */
    int64_t frames;         /* total frames */
    int     channels;
    int     sample_rate;

    /* transport (null backend) */
    int     playing;
    int64_t cursor_frames;  /* play position when last paused/seeked */
    int64_t play_start_ns;  /* monotonic clock at the last play() */
    double  volume;         /* 0.0 .. 1.0 */
} AudioSound;

/* Current play cursor in frames, advancing over wall-clock while playing,
 * clamped to [0, frames]; auto-stops at end. */
static int64_t sound_cursor(AudioSound* s) {
    if (!s->playing) return s->cursor_frames;
    int64_t elapsed_ns = audio_now_ns() - s->play_start_ns;
    if (elapsed_ns < 0) elapsed_ns = 0;
    int64_t advanced = (int64_t)((double)elapsed_ns * (double)s->sample_rate
                                 / 1000000000.0);
    int64_t pos = s->cursor_frames + advanced;
    if (pos >= s->frames) {
        /* reached the end — settle the state so is_playing() reports done */
        s->playing = 0;
        s->cursor_frames = s->frames;
        return s->frames;
    }
    return pos;
}

void aether_audio_unload(void* sound) {
    if (!sound) return;
    AudioSound* s = (AudioSound*)sound;
    if (s->samples) {
        aether_caps_free(s->samples,
                         (size_t)s->frames * (size_t)s->channels * sizeof(float));
    }
    aether_caps_free(s, sizeof(AudioSound));
}

/* ---- transport --------------------------------------------------------- */

int aether_audio_play(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    if (s->playing) return 1;
    /* resume from the settled cursor (rewound to 0 if already at end) */
    if (s->cursor_frames >= s->frames) s->cursor_frames = 0;
    s->play_start_ns = audio_now_ns();
    s->playing = 1;
    return 1;
}

int aether_audio_pause(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    if (s->playing) {
        s->cursor_frames = sound_cursor(s);
        s->playing = 0;
    }
    return 1;
}

int aether_audio_stop(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    s->playing = 0;
    s->cursor_frames = 0;
    return 1;
}

int aether_audio_is_playing(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    (void)sound_cursor(s);   /* settle auto-stop-at-end */
    return s->playing;
}

/* ---- volume ------------------------------------------------------------ */

int aether_audio_set_volume(void* sound, double v) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    s->volume = v;
    return 1;
}

double aether_audio_get_volume(void* sound) {
    if (!sound) return 0.0;
    return ((AudioSound*)sound)->volume;
}

/* ---- position / duration ---------------------------------------------- */

static int64_t frames_to_ns(int64_t frames, int sample_rate) {
    if (sample_rate <= 0) return 0;
    return (int64_t)((double)frames * 1000000000.0 / (double)sample_rate);
}

int64_t aether_audio_position_ns(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    return frames_to_ns(sound_cursor(s), s->sample_rate);
}

int64_t aether_audio_duration_ns(void* sound) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    return frames_to_ns(s->frames, s->sample_rate);
}

int aether_audio_seek_ns(void* sound, int64_t ns) {
    if (!sound) return 0;
    AudioSound* s = (AudioSound*)sound;
    if (ns < 0) ns = 0;
    int64_t frame = (int64_t)((double)ns * (double)s->sample_rate / 1000000000.0);
    if (frame > s->frames) frame = s->frames;
    s->cursor_frames = frame;
    /* keep playing from the new spot */
    if (s->playing) s->play_start_ns = audio_now_ns();
    return 1;
}

int aether_audio_channels(void* sound) {
    if (!sound) return 0;
    return ((AudioSound*)sound)->channels;
}

int aether_audio_sample_rate(void* sound) {
    if (!sound) return 0;
    return ((AudioSound*)sound)->sample_rate;
}

/* ---- WAV decoder ------------------------------------------------------- */

static uint32_t rd_u32le(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16le(const unsigned char* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* Decode a canonical RIFF/WAVE file: PCM (8/16/24/32-bit int) or IEEE
 * float32. Converts every sample to interleaved f32 in [-1, 1]. Sets
 * g_audio_err and returns NULL on any malformed / unsupported input. */
void* aether_audio_load_wav(const char* data, int length) {
    const unsigned char* p = (const unsigned char*)data;
    if (!p || length < 44) { g_audio_err = "audio: not a WAV (too short)"; return NULL; }
    if (memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) {
        g_audio_err = "audio: not a RIFF/WAVE file"; return NULL;
    }

    int      fmt_found = 0;
    uint16_t audio_format = 0, channels = 0, bits = 0;
    uint32_t sample_rate = 0;
    const unsigned char* pcm = NULL;
    uint32_t pcm_len = 0;

    /* walk chunks starting at offset 12 */
    int off = 12;
    while (off + 8 <= length) {
        const unsigned char* ch = p + off;
        uint32_t csize = rd_u32le(ch + 4);
        const unsigned char* body = ch + 8;
        if ((int)(off + 8 + csize) > length) {
            /* clamp a truncated final chunk to what we actually have */
            csize = (uint32_t)(length - (off + 8));
        }
        if (memcmp(ch, "fmt ", 4) == 0 && csize >= 16) {
            audio_format = rd_u16le(body + 0);
            channels     = rd_u16le(body + 2);
            sample_rate  = rd_u32le(body + 4);
            bits         = rd_u16le(body + 14);
            fmt_found = 1;
        } else if (memcmp(ch, "data", 4) == 0) {
            pcm = body;
            pcm_len = csize;
        }
        off += 8 + (int)csize;
        if (csize & 1) off += 1;   /* chunks are word-aligned */
    }

    if (!fmt_found) { g_audio_err = "audio: WAV missing fmt chunk"; return NULL; }
    if (!pcm)       { g_audio_err = "audio: WAV missing data chunk"; return NULL; }
    if (channels < 1) { g_audio_err = "audio: WAV has no channels"; return NULL; }
    /* 1 = PCM int, 3 = IEEE float */
    if (audio_format != 1 && audio_format != 3) {
        g_audio_err = "audio: unsupported WAV format (only PCM / float)"; return NULL;
    }
    int bytes_per_sample = bits / 8;
    if (bytes_per_sample < 1) { g_audio_err = "audio: WAV bad bit depth"; return NULL; }

    int64_t total_samples = pcm_len / bytes_per_sample;        /* across all channels */
    int64_t frames = total_samples / channels;
    if (frames < 0) frames = 0;

    float* out = NULL;
    if (frames > 0) {
        out = (float*)aether_caps_malloc((size_t)total_samples * sizeof(float));
        if (!out) { g_audio_err = "audio: out of memory"; return NULL; }
    }

    for (int64_t i = 0; i < total_samples; i++) {
        const unsigned char* sp = pcm + i * bytes_per_sample;
        float v = 0.0f;
        if (audio_format == 3 && bits == 32) {
            float f; memcpy(&f, sp, 4); v = f;
        } else if (bits == 16) {
            int16_t s16 = (int16_t)rd_u16le(sp);
            v = (float)s16 / 32768.0f;
        } else if (bits == 8) {
            /* 8-bit WAV is UNSIGNED, centred at 128 */
            v = ((float)sp[0] - 128.0f) / 128.0f;
        } else if (bits == 24) {
            int32_t s24 = (int32_t)((uint32_t)sp[0] | ((uint32_t)sp[1] << 8)
                                    | ((uint32_t)sp[2] << 16));
            if (s24 & 0x800000) s24 |= (int32_t)0xFF000000; /* sign-extend */
            v = (float)s24 / 8388608.0f;
        } else if (bits == 32) {
            int32_t s32 = (int32_t)rd_u32le(sp);
            v = (float)s32 / 2147483648.0f;
        } else {
            aether_caps_free(out, (size_t)total_samples * sizeof(float));
            g_audio_err = "audio: unsupported WAV bit depth"; return NULL;
        }
        out[i] = v;
    }

    AudioSound* s = (AudioSound*)aether_caps_malloc(sizeof(AudioSound));
    if (!s) {
        aether_caps_free(out, (size_t)total_samples * sizeof(float));
        g_audio_err = "audio: out of memory"; return NULL;
    }
    s->samples       = out;
    s->frames        = frames;
    s->channels      = channels;
    s->sample_rate   = (int)sample_rate;
    s->playing       = 0;
    s->cursor_frames = 0;
    s->play_start_ns = 0;
    s->volume        = 1.0;
    g_audio_err = "";
    return s;
}
