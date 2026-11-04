#include "fake_hal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#expr); return 1; } } while (0)

static void init(struct fake_hal *f, struct fw_context *c)
{
    struct fw_hal hal;
    fake_init(f,&hal); assert(fw_init(c,&hal)==FW_OK);
}
static void fixture(struct fake_hal *f, struct fw_context *c, enum fw_state state)
{
    init(f,c);
    if (state!=FW_FREE) { assert(fw_create(c,0,8)==FW_OK); c->vf[0].state=state; }
    c->vf[0].deadline=100; f->tick=100;
    if (state==FW_FAILED) f->hw[0].killed=1;
}

static int edges_test(void)
{
    /* 本表由規格的二十條邊逐項抄錄，並非由核心表產生。 */
    static const int expected[8][14]={
        {1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
        {-1,2,0,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
        {-1,-1,-1,-1,3,-1,-1,-1,-1,-1,5,7,-1,-1},
        {-1,-1,-1,-1,-1,4,6,-1,-1,-1,5,-1,-1,-1},
        {-1,-1,-1,-1,-1,-1,6,5,-1,-1,5,-1,-1,-1},
        {-1,-1,-1,-1,-1,-1,-1,-1,2,6,5,7,-1,-1},
        {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,7,-1,-1},
        {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,0,6}
    };
    struct fake_hal f;
    struct fw_context c,before;
    unsigned s,e,legal=0,illegal=0,calls;
    fw_status rc;
    for (s=0;s<8;++s) for (e=0;e<14;++e) {
        fixture(&f,&c,(enum fw_state)s);
        if (e==FW_FENCE || e==FW_FLR || e==FW_DESTROY) CHECK(fake_fence(&f,0)==FW_OK);
        if (e==FW_READY_REARM) { fake_config(&f,0); f.tick=99; }
        before=c; calls=f.calls;
        rc=fw_vf_event(&c,0,(enum fw_event)e);
        if (expected[s][e]<0) {
            CHECK(rc==FW_STATE); CHECK(memcmp(&before,&c,sizeof(c))==0); CHECK(f.calls==calls); ++illegal;
        } else {
            CHECK(rc==FW_OK); CHECK((int)c.vf[0].state==expected[s][e]); CHECK(fw_check_invariants(&c)==FW_OK); ++legal;
        }
    }
    CHECK(legal==20 && illegal==92);
    before=c; CHECK(fw_vf_event(&c,0,FW_EVENT_COUNT)==FW_STATE); CHECK(memcmp(&before,&c,sizeof(c))==0);
    CHECK(fw_vf_event(&c,8,FW_CREATE)==FW_INVAL); CHECK(fw_vf_event(NULL,0,FW_CREATE)==FW_INVAL);
    printf("{\"case\":\"fsm_edges\",\"legal\":%u,\"illegal_unchanged\":%u,\"status\":\"PASS\"}\n",legal,illegal);
    return 0;
}

static int lifecycle_test(void)
{
    struct fake_hal f;
    struct fw_context c,before;
    struct fw_snapshot snap;
    unsigned i;
    init(&f,&c);
    for (i=0;i<8;++i) { CHECK(fw_create(&c,(fw_u16)i,8)==FW_OK); CHECK(fw_vf_event(&c,(fw_u16)i,FW_COMMIT)==FW_OK); }
    CHECK(c.used_credit==64 && c.used_vectors==16);
    CHECK(fw_set_reserve(&c,0,9)==FW_NOSPC); CHECK(fw_set_reserve(&c,0,0)==FW_INVAL);
    fake_config(&f,0); CHECK(fw_rearm(&c,0,1,1)==FW_OK);
    CHECK(fw_set_reserve(&c,0,4)==FW_STATE);
    CHECK(fake_fence(&f,0)==FW_OK); CHECK(fw_vf_event(&c,0,FW_FENCE)==FW_OK);
    CHECK(fw_vf_event(&c,0,FW_DRAIN_OK)==FW_OK); CHECK(fw_vf_event(&c,0,FW_RESET_DONE)==FW_OK);
    CHECK(f.hw[0].fenced && !f.hw[0].config_valid && c.vf[0].state==FW_RECOVERING);
    CHECK(fw_rearm(&c,0,1,1)==FW_STATE); fake_config(&f,0); CHECK(fw_rearm(&c,0,2,2)==FW_OK);
    CHECK(fake_fence(&f,0)==FW_OK); CHECK(fw_vf_event(&c,0,FW_DESTROY)==FW_OK);
    CHECK(fw_vf_event(&c,0,FW_RELEASE_OK)==FW_OK); CHECK(c.vf[0].epoch==3 && c.used_credit==56);
    CHECK(fw_create(&c,0,9)==FW_NOSPC); CHECK(fw_create(&c,0,8)==FW_OK);
    CHECK(fw_vf_event(&c,0,FW_ABORT_FAIL)==FW_OK); CHECK(c.used_credit==64);
    CHECK(fw_vf_event(&c,0,FW_DESTROY)==FW_OK); CHECK(fw_vf_event(&c,0,FW_RELEASE_OK)==FW_OK);
    CHECK(fw_create(&c,0,8)==FW_STATE); CHECK(f.hw[0].killed);
    CHECK(fake_global(&f,&c)==FW_OK); CHECK(c.session==2 && c.used_credit==0 && f.hw[0].fenced && !f.hw[0].killed);
    CHECK(fw_snapshot(&c,&snap)==FW_OK && snap.session==2);
    CHECK(fw_check_invariants(&c)==FW_OK);
    before=c; f.closed[0]=0; CHECK(fake_global(&f,&c)==FW_STATE); CHECK(memcmp(&before,&c,sizeof(c))==0);
    for (i=99;i<=101;++i) {
        fixture(&f,&c,FW_QUIESCING); f.tick=i;
        CHECK(fw_poll_deadlines(&c)==FW_OK); CHECK(c.vf[0].state==(i<100?FW_QUIESCING:FW_FAILED));
        fixture(&f,&c,FW_RESETTING); f.tick=i;
        CHECK(fw_poll_deadlines(&c)==FW_OK); CHECK(c.vf[0].state==(i<100?FW_RESETTING:FW_FAILED));
    }
    fixture(&f,&c,FW_RECOVERING); f.hw[0].pci_present=1; f.tick=99; CHECK(fw_poll_deadlines(&c)==FW_OK && c.vf[0].state==FW_RECOVERING);
    f.tick=100; CHECK(fw_poll_deadlines(&c)==FW_OK && c.vf[0].state==FW_FAILED);
    for (i=999;i<=1001;++i) {
        fixture(&f,&c,FW_RESETTING); f.tick=0;
        CHECK(fw_vf_event(&c,0,FW_RESET_DONE)==FW_OK && c.vf[0].deadline==1000);
        f.hw[0].pci_present=1; f.tick=i;
        CHECK(fw_poll_deadlines(&c)==FW_OK && c.vf[0].state==(i<1000?FW_RECOVERING:FW_FAILED));
    }
    fixture(&f,&c,FW_RECOVERING); f.tick=10000; before=c;
    CHECK(fw_poll_deadlines(&c)==FW_OK && !memcmp(&before,&c,sizeof(c)) && !f.hw[0].killed);
    puts("{\"case\":\"ledger_reset_deadlines\",\"status\":\"PASS\"}"); return 0;
}

static struct fw_request request(const struct fw_context *c, fw_u32 seq, fw_u32 op, fw_u32 target, fw_u32 a)
{
    struct fw_request r;
    fw_zero(&r,sizeof(r)); r.session=c->session; r.seq=seq; r.opcode=op; r.target=target; r.arg[0]=a;
    return r;
}

static int mailbox_test(void)
{
    struct fake_hal f;
    struct fw_context c,before;
    struct fw_request r;
    struct fw_reply p,saved;
    unsigned op;
    init(&f,&c); before=c;
    r=request(&c,1,FW_PROVISION,0,8); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_NO_NEG); CHECK(memcmp(&c,&before,sizeof(c))==0);
    r=request(&c,1,FW_VERSION_NEG,FW_GLOBAL,2); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_UNSUP); CHECK(memcmp(&c,&before,sizeof(c))==0);
    r.arg[0]=1; CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK); CHECK(p.value[0]==0x10000);
    before=c; saved=p; CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK); CHECK(memcmp(&c,&before,sizeof(c))==0); CHECK(memcmp(&p,&saved,sizeof(p))==0);
    r.arg[0]=2; CHECK(fw_handle_mailbox(&c,&r,&p)==FW_SEQ_CONFLICT); CHECK(memcmp(&c,&before,sizeof(c))==0);
    r=request(&c,2,FW_PROVISION,0,8); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK);
    before=c; CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK); CHECK(memcmp(&c,&before,sizeof(c))==0 && c.used_credit==8);
    r.seq=1; CHECK(fw_handle_mailbox(&c,&r,&p)==FW_STALE_SEQ); r.seq=0; CHECK(fw_handle_mailbox(&c,&r,&p)==FW_INVAL);
    r.seq=3; --r.session; CHECK(fw_handle_mailbox(&c,&r,&p)==FW_STALE_SEQ);
    for (op=0x20;op<=0x24;++op) { r=request(&c,3,op,FW_GLOBAL,0); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_UNSUP); CHECK(memcmp(&c,&before,sizeof(c))==0); }
    r=request(&c,3,FW_PROVISION,0,8); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_STATE);
    before=c; CHECK(fw_handle_mailbox(&c,&r,&p)==FW_STATE); CHECK(memcmp(&c,&before,sizeof(c))==0);
    r=request(&c,4,FW_SET_RESERVE,0,4); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK && c.used_credit==4);
    r=request(&c,5,FW_GET_STATE,0,0); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK && p.value[0]==FW_ACTIVE);
    CHECK(fake_fence(&f,0)==FW_OK); r=request(&c,6,FW_QUIESCE,0,0); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK);
    r=request(&c,7,FW_RESET,0,0); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK);
    fake_config(&f,0); r=request(&c,8,FW_REARM,0,2); r.arg[2]=1; CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK);
    CHECK(fake_fence(&f,0)==FW_OK); r=request(&c,9,FW_DESTROY_CMD,0,0); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK);
    r=request(&c,FW_U32_MAX,FW_GET_STATE,0,0); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK);
    r.seq=1; CHECK(fw_handle_mailbox(&c,&r,&p)==FW_STALE_SEQ);
    CHECK(fake_global(&f,&c)==FW_OK); r=request(&c,1,FW_VERSION_NEG,FW_GLOBAL,1); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK);
    r=request(&c,2,FW_GLOBAL_RESET,FW_GLOBAL,0); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK);
    CHECK(c.session==3 && !c.cache_valid && !c.negotiated && !c.last_seq);
    CHECK(fw_handle_mailbox(&c,&r,&p)==FW_STALE_SEQ);
    puts("{\"case\":\"mailbox_retry_protocol_rejection\",\"status\":\"PASS\"}"); return 0;
}

