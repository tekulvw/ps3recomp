/* Binary PPM (P6) from 8-bit RGBA rows: R, G and B of each pixel, alpha
 * dropped. Packed a row at a time and written with one fwrite per row: the
 * per-pixel 3-byte fwrite this replaces made a 1280x720 dump 921,600 calls,
 * about 1.3-1.7 ms per update on the presenting thread at one dump every 20
 * frames. Same bytes either way (tools/test_rsx_ppm.py). */
#ifndef RSX_PPM_H
#define RSX_PPM_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* 0 on success, -1 when the file could not be written in full. */
static inline int rsx_ppm_write_rgba(FILE* f, const unsigned char* rgba, unsigned w, unsigned h)
{
    if (fprintf(f, "P6\n%u %u\n255\n", w, h) < 0) return -1;
    if (!w || !h) return 0;
    unsigned char* row = (unsigned char*)malloc((size_t)w * 3u);
    if (!row) return -1;
    int ok = 0;
    for (unsigned y = 0; y < h && !ok; y++) {
        const unsigned char* src = rgba + (size_t)y * w * 4u;
        for (unsigned x = 0; x < w; x++) memcpy(row + (size_t)x * 3u, src + (size_t)x * 4u, 3);
        if (fwrite(row, 1, (size_t)w * 3u, f) != (size_t)w * 3u) ok = -1;
    }
    free(row);
    return ok;
}
#endif
