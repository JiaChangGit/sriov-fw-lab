/* T07/T08 的管理端流程；由 labd.c 納入，共用 PF 控制與 owner 握手函式。 */
struct recovery_run { const char *scenario; unsigned serial; };
/* 測試同步點以 token 配對 host ACK；QMP 操作完成後才繼續，不用固定 sleep 猜進度。 */
static int recovery_barrier(struct recovery_run *run,const char *fields)
{
    struct termios old,raw;
    char token[64],expected[96],line[96];
    size_t used=0;
    int ret=-1,saved;
    if (tcgetattr(STDIN_FILENO,&old)) return -1;
    raw=old; cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO,TCSANOW,&raw)) return -1;
    snprintf(token,sizeof(token),"recovery-%u",++run->serial);
    snprintf(expected,sizeof(expected),"M3_HOST_ACK %s\n",token);
    printf("{\"event\":\"M3_BARRIER\",\"scenario\":\"%s\",\"phase\":\"request\",\"barrier\":\"%s\",%s}\n",run->scenario,token,fields);
    fflush(stdout);
    int64_t deadline=lab_now_ms()+LAB_TIMEOUT_MS;
    while (used<sizeof(line)-1) {
        if (lab_wait(STDIN_FILENO,POLLIN,deadline)) goto out;
        ssize_t n=read(STDIN_FILENO,line+used,1);
        if (n<0&&errno==EINTR) continue;
        if (n!=1) goto out;
        if (line[used++]=='\n') {
            line[used]=0;
            if (strcmp(line,expected)) { errno=EPROTO; goto out; }
            ret=0; goto out;
        }
    }
    errno=EOVERFLOW;
