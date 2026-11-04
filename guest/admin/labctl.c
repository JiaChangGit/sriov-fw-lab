#include "../common/lab_wire.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc,char **argv)
{
    struct lab_message query={.type=LAB_HELLO,.request_id=1};
    if (argc!=3||strcmp(argv[2],"status")) {
        fprintf(stderr,"Usage: labctl /run/lab-PID/control.sock status\n"); return 2;
    }
    int fd=lab_connect(argv[1],lab_now_ms()+5000);
    if (fd<0) { perror("labctl connect"); return 1; }
    int rc=lab_send(fd,&query,lab_now_ms()+5000)||lab_recv(fd,&query,lab_now_ms()+5000);
    close(fd);
    if (rc||query.type!=LAB_HELLO_REPLY||query.request_id!=1) {
        fprintf(stderr,"LABCTL_PROTOCOL_ERROR errno=%d\n",errno); return 1;
    }
    printf("{\"vf\":%u,\"session\":%" PRIu64 ",\"epoch\":%" PRIu64 ",\"lease_token\":%" PRIu64 ",\"ready_gen\":%u,\"operation\":%" PRIu64 "}\n",
        query.vf,query.session,query.epoch,query.lease_token,query.ready_gen,query.operation);
    return 0;
}
