/* Classic VFIO TYPE1 的資源操作。每個 lab_vfio 實例各自建立 container/group/device fd。 */
#define _GNU_SOURCE
#include "vfio.h"
#include "toy_regs.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

int lab_write_text(const char *path, const char *text)
{
    size_t length=strlen(text);
    int fd=open(path,O_WRONLY|O_CLOEXEC),saved;
    ssize_t n;
    if(fd<0) return -1;
    do { n=write(fd,text,length); } while(n<0 && errno==EINTR);
    saved=errno;
    if(close(fd) && n==(ssize_t)length) return -1;
    if(n!=(ssize_t)length) { errno=n<0?saved:EIO; return -1; }
    return 0;
}

int lab_read_text(const char *path, char *buffer, size_t length)
{
    int fd,saved;
    ssize_t n;
    if(length<2) { errno=EINVAL; return -1; }
    fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd<0) return -1;
    do { n=read(fd,buffer,length-1); } while(n<0 && errno==EINTR);
    saved=errno;
    if(close(fd) && n>=0) return -1;
    if(n<0) { errno=saved; return -1; }
    if(n==(ssize_t)length-1) { errno=EOVERFLOW; return -1; }
    buffer[n]=0;
    return 0;
}

int lab_read_u64(const char *path, uint64_t *value)
{
    char buffer[128],*end;
    unsigned long long n;
    if(lab_read_text(path,buffer,sizeof(buffer))) return -1;
    errno=0; n=strtoull(buffer,&end,0);
    if(errno || end==buffer || (*end && *end!='\n')) { errno=EINVAL; return -1; }
    *value=n;
    return 0;
}

int lab_link_basename(const char *path, char *buffer, size_t length)
{
    char target[PATH_MAX],*base;
    ssize_t n=readlink(path,target,sizeof(target)-1);
    if(n<0) return -1;
    if(n==(ssize_t)sizeof(target)-1) { errno=EOVERFLOW; return -1; }
    target[n]=0; base=strrchr(target,'/'); base=base?base+1:target;
    if(strlen(base)>=length) { errno=ENAMETOOLONG; return -1; }
    strcpy(buffer,base);
    return 0;
}

int lab_ensure_char(const char *sysfs_dev, const char *path)
{
    char text[64];
    unsigned major_num,minor_num;
    struct stat st;
    if(!stat(path,&st)) {
        if(S_ISCHR(st.st_mode)) return 0;
        errno=EINVAL; return -1;
    }
    if(errno!=ENOENT || lab_read_text(sysfs_dev,text,sizeof(text))) return -1;
    if(sscanf(text,"%u:%u",&major_num,&minor_num)!=2) { errno=EINVAL; return -1; }
    return mknod(path,S_IFCHR|0600,makedev(major_num,minor_num));
}

void lab_vfio_init(struct lab_vfio *vf)
{
    memset(vf,0,sizeof(*vf)); vf->container=-1; vf->group=-1; vf->device=-1;
}

static int region(int device, unsigned index, struct vfio_region_info *info)
{
    memset(info,0,sizeof(*info)); info->argsz=sizeof(*info); info->index=index;
    return ioctl(device,VFIO_DEVICE_GET_REGION_INFO,info);
}

static int iommu_info(struct lab_vfio *vf)
{
    unsigned char buffer[16384] __attribute__((aligned(8)));
    struct vfio_iommu_type1_info *info=(void *)buffer;
    uint32_t offset,seen[64],count=0;
    uint64_t page=(uint64_t)vf->bytes;
    int found=0;
    memset(buffer,0,sizeof(buffer)); info->argsz=sizeof(buffer);
    if(ioctl(vf->container,VFIO_IOMMU_GET_INFO,info)) return -1;
    if(info->argsz<sizeof(*info) || info->argsz>sizeof(buffer) || !(info->flags&VFIO_IOMMU_INFO_PGSIZES) || !(info->iova_pgsizes&page)) {
        errno=EPROTO; return -1;
    }
    vf->page_sizes=info->iova_pgsizes;
    offset=info->cap_offset;
    while(offset) {
        const struct vfio_info_cap_header *header;
        unsigned j;
        if(count==64 || offset>info->argsz-sizeof(*header) || (offset&3U)) { errno=EPROTO; return -1; }
        for(j=0;j<count;++j) if(seen[j]==offset) { errno=EPROTO; return -1; }
        seen[count++]=offset;
        header=(const void *)(buffer+offset);
        if(header->id==VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE) {
            const struct vfio_iommu_type1_info_cap_iova_range *range=(const void *)header;
            size_t available=info->argsz-offset;
            if(available<sizeof(*range) || header->version!=1 ||
               range->nr_iovas>(available-sizeof(*range))/sizeof(range->iova_ranges[0])) { errno=EPROTO; return -1; }
            for(j=0;j<range->nr_iovas;++j) {
                uint64_t start=range->iova_ranges[j].start,end=range->iova_ranges[j].end,choice;
                printf("{\"event\":\"iova_range\",\"vf\":\"%s\",\"start\":\"0x%" PRIx64 "\",\"end\":\"0x%" PRIx64 "\"}\n",vf->bdf,start,end);
                if(start>UINT64_MAX-(page-1)) continue;
                choice=(start+page-1)&~(page-1);
                if(choice<0x100000 && end>=0x100000+page-1) choice=0x100000;
                if(!found && choice<=end && page-1<=end-choice) { vf->iova=choice; found=1; }
            }
        }
        offset=header->next;
    }
    if(!found) { errno=ENOTSUP; return -1; }
    return 0;
}