out:
    saved=errno;
    if (tcsetattr(STDIN_FILENO,TCSANOW,&old)) return -1;
    errno=saved; return ret;
}
static int recovery_counters(struct admin *a,const char *phase,unsigned expected)
{
    char record[1536],*line;
    if (lab_read_text("/sys/kernel/debug/toy_pf0/vf-stats",record,sizeof(record))) return -1;
    for (line=record;line&&*line;line=strchr(line,'\n'),line=line?line+1:NULL) {
        unsigned vf,desc,cpl,dma,blocked,irq;
        if (sscanf(line,"vf=%u stale_desc=%u stale_cpl=%u dma_fault=%u fence_blocked=%u irq_suppressed=%u",&vf,&desc,&cpl,&dma,&blocked,&irq)!=6) { errno=EPROTO; return -1; }
        if (vf!=a->vf) continue;
        if (desc||cpl!=expected||dma||irq) { errno=EPROTO; return -1; }
        printf("{\"event\":\"RECOVERY_COUNTERS\",\"phase\":\"%s\",\"vf\":%u,\"stale_desc\":%u,\"stale_cpl\":%u,\"dma_fault\":%u,\"fence_blocked\":%u,\"irq_suppressed\":%u}\n",phase,vf,desc,cpl,dma,blocked,irq);
        return 0;
    }
    errno=ENOENT; return -1;
}
/* 保留一筆工作後執行 fence/reset，等待新設定與 READY，再 REARM；B 的進度另行核對。 */
static int recovery_handshake(struct admin *a,struct admin *b,struct recovery_run *run)
{
    struct toy_vf_ctl ctl;
    struct lab_message m;
    if (recovery_barrier(run,"\"action\":\"qmp-set\",\"property\":\"fault-vf\",\"value\":0")||
        recovery_barrier(run,"\"action\":\"qmp-set\",\"property\":\"fault-after\",\"value\":0")||
        recovery_barrier(run,"\"action\":\"qmp-set\",\"property\":\"delay-cpl-ticks\",\"value\":60000")||
        owner_run(a)||snapshot(a)||a->info.outstanding!=1||a->info.state!=TOY_STATES_ACTIVE||
        recovery_barrier(run,"\"action\":\"pending-check\",\"expected\":1")) return -1;
    printf("{\"event\":\"RECOVERY_LOADED\",\"vf\":0,\"epoch\":%" PRIu64 ",\"outstanding\":%u,\"state\":%u,\"flags\":%u}\n",(uint64_t)a->info.epoch,a->info.outstanding,a->info.state,a->info.hw_flags);
    uint64_t old_epoch=a->info.epoch;
    m=a->current; m.type=LAB_STOP_REQUEST; m.payload_len=16; memset(m.payload,0,16);
    printf("{\"event\":\"RECOVERY_STOP_REQUEST\",\"vf\":0,\"epoch\":%" PRIu64 "}\n",m.epoch);
    if (healthy_checkpoint(b,"pending")||lab_send(a->connection,&m,lab_now_ms()+LAB_TIMEOUT_MS)||
        control(a,TOY_FENCE_VF,0,&ctl)||tenant_message(a,&m,LAB_STOPPED,4)||
        lab_tuple(&m,a->original.vf,a->original.session,a->original.epoch,a->original.lease_token,a->original.operation)||
        m.ready_gen!=a->original.ready_gen||snapshot(a)||
        control(a,TOY_RESET_VF,0,&ctl)||reset_snapshot(a,"recovering",TOY_STATES_RECOVERING,0)||
        a->info.epoch<=old_epoch||recovery_counters(a,"reset",0)||
        recovery_barrier(run,"\"action\":\"pending-check\",\"expected\":0")||
        recovery_barrier(run,"\"action\":\"qmp-set\",\"property\":\"delay-cpl-ticks\",\"value\":0")) return -1;
    m=a->current; m.type=LAB_RESET_NOTICE; m.epoch=a->info.epoch; m.payload_len=16;
    memset(m.payload,0,16); lab_put32(m.payload+4,a->info.hw_flags); lab_put32(m.payload+8,a->info.outstanding);
    a->original=m;
    if (lab_send(a->connection,&m,lab_now_ms()+LAB_TIMEOUT_MS)||tenant_message(a,&m,LAB_HELLO,5)||snapshot(a)||
        lab_tuple(&m,a->original.vf,a->original.session,a->original.epoch,a->original.lease_token,a->original.operation)||
        m.ready_gen!=a->original.ready_gen||a->info.epoch!=m.epoch) { errno=ESTALE; return -1; }
    a->current=m;
    if (recovery_barrier(run,"\"action\":\"virtual-delay\",\"ticks\":200")||healthy_checkpoint(b,"slow_ready")||
        owner_inspect(a)||reset_snapshot(a,"slow_ready",TOY_STATES_RECOVERING,0)) return -1;
    if (m.ready_gen==UINT32_MAX) { errno=EOVERFLOW; return -1; }
    m.type=LAB_HELLO_REPLY; ++m.ready_gen; a->original=m;
    if (lab_send(a->connection,&m,lab_now_ms()+LAB_TIMEOUT_MS)||tenant_message(a,&m,LAB_READY,6)||
        reset_snapshot(a,"fresh_config",TOY_STATES_RECOVERING,1)||
        lab_tuple(&m,a->original.vf,a->original.session,a->original.epoch,a->original.lease_token,a->original.operation)||
        m.ready_gen!=a->original.ready_gen||a->info.ready_gen!=m.ready_gen||a->info.hw_flags!=61) { errno=ESTALE; return -1; }
    a->current=m;
    if (a->recovery_case==8) {
        if (recovery_barrier(run,"\"action\":\"qmp-set\",\"property\":\"try-stale-cpl\",\"value\":true")||
            recovery_counters(a,"callback_fenced",1)) return -1;
    } else if (recovery_barrier(run,"\"action\":\"virtual-delay\",\"ticks\":50")) return -1;
    if (owner_inspect(a)||healthy_checkpoint(b,"configured_fenced")||control(a,TOY_REARM_VF,m.ready_gen,&ctl)||
        reset_snapshot(a,"rearmed",TOY_STATES_ACTIVE,1)) return -1;
    if (a->recovery_case==8) {
        if (recovery_barrier(run,"\"action\":\"qmp-set\",\"property\":\"try-stale-cpl\",\"value\":true")||
            recovery_counters(a,"callback_rearmed",2)||owner_inspect(a)) return -1;
    }
    if (owner_run(a)||recovery_counters(a,"finished",a->recovery_case==8?2:0)||
        healthy_checkpoint(b,"fresh_io")||owner_inspect(a)) return -1;
    return 0;
}
static int recovery_case(const char *pf,const char *device,const char *tenant,unsigned number)
{
    const char *scenario=number==7?"T07":"T08";
    struct recovery_run run={.scenario=scenario};
    struct admin owners[2];
    char count_path[PATH_MAX];
    bool enabled=false;
    int ret=1,saved;
    snprintf(count_path,sizeof(count_path),"/sys/bus/pci/devices/%s/sriov_numvfs",pf);
    for (unsigned i=0;i<2;i++) owners[i]=(struct admin){.pf=-1,.listen=-1,.connection=-1,.pidfd=-1,.management=-1,
        .vf=i,.failpoint="",.fail_hit=1,.scenario=scenario,.count_path=count_path,.healthy=i,.recovery_case=number};
    if (lab_write_text(count_path,"4\n")) goto out;
    enabled=true;
    for (unsigned i=0;i<2;i++) if (isolated_owner_start(&owners[i],pf,device,tenant,16,4096)) goto out;
    struct admin *a=&owners[0],*b=&owners[1];
    struct lab_message m=b->current;
    m.type=LAB_RUN;
    if (lab_send(b->connection,&m,lab_now_ms()+LAB_TIMEOUT_MS)||owner_run(a)||
        recovery_handshake(a,b,&run)) goto out;
    m=b->current; m.type=LAB_STOP_REQUEST; m.payload_len=16; memset(m.payload,0,16);
    if (lab_send(b->connection,&m,lab_now_ms()+LAB_TIMEOUT_MS)||tenant_message(b,&m,LAB_STOPPED,3)||
        lab_tuple(&m,b->original.vf,b->original.session,b->original.epoch,b->original.lease_token,b->original.operation)||
        m.ready_gen!=b->original.ready_gen) goto out;
    b->current=m;
    for (unsigned i=0;i<2;i++) if (owner_stop_begin(&owners[i])) goto out;
    for (unsigned i=0;i<2;i++) if (owner_stop_finish(&owners[i])||release_owner(&owners[i])) goto out;
    if (lab_write_text(count_path,"0\n")) goto out;
    enabled=false;
    uint64_t count;
    if (lab_read_u64(count_path,&count)||count) { errno=EBUSY; goto out; }
    for (unsigned i=0;i<4;i++) {
        char path[PATH_MAX]; struct stat st;
        snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/virtfn%u",pf,i);
        if (!lstat(path,&st)||errno!=ENOENT) { errno=EBUSY; goto out; }
    }
    for (unsigned i=0;i<8;i++) {
        struct toy_vf_info info={.hdr=header(sizeof(info)),.vf=i};
        if (ioctl(a->pf,TOY_GET_VF,&info)||info.state||info.reserve||info.outstanding||info.lease_token||info.hw_flags!=1) { errno=EIO; goto out; }
        printf("{\"event\":\"RECOVERY_FINAL\",\"vf\":%u,\"state\":%u,\"reserve\":%u,\"outstanding\":%u,\"flags\":%u,\"lease\":%" PRIu64 "}\n",i,info.state,info.reserve,info.outstanding,info.hw_flags,(uint64_t)info.lease_token);
    }
    printf("{\"event\":\"M3_CASE\",\"id\":\"%s\",\"status\":\"PASS\",\"pending_at_reset\":1,\"callbacks\":%u,\"all_free\":true,\"owner_closed\":true}\n",scenario,number==8?2:0);
    puts("LAB_RESULT=PASS"); ret=0;
out:
    saved=errno;
    for (unsigned i=0;i<2;i++) isolated_owner_cleanup(&owners[i]);
    if (enabled&&lab_write_text(count_path,"0\n")) fprintf(stderr,"LABD_DISABLE_FAILED errno=%d\n",errno);
    if (ret) {
        fprintf(stderr,"LABD_ERROR scenario=%s errno=%d message=%s\n",scenario,saved,strerror(saved));
        printf("{\"event\":\"M3_CASE\",\"id\":\"%s\",\"status\":\"FAIL\",\"errno\":%d}\n",scenario,saved);
        puts("LAB_RESULT=FAIL");
    }
    return ret;
}
