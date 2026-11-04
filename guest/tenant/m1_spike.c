#define _GNU_SOURCE
#include "vfio.h"
#include "toy_regs.h"
#include "toy_uapi.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/pci_regs.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define VF_COUNT 4
#define CYCLES 100

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)p[0]|((uint16_t)p[1]<<8);
}
static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static int error(const char *stage)
{
    fprintf(stderr,"M1_ERROR stage=%s errno=%d message=%s\n",stage,errno,strerror(errno));
    return -1;
}
static int require(int condition, const char *stage)
{
    if(condition) return 0;
    errno=EPROTO;
    return error(stage);
}

static int host_checkpoint(const char *event)
{
    static const char expected[]="M1_HOST_CHECKED\n";
    struct termios original,raw;
    struct timespec now;
    struct pollfd input;
    int fd=-1,changed=0,saved=0,rc=-1;
    int64_t deadline;
    size_t received=0;
    /* 使用 init 配置的協定 UART，不重新打開指向 kernel UART 的 console。
     * 獨立開啟 stdin 的裝置以保留原 fd flags；先 raw/noecho 再發出事件。 */
    fd=open("/proc/self/fd/0",O_RDONLY|O_CLOEXEC|O_NONBLOCK);
    if(fd<0) goto out;
    if(tcgetattr(fd,&original)) goto out;
    raw=original;
    cfmakeraw(&raw);
    if(tcsetattr(fd,TCSANOW,&raw)) goto out;
    changed=1;
    if(tcflush(fd,TCIFLUSH) || clock_gettime(CLOCK_MONOTONIC,&now)) goto out;
    deadline=(int64_t)now.tv_sec*1000+now.tv_nsec/1000000+10000;
    printf("{\"event\":\"%s\",\"status\":\"WAITING\",\"expected_token\":\"M1_HOST_CHECKED\",\"timeout_ms\":10000}\n",event);
    if(fflush(stdout)) goto out;
    input=(struct pollfd){.fd=fd,.events=POLLIN};
    while(received<sizeof(expected)-1) {
        char byte;
        ssize_t n;
        int ready,remaining;
        if(clock_gettime(CLOCK_MONOTONIC,&now)) goto out;
        remaining=(int)(deadline-((int64_t)now.tv_sec*1000+now.tv_nsec/1000000));
        if(remaining<=0) { errno=ETIMEDOUT; goto out; }
        ready=poll(&input,1,remaining);
        if(ready<0 && errno==EINTR) continue;
        if(ready<0) goto out;
        if(!ready) { errno=ETIMEDOUT; goto out; }
        if(!(input.revents&POLLIN)) { errno=EIO; goto out; }
        n=read(fd,&byte,1);
        if(n<0 && (errno==EINTR || errno==EAGAIN)) continue;
        if(n<0) goto out;
        if(n!=1) { errno=EIO; goto out; }
        if(byte!=expected[received]) { errno=EPROTO; goto out; }
        ++received;
    }
    rc=0;
out:
    saved=errno;
    /* 成功與失敗都恢復協定 UART，且不讓清理覆蓋原始失敗原因。 */
    if(changed && tcsetattr(fd,TCSANOW,&original) && !rc) { saved=errno; rc=-1; }
    if(fd>=0 && close(fd) && !rc) { saved=errno; rc=-1; }
    if(rc) {
        errno=saved;
        return error(event);
    }
    printf("{\"event\":\"M1_HOST_CHECKPOINT_ACK\",\"checkpoint\":\"%s\",\"status\":\"PASS\"}\n",event);
    return 0;
}

