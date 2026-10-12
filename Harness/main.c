// Headless test harness: runs the core for N frames with a scripted input
// timeline and writes PNG screenshots + a WAV of the audio.
//   harness <rom> <frames> <outdir> [script]
// script lines: "<frame> <buttons-hex>" (buttons held from that frame on)
//               "<frame> shot" to take a screenshot at that frame
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "smw_core.h"

static int gWidth = 256;

static void write_png(const char *path, const uint32_t *px) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate((void *)px, gWidth, 224, 8, SMW_FRAMEBUFFER_PITCH * 4, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFStringRef s = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, s, kCFURLPOSIXPathStyle, false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(dst, img, NULL);
    CGImageDestinationFinalize(dst);
    CFRelease(dst); CFRelease(url); CFRelease(s); CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);
}

static void write_wav(const char *path, const int16_t *data, int frames) {
    FILE *f = fopen(path, "wb");
    int bytes = frames * 4;
    int rate = SMW_AUDIO_RATE;
    fwrite("RIFF", 1, 4, f);
    int v = 36 + bytes; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    short sv = 1; fwrite(&sv, 2, 1, f);
    sv = 2; fwrite(&sv, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    v = rate * 4; fwrite(&v, 4, 1, f);
    sv = 4; fwrite(&sv, 2, 1, f);
    sv = 16; fwrite(&sv, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&bytes, 4, 1, f);
    fwrite(data, 4, frames, f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s rom frames outdir [script] [--wide N] [--load f] [--save f] [--subpixel]\n", argv[0]);
        return 1;
    }
    int wide = 0, hideMask = -1, nforce = 0, opaque = 0, interp = 0, subpixel = 1;
    unsigned forceAddr[4], forceVal[4];
    const char *loadPath = NULL, *savePath = NULL, *wramPath = NULL;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--wide") && i + 1 < argc) wide = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--load") && i + 1 < argc) loadPath = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) savePath = argv[++i];
        else if (!strcmp(argv[i], "--dump-wram") && i + 1 < argc) wramPath = argv[++i];
        else if (!strcmp(argv[i], "--force") && i + 1 < argc && nforce < 4) {
            unsigned a, v;
            if (sscanf(argv[++i], "%x=%x", &a, &v) == 2) { forceAddr[nforce] = a; forceVal[nforce++] = v; }
        }
        else if (!strcmp(argv[i], "--hide") && i + 1 < argc) hideMask = (int)strtol(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--opaque")) opaque = 1;
        else if (!strcmp(argv[i], "--interp")) interp = 1;
        else if (!strcmp(argv[i], "--subpixel")) subpixel = SMW_MAX_SUBPIXEL;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("rom"); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc(len);
    fread(rom, 1, len, f);
    fclose(f);
    int frames = atoi(argv[2]);
    const char *out = argv[3];

    int evFrame[4096]; int evButtons[4096]; int evShot[4096]; int nev = 0;
    if (argc > 4 && argv[4][0] != '-') {
        FILE *sf = fopen(argv[4], "r");
        char line[256];
        while (sf && fgets(line, sizeof line, sf) && nev < 4096) {
            int fr; char word[64];
            if (line[0] == '#' || sscanf(line, "%d %63s", &fr, word) != 2) continue;
            evFrame[nev] = fr;
            evShot[nev] = strcmp(word, "shot") == 0;
            evButtons[nev] = evShot[nev] ? 0 : (int)strtol(word, NULL, 16);
            nev++;
        }
        if (sf) fclose(sf);
    }

    SMWCore *core = smw_create(rom, len);
    static uint32_t fb[SMW_FRAMEBUFFER_PITCH * 224];
    smw_set_framebuffer(core, fb, SMW_FRAMEBUFFER_PITCH);
    smw_set_subpixel(core, subpixel);
    smw_set_widescreen(core, wide);
    if (interp) smw_set_motion_capture(core, true);
    if (hideMask >= 0) smw_debug_layers(core, (uint8_t)hideMask, !opaque);
    gWidth = smw_frame_width(core) * smw_subpixel(core);
    if (loadPath) {
        FILE *lf = fopen(loadPath, "rb");
        size_t n = smw_state_size();
        void *buf = malloc(n);
        if (lf && fread(buf, 1, n, lf) == n) smw_load_state(core, buf, n);
        if (lf) fclose(lf);
        free(buf);
    }
    for (int i = 0; i < nforce; i++) smw_debug_force_wram(core, forceAddr[i], (int)forceVal[i]);
    int16_t *audio = malloc(sizeof(int16_t) * 2 * 600 * (frames + 1));
    int audioFrames = 0;
    clock_t t0 = clock();
    for (int i = 0; i < frames; i++) {
        int shot = 0;
        for (int e = 0; e < nev; e++) {
            if (evFrame[e] != i) continue;
            if (evShot[e]) shot = 1;
            else smw_set_buttons(core, 0, (uint16_t)evButtons[e]);
        }
        smw_run_frame(core);
        audioFrames += smw_read_audio(core, audio + audioFrames * 2, 600);
        if (shot || (nev == 0 && (i + 1) % 60 == 0)) {
            char path[512];
            snprintf(path, sizeof path, "%s/frame_%05d.png", out, i);
            write_png(path, fb);
            if (interp) {
                static uint32_t mid[SMW_FRAMEBUFFER_PITCH * 224];
                if (smw_render_interpolated(core, mid, SMW_FRAMEBUFFER_PITCH, 0.5f)) {
                    snprintf(path, sizeof path, "%s/frame_%05d_mid.png", out, i);
                    write_png(path, mid);
                }
            }
        }
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("%d frames in %.2fs (%.0f fps), %d audio frames\n", frames, secs, frames / secs, audioFrames);
    char path[512];
    snprintf(path, sizeof path, "%s/audio.wav", out);
    write_wav(path, audio, audioFrames);
    if (savePath) {
        size_t n = smw_state_size();
        void *buf = malloc(n);
        smw_save_state(core, buf);
        FILE *sf = fopen(savePath, "wb");
        fwrite(buf, 1, n, sf);
        fclose(sf);
        free(buf);
    }
    const uint8_t *w = smw_wram(core);
    if (wramPath) {
        FILE *wf = fopen(wramPath, "wb");
        fwrite(w, 1, 0x20000, wf);
        fwrite(smw_debug_cgram(core), 2, 256, wf); // CGRAM appended after WRAM
        fclose(wf);
    }
    printf("GameMode=$%02X\n", w[0x100]);
    smw_destroy(core);
    return 0;
}
