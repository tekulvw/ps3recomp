/* SPU writes into guest memory, ordered per 128-byte line for a concurrent
 * reader (docs/SPU_PUT_LINE_ORDER.md).
 *
 * The hardware lands an MFC write line by line, so a reader sees a line
 * either old or complete. A host copy is not line-atomic. These helpers keep
 * the part of that guarantee the FIFO walker relies on: in each line, the
 * first aligned word the write stores there is stored last, with release
 * ordering. A reader that acquire-loads that word and finds it changed sees
 * the rest of the write's bytes in the line. */
#ifndef SPU_LINE_STORE_H
#define SPU_LINE_STORE_H

#include <stdint.h>
#include <string.h>

/* One piece of a write that lies inside a single 128-byte line. `src` NULL
 * stores zeros. */
static inline void spu_line_store_piece(uint8_t* dst, const uint8_t* src, uint32_t n)
{
    if (n == 4u && !((uintptr_t)dst & 3u)) {   /* one word, stored whole */
        uint32_t v = 0;
        if (src) memcpy(&v, src, 4);
        __atomic_store_n((uint32_t*)dst, v, __ATOMIC_RELEASE);
        return;
    }
    if (n < 8u || ((uintptr_t)dst & 3u)) {
        if (src) memcpy(dst, src, n); else memset(dst, 0, n);
        return;
    }
    if (n == 128u) {   /* a whole line: constant sizes the compiler inlines */
        if (src) memcpy(dst + 4, src + 4, 124u); else memset(dst + 4, 0, 124u);
    } else {
        if (src) memcpy(dst + 4, src + 4, n - 4u); else memset(dst + 4, 0, n - 4u);
    }
    uint32_t head = 0;
    if (src) memcpy(&head, src, 4);
    __atomic_store_n((uint32_t*)dst, head, __ATOMIC_RELEASE);
}

/* `n` bytes at guest address `ea` (host address `dst`), split at line
 * boundaries. */
static inline void spu_line_store(uint8_t* dst, uint32_t ea, const uint8_t* src, uint32_t n)
{
    while (n) {
        uint32_t c = 128u - (ea & 127u);
        if (c > n) c = n;
        spu_line_store_piece(dst, src, c);
        dst += c; ea += c; n -= c;
        if (src) src += c;
    }
}

#endif
