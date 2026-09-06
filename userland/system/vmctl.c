/*
 * vmctl - Control virtual machines (docs/kernel-services/virtualization/).
 *
 *   vmctl probe              print the backend line from /dev/vmm; exit 0 present, 2 none
 *   vmctl info               print the hv.* sysctl values
 *   vmctl run [-m KIB] [-a GPA] [-e ENTRY] IMAGE
 *                            load a flat image at GPA (default 0x1000), start a real-mode
 *                            vCPU at ENTRY (default GPA), run until HLT; echo the guest's
 *                            debug console; report other exits. Exit 0 on HLT, 1 otherwise.
 */

#include <cosmo/hv.h>
#include <cosmo/sysctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int usage(void)
{
    fprintf(stderr, "usage: vmctl probe | info | run [-m KIB] [-a GPA] [-e ENTRY] IMAGE\n");
    return 2;
}

static int probe(void)
{
    int fd = open("/dev/vmm", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "vmctl: /dev/vmm: %s\n", strerror(errno));
        return 2;
    }
    char line[128];
    ssize_t n = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0) {
        fprintf(stderr, "vmctl: /dev/vmm: read failed\n");
        return 2;
    }
    line[n] = '\0';
    fputs(line, stdout);
    return strncmp(line, "none", 4) == 0 ? 2 : 0;
}

static int info(void)
{
    static const char *const names[] = { "hv.backend", "hv.vms", "hv.vcpus", "hv.exits" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char value[64];
        long n = cosmo_sysctl(names[i], value, sizeof(value));
        if (n < 0)
            return 1;
        printf("%s = %s\n", names[i], value);
    }
    return 0;
}

static void drain_console(int vm)
{
    char buf[256];
    for (;;) {
        long n = read(vm, buf, sizeof(buf));
        if (n <= 0)
            break;
        fwrite(buf, 1, (size_t)n, stdout);
    }
    fflush(stdout);
}

static int run(int argc, char **argv)
{
    unsigned long mem_kib = 1024;
    unsigned long long gpa = 0x1000, entry = 0;
    int i = 0;
    while (i < argc && argv[i][0] == '-') {
        if (i + 1 >= argc)
            return usage();
        if (strcmp(argv[i], "-m") == 0)
            mem_kib = strtoul(argv[i + 1], NULL, 0);
        else if (strcmp(argv[i], "-a") == 0)
            gpa = strtoull(argv[i + 1], NULL, 0);
        else if (strcmp(argv[i], "-e") == 0)
            entry = strtoull(argv[i + 1], NULL, 0);
        else
            return usage();
        i += 2;
    }
    if (i != argc - 1)
        return usage();
    const char *path = argv[i];
    if (entry == 0)
        entry = gpa;

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "vmctl: %s: %s\n", path, strerror(errno));
        return 1;
    }
    size_t cap = 65536, len = 0;
    unsigned char *image = malloc(cap);
    for (;;) {
        if (len == cap) {
            cap *= 2;
            image = realloc(image, cap);
        }
        size_t n = fread(image + len, 1, cap - len, f);
        if (n == 0)
            break;
        len += n;
    }
    fclose(f);
    if (len == 0) {
        fprintf(stderr, "vmctl: %s: empty image\n", path);
        return 1;
    }

    int vmm = open("/dev/vmm", O_RDWR);
    if (vmm < 0) {
        fprintf(stderr, "vmctl: /dev/vmm: %s\n", strerror(errno));
        return 1;
    }
    int vm = cosmo_vm_create(vmm);
    close(vmm);
    if (vm < 0) {
        fprintf(stderr, "vmctl: vm_create: %s\n", strerror(-vm));
        return 1;
    }
    int rc = cosmo_vm_mem(vm, 0, (uint64_t)mem_kib << 10);
    if (rc < 0) {
        fprintf(stderr, "vmctl: vm_mem(%lu KiB): %s\n", mem_kib, strerror(-rc));
        return 1;
    }
    long w = cosmo_vm_mem_write(vm, gpa, image, len);
    if (w < 0) {
        fprintf(stderr, "vmctl: load at 0x%llx: %s\n", gpa, strerror((int)-w));
        return 1;
    }
    int vcpu = cosmo_vcpu_create(vm, 0);
    if (vcpu < 0) {
        fprintf(stderr, "vmctl: vcpu_create: %s\n", strerror(-vcpu));
        return 1;
    }
    struct cosmo_vcpu_regs regs;
    cosmo_vcpu_get_regs(vcpu, &regs);