static int find_pf(char bdf[32])
{
    DIR *dir=opendir("/sys/bus/pci/devices");
    struct dirent *entry;
    unsigned count=0;
    char path[PATH_MAX];
    if(!dir) return error("enumerate_pci");
    while((entry=readdir(dir))) {
        uint64_t vendor,device;
        if(entry->d_name[0]=='.') continue;
        snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/vendor",entry->d_name);
        if(lab_read_u64(path,&vendor)) continue;
        snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/device",entry->d_name);
        if(lab_read_u64(path,&device)) continue;
        if(vendor==TOY_PCI_VENDOR && device==TOY_PCI_PF) {
            ++count;
            if(strlen(entry->d_name)>=32) { closedir(dir); errno=EOVERFLOW; return error("pf_bdf_length"); }
            strcpy(bdf,entry->d_name);
        }
    }
    if(closedir(dir)) return error("close_pci_directory");
    return require(count==1,"exactly_one_pf");
}

static int read_config(const char *bdf, unsigned char config[4096])
{
    char path[PATH_MAX];
    size_t done=0;
    int fd;
    snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/config",bdf);
    fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd<0) return error("open_config");
    while(done<4096) {
        ssize_t n=pread(fd,config+done,4096-done,(off_t)done);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { close(fd); errno=EIO; return error("read_extended_config"); }
        done+=(size_t)n;
    }
    if(close(fd)) return error("close_config");
    return 0;
}

static int standard_cap(const unsigned char *config, unsigned id, unsigned *result)
{
    unsigned offset=config[PCI_CAPABILITY_LIST]&0xfcU,seen[64]={0},hops=0;
    *result=0;
    while(offset) {
        if(offset<0x40 || offset>0xfc || seen[offset/4] || ++hops>48) { errno=EPROTO; return error("standard_cap_chain"); }
        seen[offset/4]=1;
        if(config[offset]==id) { *result=offset; return 0; }
        offset=config[offset+1]&0xfcU;
    }
    errno=ENOENT; return error("required_standard_cap");
}

static int extended_cap(const unsigned char *config, unsigned id, unsigned *result)
{
    unsigned offset=0x100,seen[1024]={0},hops=0;
    *result=0;
    while(offset) {
        uint32_t header;
        if(offset<0x100 || offset>0xffc || (offset&3U) || seen[offset/4] || ++hops>960) { errno=EPROTO; return error("extended_cap_chain"); }
        seen[offset/4]=1; header=le32(config+offset);
        /* 即使已找到目標，仍走完鏈結，避免略過後段破損或重複的 capability。 */
        if(!header || header==UINT32_MAX || !(header&0xffffU) || !((header>>16)&15U)) {
            errno=EPROTO; return error("extended_cap_header");
        }
        if((header&0xffffU)==id) {
            if(*result) { errno=EPROTO; return error("duplicate_extended_cap"); }
            *result=offset;
        }
        offset=(header>>20)&0xfffU;
    }
    if(*result) return 0;
    errno=ENOENT; return error("required_extended_cap");
}

static int resources(const char *bdf, uint64_t expected_bar0)
{
    char path[PATH_MAX];
    FILE *stream;
    unsigned index;
    snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/resource",bdf);
    stream=fopen(path,"re");
    if(!stream) return error("read_resources");
    for(index=0;index<6;++index) {
        unsigned long long start,end,flags;
        uint64_t expected=index==0?expected_bar0:index==2?TOY_LIMITS_BAR2_BYTES:0;
        if(fscanf(stream,"%llx %llx %llx",&start,&end,&flags)!=3) { fclose(stream); errno=EIO; return error("resource_format"); }
        if(expected && require(start && end>=start && end-start+1==expected && (flags&0x200U) &&
                               !(flags&0x2000U) && (index!=0 || (flags&0x100000U)),"bar_resource_size_type")) { fclose(stream); return -1; }
        if(expected) printf("{\"event\":\"pci_resource\",\"bdf\":\"%s\",\"bar\":%u,\"start\":\"0x%llx\",\"end\":\"0x%llx\",\"flags\":\"0x%llx\",\"size\":%" PRIu64 "}\n",bdf,index,start,end,flags,expected);
    }
    return fclose(stream)?error("close_resources"):0;
}

