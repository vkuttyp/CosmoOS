/*
 * vmctl - Control virtual machines (docs/kernel-services/virtualization/).
 *
 *   vmctl probe              print the backend line from /dev/vmm; exit 0 present, 2 none
 *   vmctl info               print the hv.* sysctl values
 *   vmctl run [-m KIB] [-a GPA] [-e ENTRY] IMAGE
 *                            load a flat image at GPA (default 0x1000), start a real-mode
 *                            vCPU at ENTRY (default GPA), run until HLT; echo the guest's
 *                            debug console; report other exits. Exit 0 on HLT, 1 otherwise.
 *   vmctl run --machine [-m MIB] [-c NCPUS] [--append CMDLINE] IMAGE
 *                            AArch64: the machine a guest is handed
 *                            (docs/audit/next-subsystem-machine.md). RAM at
 *                            COSMO_HVM_RAM_BASE, the image where its arm64 Image
 *                            header's text_offset says (a flat image at RAM's
 *                            start), a device tree describing the machine at the
 *                            first 2 MiB boundary past the image and in x0. The
 *                            owner is the firmware: PSCI is answered here -- CPU_ON
 *                            creates a vCPU and runs it, in turn with the others,
 *                            one tick each -- and SYSTEM_OFF ends the run. The
 *                            guest's console is echoed. Exit 0 on power-off.
 */

