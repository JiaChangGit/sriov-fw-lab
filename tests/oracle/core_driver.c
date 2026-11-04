#include "fake_hal.h"
#include <stdio.h>
#include <stdlib.h>

static void emit(const struct fw_context *c, const struct fake_hal *f, fw_status rc,
                 const struct fw_reply *reply, const fw_u32 service[8])
{
    unsigned i;
    printf("%u",(unsigned)rc);
    for(i=0;i<4;++i) printf(" %u",reply->value[i]);
    for(i=0;i<8;++i) printf(" %u",service[i]);
    printf(" %llu %llu %u %u %u %u %u %u",(unsigned long long)c->session,(unsigned long long)c->round,
        c->used_queues,c->used_vectors,c->used_credit,c->last_seq,c->negotiated,c->cache_valid);
    printf(" %u %llu %llu %u %u",c->mailbox_pending,(unsigned long long)c->pending_epoch,
        (unsigned long long)c->pending_deadline,f->hold_opcode,f->checkpoint_failures);
    for(i=0;i<FW_CP_COUNT;++i) printf(" %u",f->checkpoint_seen[i]);
    for(i=0;i<8;++i) {
        const struct fw_vf *v=&c->vf[i];
        const struct fw_hw_snapshot *h=&f->hw[i];
        printf(" %u %u %llu %llu %llu %llu %llu %llu %u %u %u %u %u %u %u %u %u",
            (unsigned)v->state,v->reserve,(unsigned long long)v->epoch,(unsigned long long)v->deadline,
            (unsigned long long)v->serviced,(unsigned long long)h->session,(unsigned long long)h->epoch,
            (unsigned long long)h->token,h->ready_gen,h->outstanding,h->fenced,h->killed,h->bme,
            h->config_valid,h->ready_pending,h->pci_present,f->closed[i]);
    }
    putchar('\n');
}

int main(void)
{
    struct fake_hal f;
    struct fw_hal hal;
    struct fw_context c;
    char line[512],*p,*end,op;
    unsigned long long a[12];
    unsigned i,n;
    fw_status rc;
    fw_u32 service[8],backlog[8];
    struct fw_request req;
    struct fw_reply reply;
    fake_init(&f,&hal);
    if(fw_init(&c,&hal)!=FW_OK) return 2;
    while(fgets(line,sizeof(line),stdin)) {
        op=line[0]; p=line+1; n=0;
        while(n<12) { a[n]=strtoull(p,&end,10); if(end==p) break; ++n; p=end; }
        for(i=n;i<12;++i) a[i]=0;
        fw_zero(&reply,sizeof(reply)); fw_zero(service,sizeof(service)); rc=FW_INVAL;
        switch(op) {
        case 'C': rc=fw_create(&c,(fw_u16)a[0],(fw_u32)a[1]); break;
        case 'E': rc=fw_vf_event(&c,(fw_u16)a[0],(enum fw_event)a[1]); break;
        case 'F':
            if(a[0]>=8) break;
            rc=fake_fence(&f,(fw_u16)a[0]);
            if(a[1] && rc==FW_OK) { f.hw[a[0]].config_valid=0; f.hw[a[0]].ready_pending=0; }
            break;
        case 'Q': if(a[0]<8) { fake_config(&f,(fw_u16)a[0]); rc=FW_OK; } break;
        case 'A': rc=fw_rearm(&c,(fw_u16)a[0],(fw_u64)a[1],(fw_u32)a[2]); break;
        case 'R': rc=fw_set_reserve(&c,(fw_u16)a[0],(fw_u32)a[1]); break;
        case 'G': rc=fake_global(&f,&c); break;
        case 'T': f.tick=(fw_u64)a[0]; rc=FW_OK; break;
        case 'P': rc=fw_poll_deadlines(&c); break;
        case 'B': rc=fw_poll_mailbox(&c,&reply); break;
        case 'H': f.hold_opcode=(fw_u32)a[0]; rc=FW_OK; break;
        case 'X': f.checkpoint_failures=(fw_u32)a[0]; rc=FW_OK; break;
        case 'O': if(a[0]<8) { f.closed[a[0]]=(fw_u8)a[1]; rc=FW_OK; } break;
        case 'S':
            for(i=0;i<8;++i) backlog[i]=(fw_u32)a[i];
            rc=fw_schedule_round(&c,backlog,service); break;
        case 'M':
            fw_zero(&req,sizeof(req)); req.session=a[0]; req.seq=(fw_u32)a[1]; req.opcode=(fw_u32)a[2]; req.target=(fw_u32)a[3];
            for(i=0;i<4;++i) req.arg[i]=(fw_u32)a[4+i];
            rc=fw_handle_mailbox(&c,&req,&reply); break;
        default: return 3;
        }
        emit(&c,&f,rc,&reply,service);
    }
    return ferror(stdin)?4:0;
}
