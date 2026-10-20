#include "../libs/input/pad_file_analog.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    PadAnalogCommand c={0};
    assert(pad_analog_parse("0x4000 4\n","owned",100,&c)==0);
    assert(pad_analog_parse("A1 owned 1 2 500 128 128 200 128\n","owned",100,&c)==1);
    assert(c.sequence==1 && c.reports==2 && c.deadline_ms==500 && c.axes[2]==200);
    const char*bad[]={"A1 wrong 2 1 500 128 128 200 128","A1 owned 0 1 500 128 128 200 128","A1 owned 2 121 500 128 128 200 128","A1 owned 2 1 100 128 128 200 128","A1 owned 2 1 10101 128 128 200 128","A1 owned 2 1 500 -1 128 200 128","A1 owned 2 1 500 256 128 200 128","A1 owned 2 1 500 128 128 200 128 extra","A1 owned 2 1 500 128 128 200","A1 owned 18446744073709551616 1 500 128 128 200 128","A1 owned 2 0 500 128 128 200 128"};
    PadAnalogCommand preserved=c;
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++){assert(pad_analog_parse(bad[i],"owned",100,&c)==-1);assert(!memcmp(&c,&preserved,sizeof c));}
    assert(pad_analog_parse("0x0040 4 99", "owned",100,&c)==0);
    PadAnalogState s={0};uint8_t axes[4]={9,9,9,9};
    assert(pad_analog_admit(&s,&c));assert(!pad_analog_admit(&s,&c));
    assert(pad_analog_axes(&s,100,axes)&&axes[2]==200);
    for(unsigned i=0;i<100;i++)assert(!pad_analog_emitted(&s,0));
    assert(s.delivered==0);assert(!pad_analog_emitted(&s,1));assert(s.delivered==1);
    assert(!pad_analog_emitted(&s,1));assert(s.phase==2);
    assert(pad_analog_axes(&s,200,axes));for(unsigned i=0;i<4;i++)assert(axes[i]==128);
    assert(!pad_analog_emitted(&s,0));assert(s.phase==2);
    assert(pad_analog_emitted(&s,1));assert(!s.phase&&s.delivered==2&&!s.expired);
    c.sequence=2;assert(pad_analog_admit(&s,&c));assert(!pad_analog_emitted(&s,1));
    assert(pad_analog_axes(&s,500,axes)&&s.expired);assert(pad_analog_emitted(&s,1));assert(s.delivered==1);
    c.sequence=3;assert(pad_analog_admit(&s,&c));c.sequence=4;assert(!pad_analog_admit(&s,&c));
    c.reports=0;memset(c.axes,128,4);assert(pad_analog_admit(&s,&c));
    assert(pad_analog_axes(&s,501,axes));assert(pad_analog_emitted(&s,1));assert(s.delivered==0);
    puts("PASS valid tagged analog and untouched legacy classification");
    puts("PASS malformed/bounds/overflow/nonce preservation, drain/hold/expiry/neutral release/replay");
}