/* 逐步填入已取得的資源；中途失敗仍由呼叫者呼叫 lab_vfio_close，不能只 free 結構。 */
int lab_vfio_open(struct lab_vfio *vf, const char *bdf, unsigned group_id)
{
    char path[PATH_MAX],node[128],sysdev[128];
    struct vfio_group_status group={.argsz=sizeof(group)};
    struct vfio_irq_info irq={.argsz=sizeof(irq),.index=VFIO_PCI_MSIX_IRQ_INDEX};
    long page=sysconf(_SC_PAGESIZE);
    uint32_t magic=0,identity=0;
    if(strlen(bdf)>=sizeof(vf->bdf) || page<=0 || ((unsigned long)page&((unsigned long)page-1))) { errno=EINVAL; return -1; }
    strcpy(vf->bdf,bdf); vf->group_id=group_id; vf->bytes=(size_t)page;
    snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/driver_override",bdf);
    if(lab_write_text(path,"vfio-pci\n")) return -1;
    if(lab_write_text("/sys/bus/pci/drivers_probe",bdf)) return -1;
    vf->bound=1;
    snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/driver",bdf);
    if(lab_link_basename(path,node,sizeof(node)) || strcmp(node,"vfio-pci")) { errno=ENODEV; return -1; }
    if(mkdir("/dev/vfio",0755) && errno!=EEXIST) return -1;
    if(lab_ensure_char("/sys/class/misc/vfio/dev","/dev/vfio/vfio")) return -1;
    snprintf(node,sizeof(node),"/dev/vfio/%u",group_id);
    snprintf(sysdev,sizeof(sysdev),"/sys/class/vfio/%u/dev",group_id);
    if(lab_ensure_char(sysdev,node)) return -1;
    vf->container=open("/dev/vfio/vfio",O_RDWR|O_CLOEXEC);
    if(vf->container<0) return -1;
    if(ioctl(vf->container,VFIO_GET_API_VERSION)!=VFIO_API_VERSION ||
       ioctl(vf->container,VFIO_CHECK_EXTENSION,VFIO_TYPE1_IOMMU)<=0) { errno=ENOTSUP; return -1; }
    vf->group=open(node,O_RDWR|O_CLOEXEC);
    if(vf->group<0 || ioctl(vf->group,VFIO_GROUP_GET_STATUS,&group)) return -1;
    if(!(group.flags&VFIO_GROUP_FLAGS_VIABLE)) { errno=EBUSY; return -1; }
    if(ioctl(vf->group,VFIO_GROUP_SET_CONTAINER,&vf->container)) return -1;
    vf->attached=1;
    if(ioctl(vf->container,VFIO_SET_IOMMU,VFIO_TYPE1_IOMMU)) return -1;
    vf->device=ioctl(vf->group,VFIO_GROUP_GET_DEVICE_FD,bdf);
    if(vf->device<0) return -1;
    if(fcntl(vf->device,F_SETFD,FD_CLOEXEC)) return -1;
    vf->info.argsz=sizeof(vf->info);
    if(ioctl(vf->device,VFIO_DEVICE_GET_INFO,&vf->info)) return -1;
    if(!(vf->info.flags&VFIO_DEVICE_FLAGS_PCI) || !(vf->info.flags&VFIO_DEVICE_FLAGS_RESET) ||
        vf->info.num_regions<=VFIO_PCI_CONFIG_REGION_INDEX || vf->info.num_irqs<=VFIO_PCI_MSIX_IRQ_INDEX) { errno=EPROTO; return -1; }
    if(region(vf->device,VFIO_PCI_BAR0_REGION_INDEX,&vf->bar0) ||
       region(vf->device,VFIO_PCI_BAR2_REGION_INDEX,&vf->bar2) ||
       region(vf->device,VFIO_PCI_CONFIG_REGION_INDEX,&vf->config) ||
       ioctl(vf->device,VFIO_DEVICE_GET_IRQ_INFO,&irq)) return -1;
    if(vf->bar0.size!=TOY_LIMITS_VF_BAR0_BYTES || vf->bar2.size!=TOY_LIMITS_BAR2_BYTES ||
       irq.count!=TOY_LIMITS_VECTORS_PER_FUNCTION || !(irq.flags&VFIO_IRQ_INFO_EVENTFD)) { errno=EPROTO; return -1; }
    if(pread(vf->device,&identity,sizeof(identity),(off_t)vf->config.offset)!=(ssize_t)sizeof(identity) ||
       identity!=(TOY_PCI_VENDOR|(TOY_PCI_VF<<16))) { errno=EPROTO; return -1; }
    if(pread(vf->device,&magic,sizeof(magic),(off_t)vf->bar0.offset)!=(ssize_t)sizeof(magic) || magic!=TOY_VF_MAGIC_VALUE) { errno=EPROTO; return -1; }
    if(iommu_info(vf)) return -1;
    printf("{\"event\":\"vfio_open\",\"vf\":\"%s\",\"group\":%u,\"container_fd\":%d,\"group_fd\":%d,\"device_fd\":%d,\"vectors\":%u,\"page_sizes\":\"0x%" PRIx64 "\",\"reset_supported\":true}\n",
           bdf,group_id,vf->container,vf->group,vf->device,irq.count,vf->page_sizes);
    return 0;
}

