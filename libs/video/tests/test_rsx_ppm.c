/* rsx_ppm.h writes the same P6 bytes as the per-pixel writer it replaced.
 * python3 tools/test_rsx_ppm.py */
#include "../rsx_ppm.h"
#include <assert.h>
static unsigned char* run(FILE* (*open)(void), int packed, const unsigned char* rgba, unsigned w, unsigned h, size_t* n)
{
    FILE* f = open();
    assert(f);
    if (packed) assert(!rsx_ppm_write_rgba(f, rgba, w, h));
    else {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < (size_t)w * h; i++) fwrite(rgba + i * 4, 1, 3, f);
    }
    fflush(f);
    *n = (size_t)ftell(f);
    unsigned char* out = malloc(*n ? *n : 1);
    rewind(f);
    assert(fread(out, 1, *n, f) == *n);
    fclose(f);
    return out;
}
static FILE* scratch(void) { return tmpfile(); }
int main(void)
{
    const unsigned sizes[][2] = { {1280, 720}, {1, 1}, {3, 5}, {17, 1}, {0, 4} };
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        const unsigned w = sizes[k][0], h = sizes[k][1];
        unsigned char* rgba = malloc((size_t)w * h * 4 + 1);
        for (size_t i = 0; i < (size_t)w * h * 4; i++) rgba[i] = (unsigned char)(i * 2654435761u >> 13);
        size_t a, b;
        unsigned char* x = run(scratch, 0, rgba, w, h, &a);
        unsigned char* y = run(scratch, 1, rgba, w, h, &b);
        assert(a == b && !memcmp(x, y, a));
        assert(a == (size_t)snprintf(NULL, 0, "P6\n%u %u\n255\n", w, h) + (size_t)w * h * 3);
        free(x); free(y); free(rgba);
    }
    printf("rsx ppm: PASS (packed rows write the same bytes as per-pixel writes)\n");
    return 0;
}
