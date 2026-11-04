#define _GNU_SOURCE
#include "lab_wire.h"
#include "toy_uapi.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct fragmented { int fd; unsigned char bytes[80]; size_t size; };
static void *fragment_sender(void *opaque)
{
    struct fragmented *f=opaque;
    for (size_t i=0;i<f->size;i++) {
        assert(send(f->fd,f->bytes+i,1,MSG_NOSIGNAL)==1);
        usleep(100);
    }
    return NULL;
}
static int fd_count(void)
{
    DIR *d=opendir("/proc/self/fd"); assert(d); int count=0;
    while (readdir(d)) ++count;
    assert(!closedir(d)); return count;
}
static void pair(int fds[2]) { assert(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,fds)); }
int main(void)
{
    int baseline=fd_count(),fds[2];
    unsigned char body[4096];
    struct lab_message request={.type=LAB_HELLO,.request_id=7,.session=11,.epoch=13,
        .lease_token=17,.operation=19,.vf=2},decoded;
    assert(lab_crc32("123456789",9)==0xcbf43926u);
    assert(sizeof(struct toy_info)==56&&sizeof(struct toy_vf_info)==72&&sizeof(struct toy_vf_ctl)==80&&sizeof(struct toy_event)==64);
    assert(_IOC_SIZE(TOY_REARM_VF)==80&&_IOC_NR(TOY_REARM_VF)==7);
    int n=lab_encode(body,sizeof(body),&request); assert(n==64);
    assert(!lab_decode(&decoded,body,(size_t)n));
    assert(!lab_tuple(&decoded,2,11,13,17,19));
    assert(lab_tuple(&decoded,2,11,14,17,19)<0&&errno==ESTALE);
    pair(fds);
    struct fragmented f={.fd=fds[0],.size=68};
    lab_put32(f.bytes,64); memcpy(f.bytes+4,body,64);
    pthread_t thread; assert(!pthread_create(&thread,NULL,fragment_sender,&f));
    assert(!lab_recv(fds[1],&decoded,lab_now_ms()+2000));
    assert(!pthread_join(thread,NULL));
    assert(!lab_peer(fds[1],getuid(),getpid()));
    assert(lab_peer(fds[1],getuid(),getpid()+1)<0&&errno==EACCES);
    assert(lab_peer(fds[1],getuid()+1,getpid())<0&&errno==EACCES);
    close(fds[0]); close(fds[1]);
    for (unsigned i=0;i<3;i++) {
        static const uint32_t sizes[]={63,4097,UINT32_MAX}; unsigned char length[4];
        pair(fds); lab_put32(length,sizes[i]); assert(write(fds[0],length,4)==4);
        assert(lab_recv(fds[1],&decoded,lab_now_ms()+100)<0&&errno==EMSGSIZE);
        close(fds[0]); close(fds[1]);
    }
    for (unsigned cut=0;cut<68;cut++) {
        pair(fds); if (cut) assert(write(fds[0],f.bytes,cut)==cut);
        assert(!shutdown(fds[0],SHUT_WR));
        assert(lab_recv(fds[1],&decoded,lab_now_ms()+100)<0&&errno==ECONNRESET);
        close(fds[0]); close(fds[1]);
    }
    pair(fds);
    assert(lab_recv(fds[1],&decoded,lab_now_ms()+5)<0&&errno==ETIMEDOUT);
    close(fds[0]); close(fds[1]);
    /* 接收方拒絕並關閉真正 SCM_RIGHTS 安裝的 fd，不能只忽略 ancillary。 */
    pair(fds); int exported=open("/dev/null",O_RDONLY|O_CLOEXEC); assert(exported>=0);
    unsigned char control[CMSG_SPACE(sizeof(int))]; memset(control,0,sizeof(control));
    struct iovec iov={f.bytes,68};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
    c->cmsg_level=SOL_SOCKET; c->cmsg_type=SCM_RIGHTS; c->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c),&exported,sizeof(exported));
    int before=fd_count(); assert(sendmsg(fds[0],&msg,MSG_NOSIGNAL)==68);
    assert(lab_recv(fds[1],&decoded,lab_now_ms()+100)<0&&errno==EPROTO);
    assert(fd_count()==before); close(exported); close(fds[0]); close(fds[1]);
    static const unsigned corrupt[]={0,4,6,56,60};
    for (unsigned i=0;i<sizeof(corrupt)/sizeof(corrupt[0]);i++) {
        assert(lab_encode(body,sizeof(body),&request)==64); body[corrupt[i]]^=0x80;
        assert(lab_decode(&decoded,body,64)<0&&errno==EPROTO);
    }
    assert(lab_encode(body,sizeof(body),&request)==64);
    assert(lab_decode(&decoded,body,65)<0&&errno==EPROTO);
    request.type=LAB_STOP; request.payload_len=16; lab_put32(request.payload+12,1);
    assert(lab_encode(body,sizeof(body),&request)<0&&errno==EPROTO);
    request.payload_len=UINT32_MAX;
    assert(lab_encode(body,sizeof(body),&request)<0&&errno==EPROTO);
    request.type=LAB_INSPECT; request.payload_len=0;
    assert(lab_encode(body,sizeof(body),&request)==64);
    assert(!lab_decode(&decoded,body,64)&&decoded.type==LAB_INSPECT);
    request.type=LAB_INSPECT_REPLY; request.payload_len=16;
    for (unsigned i=0;i<3;i++) lab_put32(request.payload+i*4,0x11223344u+i);
    lab_put32(request.payload+12,0);
    assert(lab_encode(body,sizeof(body),&request)==80);
    assert(!lab_decode(&decoded,body,80)&&lab_get32(decoded.payload+8)==0x11223346u);
    body[79]=1;
    assert(lab_decode(&decoded,body,80)<0&&errno==EPROTO);
    request.payload_len=12;
    assert(lab_encode(body,sizeof(body),&request)<0&&errno==EPROTO);
    assert(fd_count()==baseline);
    puts("WIRE_TEST_PASS partial_bytes=68 eof_cuts=68 oversized=3 credential_mismatch=2 ancillary_fd_leak=0 abi_sizes=56,72,80,64");
    return 0;
}
