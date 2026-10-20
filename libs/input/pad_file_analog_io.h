#ifndef PAD_FILE_ANALOG_IO_H
#define PAD_FILE_ANALOG_IO_H
#include "pad_file_analog.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <fcntl.h>
static atomic_flag pad_analog_lock=ATOMIC_FLAG_INIT;
static atomic_int pad_analog_configured=0; /* 0 unknown,1 off,2 on */
static PadAnalogState pad_analog_runtime;
static char pad_analog_path[3800],pad_analog_nonce[65];
static int pad_analog_token(const char*s){
    if(!s||!*s||strlen(s)>64)return 0;
    for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-'))return 0;
    return 1;
}
/* The caller holds this lock across the actual final guest copy and commit.
 * Default-off calls return before taking the lock after first initialization. */
static int pad_file_analog_begin(uint64_t now,uint8_t axes[4],int*active){
    *active=0;
    if(atomic_load_explicit(&pad_analog_configured,memory_order_acquire)==1)return 0;
    /* Another guest caller owns this optional diagnostic state. Keep this
     * report on the ordinary path; a later positive report retries admission. */
    if(atomic_flag_test_and_set_explicit(&pad_analog_lock,memory_order_acquire))return 0;
    if(!atomic_load_explicit(&pad_analog_configured,memory_order_relaxed)){
        const char*p=getenv("PAD_FILE"),*n=getenv("PAD_FILE_ANALOG_NONCE");
        if(p&&p[0]=='/'&&strlen(p)<sizeof pad_analog_path&&pad_analog_token(n)){
            strcpy(pad_analog_path,p);strcpy(pad_analog_nonce,n);
            atomic_store_explicit(&pad_analog_configured,2,memory_order_release);
        }else atomic_store_explicit(&pad_analog_configured,1,memory_order_release);
    }
    if(atomic_load_explicit(&pad_analog_configured,memory_order_relaxed)!=2){
        atomic_flag_clear_explicit(&pad_analog_lock,memory_order_release);return 0;
    }
    int fd=open(pad_analog_path,O_RDWR|O_NOFOLLOW);
    if(fd>=0){
        struct stat st;char line[256];ssize_t n=-1;
        if(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==getuid()&&st.st_size>0&&st.st_size<(off_t)sizeof line)
            n=read(fd,line,(size_t)st.st_size);
        if(n>0&&n==st.st_size){
            line[n]=0;PadAnalogCommand c;
            if(!memchr(line,0,(size_t)n)&&pad_analog_parse(line,pad_analog_nonce,now,&c)==1
               &&c.sequence>pad_analog_runtime.last_sequence
               &&(!pad_analog_runtime.phase||!c.reports)){
                /* Truncate only this inspected inode, never a replacement path.
                 * Rejection leaves the request intact for controller diagnosis. */
                if(!ftruncate(fd,0))pad_analog_admit(&pad_analog_runtime,&c);
            }
        }
        close(fd);
    }
    *active=pad_analog_axes(&pad_analog_runtime,now,axes);
    return 1;
}
static int pad_file_analog_commit(int positive_len,long pid){
    int result=0;
    if(pad_analog_emitted(&pad_analog_runtime,positive_len)){
        char ack[3900],tmp[4000];
        snprintf(ack,sizeof ack,"%s.ack",pad_analog_path);
        snprintf(tmp,sizeof tmp,"%s.tmp.%ld.%llu",ack,pid,(unsigned long long)pad_analog_runtime.command.sequence);
        int fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        if(fd>=0){
            FILE*f=fdopen(fd,"w");
            if(f){
                int ok=fprintf(f,"{\"nonce\":\"%s\",\"sequence\":%llu,\"pid\":%ld,\"delivered\":%u,\"requested\":%u,\"status\":\"%s\",\"axes\":[%u,%u,%u,%u],\"neutral_delivered\":true}\n",
                    pad_analog_nonce,(unsigned long long)pad_analog_runtime.command.sequence,pid,
                    pad_analog_runtime.delivered,pad_analog_runtime.command.reports,
                    pad_analog_runtime.expired?"expired":pad_analog_runtime.command.reports?"consumed":"released",
                    pad_analog_runtime.command.axes[0],pad_analog_runtime.command.axes[1],pad_analog_runtime.command.axes[2],pad_analog_runtime.command.axes[3])>0;
                if(fclose(f))ok=0;
                if(ok&&!rename(tmp,ack))result=1;else unlink(tmp);
            }else{close(fd);unlink(tmp);}
        }
        if(!result){fprintf(stderr,"[cellPad] analog terminal ACK write failed\n");result=-1;}
    }
    atomic_flag_clear_explicit(&pad_analog_lock,memory_order_release);
    return result;
}
#endif
