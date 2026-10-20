#define _DARWIN_C_SOURCE
#include "../libs/input/pad_file_analog_io.h"
#include <assert.h>
static void publish(const char*path,const char*line){FILE*f=fopen(path,"w");assert(f);assert(fputs(line,f)>=0);assert(!fclose(f));}
int main(int argc,char**argv){
    assert(argc==2);const char*path=argv[1];char ack[1024];assert(snprintf(ack,sizeof ack,"%s.ack",path)>0);
    unlink(ack);
    /* Contention must not spin or touch axes/state; ordinary caller proceeds. */
    assert(!atomic_flag_test_and_set(&pad_analog_lock));
    uint8_t untouched_axes[4]={1,2,3,4};int untouched_active=99;
    assert(!pad_file_analog_begin(100,untouched_axes,&untouched_active));
    assert(!untouched_active&&untouched_axes[0]==1&&untouched_axes[3]==4);
    atomic_flag_clear(&pad_analog_lock);
    assert(!setenv("PAD_FILE",path,1));assert(!setenv("PAD_FILE_ANALOG_NONCE","owned",1));
    publish(path,"0x4000 4\n");uint8_t a[4];int active;
    assert(pad_file_analog_begin(100,a,&active)&&!active);assert(pad_file_analog_commit(1,321)==0);
    char line[256];FILE*f=fopen(path,"r");assert(fgets(line,sizeof line,f)&&!strcmp(line,"0x4000 4\n"));fclose(f);
    publish(path,"A1 owned 1 2 500 128 128 200 128\n");
    assert(pad_file_analog_begin(100,a,&active)&&active&&a[2]==200);
    assert(access(ack,F_OK)!=0);assert(!pad_file_analog_commit(0,321));
    assert(pad_file_analog_begin(101,a,&active)&&active);assert(!pad_file_analog_commit(1,321));
    assert(pad_file_analog_begin(102,a,&active)&&active);assert(!pad_file_analog_commit(1,321));
    assert(access(ack,F_OK)!=0);
    assert(pad_file_analog_begin(103,a,&active)&&active);for(unsigned i=0;i<4;i++)assert(a[i]==128);
    assert(pad_file_analog_commit(1,321)==1);
    f=fopen(ack,"r");assert(f&&fgets(line,sizeof line,f));fclose(f);
    assert(strstr(line,"\"sequence\":1")&&strstr(line,"\"delivered\":2")&&strstr(line,"\"pid\":321")&&strstr(line,"\"neutral_delivered\":true"));
    publish(path,"A1 foreign 2 1 500 128 128 255 128\n");
    assert(pad_file_analog_begin(200,a,&active)&&!active);assert(!pad_file_analog_commit(1,321));
    f=fopen(path,"r");assert(fgets(line,sizeof line,f)&&strstr(line,"foreign"));fclose(f);
    publish(path,"A1 owned 1 2 500 128 128 200 128\n");
    assert(pad_file_analog_begin(200,a,&active)&&!active);assert(!pad_file_analog_commit(1,321));
    publish(path,"A1 owned 2 120 500 0 255 0 255\n");
    assert(pad_file_analog_begin(200,a,&active)&&active&&a[0]==0&&a[1]==255);assert(!pad_file_analog_commit(1,321));
    assert(pad_file_analog_begin(500,a,&active)&&active);assert(pad_file_analog_commit(1,321)==1);
    f=fopen(ack,"r");assert(f&&fgets(line,sizeof line,f));fclose(f);assert(strstr(line,"\"status\":\"expired\"")&&strstr(line,"\"delivered\":1"));
    publish(path,"A1 owned 3 120 900 128 128 200 128\n");
    assert(pad_file_analog_begin(501,a,&active)&&active);assert(!pad_file_analog_commit(1,321));
    publish(path,"A1 owned 4 0 900 128 128 128 128\n");
    assert(pad_file_analog_begin(502,a,&active)&&active);for(unsigned i=0;i<4;i++)assert(a[i]==128);
    assert(pad_file_analog_commit(1,321)==1);
    f=fopen(ack,"r");assert(f&&fgets(line,sizeof line,f));fclose(f);assert(strstr(line,"\"status\":\"released\""));
    unlink(path);assert(!symlink(ack,path));
    assert(pad_file_analog_begin(503,a,&active)&&!active);assert(!pad_file_analog_commit(1,321));
    f=fopen(ack,"r");assert(f&&fgets(line,sizeof line,f));fclose(f);assert(strstr(line,"\"status\":\"released\""));
    unlink(path);
    puts("PASS real file admission, legacy unchanged, no drain/early ACK, final neutral ACK");
    puts("PASS foreign/replayed request refusal, exact axes, expiry, explicit release, symlink refusal");
}
