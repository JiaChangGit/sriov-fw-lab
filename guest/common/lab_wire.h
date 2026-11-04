#ifndef LAB_WIRE_H
#define LAB_WIRE_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#define LAB_FRAME_MAX 4096u
#define LAB_HEADER_SIZE 64u
#define LAB_TIMEOUT_MS 120000
enum lab_message_type { LAB_HELLO=1, LAB_HELLO_REPLY, LAB_READY, LAB_STOP_REQUEST,
    LAB_STOPPED, LAB_RESET_NOTICE, LAB_RUN, LAB_EXIT_ACK, LAB_ERROR, LAB_STOP,
    LAB_INSPECT, LAB_INSPECT_REPLY };
struct lab_message {
    uint16_t type;
    uint64_t request_id, session, epoch, lease_token, operation;
    uint32_t vf, ready_gen, payload_len;
    unsigned char payload[LAB_FRAME_MAX-LAB_HEADER_SIZE];
};
uint16_t lab_get16(const void *p);
uint32_t lab_get32(const void *p);
uint64_t lab_get64(const void *p);
void lab_put16(void *p, uint16_t v);
void lab_put32(void *p, uint32_t v);
void lab_put64(void *p, uint64_t v);
int64_t lab_now_ms(void);
int lab_wait(int fd, short events, int64_t deadline);
int lab_encode(unsigned char *body, size_t cap, const struct lab_message *m);
int lab_decode(struct lab_message *m, const unsigned char *body, size_t n);
int lab_send(int fd, const struct lab_message *m, int64_t deadline);
int lab_recv(int fd, struct lab_message *m, int64_t deadline);
int lab_peer(int fd, uid_t uid, pid_t pid);
int lab_tuple(const struct lab_message *m, uint32_t vf, uint64_t session,
              uint64_t epoch, uint64_t lease, uint64_t operation);
int lab_listen(const char *path);
int lab_connect(const char *path, int64_t deadline);
uint32_t lab_crc32(const void *p, size_t n);
#endif
