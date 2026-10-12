// SMWCore — native SNES runtime that executes Super Mario World as assembled
// from the SMWDisX disassembly. Public C API consumed by the Swift front end.
#ifndef SMW_CORE_H
#define SMW_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMW_SCREEN_WIDTH 256
#define SMW_SCREEN_HEIGHT 224
#define SMW_AUDIO_RATE 32000

// Joypad bits, laid out exactly like the auto-joypad registers $4218/$4219.
enum {
    SMW_BTN_R = 1 << 4,
    SMW_BTN_L = 1 << 5,
    SMW_BTN_X = 1 << 6,
    SMW_BTN_A = 1 << 7,
    SMW_BTN_RIGHT = 1 << 8,
    SMW_BTN_LEFT = 1 << 9,
    SMW_BTN_DOWN = 1 << 10,
    SMW_BTN_UP = 1 << 11,
    SMW_BTN_START = 1 << 12,
    SMW_BTN_SELECT = 1 << 13,
    SMW_BTN_Y = 1 << 14,
    SMW_BTN_B = 1 << 15,
};

typedef struct SMWCore SMWCore;

// Creates a runtime for a LoROM image. The ROM bytes are copied.
SMWCore *smw_create(const uint8_t *rom, size_t length);
void smw_destroy(SMWCore *core);
void smw_reset(SMWCore *core, bool hard);

// Destination for video output: SMW_SCREEN_WIDTH x SMW_SCREEN_HEIGHT pixels,
// 32-bit BGRA8 (little endian 0xAARRGGBB). `pitch` is in pixels. The buffer can
// live in GPU-shared memory so the frame is presented without a copy.
void smw_set_framebuffer(SMWCore *core, uint32_t *pixels, int pitch);

void smw_set_buttons(SMWCore *core, int port, uint16_t buttons);

// Widescreen: renders `extra` additional columns on each side of the classic
// 256-pixel picture (0..128). The centre stays bit-exact; margins show level
// geometry the game already keeps in VRAM and are only opened during
// gameplay inside level bounds. The framebuffer pitch must be >= 512.
#define SMW_MAX_FRAME_WIDTH 512
void smw_set_widescreen(SMWCore *core, int extra);
int smw_frame_width(const SMWCore *core);

// Sub-pixel parallax scroll (display only): with 4, every line is written as
// smw_frame_width() * 4 samples and background layers that scroll at a
// different speed than layer 1 move smoothly instead of in uneven whole-pixel
// steps. The framebuffer pitch must then be >= SMW_FRAMEBUFFER_PITCH.
#define SMW_MAX_SUBPIXEL 4
#define SMW_FRAMEBUFFER_PITCH (SMW_MAX_FRAME_WIDTH * SMW_MAX_SUBPIXEL)
void smw_set_subpixel(SMWCore *core, int samplesPerPixel);
int smw_subpixel(const SMWCore *core);

// Emulates exactly one video frame (262 scanlines).
void smw_run_frame(SMWCore *core);

// Drains generated audio as interleaved stereo int16 at SMW_AUDIO_RATE.
// Returns the number of stereo frames written.
int smw_read_audio(SMWCore *core, int16_t *out, int max_frames);

// Direct views into console memory (for save files and game-aware features).
const uint8_t *smw_wram(const SMWCore *core);
uint8_t *smw_sram(SMWCore *core, size_t *length);
uint64_t smw_frame_count(const SMWCore *core);
// True if the game changed battery RAM since the last call (it just saved).
bool smw_sram_take_dirty(SMWCore *core);

// Sound commands the game sent to the audio CPU during the last frame, encoded
// as (port << 8) | id. Ports 0, 1 and 3 carry sound effects, port 2 music
// (see SMWDisX constants.asm). Used for game-aware haptics.
int smw_sound_event_count(const SMWCore *core);
uint16_t smw_sound_event(const SMWCore *core, int index);

// Remaster options. None of them touch game logic.
typedef enum {
    SMW_AUDIO_INTERP_GAUSSIAN = 0, // authentic S-DSP gaussian filter
    SMW_AUDIO_INTERP_CUBIC = 1,    // crisper cubic interpolation
} SMWAudioInterpolation;
void smw_set_audio_interpolation(SMWCore *core, SMWAudioInterpolation mode);

// Motion interpolation for 120 Hz+ displays. With capture enabled, every
// displayed frame records its per-line PPU state; smw_render_interpolated then
// draws the moment `t` (0..1) between the previous and the latest displayed
// frame. Returns false when there is no frame pair yet.
void smw_set_motion_capture(SMWCore *core, bool enabled);
bool smw_render_interpolated(SMWCore *core, uint32_t *pixels, int pitch, float t);

// Tooling (asset extraction, debugging): hide layers (bits 0-3 BG1-4, bit 4
// OBJ) and optionally output the backdrop as transparent pixels.
void smw_debug_layers(SMWCore *core, uint8_t hideMask, bool transparentBackdrop);
const uint16_t *smw_debug_cgram(const SMWCore *core); // 256 BGR555 colors
const uint8_t *smw_debug_oam(const SMWCore *core);    // 544 bytes of PPU OAM
// Pins a WRAM byte: every write stores `value` instead (value < 0 clears all).
void smw_debug_force_wram(SMWCore *core, uint32_t address, int value);

// Save states: a flat binary snapshot of the whole machine.
size_t smw_state_size(void);
void smw_save_state(const SMWCore *core, void *buffer);
bool smw_load_state(SMWCore *core, const void *buffer, size_t length);
// Checksum of the game RAM stored in a state (netplay desync detection).
uint32_t smw_state_wram_crc(const void *state);

// Compact XOR/RLE deltas between two states of equal length (used for rewind).
// Applying the delta of (a, b) to a turns it into b and vice versa.
size_t smw_delta_bound(size_t length);
size_t smw_delta_encode(const uint8_t *a, const uint8_t *b, size_t length, uint8_t *out);
void smw_delta_apply(uint8_t *state, const uint8_t *delta, size_t deltaLength);

#ifdef __cplusplus
}
#endif

#endif