static int codec_test(void)
{
    fw_u8 bytes[32],again[32];
    struct fw_sq sq={0x123456789abcdef0ULL,0x1122334455667788ULL,0xaa,4096,0xbeef,1,0}, out;
    struct fw_cq cq={0x1234,0,32,0x123456789abcdef0ULL,0xcbf43926,1}, cp;
    struct fw_queue_config cfg={0x1000,0x3000,1,1,256,256};
    fw_u32 pending;
    unsigned i;
    CHECK(fw_crc32((const fw_u8 *)"123456789",9)==0xcbf43926U); CHECK(fw_crc32(NULL,0)==0);
    CHECK(fw_encode_sq(&sq,bytes)==FW_OK); CHECK(bytes[0]==0xf0 && bytes[7]==0x12 && bytes[14]==0xef && bytes[15]==0xbe);
    CHECK(fw_decode_sq(bytes,&out)==FW_OK); CHECK(fw_encode_sq(&out,again)==FW_OK && memcmp(bytes,again,32)==0);
    bytes[13]=1; CHECK(fw_decode_sq(bytes,&out)==FW_BAD_DESC); bytes[13]=0; bytes[12]=3; CHECK(fw_decode_sq(bytes,&out)==FW_BAD_DESC);
    sq.len=0; CHECK(fw_encode_sq(&sq,bytes)==FW_BAD_DESC); sq.len=4097; CHECK(fw_encode_sq(&sq,bytes)==FW_BAD_DESC);
    sq.len=4; sq.addr=FW_U64_MAX-2; CHECK(fw_encode_sq(&sq,bytes)==FW_BAD_DESC);
    CHECK(fw_encode_cq(&cq,bytes)==FW_OK && bytes[24]==1 && bytes[25]==0); CHECK(fw_decode_cq(bytes,&cp)==FW_OK);
    CHECK(fw_encode_cq(&cp,again)==FW_OK && memcmp(bytes,again,32)==0);
    for (i=25;i<32;++i) { bytes[i]=1; CHECK(fw_decode_cq(bytes,&cp)==FW_BAD_DESC); bytes[i]=0; }
    bytes[24]=2; CHECK(fw_decode_cq(bytes,&cp)==FW_BAD_DESC); cq.status=16; CHECK(fw_encode_cq(&cq,bytes)==FW_BAD_DESC);
    CHECK(fw_queue_validate(&cfg,1,1)==FW_OK); cfg.sq_base++; CHECK(fw_queue_validate(&cfg,1,1)==FW_BAD_QUEUE); cfg.sq_base--;
    cfg.cq_base=cfg.sq_base; CHECK(fw_queue_validate(&cfg,1,1)==FW_BAD_QUEUE); cfg.cq_base=FW_U64_MAX-63;
    CHECK(fw_queue_validate(&cfg,1,1)==FW_BAD_QUEUE); cfg.cq_base=0x3000; cfg.sq_entries=17; CHECK(fw_queue_validate(&cfg,1,1)==FW_BAD_QUEUE);
    cfg.sq_entries=256; CHECK(fw_queue_validate(&cfg,2,1)==FW_BAD_QUEUE); CHECK(fw_queue_validate(&cfg,1,2)==FW_BAD_QUEUE);
    CHECK(fw_ring_pending(7,FW_U32_MAX-8,16,&pending)==FW_OK && pending==16);
    CHECK(fw_ring_pending(8,FW_U32_MAX-8,16,&pending)==FW_BAD_QUEUE); CHECK(fw_ring_pending(0,1,16,&pending)==FW_BAD_QUEUE);
    for (i=16;i<=256;i*=2) { CHECK(fw_ring_phase(0,i)==1); CHECK(fw_ring_phase(i,i)==0); CHECK(fw_ring_phase(2*i,i)==1); }
    CHECK(fw_ring_phase(FW_U32_MAX,16)==0 && fw_ring_phase(0,16)==1 && fw_ring_phase(0,0)==0);
    CHECK(fw_decode_sq(NULL,&out)==FW_INVAL); CHECK(fw_encode_sq(NULL,bytes)==FW_INVAL);
    CHECK(fw_decode_cq(NULL,&cp)==FW_INVAL); CHECK(fw_encode_cq(NULL,bytes)==FW_INVAL);
    CHECK(fw_queue_validate(NULL,1,1)==FW_INVAL); CHECK(fw_ring_pending(0,0,16,NULL)==FW_INVAL);
    puts("{\"case\":\"codec_crc_ring_bounds\",\"status\":\"PASS\"}"); return 0;
}

