/* Exercise the actual non-Windows implementation without creating a GPU. */
#undef _WIN32
#include "../rsx_live_draw.c"
static void lease(void) {}
static int barrier(void) { return 1; }
int main(void)
{
    rsx_live_draw_set_report_barrier(barrier);
    if (!rsx_live_draw_set_report_lease(lease,lease) ||
        rsx_live_draw_set_report_lease(lease,NULL) ||
        !rsx_live_draw_set_report_lease(NULL,NULL)) return 1;
    return rsx_live_draw_report_batch_lifetime()!=0 ||
        rsx_live_draw_report_batch_admit(1)!=0 ||
        rsx_live_draw_report_batch_issue(1)!=0 ||
        rsx_live_draw_report_batch_status(1,1,1)!=-1;
}
