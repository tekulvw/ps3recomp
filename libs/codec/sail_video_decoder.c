/* Optional synchronous picture decoder. No player state or guest pointers. */
#include "sail_video_decoder.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

void sail_audio_pcm_release(SailAudioPCM* a) { if(a){free(a->samples);memset(a,0,sizeof(*a));} }
void sail_video_frame_release(SailVideoFrame* frame)
{
    if (frame) { free(frame->rgba); memset(frame, 0, sizeof(*frame)); }
}
#ifndef PS3RECOMP_SAIL_FFMPEG
int sail_video_available(void) { return 0; }
int sail_audio_decode_pcm(FILE*f,uint64_t o,uint64_t n,unsigned t,SailAudioPCM*a)
{(void)f;(void)o;(void)n;(void)t;(void)a;return SAIL_VIDEO_UNAVAILABLE;}
int sail_video_open(FILE*f,uint64_t o,uint64_t n,unsigned s,SailVideoDecoder**d)
{ (void)f;(void)o;(void)n;(void)s;if(d)*d=NULL;return SAIL_VIDEO_UNAVAILABLE; }
int sail_video_next(SailVideoDecoder*d,SailVideoFrame*out)
{ (void)d;(void)out;return SAIL_VIDEO_UNAVAILABLE; }
void sail_video_close(SailVideoDecoder*d) {(void)d;}
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
    out->duration = frame->duration;
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
/* A single bounded demux/decoder lifetime. All operations belong to one
 * caller; FILE remains borrowed and all guest callback work stays elsewhere. */
struct SailVideoDecoder {
    Payload payload;
    int64_t saved;
    AVFormatContext* format;
    AVIOContext* io;
    AVCodecContext* codec;
    AVPacket* packet;
    AVFrame* frame;
    unsigned packets, ordinal;
    int selected, pending, demux_eof, flushing, ended, failed;
};
/* FFmpeg's MPEG-PS demuxer deliberately accepts short PES packets. Its EOF
 * alone cannot establish an intact bounded payload. Check declared packet
 * spans independently, without decoding or retaining movie contents. This
 * supports finite PES packets; zero-length video PES is explicitly unsupported.
 * A stream shortened exactly at a complete packet boundary is not distinguishable
 * from a shorter valid stream without an external content manifest. */