static int pci_caps(const char *bdf, int is_vf, unsigned *first, unsigned *stride)
{
    unsigned char config[4096];
    unsigned msix,pcie,sriov=0,ari,acs;
    uint32_t table,pba,bar;
    uint64_t vendor,device;
    char path[PATH_MAX];
    if(read_config(bdf,config)) return -1;
    snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/vendor",bdf);
    if(lab_read_u64(path,&vendor)) return error("enumerated_vendor");
    snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/device",bdf);
    if(lab_read_u64(path,&device)) return error("enumerated_device");
    /* VF 的識別字來自 PF SR-IOV capability；原始 config ID 依法可為 ffff。 */
    if(require(vendor==TOY_PCI_VENDOR && device==(is_vf?TOY_PCI_VF:TOY_PCI_PF) &&
               (is_vf || (le16(config)==TOY_PCI_VENDOR && le16(config+2)==TOY_PCI_PF)) &&
               config[PCI_REVISION_ID]==TOY_PCI_REVISION &&
               (le32(config+PCI_CLASS_REVISION)>>8)==TOY_PCI_CLASS_CODE,"pci_identity")) return -1;
    if(standard_cap(config,PCI_CAP_ID_MSIX,&msix) || standard_cap(config,PCI_CAP_ID_EXP,&pcie)) return -1;
    if(extended_cap(config,PCI_EXT_CAP_ID_ARI,&ari) || extended_cap(config,PCI_EXT_CAP_ID_ACS,&acs)) return -1;
    if(require(ari<=4096-8 && acs<=4096-8 && ari!=acs,"ari_acs_capability_bounds")) return -1;
    table=le32(config+msix+PCI_MSIX_TABLE); pba=le32(config+msix+PCI_MSIX_PBA); bar=le32(config+PCI_BASE_ADDRESS_0);
    if(require(msix<=0xf4 && pcie<=0xc4 && (le16(config+msix+PCI_MSIX_FLAGS)&PCI_MSIX_FLAGS_QSIZE)==1 &&
               (table&7U)==2 && (table&~7U)==0 && (pba&7U)==2 && (pba&~7U)==0x1000 &&
               (is_vf || (bar&15U)==PCI_BASE_ADDRESS_MEM_TYPE_64),"bar_msix_contract")) return -1;
    if(is_vf && require(le32(config+pcie+PCI_EXP_DEVCAP)&PCI_EXP_DEVCAP_FLR,"vf_flr_capability")) return -1;
    if(!is_vf) {
        if(extended_cap(config,PCI_EXT_CAP_ID_SRIOV,&sriov)) return -1;
        if(require(sriov<=4096-PCI_EXT_CAP_SRIOV_SIZEOF && le16(config+sriov+PCI_SRIOV_TOTAL_VF)==8 &&
                   le16(config+sriov+PCI_SRIOV_INITIAL_VF)==8 && le16(config+sriov+PCI_SRIOV_VF_DID)==TOY_PCI_VF &&
                   le32(config+sriov+PCI_SRIOV_SUP_PGSIZE)!=0 && le32(config+sriov+PCI_SRIOV_SYS_PGSIZE)!=0,"sriov_contract")) return -1;
        *first=le16(config+sriov+PCI_SRIOV_VF_OFFSET); *stride=le16(config+sriov+PCI_SRIOV_VF_STRIDE);
        if(require(*first && *stride,"vf_routing_offset_stride")) return -1;
        printf("{\"event\":\"sriov_capability\",\"pf\":\"%s\",\"offset\":%u,\"total_vfs\":8,\"initial_vfs\":8,\"first_vf_offset\":%u,\"stride\":%u,\"supported_page_sizes\":%u,\"system_page_size\":%u}\n",bdf,sriov,*first,*stride,le32(config+sriov+PCI_SRIOV_SUP_PGSIZE),le32(config+sriov+PCI_SRIOV_SYS_PGSIZE));
    }
    printf("{\"event\":\"function_caps\",\"bdf\":\"%s\",\"kind\":\"%s\",\"msix_offset\":%u,\"pcie_offset\":%u,\"ari_offset\":%u,\"acs_offset\":%u",bdf,is_vf?"VF":"PF",msix,pcie,ari,acs);
    if(!is_vf) printf(",\"sriov_offset\":%u",sriov);
    printf(",\"vectors\":2,\"flr\":%s,\"raw_vendor\":%u,\"raw_device\":%u,\"enumerated_vendor\":%" PRIu64 ",\"enumerated_device\":%" PRIu64 "}\n",(le32(config+pcie+PCI_EXP_DEVCAP)&PCI_EXP_DEVCAP_FLR)?"true":"false",le16(config),le16(config+2),vendor,device);
    return resources(bdf,is_vf?TOY_LIMITS_VF_BAR0_BYTES:TOY_LIMITS_PF_BAR0_BYTES);
}

