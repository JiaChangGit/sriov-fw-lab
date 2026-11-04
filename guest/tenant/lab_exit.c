#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/io.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    unsigned code=(argc==2 && argv[1][0]=='0' && argv[1][1]==0)?0U:1U;
    if(ioperm(0xf4,4,1)) { perror("DEBUG_EXIT_IOPERM"); return 1; }
    fflush(NULL);
    outl(code,0xf4);
    /* 若 emulator 沒有 debug-exit 裝置，不冒充成功。 */
    fprintf(stderr,"DEBUG_EXIT_RETURNED code=%u\n",code);
    return 1;
}