static int validate_program_spans(FILE* f, int64_t offset, int64_t length)
{
    if(sail_seek(f,offset,SEEK_SET))return SAIL_VIDEO_IO;
    int64_t pos=0;uint32_t prefix=UINT32_MAX;unsigned packets=0;
    while(pos<length){
        int c=fgetc(f);if(c==EOF)return SAIL_VIDEO_IO;
        ++pos;prefix=(prefix<<8)|(unsigned)c;
        if((prefix & 0xffffff00u)!=0x00000100u || c<0xb9)continue;
        if(++packets>SAIL_VIDEO_MAX_PACKETS)return SAIL_VIDEO_LIMIT;
        int64_t start=pos-4,end=pos;
        if(c==0xba){
            if(length-pos<1)return SAIL_VIDEO_DECODE;
            int first=fgetc(f);if(first==EOF)return SAIL_VIDEO_IO;
            if((first & 0xc0)==0x40){
                if(length-start<14)return SAIL_VIDEO_DECODE;
                if(sail_seek(f,offset+start+13,SEEK_SET))return SAIL_VIDEO_IO;
                int stuffing=fgetc(f);if(stuffing==EOF)return SAIL_VIDEO_IO;
                end=start+14+(stuffing & 7);
            }else if((first & 0xf0)==0x20)end=start+12;
            else return SAIL_VIDEO_UNAVAILABLE;
        }else if(c!=0xb9){
            if(length-pos<2)return SAIL_VIDEO_DECODE;
            int hi=fgetc(f),lo=fgetc(f);if(hi==EOF || lo==EOF)return SAIL_VIDEO_IO;
            unsigned bytes=((unsigned)hi<<8)|(unsigned)lo;
            if(!bytes && c>=0xe0 && c<=0xef)return SAIL_VIDEO_UNAVAILABLE;
            end=pos+2+bytes;
        }
        if(end>length)return SAIL_VIDEO_DECODE;
        if(sail_seek(f,offset+end,SEEK_SET))return SAIL_VIDEO_IO;
        pos=end;prefix=UINT32_MAX;
    }
    return SAIL_VIDEO_OK;
}
void sail_video_close(SailVideoDecoder* d)
{
    if(!d)return;
    av_frame_free(&d->frame);av_packet_free(&d->packet);avcodec_free_context(&d->codec);
    avformat_close_input(&d->format);
    if(d->io){av_freep(&d->io->buffer);avio_context_free(&d->io);}
    if(d->payload.file && d->saved>=0)(void)sail_seek(d->payload.file,d->saved,SEEK_SET);
    free(d);
}
int sail_video_open(FILE* f,uint64_t offset,uint64_t length,unsigned stream_id,SailVideoDecoder** out)
{
    if(!out || *out || !f || !length || length>SAIL_VIDEO_MAX_PAYLOAD ||
        offset>INT64_MAX || length>(uint64_t)INT64_MAX-offset || stream_id<0x1e0 || stream_id>0x1ef)
        return SAIL_VIDEO_INVALID;
    if(!sail_video_available())return SAIL_VIDEO_ABI;
    int64_t saved=sail_tell(f);
    if(saved<0 || sail_seek(f,0,SEEK_END))return SAIL_VIDEO_IO;
    int64_t size=sail_tell(f);
    if(sail_seek(f,saved,SEEK_SET))return SAIL_VIDEO_IO;
    if(size<0 || offset+length>(uint64_t)size)return SAIL_VIDEO_INVALID;
    int spans=validate_program_spans(f,(int64_t)offset,(int64_t)length);
    if(sail_seek(f,saved,SEEK_SET))return SAIL_VIDEO_IO;
    if(spans!=SAIL_VIDEO_OK)return spans;
    SailVideoDecoder* d=calloc(1,sizeof(*d));if(!d)return SAIL_VIDEO_LIMIT;
    d->payload=(Payload){f,(int64_t)offset,(int64_t)length,0};d->saved=saved;d->selected=-1;
    int result=SAIL_VIDEO_DECODE,rc;
    d->format=avformat_alloc_context();if(!d->format)goto fail;
    uint8_t* buffer=av_malloc(32768);if(!buffer)goto fail;
    d->io=avio_alloc_context(buffer,32768,0,&d->payload,payload_read,NULL,payload_seek);
    if(!d->io){av_free(buffer);goto fail;}
    d->format->pb=d->io;d->format->flags|=AVFMT_FLAG_CUSTOM_IO;
    d->format->probesize=1024*1024;d->format->max_analyze_duration=2*AV_TIME_BASE;
    if(avformat_open_input(&d->format,NULL,av_find_input_format("mpeg"),NULL)<0)goto fail;
    d->packet=av_packet_alloc();d->frame=av_frame_alloc();if(!d->packet || !d->frame)goto fail;
    while((rc=av_read_frame(d->format,d->packet))>=0){
        if(++d->packets>SAIL_VIDEO_MAX_PACKETS){result=SAIL_VIDEO_LIMIT;goto fail;}
        if(d->packet->stream_index<0 || (unsigned)d->packet->stream_index>=d->format->nb_streams)goto fail;
        AVStream* st=d->format->streams[d->packet->stream_index];
        if(st->id==(int)stream_id && st->codecpar->codec_type==AVMEDIA_TYPE_VIDEO){
            if(st->codecpar->codec_id!=AV_CODEC_ID_MPEG2VIDEO){result=SAIL_VIDEO_UNAVAILABLE;goto fail;}
            d->selected=d->packet->stream_index;d->pending=1;break;
        }
        av_packet_unref(d->packet);
    }
    if(d->selected<0){result=rc==AVERROR_EOF?SAIL_VIDEO_NOT_FOUND:SAIL_VIDEO_DECODE;goto fail;}
    const AVCodec* decoder=avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    if(!decoder){result=SAIL_VIDEO_UNAVAILABLE;goto fail;}
    d->codec=avcodec_alloc_context3(decoder);
    AVStream* st=d->format->streams[d->selected];
    if(!d->codec || avcodec_parameters_to_context(d->codec,st->codecpar)<0)goto fail;
    d->codec->thread_count=1;d->codec->pkt_timebase=st->time_base;
    d->codec->max_pixels=(int64_t)SAIL_VIDEO_MAX_DIMENSION*SAIL_VIDEO_MAX_DIMENSION;
    d->codec->err_recognition=AV_EF_EXPLODE;
    if(avcodec_open2(d->codec,decoder,NULL)<0)goto fail;
    *out=d;return SAIL_VIDEO_OK;
fail:
    sail_video_close(d);return result;
}
int sail_video_next(SailVideoDecoder* d,SailVideoFrame* out)
{
    if(!d || !out || out->rgba)return SAIL_VIDEO_INVALID;
    memset(out,0,sizeof(*out));
    if(d->failed)return d->failed;
    if(d->ended)return SAIL_VIDEO_EOF;
    int result=SAIL_VIDEO_DECODE;
    for(;;){
        int rc=avcodec_receive_frame(d->codec,d->frame);
        if(rc>=0){
            if(++d->ordinal>2000){result=SAIL_VIDEO_LIMIT;goto fail;}
            if(d->frame->decode_error_flags || (d->frame->flags & AV_FRAME_FLAG_CORRUPT))goto fail;
            AVStream* st=d->format->streams[d->selected];
            result=export_picture(d->frame,st,d->ordinal,out);
            if(!result && !out->rate_num){out->rate_num=d->codec->framerate.num;out->rate_den=d->codec->framerate.den;}
            av_frame_unref(d->frame);
            if(result)goto fail;
            return SAIL_VIDEO_OK;
        }
        if(rc==AVERROR_EOF){
            if(!d->demux_eof || !d->flushing)goto fail;
            d->ended=1;return SAIL_VIDEO_EOF;
        }
        if(rc!=AVERROR(EAGAIN) || d->flushing)goto fail;
        if(d->pending){
            if(d->packet->flags & AV_PKT_FLAG_CORRUPT)goto fail;
            rc=avcodec_send_packet(d->codec,d->packet);
            if(rc<0)goto fail; /* receive EAGAIN then send EAGAIN is invalid. */
            av_packet_unref(d->packet);d->pending=0;continue;
        }
        if(d->demux_eof){
            if(avcodec_send_packet(d->codec,NULL)<0)goto fail;
            d->flushing=1;continue;
        }
        while((rc=av_read_frame(d->format,d->packet))>=0){
            if(++d->packets>SAIL_VIDEO_MAX_PACKETS){result=SAIL_VIDEO_LIMIT;goto fail;}
            if(d->packet->stream_index==d->selected){d->pending=1;break;}
            av_packet_unref(d->packet);
        }
        if(!d->pending){
            if(rc!=AVERROR_EOF || d->io->error<0 || ferror(d->payload.file))goto fail;
            d->demux_eof=1;
        }
    }
fail:
    d->failed=result;sail_video_frame_release(out);av_frame_unref(d->frame);return result;
}

