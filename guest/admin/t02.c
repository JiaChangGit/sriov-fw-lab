#define _GNU_SOURCE
#include "t02.h"
#include "../common/lab_wire.h"
#include "../tenant/vfio.h"
#include "toy_uapi.h"
#include "toy_regs.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

struct t02 {
    int fd; unsigned serial, cases; uint64_t operation;
    const char *scenario;
    char count_path[PATH_MAX],vf_bdfs[4][32];
};
static struct toy_uapi_header header(size_t n)
{ return (struct toy_uapi_header){n,TOY_UAPI_MAJOR,TOY_UAPI_MINOR}; }

static int barrier(struct t02 *t,const char *action,const char *slot,const char *name,unsigned hit,unsigned vf)
{
    struct termios old,raw;
    char token[64],expected[96],line[96];
    size_t used=0;
    int ret=-1,saved;
    if (tcgetattr(STDIN_FILENO,&old)) return -1;
    raw=old; cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO,TCSANOW,&raw)) return -1;
    snprintf(token,sizeof(token),"t02-%u",++t->serial);
    snprintf(expected,sizeof(expected),"M3_HOST_ACK %s\n",token);
    printf("{\"event\":\"M3_BARRIER\",\"scenario\":\"%s\",\"phase\":\"request\",\"barrier\":\"%s\",\"action\":\"%s\",\"slot\":\"%s\",\"name\":\"%s\",\"hit\":%u,\"vf\":%u}\n",t->scenario,token,action,slot,name,hit,vf);
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
int lab_inventory_checkpoint(const char *scenario,unsigned serial,const char *name)
{
    struct t02 t={.scenario=scenario,.serial=serial};
    return barrier(&t,"inventory","",name,0,0);
}

