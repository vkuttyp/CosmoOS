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
#include <cosmo/thread.h>
#include <stdbool.h>
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
#include <arpa/inet.h>
#include <sys/socket.h>
#include <uapi/cosmo/netctl.h>

static int usage(void)
{
    fprintf(stderr, "usage: vmctl probe | info | run [-m KIB] [-a GPA] [-e ENTRY] IMAGE\n"
                    "       vmctl run --machine [-m MIB] [-c NCPUS] [--disk FILE | --disk-rw FILE] [--net loop | --net tap] "
                    "[--append CMDLINE] IMAGE\n"
                    "       vmctl port-forward add PROTO HOSTPORT GUESTADDR GUESTPORT | del PROTO HOSTPORT | list\n"
                    "       vmctl filter add|del GUESTADDR DIR PROTO DST[/PREFIX] PORT VERDICT [INDEX]\n"
                    "                    | add|del host world|out PROTO SRC[/PREFIX] DST[/PREFIX] PORT VERDICT\n"
                    "                                                 [world|guest|any] [INDEX]\n"
                    "                    | policy GUESTADDR DIR VERDICT | policy host world|out VERDICT | list\n"
                    "         DIR any(=uplink+guest, never host)|uplink|guest|host  PROTO any|icmp|tcp|udp\n"
                    "         SRC (host rules only) addr[/prefix]|any  DST addr[/prefix]|any\n"
                    "         PORT n|any (icmp: type 0-255|echo-request|echo-reply|any)  VERDICT accept|drop\n"
                    "         a trailing world|guest|any on a host `out` rule is the egress it applies to\n");
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
#define PSCI_DENIED           (-3)
#define PSCI_ALREADY_ON       (-4)

/* PSCI function ids: SMCCC fast calls, 32- or 64-bit, in the standard
 * service range 0x84000000..0x8400001F / 0xC4000000..0xC400001F. */
static int is_psci(uint64_t fn)
{
    return (fn & 0xBFFFFFE0ull) == 0x84000000ull;
}

/* --- the virtio-mmio block device (docs/audit/next-subsystem-vblk.md) --- */

struct vio {
    cosmo_mutex_t lock;   /* every entry point takes it; see the rule above vio_service_locked */
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

static void vio_reg_locked(struct vio *v, unsigned off, int write, uint64_t *val);

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
/*
 * The device models' locking rule, stated once because it governs every
 * entry point here and a second threaded program will copy it
 * (docs/audit/next-subsystem-vcpu-threads.md):
 *
 *  - **one mutex per device model**, taken by whoever *enters* the model
 *    from outside -- the MMIO dispatch, the drain, the tap poll -- and not
 *    by the functions those call, which is why each model has a `_locked`
 *    core the way libc's allocator does;
 *  - **the lock is never held across `cosmo_vcpu_run`**, so a guest looping
 *    in MMIO cannot stall another vCPU;
 *  - the model's file descriptor belongs to its lock, so a read and a write
 *    cannot interleave inside one request;
 *  - the console drain belongs to the main thread alone.
 *
 * The locks are here before there is a second thread to contend them. A
 * lock nothing contends is still correct, and landing them first means the
 * thread change that follows is not also a locking change.
 */
static int vio_service_locked(struct vio *v)
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

/* The drain's entry: take the lock, and decide *under* it whether there is
 * anything to drain. Reading `draining` outside would be a race with the
 * vCPU thread that set it. Returns what the core returned, or 0 for "not
 * draining", which the caller treats the same way. */
static int vio_drain(struct vio *v)
{
    cosmo_mutex_lock(&v->lock);
    int served = v->draining ? vio_service_locked(v) : 0;
    if (v->draining && served <= 0)
        v->draining = 0;
    cosmo_mutex_unlock(&v->lock);
    return served;
}

/* The transport registers (virtio-mmio v2). `*val` is the read result.
 * An entry point: it takes the model's lock for the whole access. */
static void vio_reg(struct vio *v, unsigned off, int write, uint64_t *val)
{
    cosmo_mutex_lock(&v->lock);
    vio_reg_locked(v, off, write, val);
    cosmo_mutex_unlock(&v->lock);
}

static void vio_reg_locked(struct vio *v, unsigned off, int write, uint64_t *val)
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
        if (vio_service_locked(v) > 0)
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
    cosmo_mutex_t lock;                   /* every entry point takes it; the rule is above vio_service_locked */
    int vm;
    int present;                          /* --net loop or --net tap given */
    int tap_fd;                           /* >= 0 in --net tap: the /dev/net/tap channel */
    uint32_t feat_sel, status, queue_sel;
    struct vq_queue rq, tq;               /* receive (queue 0), transmit (queue 1) */
    int irq_pending;
    int draining;
    uint8_t mac[6];
    struct { uint8_t buf[VNET_FRAME_MAX]; uint32_t len; } wire[VNET_WIRE_SLOTS];
    unsigned wire_head, wire_count;       /* the loopback FIFO (--net loop) */
};

static struct vnet_dev g_vnet;

static void vnet_reg_locked(struct vnet_dev *d, unsigned off, int write, uint64_t *val);

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
    if (d->tap_fd >= 0) {                            /* --net tap: write the channel */
        ssize_t n = write(d->tap_fd, frame, len);
        return n == (ssize_t)len ? 0 : 0;           /* a short write is a drop, like a full wire */
    }
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
    if (d->tap_fd >= 0) {                            /* --net tap: read the channel (0 = none now) */
        ssize_t n = read(d->tap_fd, buf, max);
        return n > 0 ? (int)n : 0;
    }
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
static int vnet_service_locked(struct vnet_dev *d)
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
/*
 * The drain and the tap poll, both entry points. `draining` is decided
 * under the lock for the same reason the block model's is: the thread that
 * set it is not the thread reading it.
 */
static int vnet_drain(struct vnet_dev *d)
{
    cosmo_mutex_lock(&d->lock);
    int served = d->draining ? vnet_service_locked(d) : 0;
    if (d->draining && served <= 0)
        d->draining = 0;
    cosmo_mutex_unlock(&d->lock);
    return served;
}

/* A tap delivers host->guest frames at any time, not only on a guest kick,
 * so the supervisor polls this; it services both directions. */
static int vnet_poll(struct vnet_dev *d)
{
    cosmo_mutex_lock(&d->lock);
    int served = vnet_service_locked(d);
    cosmo_mutex_unlock(&d->lock);
    return served;
}

/* An entry point: it takes the model's lock for the whole access. */
static void vnet_reg(struct vnet_dev *d, unsigned off, int write, uint64_t *val)
{
    cosmo_mutex_lock(&d->lock);
    vnet_reg_locked(d, off, write, val);
    cosmo_mutex_unlock(&d->lock);
}

static void vnet_reg_locked(struct vnet_dev *d, unsigned off, int write, uint64_t *val)
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
        if (vnet_service_locked(d) > 0)
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

/*
 * The vCPU lifecycle. One word per vCPU, and it only ever increases:
 *
 *   PARKED  created, never started; waiting to be told which guest to run
 *   RUNNING CPU_ON wrote the entry and context, and woke it
 *   QUIT    leave without running: the machine is stopping
 *
 * The monotonicity is enforced, not merely usual. `SYSTEM_OFF` writes QUIT
 * to every word; a *concurrent* `CPU_ON` -- from a vCPU thread that has not
 * been kicked yet, which is the normal state of affairs during shutdown --
 * would otherwise store RUNNING over that QUIT and revive the target, whose
 * thread would then run a guest nobody is waiting to stop. So CPU_ON
 * releases a thread with a compare-and-swap from PARKED and never a store.
 */
#define VCPU_PARKED  0u
#define VCPU_RUNNING 1u
#define VCPU_QUIT    2u

/* 16 KB: these threads run a vCPU and print; they do not recurse. */
#define VCPU_STACK_BYTES (16u * 1024u)

/* How long the supervisor sleeps between drains when nothing exits. Every
 * kernel sleep is at least one scheduler tick, so this is a floor rather
 * than a period; an exiting thread wakes it early. */
#define MACHINE_DRAIN_INTERVAL_NS 2000000ull

struct machine;
struct vcpu_arg {
    struct machine *m;
    unsigned index;
};

struct machine {
    int vm;
    int vcpu[COSMO_HV_VCPUS_MAX];      /* -1: not created */
    unsigned nr_cpus;                  /* what the device tree promises */

    /* The lifecycle. `park[]` is the single source of truth for whether a
     * vCPU is running: PSCI's ALREADY_ON is park[] == RUNNING. */
    volatile unsigned park[COSMO_HV_VCPUS_MAX];
    volatile unsigned live;            /* threads created and not yet returned */
    volatile unsigned stopping;        /* SYSTEM_OFF has begun; CPU_ON refuses */
    volatile unsigned off_asked;       /* a guest asked for the power-off */
    volatile unsigned failed;          /* a thread hit a fatal exit; the process fails */
    volatile unsigned entered[COSMO_HV_VCPUS_MAX];   /* the thread is about to enter its guest */
    uint64_t entry[COSMO_HV_VCPUS_MAX];
    uint64_t ctx[COSMO_HV_VCPUS_MAX];
    cosmo_thread_t thread[COSMO_HV_VCPUS_MAX];
    int started[COSMO_HV_VCPUS_MAX];
    struct vcpu_arg arg[COSMO_HV_VCPUS_MAX];
};

/*
 * Release a parked thread: the entry and context are already written, and
 * the swap publishes them. Release ordering on the swap, acquire on the
 * thread's read, because a thread that could see RUNNING and then a stale
 * entry point would enter its guest at whatever the word last held.
 *
 * Returns 0, or -EBUSY if it was already running, or -EPERM once QUIT is
 * set -- a guest asking for a CPU while the machine powers off.
 */
static int machine_release(struct machine *m, unsigned c)
{
    unsigned want = VCPU_PARKED;
    if (!__atomic_compare_exchange_n(&m->park[c], &want, VCPU_RUNNING, false,
                                     __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
        return want == VCPU_RUNNING ? -EBUSY : -EPERM;
    (void)cosmo_futex_wake(&m->park[c], 1);
    /*
     * And wait for it to be running before answering, which is what PSCI
     * means: a CPU_ON that returns SUCCESS says the target is powered on
     * and executing at the entry point, not that it has been queued.
     *
     * On hardware that is free -- the core starts on its own -- and here it
     * is the difference between a secondary that runs and one that does
     * not. A guest that starts a CPU and powers the machine off two
     * instructions later (`guest_dtb.c` does exactly that) would otherwise
     * race the host scheduler for its secondary's first instruction, which
     * is the race the round-robin's `fresh[]` array and its 64-turn grace
     * existed to paper over. Making CPU_ON honest removes the need for
     * both, rather than replacing one correction with another.
     *
     * Bounded, and a timeout is still SUCCESS: the vCPU *is* on, and a host
     * that cannot schedule a thread in a fifth of a second has a problem
     * this firmware call cannot fix.
     */
    uint64_t deadline = cosmo_clock_ns() + 200000000ull;
    while (__atomic_load_n(&m->entered[c], __ATOMIC_ACQUIRE) == 0) {
        uint64_t now = cosmo_clock_ns();
        if (now >= deadline)
            break;
        (void)cosmo_futex_wait(&m->entered[c], 0, deadline - now);
    }
    return 0;
}

/*
 * Tell every vCPU thread to leave, and make the ones inside a guest notice.
 * Both halves are needed and neither is sufficient: the kick reaches a
 * thread inside `cosmo_vcpu_run` and nothing else, while the QUIT and the
 * wake reach one still parked on its futex -- which a secondary that was
 * never CPU_ON'd will be forever. Without the wake the live count never
 * reaches zero and the supervisor waits for a shutdown that cannot happen.
 *
 * `stopping` is set first, so a CPU_ON that has not yet reached its swap
 * refuses early; the swap is what makes the race safe whichever order the
 * two land in.
 */
static void machine_quit_all(struct machine *m)
{
    __atomic_store_n(&m->stopping, 1u, __ATOMIC_RELEASE);
    for (unsigned c = 0; c < COSMO_HV_VCPUS_MAX; c++) {
        __atomic_store_n(&m->park[c], VCPU_QUIT, __ATOMIC_RELEASE);
        (void)cosmo_futex_wake(&m->park[c], 1);
        if (m->vcpu[c] >= 0)
            (void)cosmo_vcpu_stop(m->vcpu[c]);
    }
}

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
        /* Refuse early once the machine is stopping; the swap below is
         * what makes the race safe whichever order the two land in. */
        if (__atomic_load_n(&m->stopping, __ATOMIC_ACQUIRE)) {
            ret = PSCI_DENIED;
            break;
        }
        /* The vCPU and its thread both exist already, parked: what CPU_ON
         * does now is write the entry and release the thread. Neither can
         * fail for want of memory, which is why they are made up front. */
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
        int r = machine_release(m, target);
        ret = r == 0 ? PSCI_SUCCESS : (r == -EBUSY ? PSCI_ALREADY_ON : PSCI_DENIED);
        break;
    }
    case PSCI_CPU_OFF:
        /* This vCPU's own thread is the caller; it leaves when this returns.
         * The word stays RUNNING -- monotonic -- and the thread's exit is
         * what the live count sees. */
        ret = PSCI_SUCCESS;
        off = 2;   /* this vCPU only */
        break;
    case PSCI_AFFINITY_INFO: {
        unsigned target = (unsigned)(x->hypercall.a0 & 0xFFu);
        ret = target < COSMO_HV_VCPUS_MAX &&
              __atomic_load_n(&m->park[target], __ATOMIC_ACQUIRE) == VCPU_RUNNING ? 0 : 1;   /* ON : OFF */
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

/*
 * One vCPU's thread: wait to be told which guest to run, run it until the
 * machine stops, then leave. Every exit the old round-robin handled is
 * handled here, minus the turn-taking -- there are no turns now.
 */
static void *vcpu_thread(void *arg)
{
    struct vcpu_arg *va = arg;
    struct machine *m = va->m;
    unsigned cpu = va->index;
    struct cosmo_vm_exit x;
    int status = 0;

    /* Parked until CPU_ON releases this vCPU, or shutdown tells it to go.
     * The acquire is what makes the entry point and context the releasing
     * thread wrote visible here. */
    for (;;) {
        unsigned st = __atomic_load_n(&m->park[cpu], __ATOMIC_ACQUIRE);
        if (st != VCPU_PARKED)
            break;
        (void)cosmo_futex_wait(&m->park[cpu], VCPU_PARKED, 0);
    }

    /* Tell whoever released this vCPU that it is running: CPU_ON waits for
     * this, so that it can promise what PSCI says it promises. */
    __atomic_store_n(&m->entered[cpu], 1u, __ATOMIC_RELEASE);
    (void)cosmo_futex_wake(&m->entered[cpu], 1);

    while (__atomic_load_n(&m->park[cpu], __ATOMIC_ACQUIRE) == VCPU_RUNNING) {
        memset(&x, 0, sizeof(x));
        /* Untimed: nothing else needs this thread, and the kick is what
         * ends a run that the guest will not end itself. */
        int rc = cosmo_vcpu_run(m->vcpu[cpu], &x);
        if (rc < 0) {
            fprintf(stderr, "vmctl: vcpu %u: vcpu_run: %s\n", cpu, strerror(-rc));
            status = 1;
            break;
        }
        int leave = 0;
        switch (x.kind) {
        case COSMO_VM_EXIT_STOPPED:
            /* The owner asked this vCPU to leave: the loop's own condition
             * decides whether that was the machine stopping. */
            break;
        case COSMO_VM_EXIT_PREEMPTED:
        case COSMO_VM_EXIT_WFI:
            break;   /* run again; a WFI's wake is an interrupt the kernel delivers */
        case COSMO_VM_EXIT_HYPERCALL:
            if (is_psci(x.hypercall.nr)) {
                int off = psci_answer(m, cpu, &x);
                if (off == 1) {
                    /* SYSTEM_OFF: the machine stops. Every other thread is
                     * told to quit and kicked; this one leaves here, because
                     * its guest must not run past the call. */
                    __atomic_store_n(&m->off_asked, 1u, __ATOMIC_RELEASE);
                    machine_quit_all(m);
                    leave = 1;
                } else if (off == 2) {
                    leave = 1;   /* CPU_OFF: this vCPU only */
                }
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
                break;
            }
            if (x.mmio.gpa >= COSMO_HVM_VIRTIO1_BASE &&
                x.mmio.gpa < COSMO_HVM_VIRTIO1_BASE + COSMO_HVM_VIRTIO1_SIZE) {
                uint64_t val = x.mmio.value;
                vnet_reg(&g_vnet, (unsigned)(x.mmio.gpa - COSMO_HVM_VIRTIO1_BASE), x.mmio.write, &val);
                if (!x.mmio.write)
                    x.mmio.value = val;
                break;
            }
            printf("vmctl: cpu %u: mmio %s at 0x%llx, %u byte(s), x%u, value 0x%llx, rip 0x%llx: no device; stopping\n",
                   cpu, x.mmio.write ? "write" : "read", (unsigned long long)x.mmio.gpa, x.mmio.size, x.mmio.reg,
                   (unsigned long long)x.mmio.value, (unsigned long long)x.rip);
            status = 1;
            leave = 1;
            break;
        case COSMO_VM_EXIT_SYSREG:
            printf("vmctl: cpu %u: system register %s (iss 0x%x) into x%u at 0x%llx: no model; stopping\n", cpu,
                   x.sysreg.write ? "write" : "read", x.sysreg.iss, x.sysreg.reg, (unsigned long long)x.rip);
            status = 1;
            leave = 1;
            break;
        case COSMO_VM_EXIT_SHUTDOWN:
            printf("vmctl: cpu %u: guest shutdown at 0x%llx\n", cpu, (unsigned long long)x.rip);
            status = 1;
            leave = 1;
            break;
        case COSMO_VM_EXIT_FAIL:
            printf("vmctl: cpu %u: entry failed: code 0x%x info 0x%llx 0x%llx\n", cpu, x.fail.code,
                   (unsigned long long)x.fail.info1, (unsigned long long)x.fail.info2);
            status = 1;
            leave = 1;
            break;
        default:
            printf("vmctl: cpu %u: unknown exit %u\n", cpu, x.kind);
            status = 1;
            leave = 1;
            break;
        }
        if (leave)
            break;
    }

    /*
     * A vCPU that failed takes the machine with it: the others have no way
     * to know, and a half-stopped machine would hang the supervisor.
     */
    if (status) {
        __atomic_store_n(&m->failed, 1u, __ATOMIC_RELEASE);
        machine_quit_all(m);
    }
    /* Release, and the wake last: everything this thread did -- its console
     * bytes, its exit reason -- must be visible to the supervisor that sees
     * the count reach zero, and a joiner woken before the count dropped
     * would see a machine that is not finished. */
    __atomic_fetch_sub(&m->live, 1u, __ATOMIC_ACQ_REL);
    (void)cosmo_futex_wake(&m->live, 1);
    return NULL;
}

static int run_machine(int argc, char **argv)
{
    unsigned long mem_mib = 16;
    unsigned nr_cpus = 1;
    const char *bootargs = NULL, *disk = NULL;
    int disk_writable = 0;
    const char *net = NULL;   /* "loop" or "tap" */
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
            if (strcmp(argv[i + 1], "loop") != 0 && strcmp(argv[i + 1], "tap") != 0)
                return usage();   /* the loopback wire, or the host tap */
            net = argv[i + 1];
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
    /*
     * Every vCPU the device tree promises, created now rather than when
     * `CPU_ON` asks for one. That is what lets `CPU_ON` be a register write
     * and a wake, with nothing in it that can fail for want of memory --
     * and it means a machine that cannot have its CPUs says so here, before
     * the guest has run an instruction, instead of from inside a firmware
     * call the guest has no good answer for.
     */
    for (unsigned c = 0; c < m.nr_cpus; c++) {
        m.vcpu[c] = cosmo_vcpu_create(m.vm, c);
        if (m.vcpu[c] < 0) {
            fprintf(stderr, "vmctl: vcpu_create(%u): %s\n", c, strerror(-m.vcpu[c]));
            return 1;
        }
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
    /* The virtio-net device: a loopback wire (--net loop) or the host stack
     * through /dev/net/tap (--net tap). Without --net the transport reports
     * DeviceID 0 and the guest's driver skips the node. */
    memset(&g_vnet, 0, sizeof(g_vnet));
    g_vnet.vm = m.vm;
    g_vnet.tap_fd = -1;
    if (net != NULL) {
        g_vnet.present = 1;
        const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x00, 0x00, 0x01 };   /* locally administered */
        memcpy(g_vnet.mac, mac, 6);
        if (strcmp(net, "tap") == 0) {
            g_vnet.tap_fd = open("/dev/net/tap", O_RDWR);
            if (g_vnet.tap_fd < 0) {
                fprintf(stderr, "vmctl: /dev/net/tap: %s\n", strerror(errno));
                return 1;
            }
        }
    }
    printf("vmctl: %s: %zu bytes at 0x%llx (%s), %lu MiB at 0x%llx, %u cpu(s), device tree %zu bytes at 0x%llx%s%s\n",
           path, len, (unsigned long long)load, has_header ? "Image header" : "flat", mem_mib,
           (unsigned long long)ram_base, nr_cpus, dtb_len, (unsigned long long)dtb_gpa,
           disk ? (disk_writable ? ", virtio-blk /dev/vda (rw)" : ", virtio-blk /dev/vda (ro)") : "",
           net ? (g_vnet.tap_fd >= 0 ? ", virtio-net eth0 (tap)" : ", virtio-net eth0 (loop)") : "");

    /*
     * A thread per vCPU. The supervisor -- this thread -- drains the
     * guest's console and services the device models while they run, and
     * reaps them when the last one has left.
     *
     * This replaces a round-robin that ran every vCPU on one thread a tick
     * each, and with it go the two corrections that existed only because of
     * that serialisation: the `fresh[]` array that tracked which vCPU had
     * never had a turn end on its own terms, and the `SYSTEM_OFF` held for
     * such a sibling for up to 64 turns. A vCPU now runs because it is
     * running, not because a turn came round, so `SYSTEM_OFF` means what
     * PSCI says it means.
     */
    m.live = m.nr_cpus;
    for (unsigned c = 0; c < m.nr_cpus; c++) {
        m.arg[c].m = &m;
        m.arg[c].index = c;
        /*
         * Created before the guest runs and parked: PSCI secondaries start
         * powered off, so a thread per *running* vCPU would mean `CPU_ON`
         * creating one from inside its own handler, on the asking vCPU's
         * thread, where a failed create would have to become a PSCI error
         * code. This way a create that fails is a startup failure, reported
         * here, and `CPU_ON` cannot fail for want of memory.
         */
        rc = cosmo_thread_start(&m.thread[c], vcpu_thread, &m.arg[c], VCPU_STACK_BYTES);
        if (rc != 0) {
            fprintf(stderr, "vmctl: cpu %u: thread: %s\n", c, strerror(-rc));
            /* Whatever started must be told to leave, or this process
             * cannot exit: the threads are parked on words only this
             * function writes. */
            m.live = c;
            machine_quit_all(&m);
            for (unsigned j = 0; j < c; j++)
                (void)cosmo_thread_join(&m.thread[j], NULL);
            return 1;
        }
        m.started[c] = 1;
    }

    /* vCPU 0 is the one the guest starts with; the rest wait for CPU_ON. */
    machine_release(&m, 0);

    /*
     * The supervisor loop. `cosmo_thread_join` blocks until its thread
     * exits, so a main thread that joined first would drain nothing -- and
     * the guest's console ring drops its *oldest* bytes when full, which is
     * output silently lost and the harness's markers are made of exactly
     * that output. So: drain, service, and wait on the live count with a
     * bounded wait; the joins happen after it reaches zero, where they reap
     * rather than wait.
     *
     * The bound is what keeps the ring drained while nothing exits; the
     * wake an exiting thread sends is what stops the last drain from being
     * a whole interval late. Every kernel sleep is at least one scheduler
     * tick, so the real interval is a tick either way.
     */
    for (;;) {
        drain_console(m.vm);
        (void)vio_drain(&g_vio);
        (void)vnet_drain(&g_vnet);
        /* A tap delivers host->guest frames at any time, not only on a
         * guest kick, so poll it here rather than in a vCPU thread. */
        if (g_vnet.tap_fd >= 0)
            (void)vnet_poll(&g_vnet);
        unsigned live = __atomic_load_n(&m.live, __ATOMIC_ACQUIRE);
        if (live == 0)
            break;
        (void)cosmo_futex_wait(&m.live, live, MACHINE_DRAIN_INTERVAL_NS);
    }
    for (unsigned c = 0; c < COSMO_HV_VCPUS_MAX; c++)
        if (m.started[c])
            (void)cosmo_thread_join(&m.thread[c], NULL);
    drain_console(m.vm);   /* whatever the last thread wrote on its way out */

    /*
     * The verdict. A held power-off used to be printed by the loop that
     * honoured it; now the guest's request and the machine's stopping are
     * two different threads' work, so the message belongs here -- once,
     * however many vCPUs asked.
     */
    if (m.failed)
        return 1;
    printf(m.off_asked ? "vmctl: guest powered off\n" : "vmctl: every cpu is off\n");
    return 0;
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

static int pf_proto(const char *s, uint8_t *out)
{
    if (strcmp(s, "tcp") == 0) { *out = COSMO_NETCTL_PROTO_TCP; return 0; }
    if (strcmp(s, "udp") == 0) { *out = COSMO_NETCTL_PROTO_UDP; return 0; }
    return -1;
}

/* Parse a port in [1,65535]; rejects trailing junk, overflow and out-of-range
 * rather than silently wrapping to uint16_t. Returns -1 on a bad argument. */
static int pf_port(const char *s, uint16_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 65535)
        return -1;
    *out = (uint16_t)v;
    return 0;
}

/* --- the forwarding firewall (docs/audit/next-subsystem-firewall.md) ----- */

static int fw_dir(const char *s, uint8_t *out)
{
    if (strcmp(s, "any") == 0)         *out = COSMO_NETCTL_DIR_ANY;
    else if (strcmp(s, "uplink") == 0) *out = COSMO_NETCTL_DIR_TO_UPLINK;
    else if (strcmp(s, "guest") == 0)  *out = COSMO_NETCTL_DIR_TO_GUEST;
    else if (strcmp(s, "host") == 0)   *out = COSMO_NETCTL_DIR_TO_HOST;
    else if (strcmp(s, "world") == 0)  *out = COSMO_NETCTL_DIR_FROM_UPLINK;   /* the host chain: the world -> the host */
    else if (strcmp(s, "out") == 0)    *out = COSMO_NETCTL_DIR_OUTPUT;       /* the OUTPUT chain: the host -> anywhere */
    else return -1;
    return 0;
}

/* The egress an OUTPUT rule applies to. Not positional: it is recognised by
 * name wherever it appears among the optional trailing words, so
 * `... drop guest 0` and `... drop 0 guest` both read. */
static int fw_scope(const char *s, uint8_t *out)
{
    if (strcmp(s, "world") == 0)      *out = COSMO_NETCTL_SCOPE_WORLD;
    else if (strcmp(s, "guest") == 0) *out = COSMO_NETCTL_SCOPE_GUEST;
    else if (strcmp(s, "any") == 0)   *out = COSMO_NETCTL_SCOPE_ANY;
    else return -1;
    return 0;
}

/* The policy object a filter command names: a guest by address, or the host
 * itself by the word `host` (COSMO_NETCTL_HOST_ADDR -- the one address no
 * guest can have). */
static int fw_guest(const char *s, uint32_t *out)
{
    if (strcmp(s, "host") == 0) { *out = COSMO_NETCTL_HOST_ADDR; return 0; }
    return inet_pton(AF_INET, s, out) == 1 ? 0 : -1;
}

static int fw_proto(const char *s, uint8_t *out)
{
    if (strcmp(s, "any") == 0)       *out = COSMO_NETCTL_PROTO_ANY;
    else if (strcmp(s, "icmp") == 0) *out = COSMO_NETCTL_PROTO_ICMP;
    else if (strcmp(s, "tcp") == 0)  *out = COSMO_NETCTL_PROTO_TCP;
    else if (strcmp(s, "udp") == 0)  *out = COSMO_NETCTL_PROTO_UDP;
    else return -1;
    return 0;
}

static int fw_verdict(const char *s, uint8_t *out)
{
    if (strcmp(s, "accept") == 0)    *out = COSMO_NETCTL_VERDICT_ACCEPT;
    else if (strcmp(s, "drop") == 0) *out = COSMO_NETCTL_VERDICT_DROP;
    else return -1;
    return 0;
}

/* "any" -> 0/0; "a.b.c.d" -> /32; "a.b.c.d/n" -> /n. */
static int fw_dst(const char *s, uint32_t *addr, uint8_t *prefix)
{
    if (strcmp(s, "any") == 0) { *addr = 0; *prefix = 0; return 0; }
    char buf[32];
    if (strlen(s) >= sizeof(buf)) return -1;
    strcpy(buf, s);
    char *slash = strchr(buf, '/');
    unsigned p = 32;
    if (slash) {
        *slash = 0;
        char *end;
        unsigned long v = strtoul(slash + 1, &end, 10);
        if (*end || v > 32) return -1;
        p = (unsigned)v;
    }
    if (inet_pton(AF_INET, buf, addr) != 1) return -1;
    *prefix = (uint8_t)p;
    return 0;
}

static const char *fw_dir_name(uint8_t d)
{
    return d == COSMO_NETCTL_DIR_TO_UPLINK ? "uplink" : d == COSMO_NETCTL_DIR_TO_GUEST ? "guest" :
           d == COSMO_NETCTL_DIR_TO_HOST ? "host" : d == COSMO_NETCTL_DIR_FROM_UPLINK ? "world" :
           d == COSMO_NETCTL_DIR_OUTPUT ? "out" : "any";
}
static const char *fw_proto_name(uint8_t p)
{
    return p == COSMO_NETCTL_PROTO_TCP ? "tcp" : p == COSMO_NETCTL_PROTO_UDP ? "udp" :
           p == COSMO_NETCTL_PROTO_ICMP ? "icmp" : "any";
}
static const char *fw_verdict_name(uint8_t v)
{
    return v == COSMO_NETCTL_VERDICT_ACCEPT ? "accept" : "drop";
}

/* vmctl filter add    GUESTADDR DIR PROTO DST[/PREFIX] PORT VERDICT [INDEX]
 *              add    host world PROTO SRC[/PREFIX] DST[/PREFIX] PORT VERDICT [INDEX]
 *              del    (the same tuple as add)
 *              policy GUESTADDR DIR VERDICT | policy host world VERDICT
 *              list
 * A rule is named by its whole tuple (there is no id to return through a
 * write); `del` gives the tuple `add` installed. The host's rules (`host`,
 * direction `world`: what the uplink may ask of the host) are the only ones
 * with a SRC argument -- a guest's source is the guest, and its command
 * writes 0/0 there. Over /dev/net/tapctl. */
static int filter(int argc, char **argv)
{
    if (argc < 1)
        return usage();
    int fd = open("/dev/net/tapctl", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "vmctl: cannot open /dev/net/tapctl: %s\n", strerror(errno));
        return 1;
    }
    int rc = 1;
    const char *verb = argv[0];
    if (strcmp(argv[0], "list") == 0) {
        /* Sized to the ABI's maximum snapshot, so every configuration the
         * control plane lets an operator install can also be listed. */
        static unsigned char buf[COSMO_NETCTL_SNAPSHOT_MAX];
        int64_t n = read(fd, buf, sizeof(buf));
        if (n < (int64_t)sizeof(struct cosmo_netctl_list)) {
            fprintf(stderr, "vmctl: list failed: %s\n", strerror(errno));
            goto out;
        }
        struct cosmo_netctl_list ph;
        memcpy(&ph, buf, sizeof(ph));
        /* Walk only a snapshot of the version this vmctl speaks: the record
         * layouts and the ICMP selector's meaning are version-bound, and a
         * misread listing is worse than none. */
        if (ph.version != COSMO_NETCTL_VERSION) {
            fprintf(stderr, "vmctl: snapshot version %u, this vmctl speaks %u\n", ph.version, COSMO_NETCTL_VERSION);
            goto out;
        }
        size_t off = sizeof(ph) + (size_t)ph.count * sizeof(struct cosmo_netctl_rule);
        struct cosmo_netctl_filter_list fh;
        if ((size_t)n < off + sizeof(fh)) {
            fprintf(stderr, "vmctl: snapshot carries no filter section\n");
            goto out;
        }
        memcpy(&fh, buf + off, sizeof(fh));
        if (fh.version != COSMO_NETCTL_VERSION) {
            fprintf(stderr, "vmctl: filter section version %u, this vmctl speaks %u\n", fh.version, COSMO_NETCTL_VERSION);
            goto out;
        }
        off += sizeof(fh);
        for (unsigned i = 0; i < fh.guest_count; i++, off += sizeof(struct cosmo_netctl_filter_guest)) {
            struct cosmo_netctl_filter_guest fg;
            memcpy(&fg, buf + off, sizeof(fg));
            if (fg.guest_addr == COSMO_NETCTL_HOST_ADDR) {   /* the host's record: its own two directions */
                printf("host policy: world %s, out %s\n", fw_verdict_name(fg.policy_from_uplink),
                       fw_verdict_name(fg.policy_output));
                continue;
            }
            char ip[16];
            inet_ntop(AF_INET, &fg.guest_addr, ip, sizeof(ip));
            printf("%s policy: uplink %s, guest %s, host %s\n", ip, fw_verdict_name(fg.policy_to_uplink),
                   fw_verdict_name(fg.policy_to_guest), fw_verdict_name(fg.policy_to_host));
        }
        for (unsigned i = 0; i < fh.rule_count; i++, off += sizeof(struct cosmo_netctl_filter_rule)) {
            struct cosmo_netctl_filter_rule fr;
            memcpy(&fr, buf + off, sizeof(fr));
            char ip[16], dst[16], src[16], from[40] = "";
            if (fr.guest_addr == COSMO_NETCTL_HOST_ADDR)
                strcpy(ip, "host");
            else
                inet_ntop(AF_INET, &fr.guest_addr, ip, sizeof(ip));
            inet_ntop(AF_INET, &fr.dst_addr, dst, sizeof(dst));
            if (fr.guest_addr == COSMO_NETCTL_HOST_ADDR) {   /* only a host rule has a source */
                inet_ntop(AF_INET, &fr.src_addr, src, sizeof(src));
                snprintf(from, sizeof(from), "from %s/%u ", fr.src_prefix ? src : "any", fr.src_prefix);
            }
            /* An OUTPUT rule's egress scope, printed only where it means
             * something (the ABI keeps it ANY everywhere else). */
            const char *scope = "";
            if (fr.direction == COSMO_NETCTL_DIR_OUTPUT)
                scope = fr.scope == COSMO_NETCTL_SCOPE_WORLD ? " world" :
                        fr.scope == COSMO_NETCTL_SCOPE_GUEST ? " guest" : " any";
            /* The selector prints in its protocol's terms: a port, or an ICMP type. */
            char sel[16];
            if (fr.proto == COSMO_NETCTL_PROTO_ICMP) {
                if (fr.dst_port == COSMO_NETCTL_ICMP_TYPE_ANY) strcpy(sel, "any");
                else if (fr.dst_port == 8)                     strcpy(sel, "echo-request");
                else if (fr.dst_port == 0)                     strcpy(sel, "echo-reply");
                else                                           snprintf(sel, sizeof(sel), "type%u", fr.dst_port);
            } else if (fr.dst_port == 0) {
                strcpy(sel, "any");
            } else {
                snprintf(sel, sizeof(sel), "%u", fr.dst_port);
            }
            printf("%s [%u] %s %s %s%s/%u %s %s%s\n", ip, fr.index, fw_dir_name(fr.direction),
                   fw_proto_name(fr.proto), from, fr.dst_prefix ? dst : "any", fr.dst_prefix, sel,
                   fw_verdict_name(fr.verdict), scope);
        }
        rc = 0;
        goto out;
    }

    struct cosmo_netctl_filter c;
    memset(&c, 0, sizeof(c));
    c.version = COSMO_NETCTL_VERSION;
    if (strcmp(argv[0], "policy") == 0) {
        if (argc != 4 || fw_guest(argv[1], &c.guest_addr) != 0 || fw_dir(argv[2], &c.direction) != 0 ||
            c.direction == COSMO_NETCTL_DIR_ANY || fw_verdict(argv[3], &c.verdict) != 0) {
            usage(); goto out;
        }
        c.op = COSMO_NETCTL_FILTER_POLICY;
    } else if (strcmp(argv[0], "add") == 0 || strcmp(argv[0], "del") == 0) {
        int add = argv[0][0] == 'a';
        if (argc < 2 || fw_guest(argv[1], &c.guest_addr) != 0) { usage(); goto out; }
        /* A host rule carries one more argument, its source, before the
         * destination; the rest of the tuple is in the same order after it. */
        int host = c.guest_addr == COSMO_NETCTL_HOST_ADDR;
        int tuple = host ? 8 : 7;
        if (argc < tuple) { usage(); goto out; }
        if (host && fw_dst(argv[4], &c.src_addr, &c.src_prefix) != 0) { usage(); goto out; }
        char **rest = argv + (host ? 1 : 0);   /* rest[4] = DST, rest[5] = PORT, rest[6] = VERDICT */
        if (fw_dir(argv[2], &c.direction) != 0 ||
            fw_proto(argv[3], &c.proto) != 0 || fw_dst(rest[4], &c.dst_addr, &c.dst_prefix) != 0 ||
            fw_verdict(rest[6], &c.verdict) != 0) {
            usage(); goto out;
        }
        /* What may follow the tuple: an egress scope (a host `out` rule
         * only) and, for `add`, an insert index -- in either order, each
         * recognised by what it looks like rather than by its position, and
         * each at most once. A repeated word is refused rather than letting
         * the last one win: `guest world` would otherwise install a
         * world-scoped rule, and `0 5` insert at 5, neither of which is what
         * the command says. */
        int got_scope = 0, got_index = 0;
        for (int i = tuple; i < argc; i++) {
            uint8_t sc;
            if (fw_scope(argv[i], &sc) == 0) {
                if (!host || c.direction != COSMO_NETCTL_DIR_OUTPUT || got_scope) { usage(); goto out; }
                c.scope = sc;
                got_scope = 1;
                continue;
            }
            char *end;
            unsigned long v = strtoul(argv[i], &end, 10);
            if (!add || got_index || *argv[i] == 0 || *end || v > 0xffff) { usage(); goto out; }
            c.at_index = (uint16_t)v;
            got_index = 1;
        }
        argv = rest;   /* the selector reads from the same position for both shapes */
        /* The transport selector follows the protocol: a port for tcp/udp/any,
         * an ICMP type (a number, echo-request or echo-reply) for icmp; `any`
         * is the wildcard in the protocol's own encoding -- 0 for a port,
         * ICMP_TYPE_ANY for a type (type 0 is echo-reply, so 0 cannot mean any). */
        if (c.proto == COSMO_NETCTL_PROTO_ICMP) {
            if (strcmp(argv[5], "any") == 0)               c.dst_port = COSMO_NETCTL_ICMP_TYPE_ANY;
            else if (strcmp(argv[5], "echo-request") == 0) c.dst_port = 8;
            else if (strcmp(argv[5], "echo-reply") == 0)   c.dst_port = 0;
            else {
                char *end;
                unsigned long v = strtoul(argv[5], &end, 10);
                if (*argv[5] == 0 || *end || v > 255) { fprintf(stderr, "vmctl: bad ICMP type\n"); goto out; }
                c.dst_port = (uint16_t)v;
            }
        } else if (strcmp(argv[5], "any") != 0 && pf_port(argv[5], &c.dst_port) != 0) {
            fprintf(stderr, "vmctl: bad port\n"); goto out;
        }
        c.op = add ? COSMO_NETCTL_FILTER_ADD : COSMO_NETCTL_FILTER_DEL;
    } else {
        usage();
        goto out;
    }
    if (write(fd, &c, sizeof(c)) != (int64_t)sizeof(c)) {
        fprintf(stderr, "vmctl: filter %s failed: %s\n", verb, strerror(errno));
        goto out;
    }
    rc = 0;
out:
    close(fd);
    return rc;
}

/* vmctl port-forward add PROTO HOSTPORT GUESTADDR GUESTPORT
 *                   del PROTO HOSTPORT
 *                   list
 * Configures the guest's inbound port-forwards at runtime through
 * /dev/net/tapctl (docs/audit/next-subsystem-netctl.md). */
static int port_forward(int argc, char **argv)
{
    if (argc < 1)
        return usage();
    int fd = open("/dev/net/tapctl", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "vmctl: cannot open /dev/net/tapctl: %s\n", strerror(errno));
        return 1;
    }
    int rc = 1;
    if (strcmp(argv[0], "list") == 0) {
        /* The read returns the whole snapshot -- the port-forward list and
         * the filter section behind it -- or refuses a short buffer, so this
         * must hold every configuration the ABI allows, as `filter list` does. */
        static unsigned char buf[COSMO_NETCTL_SNAPSHOT_MAX];
        int64_t n = read(fd, buf, sizeof(buf));
        if (n < (int64_t)sizeof(struct cosmo_netctl_list)) {
            fprintf(stderr, "vmctl: list failed: %s\n", strerror(errno));
            goto out;
        }
        struct cosmo_netctl_list *h = (struct cosmo_netctl_list *)buf;
        if (h->version != COSMO_NETCTL_VERSION) {
            fprintf(stderr, "vmctl: snapshot version %u, this vmctl speaks %u\n", h->version, COSMO_NETCTL_VERSION);
            goto out;
        }
        struct cosmo_netctl_rule *r = (struct cosmo_netctl_rule *)(buf + sizeof(*h));
        for (unsigned i = 0; i < h->count; i++) {
            uint32_t a = r[i].guest_addr;
            char ip[16];
            inet_ntop(AF_INET, &a, ip, sizeof(ip));
            printf("%s %u -> %s:%u\n", r[i].proto == COSMO_NETCTL_PROTO_TCP ? "tcp" : "udp",
                   r[i].host_port, ip, r[i].guest_port);
        }
        rc = 0;
        goto out;
    }

    struct cosmo_netctl cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = COSMO_NETCTL_VERSION;
    if (strcmp(argv[0], "add") == 0) {
        if (argc != 5 || pf_proto(argv[1], &cmd.proto) != 0) { usage(); goto out; }
        cmd.op = COSMO_NETCTL_FORWARD_ADD;
        uint32_t a;
        if (pf_port(argv[2], &cmd.host_port) != 0 || pf_port(argv[4], &cmd.guest_port) != 0) {
            fprintf(stderr, "vmctl: bad port\n"); goto out;
        }
        if (inet_pton(AF_INET, argv[3], &a) != 1) { fprintf(stderr, "vmctl: bad guest address\n"); goto out; }
        cmd.guest_addr = a;
    } else if (strcmp(argv[0], "del") == 0) {
        if (argc != 3 || pf_proto(argv[1], &cmd.proto) != 0) { usage(); goto out; }
        cmd.op = COSMO_NETCTL_FORWARD_DEL;
        if (pf_port(argv[2], &cmd.host_port) != 0) { fprintf(stderr, "vmctl: bad port\n"); goto out; }
    } else {
        usage();
        goto out;
    }
    if (write(fd, &cmd, sizeof(cmd)) != (int64_t)sizeof(cmd)) {
        fprintf(stderr, "vmctl: port-forward %s failed: %s\n", argv[0], strerror(errno));
        goto out;
    }
    rc = 0;
out:
    close(fd);
    return rc;
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
    if (strcmp(argv[1], "port-forward") == 0)
        return port_forward(argc - 2, argv + 2);
    if (strcmp(argv[1], "filter") == 0)
        return filter(argc - 2, argv + 2);
    return usage();
}