static int get_info(int pf, struct toy_info *info)
{
    memset(info,0,sizeof(*info)); info->hdr=(struct toy_uapi_header){sizeof(*info),TOY_UAPI_MAJOR,TOY_UAPI_MINOR};
    return ioctl(pf,TOY_GET_INFO,info)?error("pf_get_info"):0;
}
static int get_vf(int pf, unsigned index, struct toy_vf_info *vf)
{
    memset(vf,0,sizeof(*vf)); vf->hdr=(struct toy_uapi_header){sizeof(*vf),TOY_UAPI_MAJOR,TOY_UAPI_MINOR}; vf->vf=index;
    return ioctl(pf,TOY_GET_VF,vf)?error("pf_get_vf"):0;
}
static int bdf_rid(const char *bdf, unsigned *rid)
{
    unsigned segment,bus,device,function;
    char trailing;
    if(sscanf(bdf,"%x:%x:%x.%x%c",&segment,&bus,&device,&function,&trailing)!=4 || segment || bus>255 || device>31 || function>7) {
        errno=EPROTO; return error("bdf_parse");
    }
    *rid=bus*256U+device*8U+function;
    return 0;
}

static int vf_links(const char *pf_bdf, char bdfs[VF_COUNT][32], unsigned groups[VF_COUNT], unsigned first, unsigned stride, int verbose)
{
    char path[PATH_MAX],value[64];
    unsigned i,j,pf_rid;
    if(bdf_rid(pf_bdf,&pf_rid)) return -1;
    for(i=0;i<VF_COUNT;++i) {
        unsigned rid;
        char *end;
        unsigned long group;
        snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/virtfn%u",pf_bdf,i);
        if(lab_link_basename(path,bdfs[i],32)) return error("virtfn_link");
        snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/physfn",bdfs[i]);
        if(lab_link_basename(path,value,sizeof(value)) || require(!strcmp(value,pf_bdf),"physfn_link")) return -1;
        if(bdf_rid(bdfs[i],&rid) || require(rid==pf_rid+first+i*stride && rid<=65535,"unique_vf_requester")) return -1;
        snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/iommu_group",bdfs[i]);
        if(lab_link_basename(path,value,sizeof(value))) return error("iommu_group_missing");
        errno=0; group=strtoul(value,&end,10);
        if(errno || *end || group>UINT_MAX) { errno=EPROTO; return error("group_id_parse"); }
        groups[i]=(unsigned)group;
        for(j=0;j<i;++j) if(require(groups[i]!=groups[j] && strcmp(bdfs[i],bdfs[j]),"distinct_vf_groups")) return -1;
        if(verbose) printf("{\"event\":\"vf_link\",\"index\":%u,\"vf\":\"%s\",\"physfn\":\"%s\",\"rid\":%u,\"group\":%u}\n",i,bdfs[i],pf_bdf,rid,groups[i]);
    }
    return 0;
}