static int driver_arm(const char *slot,const char *name,unsigned hit)
{
    char path[128],record[96];
    snprintf(path,sizeof(path),"/sys/kernel/debug/toy_pf0/%s",slot);
    snprintf(record,sizeof(record),"%s %u\n",name,hit);
    return lab_write_text(path,record);
}
static int driver_check(const char *slot,const char *name,unsigned hit)
{
    char path[128],record[160],actual[64]; unsigned armed,seen,fired;
    snprintf(path,sizeof(path),"/sys/kernel/debug/toy_pf0/%s",slot);
    if (lab_read_text(path,record,sizeof(record))) return -1;
    if (sscanf(record,"name=%63s hit=%u seen=%u fired=%u",actual,&armed,&seen,&fired)!=4||
        strcmp(actual,name)||armed!=hit||seen<hit||fired!=1) { errno=EPROTO; return -1; }
    printf("{\"event\":\"T02_DRIVER_HIT\",\"slot\":\"%s\",\"name\":\"%s\",\"hit\":%u,\"seen\":%u,\"fired\":%u}\n",slot,name,hit,seen,fired);
    return driver_arm(slot,"off",1);
}
static int ledger(struct t02 *t,int retained,const char *phase)
{
    uint64_t num;
    if (lab_read_u64(t->count_path,&num)||num) { errno=EBUSY; return -1; }
    for (unsigned i=0;i<4;i++) {
        char path[PATH_MAX];
        if (!t->vf_bdfs[i][0]) { errno=EPROTO; return -1; }
        snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s",t->vf_bdfs[i]);
        if (!access(path,F_OK)||errno!=ENOENT) { errno=EBUSY; return -1; }
    }
    for (unsigned vf=0;vf<8;vf++) {
        struct toy_vf_info info={.hdr=header(sizeof(info)),.vf=vf};
        if (ioctl(t->fd,TOY_GET_VF,&info)) return -1;
        bool failed=(int)vf==retained;
        if (info.state!=(failed?TOY_STATES_FAILED:TOY_STATES_FREE)||info.reserve!=(failed?8u:0u)||
            info.outstanding||!(info.hw_flags&TOY_HW_FLAGS_FENCED)||
            (info.hw_flags&(TOY_HW_FLAGS_CONFIG_VALID|TOY_HW_FLAGS_PCI_PRESENT))||
            !!(info.hw_flags&TOY_HW_FLAGS_KILLED)!=failed) { errno=EPROTO; return -1; }
        printf("{\"event\":\"T02_LEDGER\",\"case_index\":%u,\"phase\":\"%s\",\"vf\":%u,\"state\":%u,\"reserve\":%u,\"flags\":%u,\"outstanding\":%u}\n",t->cases,phase,vf,info.state,info.reserve,info.hw_flags,info.outstanding);
    }
    return 0;
}
static int recover(struct t02 *t)
{
    struct toy_info info={.hdr=header(sizeof(info))};
    struct toy_vf_ctl ctl={.hdr=header(sizeof(ctl)),.vf=TOY_CTL_GLOBAL,.operation=++t->operation};
    if (ioctl(t->fd,TOY_GET_INFO,&info)) return -1;
    ctl.session=info.session;
    if (ioctl(t->fd,TOY_GLOBAL_RECOVER,&ctl)||ctl.session<=info.session) return -1;
    printf("{\"event\":\"T02_GLOBAL_RECOVER\",\"case_index\":%u,\"before_session\":%" PRIu64 ",\"after_session\":%" PRIu64 "}\n",t->cases,(uint64_t)info.session,(uint64_t)ctl.session);
    return 0;
}
static int clean_cycle(struct t02 *t)
{
    if (lab_write_text(t->count_path,"4\n")) return -1;
    for (unsigned i=0;i<4;i++) {
        char path[PATH_MAX];
        snprintf(path,sizeof(path),"%.*svirtfn%u",(int)(strlen(t->count_path)-strlen("sriov_numvfs")),t->count_path,i);
        if (lab_link_basename(path,t->vf_bdfs[i],sizeof(t->vf_bdfs[i]))) return -1;
    }
    if (lab_write_text(t->count_path,"0\n")) return -1;
    return 0;
}
static int fault_case(struct t02 *t,const char *point,unsigned hit,int driver,const char *unwind,int unwind_driver,int retained)
{
    ++t->cases;
    if (driver?driver_arm("forward",point,hit):barrier(t,"failpoint-arm","forward",point,hit,UINT32_MAX)) return -1;
    if (unwind && (unwind_driver?driver_arm("unwind",unwind,1):barrier(t,"failpoint-arm","unwind",unwind,1,(unsigned)retained))) return -1;
    errno=0;
    int write_ret=lab_write_text(t->count_path,"4\n"),write_errno=errno;
    if (!write_ret||!write_errno) { errno=EPROTO; return -1; }
    if (driver?driver_check("forward",point,hit):barrier(t,"failpoint-check","forward",point,hit,UINT32_MAX)) return -1;
    if (unwind && (unwind_driver?driver_check("unwind",unwind,1):barrier(t,"failpoint-check","unwind",unwind,1,(unsigned)retained))) return -1;
    if (ledger(t,retained,"after_fault")) return -1;
    if (retained>=0&&recover(t)) return -1;
    if (clean_cycle(t)||ledger(t,-1,"after_clean_cycle")) return -1;
    printf("{\"event\":\"T02_PROVISION_CASE\",\"case_index\":%u,\"name\":\"%s\",\"hit\":%u,\"driver\":%d,\"unwind\":\"%s\",\"retained_vf\":%d,\"expected_errno\":%d,\"clean_cycle\":true,\"status\":\"PASS\"}\n",t->cases,point,hit,driver,unwind?unwind:"",retained,write_errno);
    return 0;
}
int lab_t02_provision(const char *pf,int fd)
{
    static const char *const device_points[]={"provision.before_create","provision.after_create","provision.before_commit"};
    struct t02 t={.fd=fd,.scenario="T02-provision"};
    snprintf(t.count_path,sizeof(t.count_path),"/sys/bus/pci/devices/%s/sriov_numvfs",pf);
    if (barrier(&t,"inventory","","cold",0,0)||clean_cycle(&t)||barrier(&t,"inventory","","initial",0,0)) return -1;
    for (unsigned i=0;i<3;i++) for (unsigned hit=1;hit<=4;hit++)
        if (fault_case(&t,device_points[i],hit,0,NULL,0,-1)) return -1;
    for (unsigned hit=1;hit<=4;hit++)
        if (fault_case(&t,"provision.host_record",hit,1,NULL,0,-1)) return -1;
    if (fault_case(&t,"provision.pci_enable",1,1,NULL,0,-1)||
        fault_case(&t,"provision.after_pci_enable",1,1,NULL,0,-1)) return -1;
    for (unsigned hit=1;hit<=4;hit++)
        if (fault_case(&t,"provision.before_commit",hit,0,"provision.abort_release",0,(int)hit-1)) return -1;
    if (fault_case(&t,"provision.pci_enable",1,1,"destroy.release",0,3)||
        fault_case(&t,"provision.pci_enable",1,1,"unwind.destroy",1,3)||
        barrier(&t,"inventory","","final",0,0)) return -1;
    printf("{\"event\":\"M3_CASE\",\"id\":\"T02_PROVISION\",\"scenario\":\"T02-provision\",\"status\":\"PASS\",\"cases\":%u,\"retained_cases\":6,\"owner_failure_tests\":\"NOT_RUN\",\"partial_pci_enumeration\":\"NOT_RUN\"}\n",t.cases);
    puts("LAB_RESULT=PASS"); return 0;
}

