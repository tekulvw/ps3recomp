/* Optional synchronous picture decoder. No player state or guest pointers. */
#include "sail_video_decoder.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

void sail_video_frame_release(SailVideoFrame* frame)
{
    if (frame) { free(frame->rgba); memset(frame, 0, sizeof(*frame)); }
}
#ifndef PS3RECOMP_SAIL_FFMPEG
int sail_video_available(void) { return 0; }
int sail_video_decode_picture(FILE* f,uint64_t offset,uint64_t length,
                              unsigned stream,unsigned ordinal,SailVideoFrame* out)
{
    (void)f;(void)offset;(void)length;(void)stream;(void)ordinal;(void)out;
    return SAIL_VIDEO_UNAVAILABLE;
}
#else
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#if LIBAVCODEC_VERSION_MAJOR != 63 || LIBAVFORMAT_VERSION_MAJOR != 63 || LIBAVUTIL_VERSION_MAJOR != 61 || LIBSWSCALE_VERSION_MAJOR != 10
#error Sail video currently requires verified FFmpeg 9 public API headers
#endif
#ifdef _WIN32
#define sail_seek _fseeki64
#define sail_tell _ftelli64
#else
#define sail_seek fseeko
#define sail_tell ftello
#endif
#define SAIL_VIDEO_MAX_PAYLOAD (512ull*1024*1024)
#define SAIL_VIDEO_MAX_DIMENSION 4096
#define SAIL_VIDEO_MAX_PACKETS 250000u

typedef struct Payload {
    FILE* file;
    int64_t offset, length, position;
} Payload;

