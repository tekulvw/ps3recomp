/* Original synthetic test generator, dedicated to the public domain (CC0-1.0). */
#include <stdio.h>
#include <stdlib.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
static void die(const char *op,int r){char msg[256];av_strerror(r,msg,sizeof msg);fprintf(stderr,"%s: %s (%d)\n",op,msg,r);exit(1);}
static void packets(AVCodecContext*c,AVFormatContext*o,AVStream*s,AVPacket*p){int r;while((r=avcodec_receive_packet(c,p))>=0){av_packet_rescale_ts(p,c->time_base,s->time_base);p->stream_index=s->index;r=av_interleaved_write_frame(o,p);av_packet_unref(p);if(r<0)die("write packet",r);}if(r!=AVERROR_EOF&&r!=AVERROR(EAGAIN))die("receive packet",r);}
int main(int argc,char**argv){if(argc!=2)return 2;const AVCodec*enc=avcodec_find_encoder_by_name("mpeg2video");if(!enc){fprintf(stderr,"encoder absent\n");return 3;}
 AVFormatContext*o=NULL;int r=avformat_alloc_output_context2(&o,NULL,"mpeg",argv[1]);if(r<0)die("muxer",r);o->flags|=AVFMT_FLAG_BITEXACT;AVStream*s=avformat_new_stream(o,NULL);AVCodecContext*c=avcodec_alloc_context3(enc);
 c->width=64;c->height=48;c->pix_fmt=AV_PIX_FMT_YUV420P;c->time_base=(AVRational){1001,30000};c->framerate=(AVRational){30000,1001};c->sample_aspect_ratio=(AVRational){1,1};c->gop_size=12;c->max_b_frames=2;c->bit_rate=200000;c->thread_count=1;c->flags|=AV_CODEC_FLAG_BITEXACT;c->color_range=AVCOL_RANGE_MPEG;c->colorspace=AVCOL_SPC_BT709;c->color_primaries=AVCOL_PRI_BT709;c->color_trc=AVCOL_TRC_BT709;
 if(o->oformat->flags&AVFMT_GLOBALHEADER)c->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;r=avcodec_open2(c,enc,NULL);if(r<0)die("encoder open",r);r=avcodec_parameters_from_context(s->codecpar,c);if(r<0)die("stream parameters",r);s->time_base=c->time_base;
 r=avio_open(&o->pb,argv[1],AVIO_FLAG_WRITE);if(r<0)die("output",r);r=avformat_write_header(o,NULL);if(r<0)die("header",r);
 AVFrame*f=av_frame_alloc();AVPacket*p=av_packet_alloc();f->format=c->pix_fmt;f->width=c->width;f->height=c->height;f->colorspace=c->colorspace;f->color_range=c->color_range;f->color_primaries=c->color_primaries;f->color_trc=c->color_trc;r=av_frame_get_buffer(f,32);if(r<0)die("frame",r);
 for(int n=0;n<32;n++){r=av_frame_make_writable(f);if(r<0)die("writable",r);for(int y=0;y<48;y++)for(int x=0;x<64;x++)f->data[0][y*f->linesize[0]+x]=(unsigned char)(16+((x*3+y*2+n*5)%220));for(int y=0;y<24;y++)for(int x=0;x<32;x++){f->data[1][y*f->linesize[1]+x]=(unsigned char)(16+((x*7+n*3)%225));f->data[2][y*f->linesize[2]+x]=(unsigned char)(16+((y*9+n*7)%225));}f->pts=n;r=avcodec_send_frame(c,f);if(r<0)die("send frame",r);packets(c,o,s,p);}
 r=avcodec_send_frame(c,NULL);if(r<0)die("flush",r);packets(c,o,s,p);r=av_write_trailer(o);if(r<0)die("trailer",r);printf("encoded=32 width=64 height=48 codec=mpeg2video mux=mpeg color=bt709 range=mpeg fps=30000/1001\n");av_frame_free(&f);av_packet_free(&p);avcodec_free_context(&c);avio_closep(&o->pb);avformat_free_context(o);return 0;}