#endif

#ifdef PS3RECOMP_SAIL_FFMPEG
/* Parse top-level MPEG program packets. Never scan PES payloads for start codes:
 * compressed video may contain a byte sequence resembling a private audio PES. */
int sail_audio_decode_pcm(FILE* file,uint64_t offset,uint64_t length,unsigned track,SailAudioPCM* out)
{
    if(!out || out->samples || !file || track>9 || !length || length>SAIL_VIDEO_MAX_PAYLOAD ||
        offset>INT64_MAX || length>(uint64_t)INT64_MAX-offset)return SAIL_VIDEO_INVALID;
    if(!sail_video_available())return SAIL_VIDEO_ABI;
    const int64_t saved=sail_tell(file); if(saved<0)return SAIL_VIDEO_IO;
    int result=SAIL_VIDEO_INVALID;uint8_t* compressed=NULL;float* pcm=NULL;
    AVCodecContext* codec=NULL;AVPacket* packet=NULL;AVFrame* frame=NULL;
    size_t used=0;uint64_t pos=offset,end=offset+length,first_pts=UINT64_MAX;
    if(sail_seek(file,0,SEEK_END) || sail_tell(file)<0 || end>(uint64_t)sail_tell(file))goto done;
    compressed=malloc(8u<<20);if(!compressed){result=SAIL_VIDEO_LIMIT;goto done;}
    uint8_t bytes[65536];unsigned packets=0;
    while(pos+6<=end){
        if(++packets>SAIL_VIDEO_MAX_PACKETS){result=SAIL_VIDEO_LIMIT;goto done;}
        if(sail_seek(file,pos,SEEK_SET) || fread(bytes,1,6,file)!=6){result=SAIL_VIDEO_IO;goto done;}
        if(bytes[0] || bytes[1] || bytes[2]!=1){
            /* Zero alignment padding is allowed outside packets only. */
            if(bytes[0])goto done;++pos;continue;
        }
        const unsigned code=bytes[3];
        if(code==0xb9){pos+=4;break;}
        if(code==0xba){
            if(end-pos<14 || fread(bytes+6,1,8,file)!=8)goto done;
            pos+=14+(bytes[13]&7);if(pos>end)goto done;continue;
        }
        if(code<0xbb)goto done;
        const unsigned n=((unsigned)bytes[4]<<8)|bytes[5];
        if(!n || pos+6+n>end)goto done;
        if(code==0xbd){
            if(fread(bytes,1,n,file)!=n){result=SAIL_VIDEO_IO;goto done;}
            if(n<7 || (bytes[0]&0xc0)!=0x80)goto done;
            const unsigned payload=3+bytes[2];if(payload+4>n)goto done;
            if(bytes[payload]==track){
                if(first_pts==UINT64_MAX){
                    if(!(bytes[1]&0x80) || bytes[2]<5)goto done;
                    const uint8_t* p=bytes+3;
                    if(!(p[0]&1) || !(p[2]&1) || !(p[4]&1))goto done;
                    first_pts=((uint64_t)((p[0]>>1)&7)<<30)|((uint64_t)p[1]<<22)|
                        ((uint64_t)(p[2]>>1)<<15)|((uint64_t)p[3]<<7)|(p[4]>>1);
                }
                const unsigned count=n-payload-4;
                if(count>(8u<<20)-used){result=SAIL_VIDEO_LIMIT;goto done;}
                memcpy(compressed+used,bytes+payload+4,count);used+=count;
            }
        }
        pos+=6+n;
    }
    if(used<8 || compressed[0]!=0x0f || compressed[1]!=0xd0 || first_pts==UINT64_MAX)goto done;
    const unsigned block=((((unsigned)compressed[2]&3)<<8)|compressed[3])*8+8;
    const unsigned map[8]={0,1,2,3,4,6,7,8},channels=map[(compressed[2]>>2)&7];
    if((channels!=2 && channels!=6) || used%(block+8))goto done;
    const size_t blocks=used/(block+8),samples=blocks*2048;
    if(samples>4000000){result=SAIL_VIDEO_LIMIT;goto done;}
    pcm=malloc(samples*channels*sizeof(float));if(!pcm){result=SAIL_VIDEO_LIMIT;goto done;}
    const AVCodec* decoder=avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
    if(!decoder){result=SAIL_VIDEO_UNAVAILABLE;goto done;}
    codec=avcodec_alloc_context3(decoder);packet=av_packet_alloc();frame=av_frame_alloc();
    if(!codec || !packet || !frame){result=SAIL_VIDEO_LIMIT;goto done;}
    codec->block_align=block;codec->sample_rate=48000;av_channel_layout_default(&codec->ch_layout,channels);
    if(avcodec_open2(codec,decoder,NULL)<0){result=SAIL_VIDEO_DECODE;goto done;}
    size_t written=0;
    for(size_t b=0;b<blocks;++b){
        const uint8_t* p=compressed+b*(block+8);
        if(memcmp(p,compressed,8)){result=SAIL_VIDEO_INVALID;goto done;}
        if(av_new_packet(packet,block)<0){result=SAIL_VIDEO_LIMIT;goto done;}
        memcpy(packet->data,p+8,block);
        if(avcodec_send_packet(codec,packet)<0){result=SAIL_VIDEO_DECODE;goto done;}
        av_packet_unref(packet);
        int rc=avcodec_receive_frame(codec,frame);
        if(rc<0 || frame->format!=AV_SAMPLE_FMT_FLTP || frame->nb_samples!=2048 ||
            frame->ch_layout.nb_channels!=(int)channels){result=SAIL_VIDEO_DECODE;goto done;}
        for(unsigned i=0;i<2048;++i)for(unsigned c=0;c<channels;++c)
            pcm[(written+i)*channels+c]=((float*)frame->extended_data[c])[i];
        written+=2048;av_frame_unref(frame);
        if(avcodec_receive_frame(codec,frame)!=AVERROR(EAGAIN)){result=SAIL_VIDEO_DECODE;goto done;}
    }
    if(avcodec_send_packet(codec,NULL)<0 || avcodec_receive_frame(codec,frame)!=AVERROR_EOF){result=SAIL_VIDEO_DECODE;goto done;}
    out->samples=pcm;pcm=NULL;out->channels=channels;out->rate=48000;out->count=(uint32_t)written;out->pts=first_pts;
    result=SAIL_VIDEO_OK;
done:
    av_frame_free(&frame);av_packet_free(&packet);avcodec_free_context(&codec);free(compressed);free(pcm);
    if(sail_seek(file,saved,SEEK_SET)){sail_audio_pcm_release(out);return SAIL_VIDEO_IO;}
    return result;
}
#endif
