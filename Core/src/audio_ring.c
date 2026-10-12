#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "smw_audio.h"

struct SMWAudioRing {
    int16_t *data;
    uint32_t capacity; // power of two, in stereo frames
    _Atomic uint32_t write;
    _Atomic uint32_t read;
    _Atomic uint32_t target;
    double inputRate;
    // consumer state
    float hist[4][2];
    double frac;
    float gain; // ramps to avoid clicks on underrun
    bool starved;
    _Atomic uint32_t underruns;
};

SMWAudioRing *smw_audio_ring_create(uint32_t capacityFrames, double inputRate) {
    uint32_t cap = 1;
    while (cap < capacityFrames) cap <<= 1;
    SMWAudioRing *r = calloc(1, sizeof(*r));
    r->data = calloc(cap * 2, sizeof(int16_t));
    r->capacity = cap;
    r->inputRate = inputRate;
    r->starved = true; // fill to the target before the first sound
    atomic_store(&r->target, cap / 4);
    return r;
}

void smw_audio_ring_destroy(SMWAudioRing *r) {
    if (!r) return;
    free(r->data);
    free(r);
}

void smw_audio_ring_set_target(SMWAudioRing *r, uint32_t targetFrames) { atomic_store(&r->target, targetFrames); }

void smw_audio_ring_clear(SMWAudioRing *r) {
    atomic_store(&r->read, atomic_load(&r->write));
    r->starved = true;
}

uint32_t smw_audio_ring_underruns(const SMWAudioRing *r) { return atomic_load(&((SMWAudioRing *)r)->underruns); }

uint32_t smw_audio_ring_fill(const SMWAudioRing *r) {
    return atomic_load_explicit(&r->write, memory_order_acquire) -
           atomic_load_explicit(&r->read, memory_order_acquire);
}

void smw_audio_ring_push(SMWAudioRing *r, const int16_t *stereo, int frames) {
    uint32_t w = atomic_load_explicit(&r->write, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&r->read, memory_order_acquire);
    uint32_t space = r->capacity - (w - rd);
    if ((uint32_t)frames > space) frames = (int)space; // drop newest on overflow
    for (int i = 0; i < frames; i++) {
        uint32_t idx = (w + i) & (r->capacity - 1);
        r->data[idx * 2] = stereo[i * 2];
        r->data[idx * 2 + 1] = stereo[i * 2 + 1];
    }
    atomic_store_explicit(&r->write, w + frames, memory_order_release);
}

static inline float cubic(float p0, float p1, float p2, float p3, float t) {
    float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
    float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    float c = -0.5f * p0 + 0.5f * p2;
    return ((a * t + b) * t + c) * t + p1;
}

void smw_audio_ring_render(SMWAudioRing *r, float *left, float *right, int frames, double outputRate,
                           float volume) {
    uint32_t w = atomic_load_explicit(&r->write, memory_order_acquire);
    uint32_t rd = atomic_load_explicit(&r->read, memory_order_relaxed);
    uint32_t target = atomic_load_explicit(&r->target, memory_order_relaxed);
    double fill = (double)(w - rd);
    double adjust = 1.0 + 0.005 * ((fill - target) / (double)(target ? target : 1));
    if (adjust < 0.995) adjust = 0.995;
    if (adjust > 1.005) adjust = 1.005;
    double step = r->inputRate / outputRate * adjust;
    const float scale = 1.0f / 32768.0f;

    // Prebuffer: after starting or running dry, stay silent until the buffer
    // is back at its target instead of crackling on every small hiccup.
    if (r->starved && fill < target) {
        for (int i = 0; i < frames; i++) {
            r->gain *= 0.98f;
            float g = r->gain * volume;
            left[i] = r->hist[3][0] * g;
            right[i] = r->hist[3][1] * g;
        }
        return;
    }
    r->starved = false;

    for (int i = 0; i < frames; i++) {
        while (r->frac >= 1.0) {
            r->frac -= 1.0;
            memmove(r->hist[0], r->hist[1], sizeof(r->hist[0]) * 3);
            if (rd != w) {
                uint32_t idx = rd & (r->capacity - 1);
                r->hist[3][0] = r->data[idx * 2] * scale;
                r->hist[3][1] = r->data[idx * 2 + 1] * scale;
                rd++;
                if (r->gain < 1.0f) r->gain += 1.0f / 256.0f;
            } else {
                // Underrun: hold the last sample and fade out.
                if (!r->starved) atomic_fetch_add(&r->underruns, 1);
                r->starved = true;
                r->gain *= 0.98f;
            }
        }
        float t = (float)r->frac;
        float g = r->gain * volume;
        left[i] = cubic(r->hist[0][0], r->hist[1][0], r->hist[2][0], r->hist[3][0], t) * g;
        right[i] = cubic(r->hist[0][1], r->hist[1][1], r->hist[2][1], r->hist[3][1], t) * g;
        r->frac += step;
    }
    atomic_store_explicit(&r->read, rd, memory_order_release);
}
