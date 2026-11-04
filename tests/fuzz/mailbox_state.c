#include "fake_hal.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct fake_hal f;
    struct fw_hal hal;
    struct fw_context c,before;
    struct fw_request req;
    struct fw_reply reply,again;
    size_t p;
    unsigned j;
    fw_status rc;
    fake_init(&f,&hal); assert(fw_init(&c,&hal)==FW_OK);
    for(p=0;p+8<=size;p+=8) {
        fw_zero(&req,sizeof(req)); req.session=c.session; req.seq=(fw_u32)data[p]+1;
        req.opcode=data[p+1]%40U; req.target=(data[p+2]&8U)?FW_GLOBAL:data[p+2]%8U;
        for(j=0;j<4;++j) req.arg[j]=data[p+3+j];
        if(data[p+7]&1U) { req.opcode=FW_VERSION_NEG; req.target=FW_GLOBAL; req.arg[0]=1; req.arg[1]=0; req.arg[2]=0; req.arg[3]=0; }
        if(data[p+7]&2U) { req.opcode=FW_PROVISION; req.target=data[p+2]%8U; req.arg[0]=data[p+3]%65U; req.arg[1]=0; req.arg[2]=0; req.arg[3]=0; }
        before=c;
        rc=fw_handle_mailbox(&c,&req,&reply);
        if(rc==FW_INVAL || rc==FW_UNSUP || rc==FW_NO_NEG || rc==FW_SEQ_CONFLICT || rc==FW_STALE_SEQ)
            assert(memcmp(&before,&c,sizeof(c))==0);
        if(c.session==req.session && c.cache_valid && c.last_seq==req.seq && rc!=FW_SEQ_CONFLICT) {
            before=c; assert(fw_handle_mailbox(&c,&req,&again)==rc);
            assert(memcmp(&before,&c,sizeof(c))==0 && memcmp(&reply,&again,sizeof(reply))==0);
        }
        assert(fw_check_invariants(&c)==FW_OK);
    }
    return 0;
}
