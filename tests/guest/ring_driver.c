#include "lab_ring.h"
#include <stdio.h>
#include <inttypes.h>
int main(void)
{
    uint32_t tail,head,entries;
    while (scanf("%" SCNu32 " %" SCNu32 " %" SCNu32,&tail,&head,&entries)==3) {
        if (entries<16||entries>256||(entries&(entries-1))) return 2;
        printf("%u %u %u %u %u\n",lab_ring_used(tail,head),lab_ring_index(tail,entries),
            lab_ring_phase(tail,entries),lab_ring_index(head,entries),lab_ring_phase(head,entries));
    }
    return ferror(stdin)?1:0;
}