static int group_members(const char *pf_bdf, char bdfs[VF_COUNT][32], const unsigned groups[VF_COUNT])
{
    unsigned i,j;
    char path[PATH_MAX],member_path[PATH_MAX],driver[128];
    char members[VF_COUNT][32][32];
    unsigned counts[VF_COUNT]={0};
    for(i=0;i<VF_COUNT;++i) {
        DIR *dir;
        struct dirent *entry;
        unsigned found=0,count=0;
        snprintf(path,sizeof(path),"/sys/kernel/iommu_groups/%u/devices",groups[i]);
        dir=opendir(path);
        if(!dir) return error("group_members_open");
        while((entry=readdir(dir))) {
            uint64_t class_code;
            int own;
            if(entry->d_name[0]=='.') continue;
            if(count==32 || strlen(entry->d_name)>=32) { closedir(dir); errno=EOVERFLOW; return error("group_member_bound"); }
            strcpy(members[i][count],entry->d_name);
            ++count; own=!strcmp(entry->d_name,bdfs[i]); found+=(unsigned)own;
            snprintf(member_path,sizeof(member_path),"/sys/bus/pci/devices/%s/driver",entry->d_name);
            if(lab_link_basename(member_path,driver,sizeof(driver))) {
                if(errno!=ENOENT) { closedir(dir); return error("group_driver_link"); }
                strcpy(driver,"none");
            }
            snprintf(member_path,sizeof(member_path),"/sys/bus/pci/devices/%s/class",entry->d_name);
            if(lab_read_u64(member_path,&class_code)) { closedir(dir); return error("group_member_class"); }
            printf("{\"event\":\"group_member\",\"group\":%u,\"bdf\":\"%s\",\"driver\":\"%s\",\"class\":%" PRIu64 "}\n",groups[i],entry->d_name,driver,class_code);
            if(!strcmp(entry->d_name,pf_bdf)) { closedir(dir); return require(0,"pf_in_vf_group"); }
            for(j=0;j<VF_COUNT;++j) if(j!=i && !strcmp(entry->d_name,bdfs[j])) { closedir(dir); return require(0,"other_vf_in_group"); }
            if(!own) {
                if(require((class_code>>8)==0x0604 && !strcmp(driver,"none"),"group_extra_member_viability")) { closedir(dir); return -1; }
            }
        }
        if(closedir(dir)) return error("close_group_members");
        if(require(found==1 && count>=1,"complete_group_members")) return -1;
        counts[i]=count;
    }
    printf("{\"id\":\"M1_GROUPS\",\"status\":\"PASS\",\"pf\":\"%s\",\"groups\":[",pf_bdf);
    for(i=0;i<VF_COUNT;++i) {
        printf("%s{\"vf\":\"%s\",\"group_id\":%u,\"members\":[",i?",":"",bdfs[i],groups[i]);
        for(j=0;j<counts[i];++j) printf("%s\"%s\"",j?",":"",members[i][j]);
        printf("]}");
    }
    puts("],\"pf_excluded\":true,\"complete_members_logged\":true}");
    return 0;
}

static int fd_count(void)
{
    DIR *dir=opendir("/proc/self/fd");
    struct dirent *entry;
    int count=0;
    if(!dir) return -1;
    while((entry=readdir(dir))) if(entry->d_name[0]!='.') ++count;
    if(closedir(dir)) return -1;
    return count-1;
}

