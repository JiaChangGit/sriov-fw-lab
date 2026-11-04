#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "fw_core.h"

int main(int argc, char **argv)
{
    struct fw_sq sq;
    unsigned i;
    FILE *input;
    fw_u8 *bytes;
    assert(argc == 2);
    input = fopen(argv[1], "rb");
    assert(input);
    /* 精確配置一個 descriptor，讓 sanitizer 能偵測讀取越界。 */
    bytes = malloc(32);
    assert(bytes);
    for (i = 0; i < 6; ++i) {
        fw_status status;
        assert(fread(bytes, 1, 32, input) == 32);
        status = fw_decode_sq(bytes, &sq);
        /* 舊 epoch 是有效編碼；硬體 gate 的拒絕另由實際 guest 證明。 */
        assert(status == (i == 0 ? FW_OK : FW_BAD_DESC));
        printf("T09_DECODER vector=%u status=%u bounded_bytes=32\n", i, (unsigned)status);
    }
    assert(fgetc(input) == EOF && !ferror(input));
    free(bytes);
    assert(fclose(input) == 0);
    return 0;
}