static int transactions_test(void)
{
    struct fake_hal f;
    struct fw_context c,before;
    struct fw_request r,other;
    struct fw_reply p,saved;
    unsigned point,unwind,tick,calls;
    for (point=FW_CP_BEFORE_CREATE;point<=FW_CP_BEFORE_COMMIT;++point) {
        for (unwind=0;unwind<2;++unwind) {
            init(&f,&c); c.negotiated=1;
            f.checkpoint_failures=(1U<<point)|(unwind?(1U<<FW_CP_ABORT_RELEASE):0);
            r=request(&c,1,FW_PROVISION,0,8);
            CHECK(fw_handle_mailbox(&c,&r,&p)==FW_IO);
            CHECK(f.checkpoint_seen[point]==1);
            CHECK(c.vf[0].state==(unwind && point!=FW_CP_BEFORE_CREATE?FW_FAILED:FW_FREE));
            CHECK(c.used_credit==(unwind && point!=FW_CP_BEFORE_CREATE?8U:0U));
            CHECK(f.hw[0].killed==(unwind && point!=FW_CP_BEFORE_CREATE));
            before=c; saved=p; calls=f.calls;
            CHECK(fw_handle_mailbox(&c,&r,&p)==FW_IO && !memcmp(&before,&c,sizeof(c)));
            CHECK(!memcmp(&saved,&p,sizeof(p)) && calls==f.calls);
            CHECK(fw_check_invariants(&c)==FW_OK);
        }
    }
    fixture(&f,&c,FW_ACTIVE); c.negotiated=1;
    CHECK(fake_fence(&f,0)==FW_OK); f.checkpoint_failures=1U<<FW_CP_DESTROY_RELEASE;
    r=request(&c,1,FW_DESTROY_CMD,0,0); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_IO);
    CHECK(c.vf[0].state==FW_FAILED && c.used_credit==8 && f.hw[0].killed);
    before=c; CHECK(fw_handle_mailbox(&c,&r,&p)==FW_IO && !memcmp(&before,&c,sizeof(c)));
    r.seq=2; CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK && c.used_credit==0 && c.vf[0].state==FW_FREE);
    /* 真正 RELEASE_OK 驗證失敗也必須收斂；非只有 failpoint 路徑。 */
    fixture(&f,&c,FW_ACTIVE); c.negotiated=1; f.hw[0].pci_present=1;
    CHECK(fake_fence(&f,0)==FW_OK); r=request(&c,1,FW_DESTROY_CMD,0,0);
    CHECK(fw_handle_mailbox(&c,&r,&p)==FW_STATE && c.vf[0].state==FW_FAILED && c.used_credit==8);
    for (point=FW_QUIESCE;point<=FW_RESET;++point) for (tick=99;tick<=101;++tick) {
        fixture(&f,&c,FW_ACTIVE); c.negotiated=1; f.tick=0;
        CHECK(fake_fence(&f,0)==FW_OK);
        r=request(&c,1,FW_QUIESCE,0,0);
        if (point==FW_RESET) { CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK); r.seq=2; r.opcode=FW_RESET; }
        f.hold_opcode=point;
        CHECK(fw_handle_mailbox(&c,&r,&p)==FW_BUSY && c.mailbox_pending);
        CHECK(c.vf[0].state==(point==FW_QUIESCE?FW_QUIESCING:FW_RESETTING));
        CHECK(c.vf[0].deadline==100);
        before=c; calls=f.calls;
        CHECK(fw_handle_mailbox(&c,&r,&p)==FW_BUSY && !memcmp(&before,&c,sizeof(c)) && f.calls==calls);
        other=r; other.arg[3]=1; CHECK(fw_handle_mailbox(&c,&other,&p)==FW_SEQ_CONFLICT);
        other=r; ++other.seq; other.opcode=FW_GET_STATE;
        CHECK(fw_handle_mailbox(&c,&other,&p)==FW_BUSY && !memcmp(&before,&c,sizeof(c)));
        f.tick=tick; CHECK(fw_poll_mailbox(&c,&p)==(tick<100?FW_BUSY:FW_TIMEOUT));
        CHECK(c.vf[0].state==(tick<100?(point==FW_QUIESCE?FW_QUIESCING:FW_RESETTING):FW_FAILED));
        CHECK(c.used_credit==8 && f.hw[0].fenced);
        if (tick<100) {
            f.hold_opcode=0;
            CHECK(fw_poll_mailbox(&c,&p)==FW_OK && !c.mailbox_pending);
            CHECK(c.vf[0].state==(point==FW_QUIESCE?FW_RESETTING:FW_RECOVERING));
        } else CHECK(f.hw[0].killed && !c.mailbox_pending);
        saved=p; before=c;
        CHECK(fw_handle_mailbox(&c,&r,&p)==saved.status && !memcmp(&before,&c,sizeof(c)) && !memcmp(&saved,&p,sizeof(p)));
        CHECK(fw_check_invariants(&c)==FW_OK);
    }
    for (tick=999;tick<=1001;++tick) {
        fixture(&f,&c,FW_RESETTING); f.tick=0;
        CHECK(fw_vf_event(&c,0,FW_RESET_DONE)==FW_OK); fake_config(&f,0); f.tick=tick;
        CHECK(fw_rearm(&c,0,1,1)==(tick<1000?FW_OK:FW_TIMEOUT));
        CHECK(c.vf[0].state==(tick<1000?FW_ACTIVE:FW_FAILED));
    }
    fixture(&f,&c,FW_ACTIVE); c.negotiated=1; f.tick=0; CHECK(fake_fence(&f,0)==FW_OK);
    f.hold_opcode=FW_QUIESCE; r=request(&c,1,FW_QUIESCE,0,0);
    CHECK(fw_handle_mailbox(&c,&r,&p)==FW_BUSY);
    CHECK(fake_fence(&f,0)==FW_OK); CHECK(fw_vf_event(&c,0,FW_FLR)==FW_OK);
    CHECK(fw_poll_mailbox(&c,&p)==FW_STATE && !c.mailbox_pending && c.vf[0].state==FW_RECOVERING);
    CHECK(fw_poll_mailbox(&c,&p)==FW_STATE); CHECK(fw_poll_mailbox(NULL,&p)==FW_INVAL);
    CHECK(fw_poll_mailbox(&c,NULL)==FW_INVAL);
    puts("{\"case\":\"transaction_unwind_async_deadlines\",\"status\":\"PASS\"}"); return 0;
}

