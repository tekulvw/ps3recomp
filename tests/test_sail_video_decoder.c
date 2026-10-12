#include "../libs/codec/sail_video_decoder.h"
#include <assert.h>
#include <string.h>
int main(int argc,char**argv) {
 assert(argc==3);
 FILE* f=fopen(argv[1],"rb");assert(f);
 SailVideoFrame out={0};
 assert(sail_video_available());
 assert(fseek(f,0,SEEK_END)==0);long size=ftell(f);assert(size>0);
 assert(sail_video_decode_picture(f,0,(uint64_t)size,0x1e0,30,&out)==SAIL_VIDEO_OK);
 assert(out.width==64&&out.height==48&&out.pitch==256&&out.ordinal==30);
 assert(out.pts_valid&&out.pts==135090&&out.time_base_num==1&&out.time_base_den==90000);
 assert(ftell(f)==size);
 assert(out.rate_num==30000&&out.rate_den==1001);
 assert(out.colorspace==1&&out.color_range==1); // public FFmpeg BT709 / MPEG enums
 for(unsigned i=3;i<out.pitch*out.height;i+=4)assert(out.rgba[i]==255);
 FILE* pixels=fopen(argv[2],"wb");assert(pixels);
 assert(fwrite(out.rgba,1,out.pitch*out.height,pixels)==out.pitch*out.height);assert(fclose(pixels)==0);
 sail_video_frame_release(&out);assert(!out.rgba&&!out.width);
 assert(sail_video_decode_picture(f,0,(uint64_t)size,0x1e1,30,&out)==SAIL_VIDEO_NOT_FOUND);
 assert(sail_video_decode_picture(f,0,(uint64_t)size,0x1e0,100,&out)==SAIL_VIDEO_NOT_FOUND);
 assert(sail_video_decode_picture(f,0,(uint64_t)size+1,0x1e0,30,&out)==SAIL_VIDEO_INVALID);
 assert(sail_video_decode_picture(f,0,(uint64_t)size,0x1e0,0,&out)==SAIL_VIDEO_INVALID);
 assert(sail_video_decode_picture(f,0,(uint64_t)size,0,30,&out)==SAIL_VIDEO_INVALID);
 assert(sail_video_decode_picture(f,UINT64_MAX,1,0x1e0,30,&out)==SAIL_VIDEO_INVALID);
 assert(sail_video_decode_picture(f,0,64,0x1e0,30,&out)!=SAIL_VIDEO_OK);
 assert(sail_video_decode_picture(f,0,(uint64_t)size/2,0x1e0,30,&out)==SAIL_VIDEO_DECODE);
 assert(!out.rgba&&!out.width);
 assert(sail_video_decode_picture(f,0,(uint64_t)size,0x1e0,32,&out)==SAIL_VIDEO_OK);
 assert(!out.pts_valid&&out.pts==INT64_MIN);sail_video_frame_release(&out);
 // Embed the payload between bytes that bounded AVIO must never consume.
 FILE* wrapped=tmpfile();assert(wrapped);unsigned char block[64]={0};
 assert(fwrite(block,1,64,wrapped)==64);assert(fseek(f,0,SEEK_SET)==0);
 int ch;while((ch=fgetc(f))!=EOF)assert(fputc(ch,wrapped)!=EOF);
 assert(fwrite(block,1,64,wrapped)==64);assert(fseek(wrapped,7,SEEK_SET)==0);
 assert(sail_video_decode_picture(wrapped,64,(uint64_t)size,0x1e0,30,&out)==SAIL_VIDEO_OK);
 assert(ftell(wrapped)==7);assert(out.pts==135090);sail_video_frame_release(&out);
 assert(sail_video_decode_picture(wrapped,0,64,0x1e0,30,&out)!=SAIL_VIDEO_OK);
 assert(ftell(wrapped)==7&&!out.rgba&&!out.width);assert(fclose(wrapped)==0);
 assert(fclose(f)==0);

 puts("PASS actual MPEG-2 frame, alpha, PTS, selected ES and bounded/error cases");return 0;
}
