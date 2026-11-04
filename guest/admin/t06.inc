/* 共用同一個 labd 的 lease 與 pidfd 管理，不借用其他程序的 VFIO fd。 */
static int reset_snapshot(struct admin *a,const char *phase,unsigned state,unsigned config)
{
    if (snapshot(a)||a->info.state!=state||a->info.reserve!=8||a->info.outstanding||
        a->info.lease_token!=a->lease||(a->info.hw_flags&TOY_HW_FLAGS_KILLED)||
        !!(a->info.hw_flags&TOY_HW_FLAGS_CONFIG_VALID)!=config) { errno=EPROTO; return -1; }
    printf("{\"event\":\"%s_SNAPSHOT\",\"phase\":\"%s\",\"pid\":%d,\"vf\":%u,\"state\":%u,\"epoch\":%" PRIu64 ",\"session\":%" PRIu64 ",\"ready_gen\":%u,\"flags\":%u,\"reserve\":%u,\"outstanding\":%u,\"lease\":%" PRIu64 "}\n",a->scenario,phase,a->child,a->vf,a->info.state,(uint64_t)a->info.epoch,(uint64_t)a->info.session,a->info.ready_gen,a->info.hw_flags,a->info.reserve,a->info.outstanding,(uint64_t)a->info.lease_token);
    return 0;
}
static int healthy_checkpoint(struct admin *b,const char *phase)
{
    if (owner_inspect(b)||snapshot(b)||b->info.state!=TOY_STATES_ACTIVE||b->info.hw_flags!=28||
        b->info.epoch!=b->original.epoch||b->info.session!=b->original.session||
        b->info.reserve!=8||b->info.lease_token!=b->lease) { errno=EPROTO; return -1; }
    printf("{\"event\":\"%s_HEALTHY_CHECK\",\"phase\":\"%s\",\"pid\":%d,\"epoch\":%" PRIu64 ",\"session\":%" PRIu64 ",\"state\":%u,\"flags\":%u,\"reserve\":%u,\"lease\":%" PRIu64 "}\n",b->scenario,phase,b->child,(uint64_t)b->info.epoch,(uint64_t)b->info.session,b->info.state,b->info.hw_flags,b->info.reserve,(uint64_t)b->info.lease_token);
    return 0;
}
static int reset_handshake(struct admin *a,struct admin *b)
{
    struct lab_message m=a->current;
    struct toy_vf_ctl ctl;
    uint64_t old_epoch=m.epoch;
    uint32_t old_gen=m.ready_gen;
    if (reset_snapshot(a,"before_ioctl",TOY_STATES_ACTIVE,1)||healthy_checkpoint(b,"before_ioctl")) return -1;
    m.type=LAB_RESET_NOTICE; m.payload_len=16; memset(m.payload,0,16); lab_put32(m.payload,1);
    if (lab_send(a->connection,&m,lab_now_ms()+LAB_TIMEOUT_MS)||tenant_message(a,&m,LAB_HELLO,4)||
        reset_snapshot(a,"after_ioctl",TOY_STATES_RECOVERING,0)) return -1;
    if (m.epoch<=old_epoch||m.epoch!=a->info.epoch||m.session!=a->original.session||m.ready_gen!=old_gen||
        m.operation!=a->original.operation||a->info.ready_gen!=old_gen||!(a->info.hw_flags&TOY_HW_FLAGS_FENCED)) { errno=ESTALE; return -1; }
    a->original=a->current=m;
    /* 舊 generation 的 REARM 必須遭拒；不可先放行再補 CONFIG。 */
    errno=0;
    int rc=control(a,TOY_REARM_VF,old_gen,&ctl),error=errno;
    if (!rc||error!=EBUSY||reset_snapshot(a,"rearm_rejected",TOY_STATES_RECOVERING,0)||
        owner_inspect(a)||healthy_checkpoint(b,"unconfigured")) { errno=EPROTO; return -1; }
    printf("{\"event\":\"T06_REARM_REJECT\",\"errno\":%d,\"epoch\":%" PRIu64 ",\"ready_gen\":%u}\n",error,m.epoch,old_gen);
    if (old_gen==UINT32_MAX) { errno=EOVERFLOW; return -1; }
    m.type=LAB_HELLO_REPLY; m.ready_gen=old_gen+1; a->original=m;
    if (lab_send(a->connection,&m,lab_now_ms()+LAB_TIMEOUT_MS)||tenant_message(a,&m,LAB_READY,5)||
        reset_snapshot(a,"fresh_config",TOY_STATES_RECOVERING,1)) return -1;
    if (lab_tuple(&m,a->original.vf,a->original.session,a->original.epoch,a->original.lease_token,a->original.operation)||
        m.ready_gen!=a->original.ready_gen||a->info.ready_gen!=m.ready_gen||a->info.hw_flags!=61||
        a->info.epoch!=m.epoch) { errno=ESTALE; return -1; }
    a->current=m;
    if (healthy_checkpoint(b,"configured_fenced")||owner_inspect(a)||control(a,TOY_REARM_VF,m.ready_gen,&ctl)||
        reset_snapshot(a,"rearmed",TOY_STATES_ACTIVE,1)||owner_run(a)||healthy_checkpoint(b,"fresh_io")) return -1;
    return 0;
}
static int vfio_reset_case(const char *pf,const char *device,const char *tenant)
{
    struct admin owners[2];
    char count_path[PATH_MAX];
    bool enabled=false;
    int ret=1,saved;
    snprintf(count_path,sizeof(count_path),"/sys/bus/pci/devices/%s/sriov_numvfs",pf);
    for (unsigned i=0;i<2;i++) owners[i]=(struct admin){.pf=-1,.listen=-1,.connection=-1,.pidfd=-1,.management=-1,
        .vf=i,.failpoint="",.fail_hit=1,.scenario="T06",.count_path=count_path,.vfio_reset=!i,.healthy=i};
    if (lab_write_text(count_path,"4\n")) goto out;
    enabled=true;
    for (unsigned i=0;i<2;i++) if (isolated_owner_start(&owners[i],pf,device,tenant,16,4096)) goto out;
    struct admin *a=&owners[0],*b=&owners[1];
    struct lab_message m=b->current;
    m.type=LAB_RUN;
    if (lab_send(b->connection,&m,lab_now_ms()+LAB_TIMEOUT_MS)||healthy_checkpoint(b,"started")||
        owner_run(a)||reset_handshake(a,b)||owner_stop(a)||reset_snapshot(a,"closed",TOY_STATES_RECOVERING,0)||
        healthy_checkpoint(b,"closed")) goto out;
    uint64_t closed_epoch=a->info.epoch,session=a->info.session;
    pid_t old_pid=a->child;
    if (release_owner(a)) goto out;
    isolated_owner_cleanup(a);
    *a=(struct admin){.pf=-1,.listen=-1,.connection=-1,.pidfd=-1,.management=-1,
        .vf=0,.failpoint="",.fail_hit=1,.scenario="T06",.count_path=count_path};
    if (isolated_owner_start(a,pf,device,tenant,16,4096)||a->child==old_pid||
        reset_snapshot(a,"reopened_rearmed",TOY_STATES_ACTIVE,1)||a->info.epoch<=closed_epoch||a->info.session!=session||
        healthy_checkpoint(b,"reopened")||owner_run(a)||healthy_checkpoint(b,"reopened_io")||owner_stop(a)||
        reset_snapshot(a,"reclosed",TOY_STATES_RECOVERING,0)||release_owner(a)) goto out;
    m=b->current; m.type=LAB_STOP_REQUEST; m.payload_len=16; memset(m.payload,0,16);
    if (lab_send(b->connection,&m,lab_now_ms()+LAB_TIMEOUT_MS)||tenant_message(b,&m,LAB_STOPPED,3)||
        lab_tuple(&m,b->original.vf,b->original.session,b->original.epoch,b->original.lease_token,b->original.operation)||
        m.ready_gen!=b->original.ready_gen) goto out;
    b->current=m;
    if (owner_stop(b)||release_owner(b)||lab_write_text(count_path,"0\n")) goto out;
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
        printf("{\"event\":\"T06_FINAL\",\"vf\":%u,\"state\":%u,\"reserve\":%u,\"outstanding\":%u,\"flags\":%u,\"lease\":%" PRIu64 "}\n",i,info.state,info.reserve,info.outstanding,info.hw_flags,(uint64_t)info.lease_token);
    }
    puts("{\"event\":\"M3_CASE\",\"id\":\"T06\",\"status\":\"PASS\",\"ioctl_resets\":1,\"vf_a_owners\":2,\"all_free\":true,\"owner_closed\":true}");
    puts("LAB_RESULT=PASS"); ret=0;
out:
    saved=errno;
    for (unsigned i=0;i<2;i++) isolated_owner_cleanup(&owners[i]);
    if (enabled&&lab_write_text(count_path,"0\n")) fprintf(stderr,"LABD_DISABLE_FAILED errno=%d\n",errno);
    if (ret) {
        fprintf(stderr,"LABD_ERROR scenario=T06 errno=%d message=%s\n",saved,strerror(saved));
        printf("{\"event\":\"M3_CASE\",\"id\":\"T06\",\"status\":\"FAIL\",\"errno\":%d}\n",saved);
        puts("LAB_RESULT=FAIL");
    }
    return ret;
}
