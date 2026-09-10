/*
 * mkdtb - Write the CosmoOS guest machine's device tree to a file
 * (tools/fdt/fdt.h). The build runs it to put tests/hv/virt.dtb in the
 * boot archive, so the kernel's own tests hand a guest the same blob the
 * owner would.
 *
 *   mkdtb OUT NR_CPUS RAM_MIB [BOOTARGS]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cosmo/hv_machine.h>
#include "fdt.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: mkdtb OUT NR_CPUS RAM_MIB [BOOTARGS]\n");
        return 2;
    }
    unsigned cpus = (unsigned)strtoul(argv[2], NULL, 0);
    uint64_t ram = strtoull(argv[3], NULL, 0) << 20;
    static unsigned char buf[16384];
    size_t size = 0;
    int rc = fdt_cosmo_virt(buf, sizeof(buf), cpus, COSMO_HVM_RAM_BASE, ram, argc > 4 ? argv[4] : NULL, &size);
    if (rc) {
        fprintf(stderr, "mkdtb: %d\n", rc);
        return 1;
    }
    FILE *f = fopen(argv[1], "wb");
    if (f == NULL || fwrite(buf, 1, size, f) != size) {
        perror(argv[1]);
        return 1;
    }
    fclose(f);
    return 0;
}
