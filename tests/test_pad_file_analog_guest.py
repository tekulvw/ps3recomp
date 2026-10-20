"""Compile actual cellPad final guest-copy block against neutral big-endian VM storage."""
from pathlib import Path
import subprocess, tempfile, os
sdk=Path(__file__).resolve().parents[1]
temp=tempfile.TemporaryDirectory(prefix="pad-guest-emission-")
root=Path(temp.name)
source=(sdk/"libs/input/cellPad.c").read_text()
begin=source.index('    {\n        unsigned int ea = (unsigned int)(uintptr_t)data_guest;')
end=source.index('    return CELL_OK;\n}',begin)
block=source[begin:end]
fixture=r'''
#include "pad_file_analog_io.h"
#include "cellPad.h"
#include <assert.h>
#include <stdint.h>
static unsigned char memory[4+CELL_PAD_MAX_CODES*2];
static uint64_t fake_ms=100;
static unsigned long long pad_now_ms(void){return fake_ms;}
static void vm_write32(unsigned long long ea,unsigned int value){assert(ea==0x1000);for(unsigned i=0;i<4;i++)memory[i]=(unsigned char)(value>>(24-8*i));}
static void vm_write16(unsigned long long ea,unsigned int value){assert(ea>=0x1004&&ea+2<=0x1000+sizeof memory);memory[ea-0x1000]=value>>8;memory[ea-0x1000+1]=value;}
static void emit(int positive_len){
    CellPadData d={0};CellPadData*data=&d;CellPadData*data_guest=(CellPadData*)(uintptr_t)0x1000;unsigned port_no=0;
    data->len=positive_len?CELL_PAD_LEN_CHANGE_DEFAULT:0;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]=128;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]=128;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X]=155;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y]=128;
''' +block+r'''
}
int main(int argc,char**argv){assert(argc==2||argc==3);char ack[1024];snprintf(ack,sizeof ack,"%s.ack",argv[1]);unlink(ack);
 if(argc==3){unsetenv("PAD_FILE_ANALOG_NONCE");emit(1);assert(memory[5+CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X*2]==155);assert(access(ack,F_OK)!=0);puts("PASS default-off actual guest axes unchanged");return 0;}
 assert(!setenv("PAD_FILE",argv[1],1));assert(!setenv("PAD_FILE_ANALOG_NONCE","owned",1));
 FILE*f=fopen(argv[1],"w");assert(f);fputs("A1 owned 1 2 500 128 128 200 128\n",f);fclose(f);
 emit(0);assert(access(ack,F_OK)!=0);
 emit(1);assert(memory[4+CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X*2]==0&&memory[5+CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X*2]==200);assert(access(ack,F_OK)!=0);
 emit(1);assert(access(ack,F_OK)!=0);
 emit(1);assert(memory[5+CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X*2]==128);assert(!access(ack,F_OK));
 f=fopen(ack,"r");assert(f);char line[512];assert(fgets(line,sizeof line,f));fclose(f);assert(strstr(line,"\"delivered\":2")&&strstr(line,"\"neutral_delivered\":true"));
 puts("PASS actual cellPad guest-emission block: BE axes, no len0/early ACK, actual neutral write before ACK");
}
'''
(root/'copied_guest_emission_fixture.c').write_text(fixture)
subprocess.run(['cc','-std=gnu17','-Wall','-Wextra','-Werror','-I',str(sdk/'libs/input'),'-I',str(sdk/'include'),str(root/'copied_guest_emission_fixture.c'),'-o',str(root/'test_copied_guest_emission')],check=True)
subprocess.run([str(root/'test_copied_guest_emission'),str(root/'emission-input.txt')],check=True)

subprocess.run([str(root/"test_copied_guest_emission"),str(root/"default-off-input.txt"),"off"],check=True)