int lab_t02_pci(const char *pf,int fd)
{
    const char *hook="/sys/kernel/debug/toy_sriov_fail_nth";
    struct t02 t={.fd=fd,.scenario="T02-pci"};
    snprintf(t.count_path,sizeof(t.count_path),"/sys/bus/pci/devices/%s/sriov_numvfs",pf);
    if (barrier(&t,"inventory","","cold",0,0)||clean_cycle(&t)||barrier(&t,"inventory","","initial",0,0)) return -1;
    for (unsigned index=1;index<=5;index++) {
        char value[16]; uint64_t observed;
        unsigned nth=index<=4?index:4;
        t.cases=index;
        snprintf(value,sizeof(value),"%u\n",nth);
        if (index==5&&barrier(&t,"failpoint-arm","unwind","destroy.release",1,3)) return -1;
        if (lab_write_text(hook,value)||lab_read_u64(hook,&observed)||observed!=nth) { errno=EPROTO; return -1; }
        errno=0;
        int rc=lab_write_text(t.count_path,"4\n"),error=errno;
        if (!rc||error!=ENOMEM||lab_read_u64(hook,&observed)||observed) { errno=EPROTO; return -1; }
        if (index==5&&barrier(&t,"failpoint-check","unwind","destroy.release",1,3)) return -1;
        if (ledger(&t,index==5?3:-1,"after_fault")||(index==5&&recover(&t))||
            clean_cycle(&t)||ledger(&t,-1,"after_clean_cycle")) return -1;
        printf("{\"event\":\"T02_PCI_CASE\",\"case_index\":%u,\"nth\":%u,\"expected_errno\":%d,\"hook_consumed\":true,\"retained_vf\":%d,\"pci_devices_absent\":true,\"clean_retry\":true,\"status\":\"PASS\"}\n",index,nth,error,index==5?3:-1);
    }
    if (barrier(&t,"inventory","","final",0,0)) return -1;
    puts("{\"event\":\"M3_CASE\",\"id\":\"T02_PCI\",\"status\":\"PASS\",\"variants\":5,\"retained_cases\":1,\"partial_pci_enumeration\":\"PASS\"}");
    puts("LAB_RESULT=PASS"); return 0;
}