int lab_vfio_map_probe(struct lab_vfio *vf)
{
    struct vfio_iommu_type1_dma_map map={.argsz=sizeof(map),.flags=VFIO_DMA_MAP_FLAG_READ|VFIO_DMA_MAP_FLAG_WRITE};
    vf->buffer=mmap(NULL,vf->bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(vf->buffer==MAP_FAILED) { vf->buffer=NULL; return -1; }
    memset(vf->buffer,(int)(vf->group_id&255U),vf->bytes);
    map.vaddr=(uint64_t)(uintptr_t)vf->buffer; map.iova=vf->iova; map.size=vf->bytes;
    if(ioctl(vf->container,VFIO_IOMMU_MAP_DMA,&map)) return -1;
    vf->mapped=1;
    printf("{\"event\":\"vfio_map\",\"vf\":\"%s\",\"iova\":\"0x%" PRIx64 "\",\"size\":%zu,\"permissions\":\"READ_WRITE\"}\n",vf->bdf,vf->iova,vf->bytes);
    return 0;
}

/* 依已取得的欄位回收部分初始化狀態；任何一步失敗都保留失敗回傳值。 */
int lab_vfio_close(struct lab_vfio *vf)
{
    char path[PATH_MAX];
    int failed=0;
    if(vf->mapped) {
        struct vfio_iommu_type1_dma_unmap unmap={.argsz=sizeof(unmap),.iova=vf->iova,.size=vf->bytes};
        if(ioctl(vf->container,VFIO_IOMMU_UNMAP_DMA,&unmap) || unmap.size!=vf->bytes) {
            fprintf(stderr,"VFIO_UNMAP_FAILED vf=%s actual=%" PRIu64 " expected=%zu errno=%d\n",vf->bdf,(uint64_t)unmap.size,vf->bytes,errno);
            failed=1;
        } else {
            vf->mapped=0;
            printf("{\"event\":\"vfio_unmap\",\"vf\":\"%s\",\"size\":%zu}\n",vf->bdf,vf->bytes);
        }
    }
    /* UNMAP 失敗時先關 owner fds，使 VFIO 撤銷映射後才釋放 userspace RAM。 */
    if(vf->device>=0) { if(close(vf->device)) failed=1; vf->device=-1; }
    if(vf->attached) { if(ioctl(vf->group,VFIO_GROUP_UNSET_CONTAINER)) failed=1; else vf->attached=0; }
    if(vf->group>=0) { if(close(vf->group)) failed=1; vf->group=-1; }
    if(vf->container>=0) { if(close(vf->container)) failed=1; vf->container=-1; }
    vf->mapped=0;
    if(vf->buffer) { if(munmap(vf->buffer,vf->bytes)) failed=1; vf->buffer=NULL; }
    if(vf->bound) {
        if(lab_write_text("/sys/bus/pci/drivers/vfio-pci/unbind",vf->bdf)) failed=1;
        else vf->bound=0;
    }
    if(vf->bdf[0]) {
        snprintf(path,sizeof(path),"/sys/bus/pci/devices/%s/driver_override",vf->bdf);
        if(lab_write_text(path,"\n")) failed=1;
    }
    return failed?-1:0;
}
