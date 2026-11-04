#ifndef LAB_VFIO_H
#define LAB_VFIO_H
#include <linux/vfio.h>
#include <stdint.h>
#include <stddef.h>

struct lab_vfio {
    int container, group, device, attached, bound, mapped;
    unsigned group_id;
    char bdf[32];
    void *buffer;
    size_t bytes;
    uint64_t iova, page_sizes;
    struct vfio_region_info bar0, bar2, config;
    struct vfio_device_info info;
};
void lab_vfio_init(struct lab_vfio *vf);
int lab_write_text(const char *path, const char *text);
int lab_read_text(const char *path, char *buffer, size_t length);
int lab_read_u64(const char *path, uint64_t *value);
int lab_link_basename(const char *path, char *buffer, size_t length);
int lab_ensure_char(const char *sysfs_dev, const char *path);
int lab_vfio_open(struct lab_vfio *vf, const char *bdf, unsigned group_id);
int lab_vfio_map_probe(struct lab_vfio *vf);
int lab_vfio_close(struct lab_vfio *vf);
#endif
