#ifndef PAD_FILE_ANALOG_WIN32_H
#define PAD_FILE_ANALOG_WIN32_H
#include "pad_file_analog.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static atomic_flag pad_analog_lock=ATOMIC_FLAG_INIT;
static atomic_int pad_analog_configured=0; /* 0 unknown,1 off,2 on */
static PadAnalogState pad_analog_runtime;
static char pad_analog_path[512],pad_analog_nonce[65];
static int pad_analog_canceled;
static int pad_file_analog_enabled(void){return atomic_load_explicit(&pad_analog_configured,memory_order_acquire)==2;}
static int pad_file_windows_ordinary(HANDLE file){
    BY_HANDLE_FILE_INFORMATION info;
    return GetFileType(file)==FILE_TYPE_DISK && GetFileInformationByHandle(file,&info)
        && !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
}
static int pad_analog_token(const char*s){
    if(!s||!*s||strlen(s)>64)return 0;
    for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-'))return 0;
    return 1;
}
/* Reuse the digital mailbox's focus/discard decision. A1 additionally requires
 * a session nonce and focus opt-in; no configured channel means no file IO. */
static int pad_file_analog_begin(uint64_t now,uint8_t axes[4],int*active,int allowed){
    *active=0;
    if(atomic_load_explicit(&pad_analog_configured,memory_order_acquire)==1)return 0;
    if(atomic_flag_test_and_set_explicit(&pad_analog_lock,memory_order_acquire))return 0;
    if(!atomic_load_explicit(&pad_analog_configured,memory_order_relaxed)){
        const char*p=getenv("PAD_FILE"),*n=getenv("PAD_FILE_ANALOG_NONCE"),*focus=getenv("PAD_FILE_REQUIRE_FOCUS");
        size_t length=p?strlen(p):0;
        if(length>=3 && length<sizeof pad_analog_path && isalpha((unsigned char)p[0]) && p[1]==':'
           && (p[2]=='/'||p[2]=='\\') && pad_analog_token(n) && focus&&!strcmp(focus,"1")){
            strcpy(pad_analog_path,p);strcpy(pad_analog_nonce,n);
            atomic_store_explicit(&pad_analog_configured,2,memory_order_release);
        }else atomic_store_explicit(&pad_analog_configured,1,memory_order_release);
    }
    if(atomic_load_explicit(&pad_analog_configured,memory_order_relaxed)!=2){
        atomic_flag_clear_explicit(&pad_analog_lock,memory_order_release);return 0;
    }
    if(!allowed){
        if(pad_analog_runtime.phase){pad_analog_runtime.phase=2;pad_analog_canceled=1;}
    }else{
        HANDLE file=CreateFileA(pad_analog_path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,NULL,
            OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        if(file!=INVALID_HANDLE_VALUE){
            LARGE_INTEGER size;DWORD count=0;
            char line[256];PadAnalogCommand command;int consumed=0;
            if(pad_file_windows_ordinary(file) && GetFileSizeEx(file,&size) && size.QuadPart>0 && size.QuadPart<(LONGLONG)sizeof line
               && ReadFile(file,line,(DWORD)size.QuadPart,&count,NULL) && count==(DWORD)size.QuadPart){
                line[count]=0;
                if(!memchr(line,0,count) && pad_analog_parse(line,pad_analog_nonce,now,&command)==1
                   && command.sequence>pad_analog_runtime.last_sequence
                   && (!pad_analog_runtime.phase||!command.reports)){
                    LARGE_INTEGER zero;zero.QuadPart=0;
                    consumed=SetFilePointerEx(file,zero,NULL,FILE_BEGIN)&&SetEndOfFile(file);
                }
            }
            const int closed=CloseHandle(file)!=0;
            if(consumed&&closed){pad_analog_admit(&pad_analog_runtime,&command);pad_analog_canceled=0;}
        }
    }
    *active=pad_analog_axes(&pad_analog_runtime,now,axes);
    return 1;
}
/* The same lock covers exact BE guest serialization and positive-report
 * accounting. A terminal ACK follows the actual neutral packet, never a drain. */
static int pad_file_analog_commit(int positive_len,long pid){
    int result=0;
    if(pad_analog_emitted(&pad_analog_runtime,positive_len)){
        char ack[520],tmp[600],body[1024];
        snprintf(ack,sizeof ack,"%s.ack",pad_analog_path);
        snprintf(tmp,sizeof tmp,"%s.tmp.%ld.%llu",ack,pid,(unsigned long long)pad_analog_runtime.command.sequence);
        int count=snprintf(body,sizeof body,"{\"nonce\":\"%s\",\"sequence\":%llu,\"pid\":%ld,\"delivered\":%u,\"requested\":%u,\"status\":\"%s\",\"axes\":[%u,%u,%u,%u],\"neutral_delivered\":true}\n",
            pad_analog_nonce,(unsigned long long)pad_analog_runtime.command.sequence,pid,
            pad_analog_runtime.delivered,pad_analog_runtime.command.reports,
            pad_analog_canceled?"canceled":pad_analog_runtime.expired?"expired":pad_analog_runtime.command.reports?"consumed":"released",
            pad_analog_runtime.command.axes[0],pad_analog_runtime.command.axes[1],pad_analog_runtime.command.axes[2],pad_analog_runtime.command.axes[3]);
        HANDLE file=CreateFileA(tmp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        if(file!=INVALID_HANDLE_VALUE){
            DWORD written=0;
            int ok=count>0&&count<(int)sizeof body && WriteFile(file,body,(DWORD)count,&written,NULL)
                && written==(DWORD)count && FlushFileBuffers(file);
            if(!CloseHandle(file))ok=0;
            if(ok&&MoveFileExA(tmp,ack,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))result=1;
            else DeleteFileA(tmp);
        }
        if(!result){fprintf(stderr,"[cellPad] analog terminal ACK write failed\n");result=-1;}
    }
    atomic_flag_clear_explicit(&pad_analog_lock,memory_order_release);
    return result;
}
#endif
