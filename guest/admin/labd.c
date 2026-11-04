/* Guest 管理程序：持有 PF 控制 fd/lease，啟停 tenant，並核對 socket peer 與每次握手的 tuple。 */
#define _GNU_SOURCE
#include "../common/lab_wire.h"
#include "t02.h"
#include "../tenant/vfio.h"
#include "toy_uapi.h"
#include "toy_regs.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

struct admin {
    int pf,listen,connection,pidfd,management;
    pid_t child;
    uint64_t operation,lease;
    uint32_t vf;
    bool child_reaped,leased,bound;
    char bdf[32],socket[108],management_path[108],directory[64];
    struct toy_vf_info info;
    const char *scenario,*count_path;
    const char *failpoint;
    unsigned fail_hit,group,dma_fault,recovery_case;
    bool owner_error;
    bool vfio_reset,healthy;
    struct lab_message failure,original,current;
};
static struct toy_uapi_header header(size_t size)
{ return (struct toy_uapi_header){size,TOY_UAPI_MAJOR,TOY_UAPI_MINOR}; }
static int snapshot(struct admin *a)
{
    memset(&a->info,0,sizeof(a->info)); a->info.hdr=header(sizeof(a->info)); a->info.vf=a->vf;
    return ioctl(a->pf,TOY_GET_VF,&a->info);
}
static int control(struct admin *a,unsigned long cmd,uint32_t generation,struct toy_vf_ctl *result)
{
    if (a->operation==UINT64_MAX) { errno=EOVERFLOW; return -1; }
    memset(result,0,sizeof(*result)); result->hdr=header(sizeof(*result));
    result->vf=a->vf; result->session=a->info.session; result->epoch=a->info.epoch;
    result->lease_token=a->lease; result->operation=++a->operation; result->ready_gen=generation;
    if (cmd==TOY_LEASE_RELEASE) result->flags=TOY_CTL_OWNER_CLOSED;
    int rc=ioctl(a->pf,cmd,result),saved=errno;
    printf("{\"event\":\"M3_CONTROL\",\"vf\":%u,\"command\":%lu,\"operation\":%" PRIu64 ",\"errno\":%d,\"session\":%" PRIu64 ",\"epoch\":%" PRIu64 ",\"state\":%u,\"flags\":%u,\"outstanding\":%u,\"fw_status\":%u}\n",a->vf,_IOC_NR(cmd),a->operation,rc?saved:0,(uint64_t)result->session,(uint64_t)result->epoch,result->state,result->hw_flags,result->outstanding,result->fw_status);
    errno=saved; return rc;
}
static int child_wait(struct admin *a,int timeout)
{
    int status;
    if (a->child_reaped||a->child<=0) return 0;
    if (lab_wait(a->pidfd,POLLIN,lab_now_ms()+timeout)) return -1;
    pid_t got;
    do { got=waitpid(a->child,&status,0); } while (got<0&&errno==EINTR);
    if (got!=a->child) return -1;
    a->child_reaped=true;
    if (!WIFEXITED(status)||WEXITSTATUS(status)) { errno=ECHILD; return -1; }
    printf("{\"event\":\"M3_OWNER_REAP\",\"vf\":%u,\"pid\":%d,\"exit_code\":0,\"pidfd\":true}\n",a->vf,a->child);
    return 0;
}
static int tenant_message(struct admin *a,struct lab_message *m,uint16_t type,uint64_t request)
{
    int64_t deadline=lab_now_ms()+LAB_TIMEOUT_MS;
    for (;;) {
        struct pollfd fds[3]={{a->connection,POLLIN,0},{a->pidfd,POLLIN,0},{a->management,POLLIN,0}};
        int64_t remain=deadline-lab_now_ms();
        if (remain<=0) { errno=ETIMEDOUT; return -1; }
        int count=poll(fds,3,(int)remain);
        if (count<0&&errno==EINTR) continue;
        if (count<0) return -1;
        if (!count) { errno=ETIMEDOUT; return -1; }
        if (fds[2].revents&POLLIN) {
            int fd=accept4(a->management,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);
            if (fd>=0) {
                struct ucred cred; socklen_t size=sizeof(cred);
                struct lab_message query;
                if (!getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&size)&&size==sizeof(cred)&&cred.uid==getuid()&&
                    !lab_recv(fd,&query,lab_now_ms()+1000)&&query.type==LAB_HELLO&&!snapshot(a)) {
                    query.type=LAB_HELLO_REPLY; query.vf=a->vf; query.session=a->info.session;
                    query.epoch=a->info.epoch; query.lease_token=a->lease;
                    query.ready_gen=a->info.ready_gen; query.operation=a->operation;
                    (void)lab_send(fd,&query,lab_now_ms()+1000);
                }
                close(fd);
            }
        }
        if (fds[0].revents&POLLIN) break;
        if ((fds[0].revents&(POLLHUP|POLLERR))||(fds[1].revents&POLLIN)) { errno=ECHILD; return -1; }
    }
    if (lab_recv(a->connection,m,deadline)) return -1;
    if (m->type==LAB_ERROR) {
        a->owner_error=true; a->failure=*m;
        fprintf(stderr,"CHILD_REPORTED_ERROR errno=%u stage=%u\n",lab_get32(m->payload),lab_get32(m->payload+4));
        errno=EIO; return -1;
    }
    if (m->type!=type||m->request_id!=request||m->vf!=a->vf||m->lease_token!=a->lease) { errno=EPROTO; return -1; }
    return 0;
}
static int start_owner(struct admin *a,const char *tenant,unsigned group,unsigned entries,unsigned requests)
{
    char vf_text[16],group_text[16],entries_text[16],requests_text[16],lease_text[32],operation_text[32],hit_text[16],dma_text[16],recovery_text[16];
    snprintf(vf_text,sizeof(vf_text),"%u",a->vf); snprintf(group_text,sizeof(group_text),"%u",group);
    snprintf(entries_text,sizeof(entries_text),"%u",entries); snprintf(requests_text,sizeof(requests_text),"%u",requests);
    snprintf(lease_text,sizeof(lease_text),"%" PRIu64,a->lease); snprintf(operation_text,sizeof(operation_text),"%" PRIu64,a->operation);
    snprintf(hit_text,sizeof(hit_text),"%u",a->fail_hit);
    snprintf(dma_text,sizeof(dma_text),"%u",a->dma_fault);
    snprintf(recovery_text,sizeof(recovery_text),"%u",a->recovery_case);
    a->listen=lab_listen(a->socket); if (a->listen<0) return -1;
    a->child=fork(); if (a->child<0) return -1;
    if (!a->child) {
        /* exec 前關掉所有管理 fd；owner 不繼承 PF/container/group。 */
        if (syscall(SYS_close_range,3u,UINT_MAX,0u)) _exit(125);
        execl(tenant,tenant,"--socket",a->socket,"--bdf",a->bdf,"--vf",vf_text,"--group",group_text,
              "--entries",entries_text,"--requests",requests_text,"--lease",lease_text,"--operation",operation_text,
              "--failpoint",a->failpoint,"--fail-hit",hit_text,
              "--vectors",!strcmp(a->scenario,"T09")?"1":"0",
              "--dma-fault",dma_text,
              "--recovery-case",recovery_text,
              "--reset-test",(!strcmp(a->scenario,"T06")||a->recovery_case)?"1":"0",
              "--vfio-reset",a->vfio_reset?"1":"0","--healthy",a->healthy?"1":"0",
              "--inspect",(!strcmp(a->scenario,"T04")||!strcmp(a->scenario,"T05")||!strcmp(a->scenario,"T06")||a->recovery_case)?"1":"0",(char *)NULL);
        _exit(127);
    }
    a->pidfd=(int)syscall(SYS_pidfd_open,a->child,0); if (a->pidfd<0) return -1;
    if (lab_wait(a->listen,POLLIN,lab_now_ms()+LAB_TIMEOUT_MS)) return -1;
    a->connection=accept4(a->listen,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);
    if (a->connection<0||lab_peer(a->connection,getuid(),a->child)) return -1;
    printf("{\"event\":\"M3_PEER_VERIFIED\",\"vf\":%u,\"pid\":%d,\"uid\":%u,\"pidfd\":true}\n",a->vf,a->child,getuid());
    return 0;
}
static int busy_operations(struct admin *a)
{
    struct toy_vf_info before[8],after;
    char links[4][32],path[PATH_MAX],actual[32];
    uint64_t count;
    const char *values[]={"0\n","2\n"};
    for (unsigned i=0;i<8;i++) {
        before[i]=(struct toy_vf_info){.hdr=header(sizeof(before[i])),.vf=i};
        if (ioctl(a->pf,TOY_GET_VF,&before[i])||before[i].reserve!=(i<4?8u:0u)) return -1;
        if (i<4) {
            /* 使用 PF 的 count 路徑組出每一個 virtfn link。 */
            snprintf(path,sizeof(path),"%.*svirtfn%u",(int)(strlen(a->count_path)-strlen("sriov_numvfs")),a->count_path,i);
            if (lab_link_basename(path,links[i],sizeof(links[i]))) return -1;
        }
    }
    if (!before[a->vf].lease_token) { errno=EPROTO; return -1; }
    for (unsigned op=0;op<2;op++) {
        errno=0;
        int rc=lab_write_text(a->count_path,values[op]),error=errno;
        if (!rc||(error!=EBUSY&&error!=EPERM)||lab_read_u64(a->count_path,&count)||count!=4) { errno=EPROTO; return -1; }
        for (unsigned i=0;i<8;i++) {
            after=(struct toy_vf_info){.hdr=header(sizeof(after)),.vf=i};
            if (ioctl(a->pf,TOY_GET_VF,&after)||memcmp(&before[i],&after,sizeof(after))) { errno=EPROTO; return -1; }
            if (i<4) {
                snprintf(path,sizeof(path),"%.*svirtfn%u",(int)(strlen(a->count_path)-strlen("sriov_numvfs")),a->count_path,i);
                if (lab_link_basename(path,actual,sizeof(actual))||strcmp(actual,links[i])) { errno=EPROTO; return -1; }
            }
            printf("{\"event\":\"T10_SNAPSHOT\",\"attempt\":%u,\"vf\":%u,\"state\":%u,\"reserve\":%u,\"flags\":%u,\"outstanding\":%u,\"epoch\":%" PRIu64 ",\"lease\":%" PRIu64 ",\"unchanged\":true}\n",op,i,after.state,after.reserve,after.hw_flags,after.outstanding,(uint64_t)after.epoch,(uint64_t)after.lease_token);
        }
        printf("{\"event\":\"T10_BUSY\",\"attempt\":%u,\"requested\":%u,\"errno\":%d,\"enabled\":%" PRIu64 ",\"topology_unchanged\":true,\"ledger_unchanged\":true,\"owner_pid\":%d}\n",op,op?2:0,error,count,a->child);
    }
    return 0;
}
static int owner_prepare(struct admin *a)
{
    struct lab_message m;
    struct toy_vf_ctl ctl;
    if (tenant_message(a,&m,LAB_HELLO,1)||snapshot(a)) return -1;
    if (lab_tuple(&m,a->vf,a->info.session,a->info.epoch,a->lease,a->operation)||a->info.ready_gen==UINT32_MAX) { errno=ESTALE; return -1; }
    m.type=LAB_HELLO_REPLY; m.ready_gen=a->info.ready_gen+1; a->original=m;
    if (lab_send(a->connection,&m,lab_now_ms()+LAB_TIMEOUT_MS)||tenant_message(a,&m,LAB_READY,2)||snapshot(a)) return -1;
    if (lab_tuple(&m,a->original.vf,a->original.session,a->original.epoch,a->original.lease_token,a->original.operation)||
        m.ready_gen!=a->original.ready_gen||a->info.ready_gen!=m.ready_gen||a->info.epoch!=m.epoch||a->info.session!=m.session) { errno=ESTALE; return -1; }
    if (control(a,TOY_REARM_VF,m.ready_gen,&ctl)) return -1;
    a->current=m;
    return 0;
}
static int owner_run(struct admin *a)
{
    struct lab_message m=a->current;
    if (!strcmp(a->scenario,"T10")&&busy_operations(a)) return -1;
    m.type=LAB_RUN;
    if (lab_send(a->connection,&m,lab_now_ms()+LAB_TIMEOUT_MS)||tenant_message(a,&m,LAB_STOPPED,a->current.request_id+1)) return -1;
    if (lab_tuple(&m,a->original.vf,a->original.session,a->original.epoch,a->original.lease_token,a->original.operation)||
        m.ready_gen!=a->original.ready_gen) { errno=ESTALE; return -1; }
    a->current=m;
    return 0;
}
/* 停止拆成發送與等待兩段，讓多 VF teardown 能先通知全部 owner，再逐一收尾。 */
static int owner_stop_begin(struct admin *a)
{
    struct lab_message m=a->current;
    struct toy_vf_ctl ctl;
    if (snapshot(a)) return -1;
    if (control(a,TOY_FENCE_VF,0,&ctl)||ctl.outstanding||!(ctl.hw_flags&TOY_HW_FLAGS_FENCED)) return -1;
    m.type=LAB_STOP; m.payload_len=16; memset(m.payload,0,16); lab_put32(m.payload+4,ctl.hw_flags);
    return lab_send(a->connection,&m,lab_now_ms()+LAB_TIMEOUT_MS);
}
static int owner_stop_finish(struct admin *a)
{
    struct lab_message m;
    if (tenant_message(a,&m,LAB_EXIT_ACK,a->current.request_id)) return -1;
    if (lab_tuple(&m,a->original.vf,a->original.session,a->original.epoch,a->original.lease_token,a->original.operation)||
        m.ready_gen!=a->original.ready_gen) { errno=ESTALE; return -1; }
    for (unsigned i=0;i<16;i++) if (m.payload[i]) { errno=EBUSY; return -1; }
    return child_wait(a,LAB_TIMEOUT_MS);
}
static int owner_stop(struct admin *a)
{ return owner_stop_begin(a)||owner_stop_finish(a)?-1:0; }
static int descriptor_counters(struct admin *a,const char *phase)
{
    char record[1536],*line;
    if (lab_read_text("/sys/kernel/debug/toy_pf0/vf-stats",record,sizeof(record))) return -1;
    for (line=record;line&&*line;line=strchr(line,'\n'),line=line?line+1:NULL) {
        unsigned vf,desc,cpl,dma,blocked,irq;
        if (sscanf(line,"vf=%u stale_desc=%u stale_cpl=%u dma_fault=%u fence_blocked=%u irq_suppressed=%u",&vf,&desc,&cpl,&dma,&blocked,&irq)!=6) { errno=EPROTO; return -1; }
        if (vf!=a->vf) continue;
        if (desc!=(!strcmp(phase,"after")?1u:0u)||cpl||dma||blocked||irq) { errno=EPROTO; return -1; }
        printf("{\"event\":\"T09_COUNTERS\",\"phase\":\"%s\",\"vf\":%u,\"stale_desc\":%u,\"stale_cpl\":%u,\"dma_fault\":%u,\"fence_blocked\":%u,\"irq_suppressed\":%u}\n",phase,vf,desc,cpl,dma,blocked,irq);
        return 0;
    }
    errno=ENOENT; return -1;
}
static int owner_session(struct admin *a)
{
    if (owner_prepare(a)||(!strcmp(a->scenario,"T09")&&descriptor_counters(a,"before"))||owner_run(a)||
        (!strcmp(a->scenario,"T09")&&descriptor_counters(a,"after"))||owner_stop(a)) return -1;
    return 0;
}
static int owner_inspect(struct admin *a)
{
    struct lab_message m=a->current;
    m.type=LAB_INSPECT; m.payload_len=0;
    if (lab_send(a->connection,&m,lab_now_ms()+LAB_TIMEOUT_MS)||
        tenant_message(a,&m,LAB_INSPECT_REPLY,a->current.request_id)) return -1;
    if (lab_tuple(&m,a->original.vf,a->original.session,a->original.epoch,a->original.lease_token,a->original.operation)||
        m.ready_gen!=a->original.ready_gen) { errno=ESTALE; return -1; }
    printf("{\"event\":\"%s_INSPECT\",\"vf\":%u,\"pid\":%d,\"request\":%" PRIu64 ",\"sq_crc\":%u,\"cq_crc\":%u,\"payload_crc\":%u}\n",a->scenario,a->vf,a->child,m.request_id,lab_get32(m.payload),lab_get32(m.payload+4),lab_get32(m.payload+8));
    return 0;
}
/* 呼叫者須先確認 owner 已關閉；此處解除綁定並釋放 lease，不代替 VFIO UNMAP。 */
static int release_owner(struct admin *a)
{
    char path[PATH_MAX]; struct toy_vf_ctl ctl;
    if (a->bound) {
        if (lab_write_text("/sys/bus/pci/drivers/vfio-pci/unbind",a->bdf)) return -1;
        a->bound=false;
        snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/driver_override",a->bdf);
        if (lab_write_text(path,"\n")) return -1;
    }
    if (a->leased) {
        if (snapshot(a)||control(a,TOY_LEASE_RELEASE,0,&ctl)) return -1;
        a->lease=0; a->leased=false;
    }
    return 0;
}
static int failed_owner(struct admin *a)
{
    struct lab_message m=a->failure;
    struct toy_vf_ctl ctl;
    unsigned stage=lab_get32(m.payload+4);
    if (!a->owner_error||!*a->failpoint||m.vf!=a->vf||m.lease_token!=a->lease||
        lab_get32(m.payload)!=EIO||(stage!=1&&stage!=3)||m.request_id!=(stage==1?2u:4u)||
        lab_tuple(&m,a->original.vf,a->original.session,a->original.epoch,a->original.lease_token,a->original.operation)||
        m.ready_gen!=a->original.ready_gen) { errno=EPROTO; return -1; }
    if (stage==1) {
        if (snapshot(a)||lab_tuple(&m,a->vf,a->info.session,a->info.epoch,a->lease,a->operation)||
            control(a,TOY_FENCE_VF,0,&ctl)||ctl.outstanding||!(ctl.hw_flags&TOY_HW_FLAGS_FENCED)) return -1;
        m.type=LAB_STOP; m.payload_len=16; memset(m.payload,0,16); lab_put32(m.payload+4,ctl.hw_flags);
        if (lab_send(a->connection,&m,lab_now_ms()+LAB_TIMEOUT_MS)) return -1;
    }
    if (lab_wait(a->pidfd,POLLIN,lab_now_ms()+LAB_TIMEOUT_MS)) return -1;
    int status; pid_t got;
    do { got=waitpid(a->child,&status,0); } while (got<0&&errno==EINTR);
    if (got!=a->child) return -1;
    a->child_reaped=true;
    if (!WIFEXITED(status)||WEXITSTATUS(status)!=42) { errno=ECHILD; return -1; }
    printf("{\"event\":\"T02_OWNER_REAP\",\"pid\":%d,\"exit_code\":42,\"stage\":%u}\n",a->child,stage);
    return 0;
}
static int owner_fault_matrix(const char *program)
{
    static const struct { const char *name; unsigned hits; } points[]={
        {"owner.alloc_buffer",3},{"owner.map_sq",1},{"owner.map_cq",1},{"owner.map_payload",1},
        {"owner.eventfd",2},{"owner.irq_register",1},{"owner.unmap",3},{"owner.short_unmap",3}};
    unsigned cases=0;
    for (unsigned p=0;p<sizeof(points)/sizeof(points[0]);p++) for (unsigned hit=1;hit<=points[p].hits;hit++) {
        ++cases;
        for (unsigned clean=0;clean<2;clean++) {
            char hit_text[16]; snprintf(hit_text,sizeof(hit_text),"%u",clean?1:hit);
            printf("{\"event\":\"T02_OWNER_START\",\"case_index\":%u,\"name\":\"%s\",\"hit\":%u,\"clean_retry\":%s}\n",cases,points[p].name,hit,clean?"true":"false");
            fflush(stdout);
            pid_t child=fork(); if (child<0) return 1;
            if (!child) {
                if (syscall(SYS_close_range,3u,UINT_MAX,0u)) _exit(125);
                execl(program,program,"--scenario","T02-owner-child","--failpoint",clean?"":points[p].name,
                      "--fail-hit",hit_text,(char *)NULL);
                _exit(127);
            }
            int status; pid_t got;
            do { got=waitpid(child,&status,0); } while (got<0&&errno==EINTR);
            if (got!=child||!WIFEXITED(status)||WEXITSTATUS(status)) { puts("LAB_RESULT=FAIL"); return 1; }
        }
    }
    printf("{\"event\":\"M3_CASE\",\"id\":\"T02_OWNER\",\"status\":\"PASS\",\"variants\":%u,\"clean_retries\":%u,\"partial_pci_enumeration\":\"NOT_RUN\"}\n",cases,cases);
    puts("LAB_RESULT=PASS"); return 0;
}
static int isolated_group(struct admin *a)
{
    char path[PATH_MAX];
    snprintf(path,sizeof(path),"/sys/kernel/iommu_groups/%u/devices",a->group);
    DIR *directory=opendir(path);
    if (!directory) return -1;
    struct dirent *entry;
    unsigned members=0;
    while ((entry=readdir(directory))) {
        if (entry->d_name[0]=='.') continue;
        /* 本拓樸要求群組只有自己的 VF，PF 與其他 VF 一律拒絕。 */
        if (strcmp(entry->d_name,a->bdf)) { closedir(directory); errno=EXDEV; return -1; }
        ++members;
    }
    closedir(directory);
    if (members!=1) { errno=EPROTO; return -1; }
    printf("{\"event\":\"%s_GROUP\",\"vf\":%u,\"bdf\":\"%s\",\"group\":%u,\"members\":%u}\n",a->scenario,a->vf,a->bdf,a->group,members);
    return 0;
}
static int isolated_owner_start(struct admin *a,const char *pf,const char *device,const char *tenant,unsigned entries,unsigned requests)
{
    char path[PATH_MAX],group_text[32],*end;
    struct toy_vf_ctl ctl;
    snprintf(a->directory,sizeof(a->directory),"/run/lab-%d-vf%u",getpid(),a->vf);
    if (mkdir(a->directory,0700)) return -1;
    snprintf(a->socket,sizeof(a->socket),"%s/tenant.sock",a->directory);
    snprintf(a->management_path,sizeof(a->management_path),"%s/control.sock",a->directory);
    a->management=lab_listen(a->management_path);
    a->pf=open(device,O_RDWR|O_CLOEXEC|O_NONBLOCK);
    if (a->pf<0||a->management<0) return -1;
    snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/virtfn%u",pf,a->vf);
    if (lab_link_basename(path,a->bdf,sizeof(a->bdf))||snapshot(a)||control(a,TOY_LEASE_ACQUIRE,0,&ctl)) return -1;
    a->lease=ctl.lease_token; a->leased=true;
    snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/iommu_group",a->bdf);
    if (lab_link_basename(path,group_text,sizeof(group_text))) return -1;
    errno=0; unsigned long group=strtoul(group_text,&end,10);
    if (errno||!*group_text||*end||group>UINT_MAX) { errno=EPROTO; return -1; }
    a->group=(unsigned)group;
    if (isolated_group(a)) return -1;
    snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/driver_override",a->bdf);
    if (lab_write_text(path,"vfio-pci\n")||lab_write_text("/sys/bus/pci/drivers_probe",a->bdf)) return -1;
    a->bound=true;
    return start_owner(a,tenant,a->group,entries,requests)||owner_prepare(a)?-1:0;
}
static void isolated_owner_cleanup(struct admin *a)
{
    struct toy_vf_ctl ctl;
    if (a->leased&&a->pf>=0&&!a->child_reaped&&!snapshot(a)) (void)control(a,TOY_FENCE_VF,0,&ctl);
    if (a->child>0&&!a->child_reaped) {
        if (a->pidfd>=0) (void)syscall(SYS_pidfd_send_signal,a->pidfd,SIGKILL,NULL,0);
        else (void)kill(a->child,SIGKILL);
        int status; while (waitpid(a->child,&status,0)<0&&errno==EINTR) {}
        a->child_reaped=true;
    }
    if (a->pf>=0&&release_owner(a)) fprintf(stderr,"LABD_QUARANTINE vf=%u cleanup_errno=%d\n",a->vf,errno);
    if (a->connection>=0) close(a->connection);
    if (a->listen>=0) close(a->listen);
    if (a->management>=0) close(a->management);
    if (a->pidfd>=0) close(a->pidfd);
    if (a->pf>=0) close(a->pf);
    if (a->socket[0]) unlink(a->socket);
    if (a->management_path[0]) unlink(a->management_path);
    if (a->directory[0]) rmdir(a->directory);
}
static int isolation_case(const char *pf,const char *device,const char *tenant,unsigned entries,unsigned requests,unsigned seed)
{
    struct admin owners[2];
    char count_path[PATH_MAX];
    bool enabled=false;
    int ret=1,saved;
    snprintf(count_path,sizeof(count_path),"/sys/bus/pci/devices/%s/sriov_numvfs",pf);
    for (unsigned i=0;i<2;i++) owners[i]=(struct admin){.pf=-1,.listen=-1,.connection=-1,.pidfd=-1,.management=-1,
        .vf=i,.failpoint="",.fail_hit=1,.scenario="T04",.count_path=count_path};
    if (lab_write_text(count_path,"4\n")) goto out;
    enabled=true;
    for (unsigned i=0;i<2;i++) if (isolated_owner_start(&owners[i],pf,device,tenant,entries,requests)) goto out;
    if (owners[0].group==owners[1].group||owners[0].child==owners[1].child||!strcmp(owners[0].bdf,owners[1].bdf)) { errno=EXDEV; goto out; }
    /* B 保留初始映射供 A 寫入期間比對，A 再保留完成結果供 B 運作期間比對。 */
    if (owner_run(&owners[0])||owner_inspect(&owners[1])||owner_run(&owners[1])||owner_inspect(&owners[0])) goto out;
    for (unsigned i=0;i<2;i++) if (owner_stop(&owners[i])||release_owner(&owners[i])) goto out;
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
        if (ioctl(owners[0].pf,TOY_GET_VF,&info)||info.state!=TOY_STATES_FREE||info.reserve||info.outstanding||
            info.lease_token||info.hw_flags!=TOY_HW_FLAGS_FENCED) { errno=EIO; goto out; }
        printf("{\"event\":\"T04_FINAL\",\"vf\":%u,\"state\":%u,\"reserve\":%u,\"outstanding\":%u,\"flags\":%u,\"lease\":%" PRIu64 "}\n",i,info.state,info.reserve,info.outstanding,info.hw_flags,(uint64_t)info.lease_token);
    }
    printf("{\"event\":\"M3_CASE\",\"id\":\"T04\",\"status\":\"PASS\",\"checks\":{\"entries\":%u,\"requests\":%u,\"seed\":%u,\"owners\":2,\"owner_closed\":true,\"all_free\":true}}\n",entries,requests,seed);
    puts("LAB_RESULT=PASS"); ret=0;