#include <cosmo/hv.h>
#include <cosmo/sysctl.h>
#include <uapi/cosmo/hv_machine.h>
#include "../../tools/fdt/fdt.h"
#include "vblk.h"
#include "vnet.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int usage(void)
{
    fprintf(stderr, "usage: vmctl probe | info | run [-m KIB] [-a GPA] [-e ENTRY] IMAGE\n"
                    "       vmctl run --machine [-m MIB] [-c NCPUS] [--disk FILE | --disk-rw FILE] [--net loop] "
                    "[--append CMDLINE] IMAGE\n");
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

/* The whole file, or NULL with the reason printed. Every allocation is
 * checked: an image is what a user hands us, and "large" is not an
 * error a crash should report. */
static unsigned char *read_image(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "vmctl: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    size_t cap = 65536, len = 0;
    unsigned char *image = malloc(cap);
    if (image == NULL) {
        fprintf(stderr, "vmctl: %s: out of memory\n", path);
        fclose(f);
        return NULL;
    }
    for (;;) {
        if (len == cap) {
            unsigned char *bigger = realloc(image, cap * 2);
            if (bigger == NULL) {
                fprintf(stderr, "vmctl: %s: out of memory at %zu bytes\n", path, cap);
                free(image);
                fclose(f);
                return NULL;
            }
            image = bigger;
            cap *= 2;
        }
        size_t n = fread(image + len, 1, cap - len, f);
        if (n == 0)
            break;
        len += n;
    }
    fclose(f);
    if (len == 0) {
        fprintf(stderr, "vmctl: %s: empty image\n", path);
        free(image);
        return NULL;
    }
    *len_out = len;
    return image;
}

/* --- machine mode: the machine a guest is handed ------------------------ */

#define PSCI_VERSION          0x84000000ull
#define PSCI_CPU_OFF          0x84000002ull
#define PSCI_CPU_ON           0xC4000003ull
#define PSCI_AFFINITY_INFO    0xC4000004ull
#define PSCI_MIGRATE_INFO     0x84000006ull
#define PSCI_SYSTEM_OFF       0x84000008ull
#define PSCI_SYSTEM_RESET     0x84000009ull
#define PSCI_FEATURES         0x8400000Aull
#define PSCI_SUCCESS          0
#define PSCI_NOT_SUPPORTED    (-1)
#define PSCI_INVALID_PARAMS   (-2)
#define PSCI_ALREADY_ON       (-4)

/* PSCI function ids: SMCCC fast calls, 32- or 64-bit, in the standard
 * service range 0x84000000..0x8400001F / 0xC4000000..0xC400001F. */
static int is_psci(uint64_t fn)
{
    return (fn & 0xBFFFFFE0ull) == 0x84000000ull;
}

/* --- the virtio-mmio block device (docs/audit/next-subsystem-vblk.md) --- */

struct vio {
    int vm;               /* for cosmo_vm_mem_* and cosmo_vm_raise_spi */
    int disk_fd;          /* -1 when no --disk: the transport reports no device */
    int writable;         /* --disk-rw: the disk is read-write, and offers flush */
    uint64_t capacity;    /* sectors */
    uint32_t feat_sel, status;
    struct vq_queue q;
    int irq_pending;
    int draining;         /* a notify left work the run loop is still serving */
};

static struct vio g_vio;

static int vio_read_guest(void *c, uint64_t gpa, void *buf, uint32_t len)
{
    struct vio *v = c;
    return cosmo_vm_mem_read(v->vm, gpa, buf, len) == (long)len ? 0 : -1;
}
static int vio_write_guest(void *c, uint64_t gpa, const void *buf, uint32_t len)
{
    struct vio *v = c;
    return cosmo_vm_mem_write(v->vm, gpa, buf, len) == (long)len ? 0 : -1;
}
static int vio_disk_read(void *c, uint64_t off, void *buf, uint32_t len)
{
    struct vio *v = c;
    if (lseek(v->disk_fd, (off_t)off, SEEK_SET) < 0)
        return -1;
    uint32_t done = 0;
    while (done < len) {
        ssize_t n = read(v->disk_fd, (char *)buf + done, len - done);
        if (n <= 0)
            return -1;
        done += (uint32_t)n;
    }
    return 0;
}
/* The write side, present only on a --disk-rw device (the callbacks are NULL
 * otherwise and vblk never reaches them). The device has already bounded
 * off+len to the capacity, so this never grows the file. */
static int vio_disk_write(void *c, uint64_t off, const void *buf, uint32_t len)
{
    struct vio *v = c;
    if (lseek(v->disk_fd, (off_t)off, SEEK_SET) < 0)
        return -1;
    uint32_t done = 0;
    while (done < len) {
        ssize_t n = write(v->disk_fd, (const char *)buf + done, len - done);
        if (n <= 0)
            return -1;
        done += (uint32_t)n;
    }
    return 0;
}
static int vio_disk_flush(void *c)
{
    struct vio *v = c;
    /* fsync the disk file alone: a guest that spams T_FLUSH commits its own
     * file, and cannot force synchronous commits of unrelated host mounts. */
    return fsync(v->disk_fd);
}

/* Serve one bounded batch of the available ring. vblk_process serves at
 * most VBLK_MAX_BYTES_PER_CALL and returns the number of requests it
 * completed; > 0 means it made progress and there may be more, 0 means the
 * ring is drained, < 0 a hostile ring. Raise the device's interrupt for the
 * requests just completed. */
static int vio_service(struct vio *v)
{
    struct vblk_io io = { vio_read_guest, vio_write_guest, vio_disk_read,
                          v->writable ? vio_disk_write : NULL,
                          v->writable ? vio_disk_flush : NULL,
                          v, v->capacity, VBLK_MAX_BYTES_PER_CALL };
    uint16_t before = v->q.used_idx;
    int served = vblk_process(&io, &v->q);
    /* Interrupt on anything published this call, judged by the used ring the
     * device advances, not by the return value: a call that completes a
     * request and then faults on a later one returns -1 but still owes the
     * completed request its interrupt, or a guest waiting on it hangs. The
     * return value governs draining (below); this governs notification. */
    if (v->q.used_idx != before) {
        v->irq_pending = 1;
        cosmo_vm_raise_spi(v->vm, COSMO_HVM_VIRTIO0_INTID);   /* through the guest's distributor */
    }
    return served;
}

/* The transport registers (virtio-mmio v2). `*val` is the read result. */
static void vio_reg(struct vio *v, unsigned off, int write, uint64_t *val)
{
    if (!write) {
        switch (off) {
        case 0x000: *val = 0x74726976u; return;                 /* MagicValue */
        case 0x004: *val = 2; return;                           /* Version */
        case 0x008: *val = v->disk_fd >= 0 ? 2u : 0u; return;   /* DeviceID: block, or none */
        case 0x00c: *val = 0x554d4551u; return;                 /* VendorID */
        case 0x010:                                             /* DeviceFeatures */
            /* high word (feat_sel 1): VERSION_1 (bit 32). low word: a
             * writable disk offers FLUSH, a read-only disk offers RO. */
            if (v->feat_sel == 1) { *val = 1u; return; }
            *val = v->writable ? (1u << VIRTIO_BLK_F_FLUSH) : (1u << VIRTIO_BLK_F_RO);
            return;
        case 0x034: *val = VBLK_QUEUE_MAX; return;              /* QueueNumMax */
        case 0x044: *val = (uint32_t)v->q.ready; return;
        case 0x060: *val = v->irq_pending ? 1u : 0u; return;    /* InterruptStatus: used-ring event */
        case 0x070: *val = v->status; return;
        case 0x100: *val = (uint32_t)v->capacity; return;       /* capacity low */
        case 0x104: *val = (uint32_t)(v->capacity >> 32); return;
        default: *val = 0; return;
        }
    }
    uint32_t w = (uint32_t)*val;
    switch (off) {
    case 0x014: v->feat_sel = w; break;
    case 0x038: v->q.size = (uint16_t)w; break;
    case 0x044: v->q.ready = (int)w; break;
    case 0x050:                                                /* QueueNotify */
        /* Serve a first batch now; if it made progress the ring may hold
         * more than one batch's worth, so mark it draining and let the run
         * loop finish it a batch at a time -- the guest need not notify
         * again for the rest to complete. */
        if (vio_service(v) > 0)
            v->draining = 1;
        break;
    case 0x064:                                                 /* InterruptACK */
        v->irq_pending = 0;
        cosmo_vm_lower_spi(v->vm, COSMO_HVM_VIRTIO0_INTID);
        break;
    case 0x070: v->status = w; break;
    case 0x080: v->q.desc_gpa = (v->q.desc_gpa & ~0xFFFFFFFFull) | w; break;
    case 0x084: v->q.desc_gpa = (v->q.desc_gpa & 0xFFFFFFFFull) | ((uint64_t)w << 32); break;
    case 0x090: v->q.avail_gpa = (v->q.avail_gpa & ~0xFFFFFFFFull) | w; break;
    case 0x094: v->q.avail_gpa = (v->q.avail_gpa & 0xFFFFFFFFull) | ((uint64_t)w << 32); break;
    case 0x0a0: v->q.used_gpa = (v->q.used_gpa & ~0xFFFFFFFFull) | w; break;
    case 0x0a4: v->q.used_gpa = (v->q.used_gpa & 0xFFFFFFFFull) | ((uint64_t)w << 32); break;
    default: break;
    }
}

/* --- the virtio-mmio network device (docs/audit/next-subsystem-vnet.md) --- */

#define VNET_WIRE_SLOTS 16   /* the loopback wire's depth; frames past it drop */

struct vnet_dev {
    int vm;
    int present;                          /* --net loop given */
    uint32_t feat_sel, status, queue_sel;
    struct vq_queue rq, tq;               /* receive (queue 0), transmit (queue 1) */
    int irq_pending;
    int draining;
    uint8_t mac[6];
    struct { uint8_t buf[VNET_FRAME_MAX]; uint32_t len; } wire[VNET_WIRE_SLOTS];
    unsigned wire_head, wire_count;       /* the loopback FIFO */
};

static struct vnet_dev g_vnet;

static int vnet_read_guest(void *c, uint64_t gpa, void *buf, uint32_t len)
{
    struct vnet_dev *d = c;
    return cosmo_vm_mem_read(d->vm, gpa, buf, len) == (long)len ? 0 : -1;
}
static int vnet_write_guest(void *c, uint64_t gpa, const void *buf, uint32_t len)
{
    struct vnet_dev *d = c;
    return cosmo_vm_mem_write(d->vm, gpa, buf, len) == (long)len ? 0 : -1;
}
/* The wire is a loopback FIFO: a transmitted frame is enqueued and comes back
 * on receive. It drops when full -- a NIC drops with nowhere to put a frame,
 * it does not grow without bound. */
static int vnet_wire_tx(void *c, const void *frame, uint32_t len)
{
    struct vnet_dev *d = c;
    if (len > VNET_FRAME_MAX)
        return -1;
    if (d->wire_count == VNET_WIRE_SLOTS)
        return 0;                                   /* dropped, not an error */
    unsigned slot = (d->wire_head + d->wire_count) % VNET_WIRE_SLOTS;
    memcpy(d->wire[slot].buf, frame, len);
    d->wire[slot].len = len;
    d->wire_count++;
    return 0;
}
static int vnet_wire_rx(void *c, void *buf, uint32_t max)
{
    struct vnet_dev *d = c;
    if (d->wire_count == 0)
        return 0;
    uint32_t len = d->wire[d->wire_head].len;
    if (len > max)
        len = max;
    memcpy(buf, d->wire[d->wire_head].buf, len);
    d->wire_head = (d->wire_head + 1) % VNET_WIRE_SLOTS;
    d->wire_count--;
    return (int)len;
}

/* Serve both queues: drain transmit to the wire, then fill receive buffers
 * from it. Interrupt on any used-ring advance; return whether either made
 * progress, so the run loop keeps draining until neither does. */
static int vnet_service(struct vnet_dev *d)
{
    struct vnet_io io = { vnet_read_guest, vnet_write_guest, vnet_wire_tx, vnet_wire_rx,
                          d, VNET_MAX_BYTES_PER_CALL };
    uint16_t tb = d->tq.used_idx, rb = d->rq.used_idx;
    int t = vnet_process_tx(&io, &d->tq);
    int r = vnet_process_rx(&io, &d->rq);
    if (d->tq.used_idx != tb || d->rq.used_idx != rb) {
        d->irq_pending = 1;
        cosmo_vm_raise_spi(d->vm, COSMO_HVM_VIRTIO1_INTID);
    }
    return (t > 0 || r > 0) ? 1 : 0;
}

/* The transport registers for the net device. QueueSel selects the receive
 * or transmit queue for the queue-shaped registers. */
static void vnet_reg(struct vnet_dev *d, unsigned off, int write, uint64_t *val)
{
    /* Only queues 0 (receive) and 1 (transmit) exist; a QueueSel past them
     * selects nothing, so the queue-shaped registers read 0 and ignore
     * writes rather than aliasing an existing queue's state. */
    struct vq_queue *q = d->queue_sel == 0 ? &d->rq : d->queue_sel == 1 ? &d->tq : NULL;
    if (!write) {
        switch (off) {
        case 0x000: *val = 0x74726976u; return;                 /* MagicValue */
        case 0x004: *val = 2; return;                           /* Version */
        case 0x008: *val = d->present ? 1u : 0u; return;        /* DeviceID: network, or none */
        case 0x00c: *val = 0x554d4551u; return;                 /* VendorID */
        case 0x010:                                             /* DeviceFeatures */
            /* high word (feat_sel 1): VERSION_1 (bit 32). low word: MAC. */
            *val = d->feat_sel == 1 ? 1u : (1u << VIRTIO_NET_F_MAC);
            return;
        case 0x034: *val = VQ_MAX; return;                     /* QueueNumMax */
        case 0x044: *val = q ? (uint32_t)q->ready : 0u; return;
        case 0x060: *val = d->irq_pending ? 1u : 0u; return;    /* InterruptStatus */
        case 0x070: *val = d->status; return;
        /* config space: virtio_net_config.mac[6] at offset 0 */
        case 0x100: *val = (uint32_t)d->mac[0] | ((uint32_t)d->mac[1] << 8) |
                           ((uint32_t)d->mac[2] << 16) | ((uint32_t)d->mac[3] << 24); return;
        case 0x104: *val = (uint32_t)d->mac[4] | ((uint32_t)d->mac[5] << 8); return;
        default: *val = 0; return;
        }
    }
    uint32_t w = (uint32_t)*val;
    switch (off) {
    case 0x014: d->feat_sel = w; break;
    case 0x030: d->queue_sel = w; break;
    case 0x050:                                                /* QueueNotify */
        if (vnet_service(d) > 0)
            d->draining = 1;
        break;
    case 0x064:                                                /* InterruptACK */
        d->irq_pending = 0;
        cosmo_vm_lower_spi(d->vm, COSMO_HVM_VIRTIO1_INTID);
        break;
    case 0x070: d->status = w; break;
    /* the queue-shaped registers act on the selected queue, and do nothing
     * when QueueSel names one that does not exist */
    case 0x038: if (q) q->size = (uint16_t)w; break;
    case 0x044: if (q) q->ready = (int)w; break;
    case 0x080: if (q) q->desc_gpa = (q->desc_gpa & ~0xFFFFFFFFull) | w; break;
    case 0x084: if (q) q->desc_gpa = (q->desc_gpa & 0xFFFFFFFFull) | ((uint64_t)w << 32); break;
    case 0x090: if (q) q->avail_gpa = (q->avail_gpa & ~0xFFFFFFFFull) | w; break;
    case 0x094: if (q) q->avail_gpa = (q->avail_gpa & 0xFFFFFFFFull) | ((uint64_t)w << 32); break;
    case 0x0a0: if (q) q->used_gpa = (q->used_gpa & ~0xFFFFFFFFull) | w; break;
    case 0x0a4: if (q) q->used_gpa = (q->used_gpa & 0xFFFFFFFFull) | ((uint64_t)w << 32); break;
    default: break;
    }
}

struct machine {
    int vm;
    int vcpu[COSMO_HV_VCPUS_MAX];      /* -1: not created */
    int running[COSMO_HV_VCPUS_MAX];
    unsigned nr_cpus;                  /* what the device tree promises */
};

static int set_x0(int vcpu, uint64_t v)
{
    struct cosmo_vcpu_regs regs;
    int rc = cosmo_vcpu_get_regs(vcpu, &regs);
    if (rc < 0)
        return rc;
#if defined(__aarch64__)
    regs.x[0] = v;
#else
    regs.rax = v;
#endif
    return cosmo_vcpu_set_regs(vcpu, &regs);
}

/* The owner is the firmware. `cpu` made the call in `x`; the answer goes
 * back in its x0. Returns 1 when the guest asked to power off. */
static int psci_answer(struct machine *m, unsigned cpu, const struct cosmo_vm_exit *x)
{
    uint64_t fn = x->hypercall.nr;
    int64_t ret = PSCI_NOT_SUPPORTED;
    int off = 0;
    switch (fn) {
    case PSCI_VERSION:
        ret = 0x00010000;   /* 1.0 */
        break;
    case PSCI_FEATURES: {
        uint64_t q = x->hypercall.a0;
        ret = (q == PSCI_VERSION || q == PSCI_CPU_OFF || q == PSCI_CPU_ON || q == PSCI_AFFINITY_INFO ||
               q == PSCI_MIGRATE_INFO || q == PSCI_SYSTEM_OFF || q == PSCI_SYSTEM_RESET || q == PSCI_FEATURES)
                  ? 0 : PSCI_NOT_SUPPORTED;
        break;
    }
    case PSCI_CPU_ON: {
        unsigned target = (unsigned)(x->hypercall.a0 & 0xFFu);   /* Aff0: the vCPU's index */
        uint64_t entry = x->hypercall.a1, ctx = x->hypercall.a2;
        if ((x->hypercall.a0 & ~0xFFull) != 0 || target >= m->nr_cpus || target >= COSMO_HV_VCPUS_MAX) {
            ret = PSCI_INVALID_PARAMS;
            break;
        }
        if (m->running[target]) {
            ret = PSCI_ALREADY_ON;
            break;
        }
        if (m->vcpu[target] < 0) {
            m->vcpu[target] = cosmo_vcpu_create(m->vm, target);
            if (m->vcpu[target] < 0) {
                fprintf(stderr, "vmctl: vcpu %u: %s\n", target, strerror(-m->vcpu[target]));
                m->vcpu[target] = -1;
                ret = PSCI_INVALID_PARAMS;
                break;
            }
        }
        struct cosmo_vcpu_regs regs;
        cosmo_vcpu_get_regs(m->vcpu[target], &regs);
#if defined(__aarch64__)
        regs.pc = entry;
        regs.x[0] = ctx;
#else
        (void)entry;   /* PSCI is AArch64's firmware interface; this path is never reached on x86 */
        (void)ctx;
#endif
        if (cosmo_vcpu_set_regs(m->vcpu[target], &regs) < 0) {
            ret = PSCI_INVALID_PARAMS;
            break;
        }
        m->running[target] = 1;
        ret = PSCI_SUCCESS;
        break;
    }
    case PSCI_CPU_OFF:
        m->running[cpu] = 0;
        ret = PSCI_SUCCESS;
        break;
    case PSCI_AFFINITY_INFO: {
        unsigned target = (unsigned)(x->hypercall.a0 & 0xFFu);
        ret = target < COSMO_HV_VCPUS_MAX && m->running[target] ? 0 : 1;   /* ON : OFF */
        break;
    }
    case PSCI_MIGRATE_INFO:
        ret = 2;   /* no migration, no Trusted OS */
        break;
    case PSCI_SYSTEM_OFF:
    case PSCI_SYSTEM_RESET:
        off = 1;
        ret = PSCI_SUCCESS;
        break;
    default:
        break;
    }
    set_x0(m->vcpu[cpu], (uint64_t)ret);
    return off;
}

static int run_machine(int argc, char **argv)
{
    unsigned long mem_mib = 16;
    unsigned nr_cpus = 1;
    const char *bootargs = NULL, *disk = NULL;
    int disk_writable = 0, net_loop = 0;
    int i = 0;
    while (i < argc && argv[i][0] == '-') {
        if (i + 1 >= argc)
            return usage();
        if (strcmp(argv[i], "-m") == 0)
            mem_mib = strtoul(argv[i + 1], NULL, 0);
        else if (strcmp(argv[i], "-c") == 0)
            nr_cpus = (unsigned)strtoul(argv[i + 1], NULL, 0);
        else if (strcmp(argv[i], "--append") == 0)
            bootargs = argv[i + 1];
        else if (strcmp(argv[i], "--disk") == 0 || strcmp(argv[i], "--disk-rw") == 0) {
            if (disk != NULL)
                return usage();   /* one disk, one mode */
            disk = argv[i + 1];
            disk_writable = strcmp(argv[i], "--disk-rw") == 0;
        } else if (strcmp(argv[i], "--net") == 0) {
            if (strcmp(argv[i + 1], "loop") != 0)
                return usage();   /* only the loopback wire, for now */
            net_loop = 1;
        } else
            return usage();
        i += 2;
    }
    if (i != argc - 1 || nr_cpus == 0 || nr_cpus > COSMO_HV_VCPUS_MAX)
        return usage();
    const char *path = argv[i];

    size_t len = 0;
    unsigned char *image = read_image(path, &len);
    if (image == NULL)
        return 1;

    /* Where the image goes: by its arm64 Image header, or at RAM's start
     * for a flat image without one. The header's image_size bounds the
     * image (it may be larger than the file: .bss), and the device tree
     * goes at the first 2 MiB boundary past that -- the boot protocol's
     * placement, relative to the image and never absolute. */
    uint64_t ram_base = COSMO_HVM_RAM_BASE, ram_bytes = (uint64_t)mem_mib << 20;
    uint64_t text_off = 0, image_size = len;
    int has_header = len >= 64 && *(const uint32_t *)(image + COSMO_HVM_IMAGE_MAGIC_OFF) == COSMO_HVM_IMAGE_MAGIC;
    if (has_header) {
        memcpy(&text_off, image + COSMO_HVM_IMAGE_TEXT_OFF, 8);
        memcpy(&image_size, image + COSMO_HVM_IMAGE_SIZE_OFF, 8);
        if (image_size < len)
            image_size = len;
    }
    uint64_t load = ram_base + text_off;
    uint64_t dtb_gpa = (load + image_size + (2ull << 20) - 1) & ~((2ull << 20) - 1);
    unsigned char dtb[8192];
    size_t dtb_len = 0;
    int rc = fdt_cosmo_virt(dtb, sizeof(dtb), nr_cpus, ram_base, ram_bytes, bootargs, &dtb_len);
    if (rc) {
        fprintf(stderr, "vmctl: device tree: %d\n", rc);
        return 1;
    }
    if (load + image_size > ram_base + ram_bytes || dtb_gpa + dtb_len > ram_base + ram_bytes) {
        fprintf(stderr, "vmctl: %lu MiB of RAM has no place for the image (0x%llx+0x%llx) and its device tree "
                        "(0x%llx+%zu); refusing rather than placing it wrong\n",
                mem_mib, (unsigned long long)load, (unsigned long long)image_size, (unsigned long long)dtb_gpa,
                dtb_len);
        return 1;
    }

    int vmm = open("/dev/vmm", O_RDWR);
    if (vmm < 0) {
        fprintf(stderr, "vmctl: /dev/vmm: %s\n", strerror(errno));
        return 1;
    }
    struct machine m;
    memset(&m, 0, sizeof(m));
    for (unsigned c = 0; c < COSMO_HV_VCPUS_MAX; c++)
        m.vcpu[c] = -1;
    m.nr_cpus = nr_cpus;
    m.vm = cosmo_vm_create(vmm);
    close(vmm);
    if (m.vm < 0) {
        fprintf(stderr, "vmctl: vm_create: %s\n", strerror(-m.vm));
        return 1;
    }
    rc = cosmo_vm_mem(m.vm, ram_base, ram_bytes);
    if (rc < 0) {
        fprintf(stderr, "vmctl: vm_mem(%lu MiB at 0x%llx): %s\n", mem_mib, (unsigned long long)ram_base, strerror(-rc));
        return 1;
    }
    long w = cosmo_vm_mem_write(m.vm, load, image, len);
    if (w >= 0)
        w = cosmo_vm_mem_write(m.vm, dtb_gpa, dtb, dtb_len);
    if (w < 0) {
        fprintf(stderr, "vmctl: load: %s\n", strerror((int)-w));
        return 1;
    }
    m.vcpu[0] = cosmo_vcpu_create(m.vm, 0);
    if (m.vcpu[0] < 0) {
        fprintf(stderr, "vmctl: vcpu_create: %s\n", strerror(-m.vcpu[0]));
        return 1;
    }
    struct cosmo_vcpu_regs regs;
    cosmo_vcpu_get_regs(m.vcpu[0], &regs);
#if defined(__aarch64__)
    regs.pc = load;
    regs.x[0] = dtb_gpa;   /* the boot protocol: x0 is the device tree, x1..x3 zero */
#endif
    rc = cosmo_vcpu_set_regs(m.vcpu[0], &regs);
    if (rc < 0) {
        fprintf(stderr, "vmctl: vcpu_regs: %s\n", strerror(-rc));
        return 1;
    }
    m.running[0] = 1;
    /* The virtio-blk device: a file, opened read-only, its size in sectors
     * the capacity. Without --disk the transport reports DeviceID 0 and the
     * guest's virtio-mmio driver skips the node. */
    memset(&g_vio, 0, sizeof(g_vio));
    g_vio.vm = m.vm;
    g_vio.disk_fd = -1;
    if (disk != NULL) {
        g_vio.writable = disk_writable;
        g_vio.disk_fd = open(disk, disk_writable ? O_RDWR : O_RDONLY);
        if (g_vio.disk_fd < 0) {
            fprintf(stderr, "vmctl: %s: %s\n", disk, strerror(errno));
            return 1;
        }
        off_t end = lseek(g_vio.disk_fd, 0, SEEK_END);
        g_vio.capacity = end > 0 ? (uint64_t)end / 512u : 0;
    }
    /* The virtio-net device: a loopback wire. Without --net the transport
     * reports DeviceID 0 and the guest's driver skips the node. */
    memset(&g_vnet, 0, sizeof(g_vnet));
    g_vnet.vm = m.vm;
    if (net_loop) {
        g_vnet.present = 1;
        const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x00, 0x00, 0x01 };   /* locally administered */
        memcpy(g_vnet.mac, mac, 6);
    }
    printf("vmctl: %s: %zu bytes at 0x%llx (%s), %lu MiB at 0x%llx, %u cpu(s), device tree %zu bytes at 0x%llx%s%s\n",
           path, len, (unsigned long long)load, has_header ? "Image header" : "flat", mem_mib,
           (unsigned long long)ram_base, nr_cpus, dtb_len, (unsigned long long)dtb_gpa,
           disk ? (disk_writable ? ", virtio-blk /dev/vda (rw)" : ", virtio-blk /dev/vda (ro)") : "",
           net_loop ? ", virtio-net eth0 (loop)" : "");

    /* One thread, every running vCPU in turn, a tick each. A vCPU that
     * asks for an interrupt (WFI) gives up its turn; its next comes round. */
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    unsigned cpu = 0;
    for (;;) {
        /* Finish serving a ring the last notify could not drain in one
         * bounded batch, one batch per turn so the guest's vCPUs keep
         * running between them. When a batch serves nothing more, the ring
         * is drained (or a hostile ring stopped it) and draining ends. */
        if (g_vio.draining && vio_service(&g_vio) <= 0)
            g_vio.draining = 0;
        if (g_vnet.draining && vnet_service(&g_vnet) <= 0)
            g_vnet.draining = 0;
        unsigned nr_running = 0;
        for (unsigned c = 0; c < COSMO_HV_VCPUS_MAX; c++)
            nr_running += m.running[c] ? 1 : 0;
        if (nr_running == 0) {
            printf("vmctl: every cpu is off\n");
            return 0;
        }
        while (!m.running[cpu])
            cpu = (cpu + 1) % COSMO_HV_VCPUS_MAX;
        rc = cosmo_vcpu_run_flags(m.vcpu[cpu], &x, nr_running > 1 ? COSMO_VCPU_RUN_ONE_TICK : 0);
        drain_console(m.vm);
        if (rc < 0) {
            fprintf(stderr, "vmctl: vcpu %u: vcpu_run: %s\n", cpu, strerror(-rc));
            return 1;
        }
        int next = 0;
        switch (x.kind) {
        case COSMO_VM_EXIT_PREEMPTED:
        case COSMO_VM_EXIT_WFI:
            next = 1;
            break;
        case COSMO_VM_EXIT_HYPERCALL:
            if (is_psci(x.hypercall.nr)) {
                if (psci_answer(&m, cpu, &x)) {
                    printf("vmctl: guest powered off\n");
                    return 0;
                }
                /* A firmware call is a natural turn boundary: the vCPU that
                 * just brought a sibling up would otherwise run on -- and
                 * a small guest reaches SYSTEM_OFF within the same tick,
                 * before the sibling has run a single instruction. The
                 * first boot of this mode did exactly that. */
                next = 1;
            } else {
                printf("vmctl: cpu %u: hypercall %llu (0x%llx 0x%llx 0x%llx 0x%llx)\n", cpu,
                       (unsigned long long)x.hypercall.nr, (unsigned long long)x.hypercall.a0,
                       (unsigned long long)x.hypercall.a1, (unsigned long long)x.hypercall.a2,
                       (unsigned long long)x.hypercall.a3);
            }
            break;
        case COSMO_VM_EXIT_MMIO:
            if (x.mmio.gpa >= COSMO_HVM_VIRTIO0_BASE &&
                x.mmio.gpa < COSMO_HVM_VIRTIO0_BASE + COSMO_HVM_VIRTIO0_SIZE) {
                uint64_t val = x.mmio.value;
                vio_reg(&g_vio, (unsigned)(x.mmio.gpa - COSMO_HVM_VIRTIO0_BASE), x.mmio.write, &val);
                if (!x.mmio.write)
                    x.mmio.value = val;   /* the kernel completes the read / steps the write */
                break;   /* run again */
            }
            if (x.mmio.gpa >= COSMO_HVM_VIRTIO1_BASE &&
                x.mmio.gpa < COSMO_HVM_VIRTIO1_BASE + COSMO_HVM_VIRTIO1_SIZE) {
                uint64_t val = x.mmio.value;
                vnet_reg(&g_vnet, (unsigned)(x.mmio.gpa - COSMO_HVM_VIRTIO1_BASE), x.mmio.write, &val);
                if (!x.mmio.write)
                    x.mmio.value = val;
                break;   /* run again */
            }
            printf("vmctl: cpu %u: mmio %s at 0x%llx, %u byte(s), x%u, value 0x%llx, rip 0x%llx: no device; stopping\n",
                   cpu, x.mmio.write ? "write" : "read", (unsigned long long)x.mmio.gpa, x.mmio.size, x.mmio.reg,
                   (unsigned long long)x.mmio.value, (unsigned long long)x.rip);
            return 1;
        case COSMO_VM_EXIT_SYSREG:
            printf("vmctl: cpu %u: system register %s (iss 0x%x) into x%u at 0x%llx: no model; stopping\n", cpu,
                   x.sysreg.write ? "write" : "read", x.sysreg.iss, x.sysreg.reg, (unsigned long long)x.rip);
            return 1;
        case COSMO_VM_EXIT_SHUTDOWN:
            printf("vmctl: cpu %u: guest shutdown at 0x%llx\n", cpu, (unsigned long long)x.rip);
            return 1;
        case COSMO_VM_EXIT_FAIL:
            printf("vmctl: cpu %u: entry failed: code 0x%x info 0x%llx 0x%llx\n", cpu, x.fail.code,
                   (unsigned long long)x.fail.info1, (unsigned long long)x.fail.info2);
            return 1;
        default:
            printf("vmctl: cpu %u: unknown exit %u\n", cpu, x.kind);
            return 1;
        }
        if (next)
            cpu = (cpu + 1) % COSMO_HV_VCPUS_MAX;
    }
}

static int run(int argc, char **argv)
{
    if (argc >= 1 && strcmp(argv[0], "--machine") == 0)
        return run_machine(argc - 1, argv + 1);
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

    size_t len = 0;
    unsigned char *image = read_image(path, &len);
    if (image == NULL)
        return 1;

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
            if (x.mmio.size)
                printf("vmctl: mmio %s at 0x%llx, %u byte(s), x%u, value 0x%llx, rip 0x%llx: no device; stopping\n",
                       x.mmio.write ? "write" : "read", (unsigned long long)x.mmio.gpa, x.mmio.size, x.mmio.reg,
                       (unsigned long long)x.mmio.value, (unsigned long long)x.rip);
            else
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