int sail_video_available(void)
{
    /* Exact matching protects public-struct field access in diagnostic builds. */
    return avcodec_version() == LIBAVCODEC_VERSION_INT &&
        avformat_version() == LIBAVFORMAT_VERSION_INT &&
        avutil_version() == LIBAVUTIL_VERSION_INT &&
        swscale_version() == LIBSWSCALE_VERSION_INT;
}
static int payload_read(void* opaque, uint8_t* buffer, int bytes)
{
    Payload* p = opaque;
    if (bytes <= 0) return AVERROR(EINVAL);
    if (p->position == p->length) return AVERROR_EOF;
    int64_t left = p->length - p->position;
    if (left < bytes) bytes = (int)left;
    if (sail_seek(p->file, p->offset + p->position, SEEK_SET)) return AVERROR(EIO);
    size_t got = fread(buffer, 1, (size_t)bytes, p->file);
    p->position += (int64_t)got;
    return got ? (int)got : AVERROR(EIO);
}
static int64_t payload_seek(void* opaque, int64_t offset, int whence)
{
    Payload* p = opaque;
    if (whence == AVSEEK_SIZE) return p->length;
    whence &= ~AVSEEK_FORCE;
    int64_t base;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = p->position;
    else if (whence == SEEK_END) base = p->length;
    else return AVERROR(EINVAL);
    /* Avoid signed overflow even for hostile demux seek requests. */
    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && offset < -base)) return AVERROR(EINVAL);
    int64_t next = base + offset;
    if (next < 0 || next > p->length) return AVERROR(EINVAL);
    p->position = next;
    return next;
}
static int export_picture(AVFrame* frame, AVStream* stream, unsigned ordinal, SailVideoFrame* out)
{
    if (frame->width <= 0 || frame->height <= 0 ||
        frame->width > SAIL_VIDEO_MAX_DIMENSION || frame->height > SAIL_VIDEO_MAX_DIMENSION)
        return SAIL_VIDEO_LIMIT;
    if (frame->format != AV_PIX_FMT_YUV420P ||
        (frame->color_range != AVCOL_RANGE_MPEG && frame->color_range != AVCOL_RANGE_JPEG))
        return SAIL_VIDEO_UNAVAILABLE;
    int coefficients;
    switch (frame->colorspace) {
    case AVCOL_SPC_BT709: coefficients = SWS_CS_ITU709; break;
    case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: coefficients = SWS_CS_ITU601; break;
    default: return SAIL_VIDEO_UNAVAILABLE; /* Never guess color metadata. */
    }
    struct SwsContext* sws = sws_getContext(frame->width, frame->height, frame->format,
        frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) return SAIL_VIDEO_DECODE;
    const int* table = sws_getCoefficients(coefficients);
    int result = SAIL_VIDEO_DECODE;
    uint8_t* rgba = NULL;
    if (sws_setColorspaceDetails(sws,table,frame->color_range == AVCOL_RANGE_JPEG,
                                 table,1,0,1<<16,1<<16) < 0) goto done;
    const uint32_t pitch = (uint32_t)frame->width * 4;
    rgba = malloc((size_t)pitch * frame->height);
    if (!rgba) { result = SAIL_VIDEO_LIMIT; goto done; }
    uint8_t* dest[4] = {rgba,0,0,0};
    int strides[4] = {(int)pitch,0,0,0};
    if (sws_scale(sws,(const uint8_t* const*)frame->data,frame->linesize,0,frame->height,
                  dest,strides) != frame->height) goto done;
    out->rgba = rgba; rgba = NULL;
    out->width = (uint32_t)frame->width; out->height = (uint32_t)frame->height;
    out->pitch = pitch; out->ordinal = ordinal;
    out->pts = frame->best_effort_timestamp;
    out->pts_valid = out->pts != AV_NOPTS_VALUE;
    out->time_base_num = stream->time_base.num; out->time_base_den = stream->time_base.den;
    out->rate_num = stream->avg_frame_rate.num; out->rate_den = stream->avg_frame_rate.den;
    out->colorspace = frame->colorspace; out->color_range = frame->color_range;
    out->interlaced = !!(frame->flags & AV_FRAME_FLAG_INTERLACED);
    result = SAIL_VIDEO_OK;
done:
    free(rgba); sws_freeContext(sws); return result;
}
static int receive_pictures(AVCodecContext* codec, AVFrame* picture, AVStream* stream,
                            unsigned wanted, unsigned* count, SailVideoFrame* out)
{
    int rc;
    while ((rc = avcodec_receive_frame(codec, picture)) >= 0) {
        ++*count;
        if (*count == wanted) {
            int result = export_picture(picture, stream, *count, out);
            if (result == SAIL_VIDEO_OK && !out->rate_num) {
                out->rate_num = codec->framerate.num;
                out->rate_den = codec->framerate.den;
            }
            av_frame_unref(picture); return result;
        }
        av_frame_unref(picture);
    }
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return SAIL_VIDEO_NOT_FOUND;
    return SAIL_VIDEO_DECODE;
}
int sail_video_decode_picture(FILE* file, uint64_t offset, uint64_t length,
                              unsigned stream_id, unsigned ordinal, SailVideoFrame* out)
{
    if (!out || out->rgba || !file || !length || length > SAIL_VIDEO_MAX_PAYLOAD ||
        offset > INT64_MAX || length > (uint64_t)INT64_MAX - offset ||
        stream_id < 0x1e0 || stream_id > 0x1ef || !ordinal || ordinal > 2000)
        return SAIL_VIDEO_INVALID;
    memset(out,0,sizeof(*out));
    if (!sail_video_available()) return SAIL_VIDEO_ABI;
    int64_t saved = sail_tell(file);
    if (saved < 0 || sail_seek(file,0,SEEK_END)) return SAIL_VIDEO_IO;
    int64_t size = sail_tell(file);
    if (sail_seek(file,saved,SEEK_SET)) return SAIL_VIDEO_IO;
    if (size < 0 || offset + length > (uint64_t)size) return SAIL_VIDEO_INVALID;
    Payload payload = {file,(int64_t)offset,(int64_t)length,0};
    AVFormatContext* format = avformat_alloc_context();
    AVIOContext* io = NULL; AVCodecContext* codec = NULL;
    AVPacket* packet = NULL; AVFrame* picture = NULL;
    int result = SAIL_VIDEO_DECODE, selected = -1;
    if (!format) goto done;
    uint8_t* io_buffer = av_malloc(32768);
    if (!io_buffer) goto done;
    io = avio_alloc_context(io_buffer,32768,0,&payload,payload_read,NULL,payload_seek);
    if (!io) { av_free(io_buffer); goto done; }
    format->pb = io; format->flags |= AVFMT_FLAG_CUSTOM_IO;
    format->probesize = 1024*1024; format->max_analyze_duration = 2*AV_TIME_BASE;
    if (avformat_open_input(&format,NULL,av_find_input_format("mpeg"),NULL) < 0) goto done;
    packet = av_packet_alloc(); picture = av_frame_alloc();
    if (!packet || !picture) goto done;
    unsigned packets = 0;
    int rc;
    /* Do not call find_stream_info: it can decode unselected streams before
     * our pixel limit is installed. Discover selected PES by bounded demux only. */
    while ((rc = av_read_frame(format,packet)) >= 0) {
        if (++packets > SAIL_VIDEO_MAX_PACKETS) { result = SAIL_VIDEO_LIMIT; goto done; }
        AVStream* st = format->streams[packet->stream_index];
        if (st->id == (int)stream_id && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (st->codecpar->codec_id != AV_CODEC_ID_MPEG2VIDEO) {
                result = SAIL_VIDEO_UNAVAILABLE; goto done;
            }
            selected = packet->stream_index; break;
        }
        av_packet_unref(packet);
    }
    if (selected < 0) { result = rc == AVERROR_EOF ? SAIL_VIDEO_NOT_FOUND : SAIL_VIDEO_DECODE; goto done; }
    AVStream* stream = format->streams[selected];
    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!decoder) { result = SAIL_VIDEO_UNAVAILABLE; goto done; }
    codec = avcodec_alloc_context3(decoder);
    if (!codec || avcodec_parameters_to_context(codec,stream->codecpar) < 0) goto done;
    codec->thread_count = 1; codec->pkt_timebase = stream->time_base;
    codec->max_pixels = (int64_t)SAIL_VIDEO_MAX_DIMENSION * SAIL_VIDEO_MAX_DIMENSION;
    codec->err_recognition = AV_EF_EXPLODE;
    if (avcodec_open2(codec,decoder,NULL) < 0) goto done;
    unsigned count = 0;
    do {
        if (packet->stream_index == selected) {
            int sent = avcodec_send_packet(codec,packet);
            if (sent == AVERROR(EAGAIN)) {
                result = receive_pictures(codec,picture,stream,ordinal,&count,out);
                if (result != SAIL_VIDEO_NOT_FOUND) goto done;
                sent = avcodec_send_packet(codec,packet);
            }
            if (sent < 0) { result = SAIL_VIDEO_DECODE; goto done; }
            result = receive_pictures(codec,picture,stream,ordinal,&count,out);
            if (result != SAIL_VIDEO_NOT_FOUND) goto done;
        }
        av_packet_unref(packet);
        if (++packets > SAIL_VIDEO_MAX_PACKETS) { result = SAIL_VIDEO_LIMIT; goto done; }
    } while ((rc = av_read_frame(format,packet)) >= 0);
    if (rc != AVERROR_EOF || avcodec_send_packet(codec,NULL) < 0) {
        result = SAIL_VIDEO_DECODE; goto done;
    }
    result = receive_pictures(codec,picture,stream,ordinal,&count,out);
done:
    av_frame_free(&picture); av_packet_free(&packet); avcodec_free_context(&codec);
    avformat_close_input(&format);
    if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    if (sail_seek(file,saved,SEEK_SET)) result = SAIL_VIDEO_IO;
    if (result != SAIL_VIDEO_OK) sail_video_frame_release(out);
    return result;
}
#endif
