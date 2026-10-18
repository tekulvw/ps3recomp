#define _POSIX_C_SOURCE 200809L
#include "../render_ablation.h"
#include <assert.h>
#include <stdlib.h>
int main(int argc,char **argv) {
    assert(argc==2);
    const int expected=atoi(argv[1]);
    rsx_render_ablation_snapshot s;
    if(expected<0) {
        assert(rsx_render_ablation_init()==-1);
        assert(rsx_render_ablation_arm()==-1);
        rsx_render_ablation_get_snapshot(&s);
        assert(s.config_status==-1 && s.active_mode==0);
        assert(!rsx_render_ablation_skip_prepare() && !rsx_render_ablation_skip_draw());
        return 0;
    }
    assert(rsx_render_ablation_init()==0);
    rsx_render_ablation_get_snapshot(&s);
    assert(s.bytes==sizeof(s) && s.configured_mode==(unsigned)expected && s.active_mode==0 && s.config_status==0);
    assert(!rsx_render_ablation_skip_prepare() && !rsx_render_ablation_skip_draw());
    assert(rsx_render_ablation_arm()==expected);
    assert(rsx_render_ablation_skip_prepare()==(expected==2));
    assert(rsx_render_ablation_skip_draw()==(expected!=0));
    rsx_render_ablation_get_snapshot(&s);
    if(s.metrics_enabled) {
        assert(s.cpu_end_observed==2 && s.gpu_draw_observed==2);
        assert(s.cpu_end_skipped==(unsigned)(expected==2));
        assert(s.gpu_draw_skipped==(unsigned)(expected!=0));
    } else assert(s.cpu_end_observed==0 && s.gpu_draw_observed==0);
    /* Configuration is immutable even if a caller changes its environment. */
    assert(setenv("PS3_RENDER_ABLATE_DRAW","1",1)==0);
    assert(setenv("PS3_RENDER_ABLATE_PREPARE","1",1)==0);
    assert(rsx_render_ablation_init()==0 && rsx_render_ablation_arm()==expected);
    return 0;
}