static int type1_probe(int pf, char bdfs[VF_COUNT][32], const unsigned groups[VF_COUNT])
{
    struct lab_vfio vf[VF_COUNT];
    struct toy_vf_info before[VF_COUNT],after;
    unsigned i,j,opened=0;
    int failed=0,baseline=fd_count();
    for(i=0;i<VF_COUNT;++i) lab_vfio_init(&vf[i]);
    for(i=0;i<VF_COUNT;++i) {
        ++opened;
        if(lab_vfio_open(&vf[i],bdfs[i],groups[i]) || lab_vfio_map_probe(&vf[i])) { error("vfio_open_type1_map"); failed=1; break; }
        for(j=0;j<i;++j) if(require(vf[i].container!=vf[j].container,"separate_containers")) { failed=1; break; }
        if(failed) break;
    }
    if(!failed) {
        for(i=0;i<VF_COUNT;++i) if(get_vf(pf,i,&before[i])) { failed=1; break; }
    }
    if(!failed) {
        if(ioctl(vf[0].device,VFIO_DEVICE_RESET)) { error("vfio_device_reset"); failed=1; }
        for(i=0;i<VF_COUNT && !failed;++i) {
            if(get_vf(pf,i,&after)) { failed=1; break; }
            if(require((i==0?after.epoch>before[i].epoch:after.epoch==before[i].epoch) &&
                       (after.hw_flags&TOY_HW_FLAGS_FENCED) && !(after.hw_flags&TOY_HW_FLAGS_CONFIG_VALID),"flr_epoch_fence_isolation")) failed=1;
            printf("{\"event\":\"vfio_reset_observed\",\"vf\":\"%s\",\"target\":%s,\"before_epoch\":%llu,\"after_epoch\":%llu,\"hw_flags\":%u}\n",bdfs[i],i==0?"true":"false",(unsigned long long)before[i].epoch,(unsigned long long)after.epoch,after.hw_flags);
        }
    }
    while(opened) if(lab_vfio_close(&vf[--opened])) { error("vfio_cleanup"); failed=1; }
    if(require(fd_count()==baseline,"vfio_no_fd_leak")) failed=1;
    if(failed) return -1;
    puts("{\"id\":\"M1_TYPE1\",\"status\":\"PASS\",\"viable_groups\":4,\"simultaneous_containers\":4,\"maps\":4,\"exact_unmaps\":4,\"remaining_fds\":0,\"remaining_maps\":0,\"vfio_reset_epoch_checked\":true}");
    return 0;
}

static int enabled_snapshot(int pf, unsigned cycle)
{
    struct toy_info info;
    struct toy_vf_info vf;
    unsigned i,total=0;
    if(get_info(pf,&info) || require(info.enabled_vfs==VF_COUNT,"enabled_vf_count")) return -1;
    for(i=0;i<8;++i) {
        if(get_vf(pf,i,&vf)) return -1;
        if(i<VF_COUNT) {
            if(require(vf.reserve==8 && (vf.hw_flags&TOY_HW_FLAGS_PCI_PRESENT) && (vf.hw_flags&TOY_HW_FLAGS_FENCED) &&
                       !(vf.hw_flags&TOY_HW_FLAGS_KILLED) && (vf.state==TOY_STATES_ACTIVE || vf.state==TOY_STATES_RECOVERING),"enabled_ledger_state")) return -1;
        } else if(require(vf.reserve==0 && vf.state==TOY_STATES_FREE,"unused_slot_free")) return -1;
        total+=vf.reserve;
    }
    if(require(total==32,"enabled_credit_sum")) return -1;
    printf("{\"event\":\"%s\",\"cycle\":%u,\"enabled\":4,\"ledger_credit\":%u}\n",cycle?"enable_snapshot":"warmup_enable_snapshot",cycle,total);
    return 0;
}

static int disabled_snapshot(int pf, const char *bdf, unsigned cycle)
{
    struct toy_info info;
    struct toy_vf_info vf;
    char path[PATH_MAX];
    struct stat st;
    unsigned i,total=0;
    if(get_info(pf,&info) || require(!info.enabled_vfs,"disabled_vf_count")) return -1;
    for(i=0;i<8;++i) {
        if(get_vf(pf,i,&vf)) return -1;
        if(require(vf.state==TOY_STATES_FREE && !vf.reserve && !vf.outstanding &&
                   (vf.hw_flags&TOY_HW_FLAGS_FENCED) && !(vf.hw_flags&(TOY_HW_FLAGS_CONFIG_VALID|TOY_HW_FLAGS_PCI_PRESENT|TOY_HW_FLAGS_KILLED)),"disabled_ledger_free")) return -1;
        total+=vf.reserve;
        snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/virtfn%u",bdf,i);
        if(!lstat(path,&st) || errno!=ENOENT) return require(0,"vf_link_removed");
    }
    printf("{\"event\":\"%s\",\"cycle\":%u,\"enabled\":0,\"ledger_credit\":%u,\"outstanding\":0,\"vf_links\":0}\n",cycle?"disable_snapshot":"warmup_disable_snapshot",cycle,total);
    return 0;
}