out:
    saved=errno;
    for (unsigned i=0;i<2;i++) isolated_owner_cleanup(&owners[i]);
    if (enabled&&lab_write_text(count_path,"0\n")) fprintf(stderr,"LABD_DISABLE_FAILED errno=%d\n",errno);
    if (ret) {
        fprintf(stderr,"LABD_ERROR scenario=T04 errno=%d message=%s\n",saved,strerror(saved));
        printf("{\"event\":\"M3_CASE\",\"id\":\"T04\",\"status\":\"FAIL\",\"errno\":%d}\n",saved);
        puts("LAB_RESULT=FAIL");
    }
    return ret;
}
static int dma_fault_counters(struct admin *a,const char *phase,unsigned expected)
{
    char record[1536],*line;
    if (lab_read_text("/sys/kernel/debug/toy_pf0/vf-stats",record,sizeof(record))||snapshot(a)) return -1;
    for (line=record;line&&*line;line=strchr(line,'\n'),line=line?line+1:NULL) {
        unsigned vf,desc,cpl,dma,blocked,irq;
        if (sscanf(line,"vf=%u stale_desc=%u stale_cpl=%u dma_fault=%u fence_blocked=%u irq_suppressed=%u",&vf,&desc,&cpl,&dma,&blocked,&irq)!=6) { errno=EPROTO; return -1; }
        if (vf!=a->vf) continue;
        if (dma!=expected||desc||cpl||a->info.state!=(expected?TOY_STATES_FAILED:TOY_STATES_ACTIVE)||
            a->info.reserve!=8||a->info.outstanding||a->info.lease_token!=a->lease||
            (expected&&(a->info.hw_flags&3u)!=3u)) { errno=EPROTO; return -1; }
        printf("{\"event\":\"T05_COUNTERS\",\"phase\":\"%s\",\"vf\":%u,\"stale_desc\":%u,\"stale_cpl\":%u,\"dma_fault\":%u,\"fence_blocked\":%u,\"irq_suppressed\":%u,\"state\":%u,\"reserve\":%u,\"outstanding\":%u,\"flags\":%u,\"lease\":%" PRIu64 "}\n",phase,vf,desc,cpl,dma,blocked,irq,a->info.state,a->info.reserve,a->info.outstanding,a->info.hw_flags,a->lease);
        return 0;
    }
    errno=ENOENT; return -1;
}
static int dma_fault_case(const char *pf,const char *device,const char *tenant,unsigned variant)
{
    struct admin owners[2];
    char count_path[PATH_MAX];
    bool enabled=false;
    int ret=1,saved;
    snprintf(count_path,sizeof(count_path),"/sys/bus/pci/devices/%s/sriov_numvfs",pf);
    for (unsigned i=0;i<2;i++) owners[i]=(struct admin){.pf=-1,.listen=-1,.connection=-1,.pidfd=-1,.management=-1,
        .vf=i,.failpoint="",.fail_hit=1,.scenario="T05",.count_path=count_path,.dma_fault=i?0:variant};
    if (!variant||variant>6) { errno=EINVAL; goto out; }
    if (lab_write_text(count_path,"4\n")) goto out;
    enabled=true;
    for (unsigned i=0;i<2;i++) if (isolated_owner_start(&owners[i],pf,device,tenant,16,4096)||dma_fault_counters(&owners[i],"before",0)) goto out;
    if (owners[0].group==owners[1].group||owners[0].child==owners[1].child) { errno=EXDEV; goto out; }
    /* A 觸發真實 IOMMU 失敗後先查 B 原始 guard，再驗證 B 的正常 I/O。 */
    if (owner_run(&owners[0])||dma_fault_counters(&owners[0],"after",1)||owner_inspect(&owners[1])||
        owner_run(&owners[1])||dma_fault_counters(&owners[1],"after",0)||owner_inspect(&owners[0])) goto out;
    for (unsigned i=0;i<2;i++) if (owner_stop(&owners[i])||release_owner(&owners[i])) goto out;
    int disable=lab_write_text(count_path,"0\n"),disable_errno=disable?errno:0;
    uint64_t count;
    if (disable||lab_read_u64(count_path,&count)||count) { errno=EPROTO; goto out; }
    enabled=false;
    for (unsigned i=0;i<4;i++) {
        char path[PATH_MAX]; struct stat st;
        snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/virtfn%u",pf,i);
        if (!lstat(path,&st)||errno!=ENOENT) { errno=EBUSY; goto out; }
        if (i<2) {
            snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s",owners[i].bdf);
            if (!lstat(path,&st)||errno!=ENOENT) { errno=EBUSY; goto out; }
        }
    }
    /* 已驗證的 DESTROY 可釋放 FAILED 配額，KILL latch 必須等全域復原才清除。 */
    for (unsigned i=0;i<8;i++) {
        struct toy_vf_info info={.hdr=header(sizeof(info)),.vf=i};
        if (ioctl(owners[0].pf,TOY_GET_VF,&info)||info.state||info.reserve||info.outstanding||info.lease_token||info.hw_flags!=(i?1u:3u)) { errno=EPROTO; goto out; }
        printf("{\"event\":\"T05_POST_DISABLE\",\"vf\":%u,\"state\":%u,\"reserve\":%u,\"flags\":%u,\"lease\":%" PRIu64 "}\n",i,info.state,info.reserve,info.hw_flags,(uint64_t)info.lease_token);
    }
    struct toy_info info={.hdr=header(sizeof(info))};
    struct toy_vf_ctl ctl={.hdr=header(sizeof(ctl)),.vf=TOY_CTL_GLOBAL,.operation=++owners[0].operation};
    if (ioctl(owners[0].pf,TOY_GET_INFO,&info)) goto out;
    ctl.session=info.session;
    if (ioctl(owners[0].pf,TOY_GLOBAL_RECOVER,&ctl)||ctl.session<=info.session) goto out;
    printf("{\"event\":\"T05_GLOBAL_RECOVER\",\"before_session\":%" PRIu64 ",\"after_session\":%" PRIu64 ",\"disable_errno\":%d,\"vfs\":0}\n",(uint64_t)info.session,(uint64_t)ctl.session,disable_errno);
    for (unsigned i=0;i<8;i++) {
        struct toy_vf_info final={.hdr=header(sizeof(final)),.vf=i};
        if (ioctl(owners[0].pf,TOY_GET_VF,&final)||final.state||final.reserve||final.outstanding||final.lease_token||final.hw_flags!=1) { errno=EPROTO; goto out; }
        printf("{\"event\":\"T05_FINAL\",\"vf\":%u,\"state\":%u,\"reserve\":%u,\"outstanding\":%u,\"flags\":%u,\"lease\":%" PRIu64 "}\n",i,final.state,final.reserve,final.outstanding,final.hw_flags,(uint64_t)final.lease_token);
    }
    printf("{\"event\":\"M3_CASE\",\"id\":\"T05\",\"status\":\"PASS\",\"checks\":{\"variant\":%u,\"entries\":16,\"healthy_requests\":4096,\"owners\":2,\"owner_closed\":true,\"all_free\":true}}\n",variant);
    puts("LAB_RESULT=PASS"); ret=0;
out:
    saved=errno;
    for (unsigned i=0;i<2;i++) isolated_owner_cleanup(&owners[i]);
    if (enabled&&lab_write_text(count_path,"0\n")) fprintf(stderr,"LABD_DISABLE_FAILED errno=%d\n",errno);
    if (ret) {
        fprintf(stderr,"LABD_ERROR scenario=T05 errno=%d message=%s\n",saved,strerror(saved));
        printf("{\"event\":\"M3_CASE\",\"id\":\"T05\",\"status\":\"FAIL\",\"errno\":%d}\n",saved);
        puts("LAB_RESULT=FAIL");
    }
    return ret;
}
static int directory_entries(const char *path)
{
    DIR *directory=opendir(path);
    if (!directory) return -1;
    struct dirent *entry;
    int count=0;
    while ((entry=readdir(directory))) {
        if (entry->d_name[0]=='.') continue;
        if (!strcmp(path,"/proc/self/fd")&&atoi(entry->d_name)==dirfd(directory)) continue;
        ++count;
    }
    closedir(directory);
    return count;
}
static int lifecycle_quiet(int fd,unsigned cycle,int baseline_fds,int baseline_tasks)
{
    unsigned contexts,leases,orphaned,events,work,pending,enabled,transaction,drained=0;
    char line[256];
    struct toy_event event;
    int64_t deadline=lab_now_ms()+5000;
    for (;;) {
        ssize_t got;
        do {
            got=read(fd,&event,sizeof(event));
            if (got==(ssize_t)sizeof(event)) {
                if (event.hdr.size!=sizeof(event)||event.type==TOY_EVENT_LOST_EVENTS||++drained>256) { errno=EPROTO; return -1; }
            } else if (got>=0) { errno=EPROTO; return -1; }
        } while (got>0||(got<0&&errno==EINTR));
        if (errno!=EAGAIN) return -1;
        if (lab_read_text("/sys/kernel/debug/toy_pf0/lifecycle",line,sizeof(line))) return -1;
        if (sscanf(line,"contexts=%u leases=%u orphaned=%u events=%u work_busy=%u pending_irqs=%u enabled=%u transaction=%u",
                   &contexts,&leases,&orphaned,&events,&work,&pending,&enabled,&transaction)!=8) { errno=EPROTO; return -1; }
        if (contexts!=1||leases||orphaned||enabled||transaction) {
            fprintf(stderr,"T01_DRIVER_REJECT %s",line); errno=EBUSY; return -1;
        }
        if (!events&&!work&&!pending) break;
        if (lab_now_ms()>=deadline) { errno=ETIMEDOUT; return -1; }
        usleep(1000);
    }
    int fds=directory_entries("/proc/self/fd"),tasks=directory_entries("/proc/self/task");
    /* waitpid 的 ECHILD 同時確認沒有仍存活或尚未 reap 的子程序，不依賴選配 proc children。 */
    int child_status;
    pid_t child;
    do { child=waitpid(-1,&child_status,WNOHANG); } while (child<0&&errno==EINTR);
    if (child!=-1||errno!=ECHILD||fds!=baseline_fds||tasks!=baseline_tasks) {
        fprintf(stderr,"T01_PROCESS_REJECT fds=%d baseline_fds=%d tasks=%d baseline_tasks=%d waitpid=%d errno=%d\n",fds,baseline_fds,tasks,baseline_tasks,child,errno);
        errno=EBUSY; return -1;
    }
    printf("{\"event\":\"T01_LIFECYCLE\",\"cycle\":%u,\"contexts\":%u,\"leases\":%u,\"orphaned\":%u,\"events\":%u,\"work_busy\":%u,\"pending_irqs\":%u,\"drained\":%u,\"fds\":%d,\"baseline_fds\":%d,\"tasks\":%d,\"baseline_tasks\":%d,\"children\":0}\n",cycle,contexts,leases,orphaned,events,work,pending,drained,fds,baseline_fds,tasks,baseline_tasks);
    return 0;
}
static int cycle_ledger(int fd,const char *pf,unsigned cycle,bool enabled)
{
    struct toy_info info={.hdr=header(sizeof(info))};
    if (ioctl(fd,TOY_GET_INFO,&info)||info.enabled_vfs!=(enabled?4u:0u)) { errno=EPROTO; return -1; }
    for (unsigned vf=0;vf<8;vf++) {
        struct toy_vf_info v={.hdr=header(sizeof(v)),.vf=vf};
        unsigned reserve=enabled&&vf<4?8u:0u;
        if (ioctl(fd,TOY_GET_VF,&v)||v.reserve!=reserve||v.outstanding||v.lease_token||
            v.state!=(reserve?TOY_STATES_ACTIVE:TOY_STATES_FREE)||v.hw_flags!=(reserve?(TOY_HW_FLAGS_FENCED|TOY_HW_FLAGS_PCI_PRESENT):TOY_HW_FLAGS_FENCED)) { errno=EPROTO; return -1; }
        if (!enabled) {
            char path[PATH_MAX]; struct stat st;
            snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/virtfn%u",pf,vf);
            if (!lstat(path,&st)||errno!=ENOENT) { errno=EBUSY; return -1; }
        }
        printf("{\"event\":\"T01_LEDGER\",\"cycle\":%u,\"enabled\":%u,\"vf\":%u,\"state\":%u,\"reserve\":%u,\"outstanding\":%u,\"flags\":%u,\"lease\":%" PRIu64 "}\n",cycle,enabled?4:0,vf,v.state,v.reserve,v.outstanding,v.hw_flags,(uint64_t)v.lease_token);
    }
    return 0;
}
static int lifecycle_case(const char *pf,const char *device,const char *tenant)
{
    char count_path[PATH_MAX];
    bool enabled=false;
    int ret=1,saved,fd=open(device,O_RDWR|O_NONBLOCK|O_CLOEXEC);
    snprintf(count_path,sizeof(count_path),"/sys/bus/pci/devices/%s/sriov_numvfs",pf);
    if (fd<0) goto out;
    int baseline_fds=directory_entries("/proc/self/fd"),baseline_tasks=directory_entries("/proc/self/task");
    if (baseline_fds<4||baseline_tasks!=1||lab_inventory_checkpoint("T01",0,"cold")) goto out;
    /* 暖機也走完整 owner 生命週期，之後另外量測一百次。 */
    for (unsigned cycle=0;cycle<=100;cycle++) {
        char bdfs[4][32];
        struct admin owners[4];
        int failed=0,error=0;
        for (unsigned vf=0;vf<4;vf++) owners[vf]=(struct admin){.pf=-1,.listen=-1,.connection=-1,.pidfd=-1,.management=-1,
            .vf=vf,.failpoint="",.fail_hit=1,.scenario="T01",.count_path=count_path};
        printf("{\"event\":\"T01_CYCLE_BEGIN\",\"cycle\":%u,\"warmup\":%s}\n",cycle,cycle?"false":"true");
        if (lab_write_text(count_path,"4\n")) goto out;
        enabled=true;
        if (cycle_ledger(fd,pf,cycle,true)) goto out;
        for (unsigned vf=0;vf<4;vf++) {
            printf("{\"event\":\"T01_OWNER_BEGIN\",\"cycle\":%u,\"vf\":%u}\n",cycle,vf);
            if (isolated_owner_start(&owners[vf],pf,device,tenant,16,0)||owner_run(&owners[vf])) { failed=1; break; }
            strcpy(bdfs[vf],owners[vf].bdf);
        }
        /* 先通知全部 owner，再等待各自清理；不可讓第一個 FLR 等其他三次串行 close。 */
        if (!failed) for (unsigned vf=0;vf<4;vf++)
            if (owner_stop_begin(&owners[vf])) { failed=1; break; }
        if (!failed) for (unsigned vf=0;vf<4;vf++)
            if (owner_stop_finish(&owners[vf])||release_owner(&owners[vf])) { failed=1; break; }
        /* 所有 lease 關閉後立即撤掉 PCI functions，再做 socket/目錄清理。 */
        if (!failed) {
            if (lab_write_text(count_path,"0\n")) failed=1;
            else enabled=false;
        }
        error=errno;
        for (unsigned vf=0;vf<4;vf++) isolated_owner_cleanup(&owners[vf]);
        if (failed) { errno=error; goto out; }
        for (unsigned vf=0;vf<4;vf++) {
            char path[PATH_MAX]; struct stat st;
            snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s",bdfs[vf]);
            if (!lstat(path,&st)||errno!=ENOENT) { errno=EBUSY; goto out; }
        }
        if (cycle_ledger(fd,pf,cycle,false)||lifecycle_quiet(fd,cycle,baseline_fds,baseline_tasks)) goto out;
        printf("{\"event\":\"T01_CYCLE_END\",\"cycle\":%u,\"owners\":4,\"pci_devices_absent\":4,\"status\":\"PASS\"}\n",cycle);
        if (!cycle&&lab_inventory_checkpoint("T01",1,"initial")) goto out;
    }
    if (lab_inventory_checkpoint("T01",2,"final")) goto out;
    puts("{\"event\":\"M3_CASE\",\"id\":\"T01\",\"status\":\"PASS\",\"cycles\":100,\"warmup_cycles\":1,\"owners_per_cycle\":4,\"remaining_fds\":0,\"remaining_maps\":0,\"remaining_work\":0}");
    puts("LAB_RESULT=PASS"); ret=0;
out:
    saved=errno;
    if (enabled&&lab_write_text(count_path,"0\n")) fprintf(stderr,"LABD_DISABLE_FAILED errno=%d\n",errno);
    if (fd>=0) close(fd);
    if (ret) {
        fprintf(stderr,"LABD_ERROR scenario=T01 errno=%d message=%s\n",saved,strerror(saved));
        printf("{\"event\":\"M3_CASE\",\"id\":\"T01\",\"status\":\"FAIL\",\"errno\":%d}\n",saved);
        puts("LAB_RESULT=FAIL");
    }
    return ret;
}
#include "t06.inc"
#include "recovery.inc"
int main(int argc,char **argv)
{
    struct admin a={.pf=-1,.listen=-1,.connection=-1,.pidfd=-1,.management=-1,.failpoint="",.fail_hit=1};
    const char *pf="0000:01:00.0",*device="/dev/toy_pf0",*tenant="/usr/bin/tenant",*scenario="T03";
    unsigned entries=16,requests=4096,seed=101;
    char path[PATH_MAX],group_text[32],count_path[PATH_MAX];
    struct toy_vf_ctl ctl;
    bool enabled=false;
    int ret=1,saved=0;
    setvbuf(stdout,NULL,_IOLBF,0);
    for (int i=1;i<argc;i++) {
        if (i+1>=argc) { errno=EINVAL; goto out; }
        const char *key=argv[i++],*value=argv[i];
        if (!strcmp(key,"--pf")) pf=value;
        else if (!strcmp(key,"--device")) device=value;
        else if (!strcmp(key,"--tenant")) tenant=value;
        else if (!strcmp(key,"--scenario")) scenario=value;
        else if (!strcmp(key,"--failpoint")) a.failpoint=value;
        else {
            char *end; errno=0; unsigned long n=strtoul(value,&end,0);
            if (errno||!*value||*end||n>UINT_MAX) { errno=EINVAL; goto out; }
            if (!strcmp(key,"--entries")) entries=(unsigned)n;
            else if (!strcmp(key,"--seed")) seed=(unsigned)n;
            else if (!strcmp(key,"--requests")) requests=(unsigned)n;
            else if (!strcmp(key,"--fail-hit")&&n) a.fail_hit=(unsigned)n;
            else if (!strcmp(key,"--dma-fault")&&n<=6) a.dma_fault=(unsigned)n;
            else { errno=EINVAL; goto out; }
        }
    }
    if (!strcmp(scenario,"T02-owner")) return owner_fault_matrix(argv[0]);
    if (!strcmp(scenario,"T01")) return lifecycle_case(pf,device,tenant);
    if (!strcmp(scenario,"T05")) return dma_fault_case(pf,device,tenant,a.dma_fault);
    if (!strcmp(scenario,"T06")) return vfio_reset_case(pf,device,tenant);
    if (!strcmp(scenario,"T07")||!strcmp(scenario,"T08")) return recovery_case(pf,device,tenant,!strcmp(scenario,"T07")?7:8);
    if (strcmp(scenario,"T03")&&strcmp(scenario,"T04")&&strcmp(scenario,"T09")&&strcmp(scenario,"T10")&&strcmp(scenario,"T02-provision")&&strcmp(scenario,"T02-pci")&&strcmp(scenario,"T02-owner-child")) { errno=EOPNOTSUPP; goto out; }
    a.scenario=scenario; a.count_path=count_path;
    if (requests<4096||(entries!=16&&entries!=32&&entries!=256)) { errno=EINVAL; goto out; }
    if (!strcmp(scenario,"T04")) return isolation_case(pf,device,tenant,entries,requests,seed);
    snprintf(a.directory,sizeof(a.directory),"/run/lab-%d",getpid());
    if (mkdir(a.directory,0700)) goto out;
    snprintf(a.socket,sizeof(a.socket),"%s/tenant.sock",a.directory);
    snprintf(a.management_path,sizeof(a.management_path),"%s/control.sock",a.directory);
    a.management=lab_listen(a.management_path); if (a.management<0) goto out;
    printf("{\"event\":\"M3_ADMIN_SOCKET\",\"path\":\"%s\"}\n",a.management_path);
    snprintf(count_path,sizeof(count_path),"/sys/bus/pci/devices/%s/sriov_numvfs",pf);
    a.pf=open(device,O_RDWR|O_CLOEXEC|O_NONBLOCK); if (a.pf<0) goto out;
    if (!strcmp(scenario,"T02-provision")) {
        if (!lab_t02_provision(pf,a.pf)) ret=0;
        goto out;
    }
    if (!strcmp(scenario,"T02-pci")) {
        if (!lab_t02_pci(pf,a.pf)) ret=0;
        goto out;
    }
    if (lab_write_text(count_path,"4\n")) goto out;
    enabled=true;
    snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/virtfn0",pf);
    if (lab_link_basename(path,a.bdf,sizeof(a.bdf))||snapshot(&a)||control(&a,TOY_LEASE_ACQUIRE,0,&ctl)) goto out;
    a.lease=ctl.lease_token; a.leased=true;
    snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/iommu_group",a.bdf);
    if (lab_link_basename(path,group_text,sizeof(group_text))) goto out;
    char *end; errno=0; unsigned long group=strtoul(group_text,&end,10);
    if (errno||*end||group>UINT_MAX) { errno=EPROTO; goto out; }
    snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/driver_override",a.bdf);
    if (lab_write_text(path,"vfio-pci\n")||lab_write_text("/sys/bus/pci/drivers_probe",a.bdf)) goto out;
    a.bound=true;
    if (start_owner(&a,tenant,(unsigned)group,entries,requests)) goto out;
    int session_ret=owner_session(&a);
    if (*a.failpoint) {
        if (!session_ret||failed_owner(&a)) goto out;
    } else if (session_ret) goto out;
    if (release_owner(&a)) goto out;
    if (lab_write_text(count_path,"0\n")) goto out;
    enabled=false;
    for (a.vf=0;a.vf<8;a.vf++) {
        if (snapshot(&a)||a.info.state!=TOY_STATES_FREE||a.info.reserve||a.info.outstanding||
            (a.info.hw_flags&(TOY_HW_FLAGS_PCI_PRESENT|TOY_HW_FLAGS_CONFIG_VALID))||!(a.info.hw_flags&TOY_HW_FLAGS_FENCED)) { errno=EIO; goto out; }
    }
    if (!strcmp(scenario,"T02-owner-child")) {
        printf("{\"event\":\"T02_OWNER_CASE\",\"name\":\"%s\",\"hit\":%u,\"pid\":%d,\"owner_closed\":true,\"all_free\":true,\"status\":\"PASS\"}\n",a.failpoint,a.fail_hit,a.child);
    } else {
        printf("{\"event\":\"M3_CASE\",\"id\":\"%s\",\"scenario\":\"%s\",\"variant\":\"entries-%u\",\"status\":\"PASS\",\"checks\":{\"entries\":%u,\"requests\":%u,\"seed\":%u,\"owner_closed\":true,\"all_free\":true}}\n",scenario,scenario,entries,entries,requests,seed);
        puts("LAB_RESULT=PASS");
    }
    ret=0;
out:
    saved=errno;
    if (ret) fprintf(stderr,"LABD_ERROR scenario=%s errno=%d message=%s\n",scenario,saved,strerror(saved));
    if (a.leased&&a.vf<8&&a.pf>=0&&!a.child_reaped) {
        if (!snapshot(&a)) (void)control(&a,TOY_FENCE_VF,0,&ctl);
    }
    if (a.child>0&&!a.child_reaped) {
        if (a.pidfd>=0) (void)syscall(SYS_pidfd_send_signal,a.pidfd,SIGKILL,NULL,0);
        else (void)kill(a.child,SIGKILL);
        int status; while (waitpid(a.child,&status,0)<0&&errno==EINTR) {}
        a.child_reaped=true;
    }
    if (ret&&a.pf>=0) {
        if (release_owner(&a)) fprintf(stderr,"LABD_QUARANTINE cleanup_errno=%d\n",errno);
        else if (enabled&&lab_write_text(count_path,"0\n")) fprintf(stderr,"LABD_DISABLE_FAILED errno=%d\n",errno);
    }
    if (a.connection>=0) close(a.connection);
    if (a.listen>=0) close(a.listen);
    if (a.management>=0) close(a.management);
    if (a.pidfd>=0) close(a.pidfd);
    if (a.pf>=0) close(a.pf);
    if (a.socket[0]) unlink(a.socket);
    if (a.management_path[0]) unlink(a.management_path);
    if (a.directory[0]) rmdir(a.directory);
    if (ret) { printf("{\"event\":\"M3_CASE\",\"id\":\"%s\",\"status\":\"FAIL\",\"errno\":%d}\n",scenario,saved); puts("LAB_RESULT=FAIL"); }
    return ret;
}
