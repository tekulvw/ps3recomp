/* Real GetInfo API, synthetic guest memory and resolver; no network/DNS. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#define gethostname fixture_gethostname
#define getaddrinfo fixture_getaddrinfo
#define freeaddrinfo fixture_freeaddrinfo
#include "../libs/network/cellNetCtl.c"
#undef gethostname
#undef getaddrinfo
#undef freeaddrinfo
#include <assert.h>
static unsigned char memory[4096];
u8*vm_base=memory;
static unsigned hostname_calls,resolver_calls,free_calls;
int fixture_gethostname(char*out,size_t bytes){++hostname_calls;snprintf(out,bytes,"fixture-host");return 0;}
int fixture_getaddrinfo(const char*host,const char*service,const struct addrinfo*hints,struct addrinfo**out)
{
 assert(!strcmp(host,"fixture-host")&&!service&&hints->ai_family==AF_INET);++resolver_calls;
 static struct sockaddr_in address;static struct addrinfo result;
 memset(&address,0,sizeof address);address.sin_family=AF_INET;assert(inet_pton(AF_INET,"10.0.0.4",&address.sin_addr)==1);
 memset(&result,0,sizeof result);result.ai_addr=(struct sockaddr*)&address;*out=&result;return 0;
}
void fixture_freeaddrinfo(struct addrinfo*info){assert(info);++free_calls;}
const char*np_psnr_identity(void){return NULL;}
void cellSysutilQueueEvent(int slot,u32 status,u32 param){(void)slot;(void)status;(void)param;}
int g_resv_store_active;uint32_t g_ww_lo,g_ww_hi;
void ppu_resv_break_store(uint64_t ea){(void)ea;}int spu_coh_is_reserved(uint32_t ea){(void)ea;return 0;}
void spu_lockline_lock(void){}void spu_lockline_unlock(void){}void spu_coh_notify_write(uint32_t ea){(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
int main(void)
{
 const u32 ea=0x100;CellNetCtlInfo*guest=(void*)(uintptr_t)ea;CellNetCtlInfo*host=(void*)(memory+ea);
 assert(cellNetCtlInit()==CELL_OK);
 setenv("PS3_NETCTL_IPV4_OVERRIDE","192.0.2.68",1);memset(memory,0xA5,sizeof memory);
 assert(cellNetCtlGetInfo(CELL_NET_CTL_INFO_IP_ADDRESS,guest)==CELL_OK);
 assert(!strcmp(host->ip_address,"192.0.2.68")&&!hostname_calls&&!resolver_calls);
 assert(memory[ea-1]==0xA5&&memory[ea+sizeof *host]==0xA5);
 const char*valid[]={"0.0.0.0","127.0.0.1","255.255.255.255","1.2.3.4"};
 for(unsigned i=0;i<sizeof valid/sizeof *valid;i++){setenv("PS3_NETCTL_IPV4_OVERRIDE",valid[i],1);assert(cellNetCtlGetInfo(CELL_NET_CTL_INFO_IP_ADDRESS,guest)==CELL_OK);assert(!strcmp(host->ip_address,valid[i]));}
 const char*invalid[]={"","1.2.3","1.2.3.4x","1.2.3.4 "," 1.2.3.4","01.2.3.4","256.1.2.3","-1.2.3.4","1..2.3","1.2.3.4.5","1234.1.1.1","localhost","::1","1234567890123456"};
 for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++){
  setenv("PS3_NETCTL_IPV4_OVERRIDE",invalid[i],1);memset(host,0xA5,sizeof *host);
  assert(cellNetCtlGetInfo(CELL_NET_CTL_INFO_IP_ADDRESS,guest)==(s32)CELL_NET_CTL_ERROR_NOT_AVAIL);
  for(unsigned b=0;b<sizeof *host;b++)assert(!((u8*)host)[b]);
 }
 assert(!hostname_calls&&!resolver_calls);
 unsetenv("PS3_NETCTL_IPV4_OVERRIDE");assert(cellNetCtlGetInfo(CELL_NET_CTL_INFO_IP_ADDRESS,guest)==CELL_OK);
 assert(!strcmp(host->ip_address,"10.0.0.4")&&hostname_calls==1&&resolver_calls==1&&free_calls==1);
 vm_write32(ea,0xdeadbeef);assert(cellNetCtlGetInfo(CELL_NET_CTL_INFO_MTU,guest)==CELL_OK);assert(vm_read32(ea)==1500);
 puts("PASS: real NetCtl API valid/invalid/unset override, exact guest bytes/guards, no DNS under override and unchanged unset resolver route");
}
