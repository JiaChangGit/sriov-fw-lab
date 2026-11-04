#define _GNU_SOURCE
#include "owner.h"
#include "../common/lab_wire.h"
#include "toy_regs.h"
#include <errno.h>
#include <dirent.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int exchange(int fd,struct lab_message *m,uint16_t sent,uint16_t expected,struct owner *inspect)
{
    struct lab_message reply;
    m->type=sent;
    if (lab_send(fd,m,lab_now_ms()+LAB_TIMEOUT_MS)) return -1;
    for (;;) {
        if (lab_recv(fd,&reply,lab_now_ms()+LAB_TIMEOUT_MS)) return -1;
        if (reply.type!=LAB_INSPECT||!inspect) break;
        uint32_t digests[OWNER_MAPS];
        if (reply.request_id!=m->request_id||lab_tuple(&reply,m->vf,m->session,m->epoch,m->lease_token,m->operation)||
            reply.ready_gen!=m->ready_gen||
            owner_unchanged(inspect,digests)) { errno=EPROTO; return -1; }
        reply.type=LAB_INSPECT_REPLY; reply.payload_len=16; memset(reply.payload,0,16);
        for (unsigned i=0;i<OWNER_MAPS;i++) lab_put32(reply.payload+i*4,digests[i]);
        if (lab_send(fd,&reply,lab_now_ms()+LAB_TIMEOUT_MS)) return -1;
    }
    if (reply.type!=expected||reply.request_id!=m->request_id||
        lab_tuple(&reply,m->vf,m->session,m->epoch,m->lease_token,m->operation)||
        (sent!=LAB_HELLO&&reply.ready_gen!=m->ready_gen)) { errno=EPROTO; return -1; }
    *m=reply; return 0;
}
static int open_fds(void)
{
    DIR *directory=opendir("/proc/self/fd");
    struct dirent *entry;
    int count=0;
    if (!directory) return -1;
    while ((entry=readdir(directory))) {
        if (entry->d_name[0]=='.') continue;
        if (atoi(entry->d_name)!=dirfd(directory)) ++count;
    }
    closedir(directory); return count;
}
/* 健康 VF 每批驗證資料與 CQ，再處理管理訊息；整段保有同一個 epoch。 */
static int healthy_work(int fd,struct lab_message *m,struct owner *o)
{
    int64_t deadline=lab_now_ms()+LAB_TIMEOUT_MS;
    while (lab_now_ms()<deadline) {
        struct pollfd p={.fd=fd,.events=POLLIN};
        struct lab_message query;
        if (owner_workload(o,256)) return -1;
        int rc=poll(&p,1,0);
        if (rc<0&&errno==EINTR) continue;
        if (rc<0) return -1;
        if (!rc) continue;
        if (!(p.revents&POLLIN)||lab_recv(fd,&query,deadline)||query.request_id!=m->request_id||
            lab_tuple(&query,m->vf,m->session,m->epoch,m->lease_token,m->operation)||query.ready_gen!=m->ready_gen) { errno=EPROTO; return -1; }
        if (query.type==LAB_STOP_REQUEST) return 0;
        if (query.type!=LAB_INSPECT) { errno=EPROTO; return -1; }
        uint32_t digests[OWNER_MAPS],flags;
        if (owner_freeze(o)||owner_unchanged(o,digests)||owner_latch(o,&flags)||
            o->epoch!=m->epoch||o->session!=m->session||(flags&3u)) { errno=EPROTO; return -1; }
        query.type=LAB_INSPECT_REPLY; query.payload_len=16; memset(query.payload,0,16);
        for (unsigned i=0;i<OWNER_MAPS;i++) lab_put32(query.payload+i*4,digests[i]);
        printf("{\"event\":\"%s\",\"pid\":%d,\"epoch\":%" PRIu64 ",\"session\":%" PRIu64 ",\"completed\":%" PRIu64 ",\"flags\":%u}\n",o->recovery_case?"RECOVERY_HEALTHY_PROGRESS":"T06_HEALTHY_PROGRESS",getpid(),o->epoch,o->session,o->completed,flags);
        if (lab_send(fd,&query,deadline)) return -1;
    }
    errno=ETIMEDOUT; return -1;
}
int main(int argc,char **argv)
{
    struct owner o;
    struct lab_message m={0},stop;
    int fd=-1,ret=1,saved,baseline=open_fds();
    unsigned stage=1;
    unsigned group=0,entries=16,requests=4096;
    const char *socket=NULL,*bdf=NULL;
    bool verified=false,inspect=false,vectors=false,vfio_reset=false,healthy=false;
    owner_init(&o); setvbuf(stdout,NULL,_IOLBF,0);
    for (int i=1;i<argc;i++) {
        if (i+1>=argc) { errno=EINVAL; goto out; }
        const char *key=argv[i++],*value=argv[i];
        char *end; unsigned long long n=0;
        if (!strcmp(key,"--socket")) { socket=value; continue; }
        if (!strcmp(key,"--bdf")) { bdf=value; continue; }
        if (!strcmp(key,"--failpoint")) {
            if (strlen(value)>=sizeof(o.failpoint)) { errno=EINVAL; goto out; }
            strcpy(o.failpoint,value); continue;
        }
        errno=0; n=strtoull(value,&end,0);
        if (errno||!*value||*end) { errno=EINVAL; goto out; }
        if (!strcmp(key,"--vf")&&n<8) m.vf=(uint32_t)n;
        else if (!strcmp(key,"--group")&&n<=UINT32_MAX) group=(unsigned)n;
        else if (!strcmp(key,"--entries")&&n<=256) entries=(unsigned)n;
        else if (!strcmp(key,"--requests")&&n<=10000000) requests=(unsigned)n;
        else if (!strcmp(key,"--lease")&&n) m.lease_token=n;
        else if (!strcmp(key,"--operation")&&n) m.operation=n;
        else if (!strcmp(key,"--fail-hit")&&n&&n<=UINT32_MAX) o.fail_hit=(unsigned)n;
        else if (!strcmp(key,"--inspect")&&n<=1) inspect=n;
        else if (!strcmp(key,"--vectors")&&n<=1) vectors=n;
        else if (!strcmp(key,"--dma-fault")&&n<=6) o.dma_fault=(unsigned)n;
        else if (!strcmp(key,"--reset-test")&&n<=1) o.reset_test=n;
        else if (!strcmp(key,"--vfio-reset")&&n<=1) vfio_reset=n;
        else if (!strcmp(key,"--healthy")&&n<=1) healthy=n;
        else if (!strcmp(key,"--recovery-case")&&(n==0||n==7||n==8)) o.recovery_case=(unsigned)n;
        else { errno=EINVAL; goto out; }
    }
    if (!socket||!bdf||!m.lease_token||!m.operation) { errno=EINVAL; goto out; }
    o.pattern=(unsigned char)(m.vf*49u);
    fd=lab_connect(socket,lab_now_ms()+LAB_TIMEOUT_MS);
    if (fd<0||lab_peer(fd,getuid(),getppid())||owner_open(&o,bdf,group)) goto out;
    m.request_id=1; m.session=o.session; m.epoch=o.epoch;
    if (exchange(fd,&m,LAB_HELLO,LAB_HELLO_REPLY,NULL)||!m.ready_gen) goto out;
    if (owner_configure(&o,entries)) goto out;
    if (m.epoch!=o.epoch||m.session!=o.session) { errno=ESTALE; goto out; }
    if (inspect&&owner_freeze(&o)) goto out;
    ++m.request_id;
    stage=2;
    if (exchange(fd,&m,LAB_READY,LAB_RUN,inspect?&o:NULL)) goto out;
    if (healthy?healthy_work(fd,&m,&o):(o.dma_fault?owner_dma_fault(&o):owner_workload(&o,requests))) goto out;
    if (inspect&&!o.dma_fault&&!o.reset_test&&owner_pattern_pair(&o)) goto out;
    if (vectors&&owner_descriptor_vectors(&o)) goto out;
    if (o.recovery_case&&!healthy) {
        ++m.request_id;
        /* 管理者在正常負載完成後才 arm 延遲，避免每一筆暖機請求都被保留。 */
        if (exchange(fd,&m,LAB_STOPPED,LAB_RUN,&o)) goto out;
        if (owner_submit_pending(&o)) goto out;
        ++m.request_id; m.type=LAB_STOPPED;
        uint64_t previous_epoch=o.epoch;
        if (lab_send(fd,&m,lab_now_ms()+LAB_TIMEOUT_MS)||lab_recv(fd,&stop,lab_now_ms()+LAB_TIMEOUT_MS)) goto out;
        if (stop.type!=LAB_STOP_REQUEST||stop.request_id!=m.request_id||
            lab_tuple(&stop,m.vf,m.session,m.epoch,m.lease_token,m.operation)||stop.ready_gen!=m.ready_gen) { errno=EPROTO; goto out; }
        stop.type=LAB_STOPPED; stop.payload_len=0;
        printf("{\"event\":\"RECOVERY_STOPPED\",\"pid\":%d,\"pending_requests\":1,\"submissions_stopped\":true}\n",getpid());
        if (lab_send(fd,&stop,lab_now_ms()+LAB_TIMEOUT_MS)||lab_recv(fd,&stop,lab_now_ms()+LAB_TIMEOUT_MS)) goto out;
        if (stop.type!=LAB_RESET_NOTICE||stop.request_id!=m.request_id||stop.vf!=m.vf||stop.session!=m.session||
            stop.epoch<=m.epoch||stop.lease_token!=m.lease_token||stop.operation!=m.operation||stop.ready_gen!=m.ready_gen||
            !(lab_get32(stop.payload+4)&TOY_HW_FLAGS_FENCED)||lab_get32(stop.payload+8)||
            owner_reset_clear(&o,"PF_RESET")||o.epoch!=stop.epoch||o.session!=stop.session) { errno=EPROTO; goto out; }
        uint32_t digests[OWNER_MAPS];
        if (owner_unchanged(&o,digests)||owner_write32(&o,TOY_VF_DOORBELL,1)) goto out;
        if (!o.pending_request||o.pending_epoch!=previous_epoch||o.cancelled) { errno=EPROTO; goto out; }
        o.pending_request=false; ++o.cancelled;
        printf("{\"event\":\"RECOVERY_CANCELLED\",\"pid\":%d,\"old_epoch\":%" PRIu64 ",\"new_epoch\":%" PRIu64 ",\"requests_marked_error\":%" PRIu64 ",\"old_maps_retained\":true}\n",getpid(),previous_epoch,o.epoch,o.cancelled);
        m=stop; ++m.request_id; m.payload_len=0;
        if (exchange(fd,&m,LAB_HELLO,LAB_HELLO_REPLY,&o)||!m.ready_gen||owner_retire(&o)||
            owner_configure(&o,entries)||owner_freeze(&o)) goto out;
        ++m.request_id;
        if (exchange(fd,&m,LAB_READY,LAB_RUN,&o)||owner_workload(&o,requests)||owner_retired_unchanged(&o)) goto out;
    }
    if (inspect&&owner_freeze(&o)) goto out;
    if (vfio_reset) {
        ++m.request_id;
        if (exchange(fd,&m,LAB_STOPPED,LAB_RESET_NOTICE,&o)||lab_get32(m.payload)!=1||owner_vfio_reset(&o)) goto out;
        m.epoch=o.epoch; m.session=o.session; m.payload_len=0; ++m.request_id;
        if (exchange(fd,&m,LAB_HELLO,LAB_HELLO_REPLY,&o)||!m.ready_gen) goto out;
        if (owner_unmap(&o,true)||owner_configure(&o,entries)||owner_freeze(&o)) goto out;
        ++m.request_id; m.payload_len=0;
        if (exchange(fd,&m,LAB_READY,LAB_RUN,&o)||owner_workload(&o,requests)||owner_freeze(&o)) goto out;
    }
    ++m.request_id; m.type=LAB_STOPPED;
    if (exchange(fd,&m,LAB_STOPPED,LAB_STOP,inspect?&o:NULL)) goto out;
    stop=m;
    if (stop.type!=LAB_STOP||stop.request_id!=m.request_id||
        lab_tuple(&stop,m.vf,m.session,m.epoch,m.lease_token,m.operation)||
        !(lab_get32(stop.payload+4)&TOY_HW_FLAGS_FENCED)||lab_get32(stop.payload+8)) { errno=EPROTO; goto out; }
    uint64_t old_epoch=o.epoch;
    uint32_t flags;
    if (owner_latch(&o,&flags)||o.session!=m.session||o.epoch<=old_epoch||!(flags&TOY_HW_FLAGS_FENCED)) { errno=ESTALE; goto out; }
    verified=true;
    stage=3;
    if (owner_close(&o,true)) goto out;
    stop.type=LAB_EXIT_ACK; memset(stop.payload,0,16);
    if (lab_send(fd,&stop,lab_now_ms()+LAB_TIMEOUT_MS)) goto out;
    if (close(fd)) { fd=-1; goto out; }
    fd=-1;
    int final_fds=open_fds();
    unsigned final_maps=0,final_buffers=0,final_events=0;
    for (unsigned i=0;i<OWNER_MAPS;i++) { final_maps+=o.maps[i].mapped; final_buffers+=o.maps[i].memory!=NULL; final_buffers+=o.held[i]!=NULL; final_buffers+=o.retired[i].memory!=NULL; final_buffers+=o.retired_guard[i]!=NULL; }
    for (unsigned i=0;i<2;i++) final_events+=o.eventfds[i]>=0;
    if (baseline<3||final_fds!=baseline||final_maps||final_buffers||final_events||o.pending_request||
        o.vfio.device>=0||o.vfio.group>=0||o.vfio.container>=0) { errno=EBUSY; goto out; }
    printf("{\"event\":\"OWNER_EXIT\",\"pid\":%d,\"vf\":%u,\"fds\":%d,\"baseline_fds\":%d,\"final_fds\":%d,\"maps\":%u,\"eventfds\":%u,\"buffers\":%u}\n",getpid(),m.vf,final_fds-baseline,baseline,final_fds,final_maps,final_events,final_buffers);
    ret=0;
out:
    saved=errno;
    if (ret) {
        fprintf(stderr,"TENANT_ERROR vf=%u errno=%d message=%s\n",m.vf,saved,strerror(saved));
        if (fd>=0&&m.request_id&&m.lease_token) {
            m.type=LAB_ERROR; ++m.request_id; m.payload_len=8;
            lab_put32(m.payload,saved>0&&saved<4096?(unsigned)saved:EIO); lab_put32(m.payload+4,stage);
            int sent=lab_send(fd,&m,lab_now_ms()+1000);
            /* 配置失敗時先等管理者 fence，再解除任何 DMA 映射。 */
            if (!sent&&!verified&&o.vfio.device>=0) {
                uint64_t error_epoch=o.epoch;
                uint32_t error_flags;
                if (!lab_recv(fd,&stop,lab_now_ms()+LAB_TIMEOUT_MS)&&stop.type==LAB_STOP&&
                    stop.request_id==m.request_id&&!lab_tuple(&stop,m.vf,m.session,m.epoch,m.lease_token,m.operation)&&
                    (lab_get32(stop.payload+4)&TOY_HW_FLAGS_FENCED)&&!lab_get32(stop.payload+8)&&
                    !owner_latch(&o,&error_flags)&&o.session==m.session&&o.epoch>error_epoch&&(error_flags&TOY_HW_FLAGS_FENCED)) verified=true;
            }
        }
    }
    if (owner_close(&o,verified)) ret=1;
    if (fd>=0) close(fd);
    if (o.failpoint[0]) {
        unsigned maps=0,buffers=0,eventfds=0;
        for (unsigned i=0;i<OWNER_MAPS;i++) { maps+=o.maps[i].mapped; buffers+=o.maps[i].memory!=NULL; buffers+=o.held[i]!=NULL; }
        for (unsigned i=0;i<2;i++) eventfds+=o.eventfds[i]>=0;
        int final=open_fds();
        bool clean=ret&&verified&&o.fail_fired&&baseline>=3&&final==baseline&&!maps&&!buffers&&!eventfds&&
                   o.vfio.device<0&&o.vfio.group<0&&o.vfio.container<0;
        printf("{\"event\":\"OWNER_FAILURE_CLEANUP\",\"pid\":%d,\"name\":\"%s\",\"hit\":%u,\"fired\":%s,\"fence_verified\":%s,\"stage\":%u,\"baseline_fds\":%d,\"final_fds\":%d,\"maps\":%u,\"buffers\":%u,\"eventfds\":%u,\"status\":\"%s\"}\n",getpid(),o.failpoint,o.fail_hit,o.fail_fired?"true":"false",verified?"true":"false",stage,baseline,final,maps,buffers,eventfds,clean?"PASS":"FAIL");
        ret=clean?42:1;
    }
    return ret;
}
