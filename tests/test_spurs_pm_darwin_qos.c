/* Actual PM entry on real Darwin pthreads; manufactured stopped instance. */
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <errno.h>
#include <assert.h>
static atomic_uint set_calls,get_calls,id_calls;
static int fail_set,fail_get,fail_id,set_result;
static qos_class_t before_class,after_class;
static int before_relative,after_relative;
static int observed_set(qos_class_t q,int rel)
{
    atomic_fetch_add(&set_calls,1);
    assert(q==QOS_CLASS_USER_INITIATED && rel==0);
    return set_result=fail_set?EPERM:pthread_set_qos_class_self_np(q,rel);
}
static int observed_get(pthread_t t,qos_class_t* q,int* rel)
{
    unsigned n=atomic_fetch_add(&get_calls,1);
    int rc=fail_get?EINVAL:pthread_get_qos_class_np(t,q,rel);
    if(!rc){if(!n){before_class=*q;before_relative=*rel;}else{after_class=*q;after_relative=*rel;}}
    return rc;
}
static int observed_id(pthread_t t,uint64_t* id)
{atomic_fetch_add(&id_calls,1);return fail_id?EINVAL:pthread_threadid_np(t,id);}
#define pthread_set_qos_class_self_np observed_set
#define pthread_get_qos_class_np observed_get
#define pthread_threadid_np observed_id
#define main prior_scheduler_main
#include "test_spurs_scheduler.c"
#undef main
#undef pthread_threadid_np
#undef pthread_get_qos_class_np
#undef pthread_set_qos_class_self_np
static void* enter_pm(void* p)
{
    errno=E2BIG;
    DWORD rc=spurs_pm_worker(p);
    assert(rc==0 && errno==E2BIG);return NULL;
}
static void* unrelated(void* p){(void)p;return NULL;}
int main(int argc,char** argv)
{
    assert(argc==2);const char* mode=argv[1];
    unsetenv("SPURS_PM_DARWIN_QOS");
    if(!strcmp(mode,"on"))setenv("SPURS_PM_DARWIN_QOS","1",1);
    else if(!strcmp(mode,"off"))setenv("SPURS_PM_DARWIN_QOS","0",1);
    else if(!strcmp(mode,"cap"))setenv("SPURS_PM_DARWIN_QOS","0",1);
    else if(!strcmp(mode,"invalid"))setenv("SPURS_PM_DARWIN_QOS","yes",1);
    else if(!strcmp(mode,"set-error")){setenv("SPURS_PM_DARWIN_QOS","1",1);fail_set=1;}
    else if(!strcmp(mode,"get-error")){setenv("SPURS_PM_DARWIN_QOS","1",1);fail_get=1;}
    else if(!strcmp(mode,"id-error")){setenv("SPURS_PM_DARWIN_QOS","1",1);fail_id=1;}
    struct SpursInst instance={0};instance.stopping=1;
    struct SpursPmSlot slot={0};slot.instance=&instance;slot.spu_num=3;
    pthread_t thread;assert(!pthread_create(&thread,NULL,unrelated,NULL));assert(!pthread_join(thread,NULL));
    assert(!atomic_load(&set_calls) && !atomic_load(&get_calls));
    unsigned repeats=!strcmp(mode,"cap")?130:1;
    for(unsigned i=0;i<repeats;++i){
        assert(!pthread_create(&thread,NULL,enter_pm,&slot));assert(!pthread_join(thread,NULL));
    }
    int enabled=!strcmp(mode,"on")||fail_set||fail_get||fail_id;
    int observed=enabled||!strcmp(mode,"off")||!strcmp(mode,"cap");
    assert(atomic_load(&set_calls)==(unsigned)enabled);
    assert(atomic_load(&get_calls)==(unsigned)(observed*2)*repeats);
    assert(atomic_load(&id_calls)==(unsigned)observed*repeats);
    if(enabled && !fail_set)assert(!set_result);
    if(!fail_get && observed){
        if(enabled && !fail_set)assert(after_class==QOS_CLASS_USER_INITIATED && after_relative==0);
        else assert(before_class==after_class && before_relative==after_relative);
    }
    puts("PASS real PM startup QoS, original worker return and unrelated pthread isolation");return 0;
}