#if defined(__aarch64__)
    regs.pc = entry;      /* the register file is per-architecture (uapi) */
#else
    regs.rip = entry;
#endif
    rc = cosmo_vcpu_set_regs(vcpu, &regs);
    if (rc < 0) {
        fprintf(stderr, "vmctl: vcpu_regs: %s\n", strerror(-rc));
        return 1;
    }
    printf("vmctl: %s: %zu bytes at 0x%llx, %lu KiB, entry 0x%llx\n", path, len, gpa, mem_kib, entry);

    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    for (;;) {
        rc = cosmo_vcpu_run(vcpu, &x);
        drain_console(vm);
        if (rc < 0) {
            fprintf(stderr, "vmctl: vcpu_run: %s\n", strerror(-rc));
            return 1;
        }
        switch (x.kind) {
        case COSMO_VM_EXIT_HLT:
            printf("vmctl: halted at 0x%llx%s\n", (unsigned long long)x.rip,
                   (x.flags & COSMO_VM_EXIT_F_IRQ_PENDING) ? " (interrupt pending)" : "");
            return 0;
        case COSMO_VM_EXIT_IO:
            printf("vmctl: io %s port 0x%x size %u%s%s value 0x%x at 0x%llx\n", x.io.write ? "out" : "in", x.io.port,
                   x.io.size, x.io.string ? " string" : "", x.io.rep ? " rep" : "", x.io.value,
                   (unsigned long long)x.rip);
            if (!x.io.write)
                x.io.value = 0;   /* nothing behind the port */
            continue;
        case COSMO_VM_EXIT_HYPERCALL:
            printf("vmctl: hypercall %llu (0x%llx 0x%llx 0x%llx 0x%llx)\n", (unsigned long long)x.hypercall.nr,
                   (unsigned long long)x.hypercall.a0, (unsigned long long)x.hypercall.a1,
                   (unsigned long long)x.hypercall.a2, (unsigned long long)x.hypercall.a3);
            continue;
        case COSMO_VM_EXIT_MMIO:
            printf("vmctl: mmio %s at 0x%llx, rip 0x%llx: no device; stopping\n", x.mmio.write ? "write" : "read",
                   (unsigned long long)x.mmio.gpa, (unsigned long long)x.rip);
            return 1;
        case COSMO_VM_EXIT_WFI:
            printf("vmctl: waiting for an interrupt at 0x%llx%s\n", (unsigned long long)x.rip,
                   (x.flags & COSMO_VM_EXIT_F_IRQ_PENDING) ? " (interrupt pending)" : "");
            return 0;
        case COSMO_VM_EXIT_SYSREG:
            printf("vmctl: system register %s (iss 0x%x) into x%u at 0x%llx: no model; stopping\n",
                   x.sysreg.write ? "write" : "read", x.sysreg.iss, x.sysreg.reg, (unsigned long long)x.rip);
            return 1;
        case COSMO_VM_EXIT_SHUTDOWN:
            printf("vmctl: guest shutdown (triple fault) at 0x%llx\n", (unsigned long long)x.rip);
            return 1;
        case COSMO_VM_EXIT_FAIL:
            printf("vmctl: entry failed: code 0x%x info 0x%llx 0x%llx\n", x.fail.code,
                   (unsigned long long)x.fail.info1, (unsigned long long)x.fail.info2);
            return 1;
        default:
            printf("vmctl: unknown exit %u\n", x.kind);
            return 1;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    if (strcmp(argv[1], "probe") == 0)
        return probe();
    if (strcmp(argv[1], "info") == 0)
        return info();
    if (strcmp(argv[1], "run") == 0)
        return run(argc - 2, argv + 2);
    return usage();
}
