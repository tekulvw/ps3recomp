/* Optional host decoder. FILE ownership stays with the caller; no guest ABI. */
#ifndef PS3RECOMP_SAIL_VIDEO_DECODER_H
#define PS3RECOMP_SAIL_VIDEO_DECODER_H
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { SAIL_VIDEO_OK=0, SAIL_VIDEO_UNAVAILABLE=-1, SAIL_VIDEO_INVALID=-2,
       SAIL_VIDEO_IO=-3, SAIL_VIDEO_DECODE=-4, SAIL_VIDEO_NOT_FOUND=-5,
       SAIL_VIDEO_LIMIT=-6, SAIL_VIDEO_ABI=-7 };
typedef struct SailVideoFrame {
    uint8_t* rgba;
    uint32_t width, height, pitch, ordinal;
    int64_t pts;
    int pts_valid, time_base_num, time_base_den;
    int rate_num, rate_den, colorspace, color_range, interlaced;
} SailVideoFrame;
int sail_video_available(void);
/* Payload bounds refer to an already validated local file. Only MPEG-2 PES
 * stream ids 0x1E0..0x1EF are accepted. Ordinal is one-based, <=2000.
 * Output must be zero-initialized; failure leaves it zero. No network I/O. */
int sail_video_decode_picture(FILE* file, uint64_t offset, uint64_t length,
                              unsigned stream_id, unsigned ordinal, SailVideoFrame* out);
void sail_video_frame_release(SailVideoFrame* frame);
#ifdef __cplusplus
}
#endif
#endif
