#include "fake_hal.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct fake_hal f;
    struct fw_hal hal;
    struct fw_context c;
    struct fw_sq sq;
    struct fw_cq cq;
    struct fw_queue_config cfg;
    fw_u8 encoded[32];
    fw_u32 backlog[8],service[8],sum=0;
    unsigned i;
    if(size<32) return 0;
    if(fw_decode_sq(data,&sq)==FW_OK) {
        assert(fw_encode_sq(&sq,encoded)==FW_OK && !memcmp(data,encoded,32));
        assert(sq.len>=1 && sq.len<=4096 && sq.addr<=FW_U64_MAX-sq.len);
    }
    if(fw_decode_cq(data,&cq)==FW_OK) assert(fw_encode_cq(&cq,encoded)==FW_OK && !memcmp(data,encoded,32));
    cfg.sq_base=(fw_u64)data[0]*64; cfg.cq_base=(fw_u64)data[1]*64;
    cfg.sq_entries=data[2]; cfg.cq_entries=data[3]; cfg.session=1; cfg.epoch=1;
    (void)fw_queue_validate(&cfg,1,1);
    fake_init(&f,&hal); assert(fw_init(&c,&hal)==FW_OK);
    for(i=0;i<8;++i) {
        assert(fw_create(&c,(fw_u16)i,1U+data[8+i]%8U)==FW_OK);
        assert(fw_vf_event(&c,(fw_u16)i,FW_COMMIT)==FW_OK);
        fake_config(&f,(fw_u16)i); assert(fw_rearm(&c,(fw_u16)i,1,1)==FW_OK);
        if(data[16+i]&1U) f.hw[i].fenced=1;
        backlog[i]=data[24+i];
    }
    c.round=data[7];
    assert(fw_schedule_round(&c,backlog,service)==FW_OK);
    for(i=0;i<8;++i) {
        assert(service[i]<=backlog[i]);
        if(f.hw[i].fenced) assert(!service[i]);
        else assert(service[i]>=(backlog[i]<c.vf[i].reserve?backlog[i]:c.vf[i].reserve));
        sum+=service[i];
    }
    assert(sum<=64 && fw_check_invariants(&c)==FW_OK);
    return 0;
}