static int scheduler_test(void)
{
    struct fake_hal f;
    struct fw_context c,before;
    fw_u32 backlog[8]={100,100,100,100,0,0,0,0}, service[8];
    unsigned i,j;
    init(&f,&c);
    for (i=0;i<4;++i) { CHECK(fw_create(&c,(fw_u16)i,8)==FW_OK); CHECK(fw_vf_event(&c,(fw_u16)i,FW_COMMIT)==FW_OK); fake_config(&f,(fw_u16)i); CHECK(fw_rearm(&c,(fw_u16)i,1,1)==FW_OK); }
    for (j=0;j<1000;++j) { CHECK(fw_schedule_round(&c,backlog,service)==FW_OK); for (i=0;i<4;++i) CHECK(service[i]==16); }
    backlog[1]=8; backlog[2]=8; backlog[3]=8; CHECK(fw_schedule_round(&c,backlog,service)==FW_OK && service[0]==40);
    for (i=1;i<4;++i) CHECK(service[i]==8);
    backlog[1]=0; backlog[2]=0; backlog[3]=0; CHECK(fw_schedule_round(&c,backlog,service)==FW_OK && service[0]==64 && !service[1]);
    f.hw[0].outstanding=1; CHECK(fw_schedule_round(&c,backlog,service)==FW_OK && !service[0]); f.hw[0].outstanding=0;
    c.round=FW_U64_MAX; before=c; CHECK(fw_schedule_round(&c,backlog,service)==FW_OVERFLOW && !memcmp(&before,&c,sizeof(c)));
    c.round=0; c.vf[0].serviced=FW_U64_MAX; before=c; CHECK(fw_schedule_round(&c,backlog,service)==FW_OVERFLOW && !memcmp(&before,&c,sizeof(c)));
    CHECK(fw_schedule_round(NULL,backlog,service)==FW_INVAL);
    puts("{\"case\":\"scheduler_reserve_capacity\",\"status\":\"PASS\"}"); return 0;
}

