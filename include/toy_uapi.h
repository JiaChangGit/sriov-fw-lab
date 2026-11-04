/* 使用固定寬度與明確對齊；此檔同時供 kernel 與 userspace 使用。 */
#ifndef TOY_UAPI_H
#define TOY_UAPI_H
#include <linux/ioctl.h>
#include <linux/types.h>
#define TOY_UAPI_MAJOR 1
#define TOY_UAPI_MINOR 0
#define TOY_IOCTL_MAGIC 'T'
struct toy_uapi_header {
    __u32 size;
    __u16 abi_major;
    __u16 abi_minor;
};
struct toy_info {
    struct toy_uapi_header hdr;
    __aligned_u64 session;
    __u32 caps_vfs;
    __u32 caps_queues;
    __u32 caps_vectors;
    __u32 caps_credits;
    __u32 features;
    __u32 enabled_vfs;
    __u32 reserved[4];
};
struct toy_vf_info {
    struct toy_uapi_header hdr;
    __u32 vf;
    __u32 state;
    __aligned_u64 session;
    __aligned_u64 epoch;
    __u32 hw_flags;
    __u32 reserve;
    __u32 outstanding;
    __u32 ready_gen;
    __aligned_u64 lease_token;
    __u32 reserved[4];
};
#define TOY_GET_INFO _IOWR(TOY_IOCTL_MAGIC, 1, struct toy_info)
#define TOY_GET_VF _IOWR(TOY_IOCTL_MAGIC, 2, struct toy_vf_info)
#define TOY_CTL_OWNER_CLOSED 1u
#define TOY_CTL_GLOBAL ((__u32)~0u)
/* 操作編號屬每個開啟的控制描述元；重送必須保留原始輸入。 */
struct toy_vf_ctl {
    struct toy_uapi_header hdr;
    __u32 vf;
    __u32 flags;
    __aligned_u64 session;
    __aligned_u64 epoch;
    __aligned_u64 lease_token;
    __aligned_u64 operation;
    __u32 ready_gen;
    __u32 state;
    __u32 hw_flags;
    __u32 outstanding;
    __u32 fw_status;
    __u32 reserved[3];
};
#define TOY_LEASE_ACQUIRE _IOWR(TOY_IOCTL_MAGIC, 3, struct toy_vf_ctl)
#define TOY_LEASE_RELEASE _IOWR(TOY_IOCTL_MAGIC, 4, struct toy_vf_ctl)
#define TOY_FENCE_VF _IOWR(TOY_IOCTL_MAGIC, 5, struct toy_vf_ctl)
#define TOY_RESET_VF _IOWR(TOY_IOCTL_MAGIC, 6, struct toy_vf_ctl)
#define TOY_REARM_VF _IOWR(TOY_IOCTL_MAGIC, 7, struct toy_vf_ctl)
#define TOY_GLOBAL_RECOVER _IOWR(TOY_IOCTL_MAGIC, 8, struct toy_vf_ctl)
#define TOY_EVENT_RESET 1u
#define TOY_EVENT_FLR 2u
#define TOY_EVENT_FAULT 3u
#define TOY_EVENT_REMOVE 4u
#define TOY_EVENT_LOST_EVENTS 5u
#define TOY_EVENT_CAPACITY 64u
struct toy_event {
    struct toy_uapi_header hdr;
    __u32 type;
    __u32 vf;
    __aligned_u64 event_seq;
    __aligned_u64 session;
    __aligned_u64 epoch;
    __aligned_u64 lease_token;
    __u32 ready_gen;
    __u32 state;
    __u32 hw_flags;
    __u32 detail;
};
/* ABI 在 32/64 位元編譯都必須保有相同寬度與欄位位置。 */
_Static_assert(sizeof(struct toy_info) == 56, "toy_info size");
_Static_assert(sizeof(struct toy_vf_info) == 72, "toy_vf_info size");
_Static_assert(sizeof(struct toy_vf_ctl) == 80, "toy_vf_ctl size");
_Static_assert(__builtin_offsetof(struct toy_vf_ctl, operation) == 40, "operation offset");
_Static_assert(__builtin_offsetof(struct toy_vf_ctl, fw_status) == 64, "fw_status offset");
_Static_assert(sizeof(struct toy_event) == 64, "toy_event size");
_Static_assert(__builtin_offsetof(struct toy_event, event_seq) == 16, "event_seq offset");
_Static_assert(__builtin_offsetof(struct toy_event, detail) == 60, "detail offset");
#endif
