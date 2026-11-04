/* labd 與 tenant 的 Unix stream 協定：固定格式、長度上限與絕對期限，不能假設一次 recv 就是一筆訊息。 */
#define _GNU_SOURCE
#include "lab_wire.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

uint16_t lab_get16(const void *p) { const unsigned char *b=p; return (uint16_t)b[0]|((uint16_t)b[1]<<8); }
uint32_t lab_get32(const void *p) { const unsigned char *b=p; return (uint32_t)lab_get16(b)|((uint32_t)lab_get16(b+2)<<16); }
uint64_t lab_get64(const void *p) { const unsigned char *b=p; return (uint64_t)lab_get32(b)|((uint64_t)lab_get32(b+4)<<32); }
void lab_put16(void *p,uint16_t v) { unsigned char *b=p; b[0]=(unsigned char)v; b[1]=(unsigned char)(v>>8); }
void lab_put32(void *p,uint32_t v) { unsigned char *b=p; lab_put16(b,(uint16_t)v); lab_put16(b+2,(uint16_t)(v>>16)); }
void lab_put64(void *p,uint64_t v) { unsigned char *b=p; lab_put32(b,(uint32_t)v); lab_put32(b+4,(uint32_t)(v>>32)); }
int64_t lab_now_ms(void) { struct timespec ts; if(clock_gettime(CLOCK_MONOTONIC,&ts)) return -1; return (int64_t)ts.tv_sec*1000+ts.tv_nsec/1000000; }
int lab_wait(int fd,short events,int64_t deadline)
{
    struct pollfd p={.fd=fd,.events=events};
    for(;;) {
        int64_t remain=deadline-lab_now_ms(); int r;
        if(remain<=0) { errno=ETIMEDOUT; return -1; }
        r=poll(&p,1,remain>INT_MAX?INT_MAX:(int)remain);
        if(r<0 && errno==EINTR) continue;
        if(r<0) return -1;
        if(!r) { errno=ETIMEDOUT; return -1; }
        if(p.revents&events) return 0;
        if(p.revents&(POLLERR|POLLNVAL|POLLHUP)) { errno=ECONNRESET; return -1; }
    }
}
static int shape(uint16_t type,uint32_t n,const unsigned char *p)
{
    switch(type) {
    case LAB_HELLO: case LAB_HELLO_REPLY: case LAB_READY: case LAB_STOPPED: case LAB_RUN: case LAB_INSPECT: return n==0?0:-1;
    case LAB_INSPECT_REPLY: return n==16 && lab_get32(p+12)==0?0:-1;
    case LAB_STOP_REQUEST: case LAB_RESET_NOTICE: case LAB_STOP: return n==16 && lab_get32(p+12)==0?0:-1;
    case LAB_EXIT_ACK: return n==16?0:-1;
    case LAB_ERROR: return n==8 && lab_get32(p)>0 && lab_get32(p)<=4095?0:-1;
    default: return -1;
    }
}
int lab_encode(unsigned char *b,size_t cap,const struct lab_message *m)
{
    size_t n=LAB_HEADER_SIZE+(size_t)m->payload_len;
    if(m->payload_len>sizeof(m->payload) || n>cap || !m->request_id || shape(m->type,m->payload_len,m->payload)) { errno=EPROTO; return -1; }
    memset(b,0,n); memcpy(b,"TLAB",4); lab_put16(b+4,1); lab_put16(b+6,m->type);
    lab_put64(b+8,m->request_id); lab_put64(b+16,m->session); lab_put64(b+24,m->epoch);
    lab_put64(b+32,m->lease_token); lab_put64(b+40,m->operation); lab_put32(b+48,m->vf);
    lab_put32(b+52,m->ready_gen); lab_put32(b+60,m->payload_len); memcpy(b+64,m->payload,m->payload_len);
    return (int)n;
}
int lab_decode(struct lab_message *m,const unsigned char *b,size_t n)
{
    uint32_t bytes;
    if(n<64 || n>LAB_FRAME_MAX || memcmp(b,"TLAB",4) || lab_get16(b+4)!=1 || lab_get32(b+56)) { errno=EPROTO; return -1; }
    bytes=lab_get32(b+60);
    if(bytes!=n-64 || shape(lab_get16(b+6),bytes,b+64) || !lab_get64(b+8)) { errno=EPROTO; return -1; }
    memset(m,0,sizeof(*m)); m->type=lab_get16(b+6); m->request_id=lab_get64(b+8);
    m->session=lab_get64(b+16); m->epoch=lab_get64(b+24); m->lease_token=lab_get64(b+32);
    m->operation=lab_get64(b+40); m->vf=lab_get32(b+48); m->ready_gen=lab_get32(b+52);
    m->payload_len=bytes; memcpy(m->payload,b+64,bytes); return 0;
}
/* Partial I/O 與 EINTR 只推進已完成的部分；重試沿用原 deadline，避免不斷延長等待。 */
static int transfer(int fd,void *buf,size_t length,int64_t deadline,bool sending)
{
    unsigned char *p=buf;
    while(length) {
        ssize_t n;
        if(lab_wait(fd,sending?POLLOUT:POLLIN,deadline)) return -1;
        if(sending) n=send(fd,p,length,MSG_DONTWAIT|MSG_NOSIGNAL);
        else {
            unsigned char control[CMSG_SPACE(16*sizeof(int))];
            struct iovec iov={.iov_base=p,.iov_len=length};
            struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
            bool invalid=false;
            n=recvmsg(fd,&msg,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
            /* 收到任何 ancillary 都拒絕；已送入本程序的 fd 必須先關閉。 */
            for(struct cmsghdr *c=CMSG_FIRSTHDR(&msg);n>=0 && c;c=CMSG_NXTHDR(&msg,c)) {
                invalid=true;
                if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS && c->cmsg_len>=CMSG_LEN(0)) {
                    size_t count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int); int *fds=(void *)CMSG_DATA(c);
                    for(size_t i=0;i<count;i++) close(fds[i]);
                }
            }
            if(n>=0 && (invalid || (msg.msg_flags&(MSG_CTRUNC|MSG_TRUNC)))) { errno=EPROTO; return -1; }
        }
        if(n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if(n<0) return -1;
        if(!n) { errno=ECONNRESET; return -1; }
        p+=(size_t)n; length-=(size_t)n;
    }
    return 0;
}
int lab_send(int fd,const struct lab_message *m,int64_t deadline)
{
    unsigned char frame[4+LAB_FRAME_MAX]; int n=lab_encode(frame+4,LAB_FRAME_MAX,m);
    if(n<0) return -1;
    lab_put32(frame,(uint32_t)n); return transfer(fd,frame,(size_t)n+4,deadline,true);
}
int lab_recv(int fd,struct lab_message *m,int64_t deadline)
{
    unsigned char frame[LAB_FRAME_MAX],prefix[4]; uint32_t n;
    if(transfer(fd,prefix,4,deadline,false)) return -1;
    n=lab_get32(prefix);
    if(n<64 || n>LAB_FRAME_MAX) { errno=EMSGSIZE; return -1; }
    if(transfer(fd,frame,n,deadline,false)) return -1;
    return lab_decode(m,frame,n);
}
/* 核對 socket 的 peer credentials；訊息內聲稱的 VF/session 仍要另經 lab_tuple 驗證。 */
int lab_peer(int fd,uid_t uid,pid_t pid)
{
    struct ucred c; socklen_t n=sizeof(c);
    if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&c,&n)) return -1;
    if(n!=sizeof(c) || c.uid!=uid || c.pid!=pid) { errno=EACCES; return -1; }
    return 0;
}
int lab_tuple(const struct lab_message *m,uint32_t vf,uint64_t session,uint64_t epoch,uint64_t lease,uint64_t operation)
{
    if(m->vf!=vf || m->session!=session || m->epoch!=epoch || m->lease_token!=lease || m->operation!=operation) { errno=ESTALE; return -1; }
    return 0;
}
static int addr(struct sockaddr_un *a,const char *path)
{
    if(strlen(path)>=sizeof(a->sun_path)) { errno=ENAMETOOLONG; return -1; }
    memset(a,0,sizeof(*a)); a->sun_family=AF_UNIX; strcpy(a->sun_path,path); return 0;
}
int lab_listen(const char *path)
{
    struct sockaddr_un a; int fd,saved;
    if(addr(&a,path)) return -1;
    fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0); if(fd<0) return -1;
    if(bind(fd,(struct sockaddr *)&a,sizeof(a)) || chmod(path,0600) || listen(fd,8)) { saved=errno; close(fd); errno=saved; return -1; }
    return fd;
}
int lab_connect(const char *path,int64_t deadline)
{
    struct sockaddr_un a; int fd,saved,error=0; socklen_t n=sizeof(error);
    if(addr(&a,path)) return -1;
    fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0); if(fd<0) return -1;
    if(!connect(fd,(struct sockaddr *)&a,sizeof(a))) return fd;
    if(errno!=EINPROGRESS || lab_wait(fd,POLLOUT,deadline) || getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&n) || error) {
        saved=error?error:errno; close(fd); errno=saved; return -1;
    }
    return fd;
}
uint32_t lab_crc32(const void *p,size_t n)
{
    const unsigned char *b=p; uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;i++) { c^=b[i]; for(unsigned j=0;j<8;j++) c=(c>>1)^((0u-(c&1u))&0xedb88320u); }
    return c^UINT32_MAX;
}
