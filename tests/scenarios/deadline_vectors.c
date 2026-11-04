#include "fake_hal.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

int main(void)
{
    unsigned phase, sample;
    /* 由正常生命週期進入待測狀態；只由 fake HAL 控制時鐘，不改寫 deadline。 */
    for (phase=0; phase<3; ++phase) for (sample=0; sample<3; ++sample) {
        struct fake_hal f;
        struct fw_hal hal;
        struct fw_context c;
        const char *names[]={"quiesce","reset","ready"};
        unsigned elapsed=(phase==2?999:99)+sample;
        fake_init(&f,&hal);
        assert(fw_init(&c,&hal)==FW_OK);
        assert(fw_create(&c,0,8)==FW_OK);
        assert(fw_vf_event(&c,0,FW_COMMIT)==FW_OK);
        f.hw[0].pci_present=1;
        fake_config(&f,0);
        assert(fw_rearm(&c,0,1,1)==FW_OK);
        f.tick=500;
        assert(fake_fence(&f,0)==FW_OK);
        assert(fw_vf_event(&c,0,FW_FENCE)==FW_OK);
        if (phase>=1) assert(fw_vf_event(&c,0,FW_DRAIN_OK)==FW_OK);
        if (phase==2) assert(fw_vf_event(&c,0,FW_RESET_DONE)==FW_OK);
        fw_u64 deadline=c.vf[0].deadline;
        f.tick=500+elapsed;
        fw_status rc=fw_poll_deadlines(&c);
        assert(fw_check_invariants(&c)==FW_OK);
        printf("{\"phase\":\"%s\",\"elapsed\":%u,\"start\":500,\"deadline\":%" PRIu64
               ",\"status_code\":%u,\"state\":%u,\"fenced\":%u,\"killed\":%u,\"reserve\":%u,\"used_credit\":%u}\n",
               names[phase],elapsed,(uint64_t)deadline,(unsigned)rc,(unsigned)c.vf[0].state,
               f.hw[0].fenced,f.hw[0].killed,c.vf[0].reserve,c.used_credit);
    }
    return 0;
}