static int failure_test(void)
{
    struct fake_hal f;
    struct fw_context c,before;
    struct fw_hal hal;
    struct fw_snapshot snap;
    struct fw_request r;
    struct fw_reply p;
    fw_u64 epochs[8];
    unsigned i;
    fake_init(&f,&hal); CHECK(fw_init(NULL,&hal)==FW_INVAL); CHECK(fw_init(&c,NULL)==FW_INVAL);
    f.failures=1; CHECK(fw_init(&c,&hal)==FW_IO); f.failures=0; f.hw[0].fenced=0; CHECK(fw_init(&c,&hal)==FW_STATE);
    f.hw[0].fenced=1; f.hw[1].session=2; CHECK(fw_init(&c,&hal)==FW_STATE);
    init(&f,&c); f.failures=1; CHECK(fw_create(&c,0,8)==FW_IO); CHECK(fw_snapshot(&c,&snap)==FW_IO); CHECK(fw_check_invariants(&c)==FW_IO);
    f.failures=0; CHECK(fw_snapshot(NULL,&snap)==FW_INVAL); CHECK(fw_check_invariants(NULL)==FW_INVAL);
    CHECK(fw_create(&c,8,8)==FW_INVAL); CHECK(fw_create(&c,0,65)==FW_INVAL); CHECK(fw_set_reserve(&c,0,8)==FW_STATE);
    fixture(&f,&c,FW_ACTIVE); CHECK(fake_fence(&f,0)==FW_OK); before=c; f.failures=2;
    CHECK(fw_vf_event(&c,0,FW_FENCE)==FW_IO && !memcmp(&before,&c,sizeof(c)));
    f.failures=0; f.tick=FW_U64_MAX; CHECK(fw_vf_event(&c,0,FW_FENCE)==FW_OVERFLOW);
    fixture(&f,&c,FW_RESETTING); f.failures=4; CHECK(fw_vf_event(&c,0,FW_RESET_DONE)==FW_IO);
    fixture(&f,&c,FW_RECOVERING); f.tick=99; fake_config(&f,0); f.failures=8; before=c;
    CHECK(fw_rearm(&c,0,1,1)==FW_IO && !memcmp(&before,&c,sizeof(c)) && f.hw[0].fenced);
    f.failures=16; CHECK(fw_vf_event(&c,0,FW_VERIFY_FAIL)==FW_IO); f.hw[0].pci_present=1; f.tick=100; CHECK(fw_poll_deadlines(&c)==FW_IO);
    fixture(&f,&c,FW_ACTIVE); CHECK(fw_vf_event(&c,0,FW_FENCE)==FW_STATE);
    f.hw[0].fenced=0; CHECK(fw_vf_event(&c,0,FW_DESTROY)==FW_STATE); f.hw[0].fenced=1;
    CHECK(fake_fence(&f,0)==FW_OK); f.closed[0]=0; CHECK(fw_vf_event(&c,0,FW_DESTROY)==FW_STATE);
    fixture(&f,&c,FW_QUIESCING); f.hw[0].outstanding=1; CHECK(fw_vf_event(&c,0,FW_DRAIN_OK)==FW_STATE);
    f.hw[0].outstanding=0; f.tick=99; CHECK(fw_vf_event(&c,0,FW_EV_TIMEOUT)==FW_STATE);
    fixture(&f,&c,FW_RESETTING); f.hw[0].outstanding=1; CHECK(fw_vf_event(&c,0,FW_RESET_DONE)==FW_STATE);
    fixture(&f,&c,FW_ACTIVE); CHECK(fake_fence(&f,0)==FW_OK); f.hw[0].config_valid=1; CHECK(fw_vf_event(&c,0,FW_FLR)==FW_STATE);
    fixture(&f,&c,FW_PROVISIONING); f.hw[0].config_valid=1; CHECK(fw_vf_event(&c,0,FW_COMMIT)==FW_STATE); CHECK(fw_vf_event(&c,0,FW_ABORT_OK)==FW_STATE);
    init(&f,&c); for(i=0;i<8;++i) epochs[i]=2;
    CHECK(fw_global_recover(NULL,2,epochs)==FW_INVAL); CHECK(fw_global_recover(&c,1,epochs)==FW_STATE);
    CHECK(fw_global_recover(&c,2,epochs)==FW_STATE); f.failures=1; CHECK(fw_global_recover(&c,2,epochs)==FW_IO);
    f.failures=0; c.session=FW_U64_MAX; CHECK(fw_global_recover(&c,1,epochs)==FW_OVERFLOW);
    init(&f,&c); c.vf[0].epoch=FW_U64_MAX; CHECK(fw_global_recover(&c,2,epochs)==FW_OVERFLOW);
    init(&f,&c); f.hw[0].epoch=FW_U64_MAX; CHECK(fake_fence(&f,0)==FW_OVERFLOW && f.hw[0].killed);
    init(&f,&c); c.vf[0].reserve=1; CHECK(fw_check_invariants(&c)==FW_STATE);
    init(&f,&c); c.used_credit=1; CHECK(fw_check_invariants(&c)==FW_STATE);
    init(&f,&c); f.hw[0].outstanding=2; CHECK(fw_check_invariants(&c)==FW_STATE);
    init(&f,&c); f.hw[0].fenced=0; CHECK(fw_check_invariants(&c)==FW_STATE);
    init(&f,&c); f.hw[0].session=2; CHECK(fw_snapshot(&c,&snap)==FW_STATE);
    init(&f,&c); r=request(&c,1,FW_VERSION_NEG,FW_GLOBAL,1); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_OK);
    r=request(&c,2,FW_GET_STATE,8,0); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_INVAL);
    r=request(&c,2,FW_PROVISION,0,0); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_INVAL);
    r=request(&c,2,FW_REARM,0,1); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_INVAL);
    r=request(&c,2,FW_GET_STATE,0,1); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_INVAL);
    r=request(&c,2,FW_GLOBAL_RESET,0,0); CHECK(fw_handle_mailbox(&c,&r,&p)==FW_INVAL);
    r.target=FW_GLOBAL; c.hal.begin_global=NULL; CHECK(fw_handle_mailbox(&c,&r,&p)==FW_UNSUP);
    CHECK(fw_handle_mailbox(NULL,&r,&p)==FW_INVAL); CHECK(fw_poll_deadlines(NULL)==FW_INVAL);
    puts("{\"case\":\"hal_errors_extremes\",\"status\":\"PASS\"}"); return 0;
}

int main(void)
{
    if (edges_test() || lifecycle_test() || mailbox_test() || codec_test() || scheduler_test() || failure_test() || transactions_test()) return 1;
    puts("CORE_UNIT_RESULT=PASS"); return 0;
}