int main(void)
{
    char pf_bdf[32]={0},bdfs[VF_COUNT][32],path[PATH_MAX],driver[64];
    unsigned groups[VF_COUNT],first=0,stride=0,cycle,i;
    int pf=-1,enabled=0,baseline=-1,failed=0;
    struct toy_info info;
    setvbuf(stdout,NULL,_IOLBF,0);
    if(find_pf(pf_bdf)) goto fail;
    snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/driver",pf_bdf);
    if(lab_link_basename(path,driver,sizeof(driver)) || require(!strcmp(driver,"toy_pf"),"pf_owned_by_toy_driver")) goto fail;
    if(lab_ensure_char("/sys/class/misc/toy_pf0/dev","/dev/toy_pf0")) { error("pf_device_node"); goto fail; }
    pf=open("/dev/toy_pf0",O_RDWR|O_CLOEXEC);
    if(pf<0) { error("open_pf_control"); goto fail; }
    if(get_info(pf,&info) || require(info.caps_vfs==8 && info.caps_queues==8 && info.caps_vectors==16 &&
       info.caps_credits==64 && (info.features&7U)==7U && info.session && !info.enabled_vfs,"pf_capabilities") ||
       pci_caps(pf_bdf,0,&first,&stride)) goto fail;
    if(host_checkpoint("M1_COLD_CHECKPOINT")) goto fail;
    baseline=fd_count();
    if(require(baseline>=0,"fd_baseline")) goto fail;
    snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/sriov_numvfs",pf_bdf);
    /* 首次使用會建立 vIOMMU 每個 requester 的快取；另做完整 100 次穩態循環。 */
    for(cycle=0;cycle<=CYCLES;++cycle) {
        if(lab_write_text(path,"4\n")) { error("enable_four_vfs"); goto fail; }
        enabled=1;
        if(enabled_snapshot(pf,cycle) || vf_links(pf_bdf,bdfs,groups,first,stride,cycle==0)) goto fail;
        if(cycle==0) {
            for(i=0;i<VF_COUNT;++i) if(pci_caps(bdfs[i],1,&first,&stride)) goto fail;
            puts("{\"id\":\"M1_CAPS\",\"status\":\"PASS\",\"pf_vectors\":2,\"vf_vectors\":2,\"vfs\":4,\"unique_requesters\":true,\"flr_declared\":true}");
            if(group_members(pf_bdf,bdfs,groups)) goto fail;
            if(type1_probe(pf,bdfs,groups)) goto fail;
        }
        if(lab_write_text(path,"0\n")) { error("disable_four_vfs"); goto fail; }
        enabled=0;
        if(disabled_snapshot(pf,pf_bdf,cycle) || require(fd_count()==baseline,"cycle_fd_no_growth")) goto fail;
        if(cycle==0 && host_checkpoint("M1_INITIAL_CHECKPOINT")) goto fail;
    }
    if(host_checkpoint("M1_FINAL_CHECKPOINT")) goto fail;
    puts("{\"id\":\"T01\",\"status\":\"PASS\",\"cycles\":100,\"vfs_per_cycle\":4,\"final_ledger\":0,\"remaining_vf_functions\":0,\"remaining_vfio_fds\":0,\"remaining_vfio_maps\":0,\"fd_growth\":0}");
    if(close(pf)) { error("close_pf"); pf=-1; goto fail; }
    puts("LAB_RESULT=PASS");
    return 0;
fail:
    failed=errno;
    if(enabled && lab_write_text(path,"0\n")) error("failure_disable_cleanup");
    if(pf>=0 && close(pf)) error("failure_pf_close");
    printf("{\"id\":\"M1_FAILURE\",\"status\":\"FAIL\",\"errno\":%d}\n",failed);
    puts("LAB_RESULT=FAIL");
    return 1;
}
