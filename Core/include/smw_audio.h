// Lock-free single-producer/single-consumer audio ring with a Catmull-Rom
// resampler and dynamic rate control. The emulator thread pushes 32 kHz
// stereo; the real-time audio callback pulls at the device rate. Small rate
// nudges (±0.5%) keep the buffer centred, absorbing the 60.0988 Hz vs display
// refresh mismatch without pops or audible pitch change.
#ifndef SMW_AUDIO_H
#define SMW_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SMWAudioRing SMWAudioRing;

SMWAudioRing *smw_audio_ring_create(uint32_t capacityFrames, double inputRate);
void smw_audio_ring_destroy(SMWAudioRing *ring);
void smw_audio_ring_set_target(SMWAudioRing *ring, uint32_t targetFrames);
void smw_audio_ring_clear(SMWAudioRing *ring);

// Producer side.
void smw_audio_ring_push(SMWAudioRing *ring, const int16_t *stereo, int frames);

// Consumer side (real-time safe: no locks, no allocation).
void smw_audio_ring_render(SMWAudioRing *ring, float *left, float *right, int frames, double outputRate,
                           float volume);
uint32_t smw_audio_ring_fill(const SMWAudioRing *ring);
// Number of times playback ran dry (diagnostics).
uint32_t smw_audio_ring_underruns(const SMWAudioRing *ring);

#ifdef __cplusplus
}
#endif

#endif
