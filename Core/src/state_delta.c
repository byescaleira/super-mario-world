// XOR + run-length deltas between consecutive save states. Two states a frame
// apart differ in a few KiB, so a rewind history costs ~1-3% of raw snapshots.
// Format: sequence of [tag][varint len] where tag 0 = unchanged run, tag 1 =
// changed run followed by `len` XOR bytes. The delta is its own inverse.
#include <stddef.h>
#include <stdint.h>

#include "smw_core.h"

static size_t put_varint(uint8_t *out, size_t v) {
    size_t n = 0;
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        out[n++] = b | (v ? 0x80 : 0);
    } while (v);
    return n;
}

static size_t get_varint(const uint8_t *in, size_t *v) {
    size_t n = 0, shift = 0, r = 0;
    uint8_t b;
    do {
        b = in[n++];
        r |= (size_t)(b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    *v = r;
    return n;
}

size_t smw_delta_bound(size_t length) { return length + length / 64 + 64; }

size_t smw_delta_encode(const uint8_t *a, const uint8_t *b, size_t length, uint8_t *out) {
    size_t i = 0, o = 0;
    while (i < length) {
        size_t start = i;
        while (i < length && a[i] == b[i]) i++;
        if (i > start) {
            out[o++] = 0;
            o += put_varint(out + o, i - start);
        }
        if (i >= length) break;
        start = i;
        // Stay in a literal run across short equal gaps (cheaper than a tag).
        while (i < length) {
            if (a[i] != b[i]) { i++; continue; }
            size_t j = i;
            while (j < length && a[j] == b[j] && j - i < 8) j++;
            if (j - i >= 8 || j == length) break;
            i = j;
        }
        out[o++] = 1;
        o += put_varint(out + o, i - start);
        for (size_t k = start; k < i; k++) out[o++] = a[k] ^ b[k];
    }
    return o;
}

void smw_delta_apply(uint8_t *state, const uint8_t *delta, size_t deltaLength) {
    size_t i = 0, pos = 0;
    while (i < deltaLength) {
        uint8_t tag = delta[i++];
        size_t len;
        i += get_varint(delta + i, &len);
        if (tag == 1) {
            for (size_t k = 0; k < len; k++) state[pos + k] ^= delta[i + k];
            i += len;
        }
        pos += len;
    }
}
