/*
 * virtio_gpu.c - virtio GPU device, 2D only (VirtIO 1.2 section 5.7): the
 * screen console at the size the display asks for. Module `virtio_gpu`,
 * depends on `virtio`. See docs/drivers/virtio/design.md, "virtio-gpu".
 *
 * One scanout (0), one host resource in B8G8R8X8, backed by guest memory
 * in chunks of at most 4 MiB (the page allocator's largest block) that
 * vm_map_direct_ranges maps back to back, so fbcon draws one linear frame
 * (fbcon_attach). The device shows that memory only when told: the
 * `vgpu` thread takes the band fbcon drew (fbcon_take_damage) every
 * VGPU_TICK_MS and sends TRANSFER_TO_HOST_2D and RESOURCE_FLUSH for it.
 * The same thread watches the configuration's events_read for a display
 * change (Parallels resizes the scanout with its window) and moves the
 * console to a frame of the new size. In panic mode fbcon calls
 * vgpu_panic_flush, which sends the two commands and polls for them.
 *
 * Commands go one at a time on controlq (queue 0), under `cmd_lock`, each
 * waiting for its response. cursorq (queue 1) is never set up: this
 * driver draws no cursor, and Parallels grants two MSI-X vectors, the
 * configuration's and one queue's.
 */

#include <kernel/completion.h>
#include <kernel/console.h>
#include <kernel/errno.h>
#include <kernel/fbcon.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/mutex.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/vmm.h>
#include <kernel/wait.h>

#include <drivers/virtio.h>

/* Commands and responses (spec 5.7.6). */
#define VGPU_CMD_GET_DISPLAY_INFO        0x0100u
#define VGPU_CMD_RESOURCE_CREATE_2D      0x0101u
#define VGPU_CMD_RESOURCE_UNREF          0x0102u
#define VGPU_CMD_SET_SCANOUT             0x0103u
#define VGPU_CMD_RESOURCE_FLUSH          0x0104u
#define VGPU_CMD_TRANSFER_TO_HOST_2D     0x0105u
#define VGPU_CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define VGPU_CMD_RESOURCE_DETACH_BACKING 0x0107u
#define VGPU_RESP_OK_NODATA              0x1100u
#define VGPU_RESP_OK_DISPLAY_INFO        0x1101u

#define VGPU_FORMAT_B8G8R8X8 2u   /* bytes B, G, R, X: a little-endian 0x00RRGGBB */
#define VGPU_MAX_SCANOUTS    16u
#define VGPU_EVENT_DISPLAY   1u

/* struct virtio_gpu_config (spec 5.7.4). */
#define VGPU_CFG_EVENTS_READ  0u
#define VGPU_CFG_EVENTS_CLEAR 4u
#define VGPU_CFG_NUM_SCANOUTS 8u

#define VGPU_MAX_W      3840u
#define VGPU_MAX_H      2160u
#define VGPU_DEFAULT_W  1024u
#define VGPU_DEFAULT_H  768u
#define VGPU_CHUNK      (4u * 1024u * 1024u)
#define VGPU_TICK_MS    16u
#define VGPU_CMD_TIMEOUT_NS  (2ull * 1000000000ull)
#define VGPU_PANIC_POLL_NS   (200ull * 1000000ull)
#define VGPU_RESIZE_RETRY_NS (1000ull * 1000000ull)

struct vgpu_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
} __packed;

struct vgpu_rect {
    uint32_t x, y, width, height;
} __packed;

struct vgpu_display_info {
    struct vgpu_hdr hdr;
    struct {
        struct vgpu_rect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[VGPU_MAX_SCANOUTS];
} __packed;

struct vgpu_create_2d {
    struct vgpu_hdr hdr;
    uint32_t resource_id, format, width, height;
} __packed;

struct vgpu_unref {
    struct vgpu_hdr hdr;
    uint32_t resource_id, padding;
} __packed;

struct vgpu_set_scanout {
    struct vgpu_hdr hdr;
    struct vgpu_rect r;
    uint32_t scanout_id, resource_id;
} __packed;

struct vgpu_flush {
    struct vgpu_hdr hdr;
    struct vgpu_rect r;
    uint32_t resource_id, padding;
} __packed;

struct vgpu_transfer {
    struct vgpu_hdr hdr;
    struct vgpu_rect r;
    uint64_t offset;
    uint32_t resource_id, padding;
} __packed;

struct vgpu_mem_entry {
    uint64_t addr;
    uint32_t length, padding;
} __packed;

struct vgpu_attach {
    struct vgpu_hdr hdr;
    uint32_t resource_id, nr_entries;
    struct vgpu_mem_entry entries[];
} __packed;

/* The backing is described a page per entry, as Linux's driver does with
 * its shmem pages: Parallels Desktop answered 0x1205 (invalid parameter)
 * to entries of 4 MiB, the chunks as allocated. */
#define VGPU_ENTRY_BYTES PAGE_SIZE

struct vgpu;

/* One frame: a host resource and the guest memory behind it. */
struct vgpu_frame {
    struct vgpu *g;                  /* its surface's panic_flush argument is the frame */
    uint32_t resource_id;
    uint32_t width, height, pitch;
    unsigned nchunks;
    void *chunk[VM_MAP_RANGES_MAX];
    dma_addr_t chunk_dma[VM_MAP_RANGES_MAX];
    size_t chunk_len[VM_MAP_RANGES_MAX];
    vaddr_t view;                    /* the chunks back to back */
    struct fbcon_surface surface;
    bool host_resource;              /* created on the device */
};

/* A request and its response, in one dma_alloc page each. */
struct vgpu_buf {
    void *req, *resp;
    dma_addr_t req_dma, resp_dma;
};

/* DMA memory a timed-out command may have left the device: freed only
 * after the reset in vgpu_teardown. */
struct vgpu_retained {
    struct vgpu_retained *next;
    void *va;
    dma_addr_t dma;
    size_t len;
};

struct vgpu {
    struct virtio_device *vdev;
    struct virtqueue *vq;
    struct mutex cmd_lock;           /* one command at a time (thread context) */
    struct vgpu_buf cmd;             /* under cmd_lock */
    struct completion done;          /* the command in flight completed */
    struct vgpu_buf panic;           /* vgpu_panic_flush's own */
    uint32_t next_resource;
    struct vgpu_frame *frame;        /* attached to fbcon */
    struct thread *thread;
    bool stopping;
    bool broken;                     /* a command timed out: none is sent again */
    bool panic_dead;                 /* a panic command was not answered: none is sent again */
    bool panic_busy;                 /* vgpu_panic_flush is running: a line it logs is not flushed */
    struct vgpu_retained *retained;  /* under cmd_lock */
    /* Under cmd_lock, the thread's: a display change not yet followed
     * (retried every VGPU_RESIZE_RETRY_NS), and rows taken from fbcon
     * whose flush failed (retried every tick). */
    bool resize_pending, resize_warned;
    uint64_t resize_retry_at;
    uint32_t pend_y0, pend_y1;
    uint64_t flushes, transfers_bytes, resizes, panic_flushes;
};

static unsigned vgpu_done(struct virtqueue *vq, unsigned budget)
{
    struct vgpu *g = vq->vdev->priv;
    uint32_t len;
    unsigned n = 0;
    void *cookie;
    for (; n < budget && (cookie = virtq_pop(vq, &len)) != NULL; n++) {
        if (cookie == &g->cmd)
            complete(&g->done);
        /* &g->panic: vgpu_panic_flush polls for it itself. */
    }
    return n;
}

static void hdr_init(void *req, uint32_t type)
{
    struct vgpu_hdr *h = req;
    memset(h, 0, sizeof(*h));
    h->type = type;
}

/* Send the request at `req_dma` (`req_len` bytes), then `data_len` bytes
 * at `data_dma` as a descriptor of their own when data_len is not 0, and
 * wait for a response of `resp_len` bytes in g->cmd.resp. 0 when its type
 * is `want`, else -EIO (the type is logged). */
static int vgpu_cmd_at(struct vgpu *g, const void *req, dma_addr_t req_dma, size_t req_len, dma_addr_t data_dma,
                       size_t data_len, size_t resp_len, uint32_t want)
{
    if (g->broken)
        return -EIO;
    struct virtq_sg sg[3] = {
        { .addr = req_dma, .len = (uint32_t)req_len },
        { .addr = data_dma, .len = (uint32_t)data_len },
        { .addr = g->cmd.resp_dma, .len = (uint32_t)resp_len },
    };
    unsigned out = 2;
    if (data_len == 0) {
        sg[1] = sg[2];
        out = 1;
    }
    memset(g->cmd.resp, 0, resp_len);
    completion_init(&g->done, "vgpu-cmd");
    int rc = virtq_add(g->vq, sg, out, 1, &g->cmd);
    if (rc)
        return rc;
    virtq_kick(g->vq);
    if (!wait_for_completion_timeout(&g->done, VGPU_CMD_TIMEOUT_NS)) {
        /* The buffers may still be the device's: never reuse them. */
        g->broken = true;
        kerror("virtio-gpu: %s: command 0x%x had no response in 2 s; the display is left as it is",
               g->vdev->dev.name, ((const struct vgpu_hdr *)req)->type);
        return -ETIMEDOUT;
    }
    uint32_t got = ((struct vgpu_hdr *)g->cmd.resp)->type;
    if (got != want) {
        kwarn("virtio-gpu: %s: command 0x%x answered 0x%x", g->vdev->dev.name, ((const struct vgpu_hdr *)req)->type,
              got);
        return -EIO;
    }
    return 0;
}

/* The common case: the request in g->cmd.req. */
static int vgpu_cmd(struct vgpu *g, size_t req_len, size_t resp_len, uint32_t want)
{
    return vgpu_cmd_at(g, g->cmd.req, g->cmd.req_dma, req_len, 0, 0, resp_len, want);
}

/* Scanout 0's size: its enabled rectangle, capped, or the default. */
static int vgpu_display_size(struct vgpu *g, uint32_t *w, uint32_t *h)
{
    if (g->broken)
        return -EIO;   /* g->cmd.req may still be the device's: not written */
    hdr_init(g->cmd.req, VGPU_CMD_GET_DISPLAY_INFO);
    int rc = vgpu_cmd(g, sizeof(struct vgpu_hdr), sizeof(struct vgpu_display_info), VGPU_RESP_OK_DISPLAY_INFO);
    if (rc)
        return rc;
    const struct vgpu_display_info *di = g->cmd.resp;
    *w = VGPU_DEFAULT_W;
    *h = VGPU_DEFAULT_H;
    if (di->pmodes[0].enabled && di->pmodes[0].r.width >= 320 && di->pmodes[0].r.height >= 200) {
        *w = di->pmodes[0].r.width < VGPU_MAX_W ? di->pmodes[0].r.width : VGPU_MAX_W;
        *h = di->pmodes[0].r.height < VGPU_MAX_H ? di->pmodes[0].r.height : VGPU_MAX_H;
        kinfo("virtio-gpu: %s: scanout 0 offers %ux%u", g->vdev->dev.name, di->pmodes[0].r.width,
              di->pmodes[0].r.height);
    } else {
        kinfo("virtio-gpu: %s: scanout 0 offers no size; %ux%u", g->vdev->dev.name, *w, *h);
    }
    return 0;
}

static int vgpu_transfer_flush(struct vgpu *g, struct vgpu_frame *f, uint32_t y0, uint32_t y1)
{
    if (g->broken)
        return -EIO;
    struct vgpu_transfer *t = g->cmd.req;
    hdr_init(t, VGPU_CMD_TRANSFER_TO_HOST_2D);
    t->r = (struct vgpu_rect){ 0, y0, f->width, y1 - y0 };
    t->offset = (uint64_t)y0 * f->pitch;
    t->resource_id = f->resource_id;
    t->padding = 0;
    int rc = vgpu_cmd(g, sizeof(*t), sizeof(struct vgpu_hdr), VGPU_RESP_OK_NODATA);
    if (rc)
        return rc;
    struct vgpu_flush *fl = g->cmd.req;
    hdr_init(fl, VGPU_CMD_RESOURCE_FLUSH);
    fl->r = (struct vgpu_rect){ 0, y0, f->width, y1 - y0 };
    fl->resource_id = f->resource_id;
    fl->padding = 0;
    rc = vgpu_cmd(g, sizeof(*fl), sizeof(struct vgpu_hdr), VGPU_RESP_OK_NODATA);
    if (rc == 0) {
        g->flushes++;
        g->transfers_bytes += (uint64_t)(y1 - y0) * f->pitch;
    }
    return rc;
}

static int vgpu_set_scanout(struct vgpu *g, uint32_t resource_id, uint32_t w, uint32_t h)
{
    if (g->broken)
        return -EIO;
    struct vgpu_set_scanout *s = g->cmd.req;
    hdr_init(s, VGPU_CMD_SET_SCANOUT);
    s->r = (struct vgpu_rect){ 0, 0, w, h };
    s->scanout_id = 0;
    s->resource_id = resource_id;
    return vgpu_cmd(g, sizeof(*s), sizeof(struct vgpu_hdr), VGPU_RESP_OK_NODATA);
}

/* Free DMA memory now, or, once a command has timed out or when `keep`
 * (the device may still own it), keep it for vgpu_teardown to free after
 * a reset the device acknowledged. */
static void vgpu_dma_keep(struct vgpu *g, bool keep, size_t len, void *va, dma_addr_t dma)
{
    if (!g->broken && !keep) {
        dma_free(&g->vdev->dev, len, va, dma);
        return;
    }
    struct vgpu_retained *r = kmalloc(sizeof(*r), 0);
    if (r == NULL)
        return;   /* leaked: never handed back while the device may own it */
    *r = (struct vgpu_retained){ g->retained, va, dma, len };
    g->retained = r;
}

static void vgpu_dma_release(struct vgpu *g, size_t len, void *va, dma_addr_t dma)
{
    vgpu_dma_keep(g, false, len, va, dma);
}

/* Drop a frame: its host resource (detached first, as the spec asks
 * before the memory goes), its view and its memory. Not attached to fbcon.
 * A detach the device refused (an error response; a timeout sets broken)
 * leaves the backing attached: the memory is kept as after a timeout, and
 * no UNREF follows (after a timeout g->cmd.req may still be the device's;
 * the reset drops the resource). */
static void vgpu_frame_free(struct vgpu *g, struct vgpu_frame *f)
{
    bool keep = false;
    if (f->host_resource && !g->broken) {
        struct vgpu_unref *u = g->cmd.req;
        hdr_init(u, VGPU_CMD_RESOURCE_DETACH_BACKING);
        u->resource_id = f->resource_id;
        u->padding = 0;
        if (vgpu_cmd(g, sizeof(*u), sizeof(struct vgpu_hdr), VGPU_RESP_OK_NODATA) != 0) {
            keep = true;
            kwarn("virtio-gpu: %s: resource %u's backing not detached: its %u KiB kept until a reset",
                  g->vdev->dev.name, f->resource_id, (unsigned)((size_t)f->pitch * f->height >> 10));
        } else {
            hdr_init(u, VGPU_CMD_RESOURCE_UNREF);
            u->resource_id = f->resource_id;
            (void)vgpu_cmd(g, sizeof(*u), sizeof(struct vgpu_hdr), VGPU_RESP_OK_NODATA);
        }
    }
    if (f->view)
        vm_unmap_phys(f->view);
    for (unsigned i = 0; i < f->nchunks; i++)
        vgpu_dma_keep(g, keep, f->chunk_len[i], f->chunk[i], f->chunk_dma[i]);
    kfree(f);
}

static bool vgpu_panic_flush(void *arg, uint32_t y0, uint32_t y1);

/* Scanout 0 back on the frame fbcon draws, all of it flushed (or off when
 * there is none): after a new frame was put on it and then given up. */
static void vgpu_restore_scanout(struct vgpu *g)
{
    if (g->frame == NULL) {
        (void)vgpu_set_scanout(g, 0, 0, 0);
        return;
    }
    if (vgpu_set_scanout(g, g->frame->resource_id, g->frame->width, g->frame->height) == 0)
        (void)vgpu_transfer_flush(g, g->frame, 0, g->frame->height);
}

/* A frame of w x h on the device and in memory, shown on scanout 0 with
 * its whole area flushed. Not yet attached to fbcon. */
static struct vgpu_frame *vgpu_frame_new(struct vgpu *g, uint32_t w, uint32_t h, int *rc_out)
{
    int rc = -ENOMEM;
    bool shown = false;   /* scanout 0 was given this frame */
    struct vgpu_frame *f = kzalloc(sizeof(*f));
    if (f == NULL)
        goto fail;
    f->g = g;
    f->width = w;
    f->height = h;
    f->pitch = w * 4u;
    size_t total = ALIGN_UP((size_t)f->pitch * h, PAGE_SIZE);
    for (size_t off = 0; off < total; f->nchunks++) {
        size_t len = total - off < VGPU_CHUNK ? total - off : VGPU_CHUNK;
        if (f->nchunks == VM_MAP_RANGES_MAX)
            goto fail;
        f->chunk[f->nchunks] = dma_alloc(&g->vdev->dev, len, &f->chunk_dma[f->nchunks], DMA_ZERO);
        if (f->chunk[f->nchunks] == NULL)
            goto fail;
        f->chunk_len[f->nchunks] = len;
        off += len;
    }
    f->view = vm_map_direct_ranges(f->chunk, f->chunk_len, f->nchunks, VM_PROT_RW, VM_CACHE_WB);
    if (f->view == 0)
        goto fail;

    rc = -EIO;
    if (g->broken)
        goto fail;
    f->resource_id = ++g->next_resource;
    struct vgpu_create_2d *c = g->cmd.req;
    hdr_init(c, VGPU_CMD_RESOURCE_CREATE_2D);
    c->resource_id = f->resource_id;
    c->format = VGPU_FORMAT_B8G8R8X8;
    c->width = w;
    c->height = h;
    rc = vgpu_cmd(g, sizeof(*c), sizeof(struct vgpu_hdr), VGPU_RESP_OK_NODATA);
    if (rc)
        goto fail;
    f->host_resource = true;

    /* The entries, a page each, in a buffer of their own (1920x1080: 2025
     * entries, 32 KiB), sent as a second descriptor after the command, as
     * Linux's driver sends them. */
    size_t entries = total / VGPU_ENTRY_BYTES;
    size_t elen = ALIGN_UP(entries * sizeof(struct vgpu_mem_entry), PAGE_SIZE);
    dma_addr_t edma;
    struct vgpu_mem_entry *ent = dma_alloc(&g->vdev->dev, elen, &edma, DMA_ZERO);
    if (ent == NULL) {
        rc = -ENOMEM;
        goto fail;
    }
    size_t e = 0;
    for (unsigned i = 0; i < f->nchunks; i++)
        for (size_t off = 0; off < f->chunk_len[i]; off += VGPU_ENTRY_BYTES)
            ent[e++] = (struct vgpu_mem_entry){ f->chunk_dma[i] + off, VGPU_ENTRY_BYTES, 0 };
    struct vgpu_attach *a = g->cmd.req;
    hdr_init(a, VGPU_CMD_RESOURCE_ATTACH_BACKING);
    a->resource_id = f->resource_id;
    a->nr_entries = (uint32_t)entries;
    rc = vgpu_cmd_at(g, a, g->cmd.req_dma, sizeof(struct vgpu_attach), edma, entries * sizeof(*ent),
                     sizeof(struct vgpu_hdr), VGPU_RESP_OK_NODATA);
    vgpu_dma_release(g, elen, ent, edma);
    if (rc)
        goto fail;
    shown = true;
    rc = vgpu_set_scanout(g, f->resource_id, w, h);
    if (rc)
        goto fail;
    rc = vgpu_transfer_flush(g, f, 0, h);
    if (rc)
        goto fail;

    f->surface = (struct fbcon_surface){
        .base = (void *)f->view, .width = w, .height = h, .pitch = f->pitch, .bpp = 32,
        .red_shift = 16, .red_bits = 8, .green_shift = 8, .green_bits = 8, .blue_shift = 0, .blue_bits = 8,
        .panic_flush = vgpu_panic_flush, .arg = f,
    };
    return f;

fail:
    if (shown)
        vgpu_restore_scanout(g);   /* before the resource goes */
    if (f)
        vgpu_frame_free(g, f);
    *rc_out = rc;
    return NULL;
}

/* Move the console to a new frame of w x h and drop the old one: the new
 * frame is complete and on the scanout before fbcon leaves the old. */
static int vgpu_show(struct vgpu *g, uint32_t w, uint32_t h)
{
    int rc = 0;
    struct vgpu_frame *f = vgpu_frame_new(g, w, h, &rc);
    if (f == NULL)
        return rc;
    rc = fbcon_attach(&f->surface);
    if (rc) {
        vgpu_restore_scanout(g);
        vgpu_frame_free(g, f);
        return rc;
    }
    struct vgpu_frame *old = g->frame;
    g->frame = f;
    g->pend_y0 = g->pend_y1 = 0;   /* the old frame's rows; the new one is all flushed */
    /* In panic mode a writer without the lock may still draw on the old
     * frame and flush it (fbcon.h): kept. */
    if (old && !console_in_panic_mode())
        vgpu_frame_free(g, old);
    /* The console's first screen, drawn by fbcon_attach. */
    uint32_t y0, y1;
    if (fbcon_take_damage(&f->surface, &y0, &y1) && vgpu_transfer_flush(g, f, y0, y1) != 0) {
        g->pend_y0 = y0;   /* the thread retries it */
        g->pend_y1 = y1;
    }
    kinfo("virtio-gpu: %s: scanout 0 %ux%u, resource %u in %u chunk(s)", g->vdev->dev.name, w, h,
          f->resource_id, f->nchunks);
    return 0;
}

static void vgpu_thread(void *arg)
{
    struct vgpu *g = arg;
    while (!__atomic_load_n(&g->stopping, __ATOMIC_ACQUIRE)) {
        thread_sleep_ms(VGPU_TICK_MS);
        mutex_lock(&g->cmd_lock);
        if (g->broken || g->frame == NULL || __atomic_load_n(&g->stopping, __ATOMIC_ACQUIRE)) {
            mutex_unlock(&g->cmd_lock);
            continue;
        }
        /* Cleared before the size is read: a change after the read
         * raises the event again. */
        if (virtio_read_config32(g->vdev, VGPU_CFG_EVENTS_READ) & VGPU_EVENT_DISPLAY) {
            virtio_write_config32(g->vdev, VGPU_CFG_EVENTS_CLEAR, VGPU_EVENT_DISPLAY);
            g->resize_pending = true;
            g->resize_warned = false;
            g->resize_retry_at = 0;
        }
        if (g->resize_pending && clock_deadline_passed(g->resize_retry_at)) {
            uint32_t w, h;
            int rc = vgpu_display_size(g, &w, &h);
            if (rc == 0 && w == g->frame->width && h == g->frame->height)
                g->resize_pending = false;
            else if (rc == 0 && (rc = vgpu_show(g, w, h)) == 0) {
                g->resize_pending = false;
                g->resizes++;
            }
            if (g->resize_pending) {
                /* Kept and retried (an allocation may succeed later);
                 * a timeout ends it with every other command. */
                if (!g->resize_warned)
                    kwarn("virtio-gpu: %s: cannot follow the display (%d); staying at %ux%u, retrying",
                          g->vdev->dev.name, rc, g->frame->width, g->frame->height);
                g->resize_warned = true;
                g->resize_retry_at = clock_deadline_ns(VGPU_RESIZE_RETRY_NS);   /* the thread may migrate */
            }
        }
        /* Rows whose flush failed stay pending, merged with new ones. */
        uint32_t y0, y1;
        if (fbcon_take_damage(&g->frame->surface, &y0, &y1)) {
            if (g->pend_y0 == g->pend_y1) {
                g->pend_y0 = y0;
                g->pend_y1 = y1;
            } else {
                g->pend_y0 = y0 < g->pend_y0 ? y0 : g->pend_y0;
                g->pend_y1 = y1 > g->pend_y1 ? y1 : g->pend_y1;
            }
        }
        if (g->pend_y0 != g->pend_y1 && vgpu_transfer_flush(g, g->frame, g->pend_y0, g->pend_y1) == 0)
            g->pend_y0 = g->pend_y1 = 0;
        mutex_unlock(&g->cmd_lock);
    }
}

/*
 * From fbcon in panic mode: no thread runs and console.c holds no lock.
 * `arg` is the frame fbcon draws on, which g->frame names only once
 * fbcon_attach has returned. The two commands go on controlq from
 * vgpu_panic's own buffers and are polled for, each for at most
 * VGPU_PANIC_POLL_NS. The queue's lock may be held by a CPU the panic
 * stopped: then nothing is sent (the serial console has the panic either
 * way). Nothing here takes a tracked lock: a thread's command answered
 * meanwhile is dropped (its waiter never runs again). A line logged from
 * inside (virtq_pop_panic's report of a bad used entry) comes back through
 * fbcon: it is not flushed, so g->panic.req is not rewritten in flight;
 * false leaves its rows pending for the next line's flush. True: the
 * band is on the screen, both commands answered OK_NODATA (the response
 * is cleared before each, as vgpu_cmd_at clears its own).
 */
static bool vgpu_panic_flush(void *arg, uint32_t y0, uint32_t y1)
{
    struct vgpu_frame *f = arg;
    struct vgpu *g = f->g;
    if (g->broken || g->panic_dead || g->panic_busy)
        return false;
    g->panic_busy = true;
    bool done = false;
    uint32_t types[2] = { VGPU_CMD_TRANSFER_TO_HOST_2D, VGPU_CMD_RESOURCE_FLUSH };
    for (unsigned k = 0; k < 2; k++) {
        if (types[k] == VGPU_CMD_TRANSFER_TO_HOST_2D) {
            struct vgpu_transfer *t = g->panic.req;
            hdr_init(t, types[k]);
            t->r = (struct vgpu_rect){ 0, y0, f->width, y1 - y0 };
            t->offset = (uint64_t)y0 * f->pitch;
            t->resource_id = f->resource_id;
        } else {
            struct vgpu_flush *fl = g->panic.req;
            hdr_init(fl, types[k]);
            fl->r = (struct vgpu_rect){ 0, y0, f->width, y1 - y0 };
            fl->resource_id = f->resource_id;
        }
        memset(g->panic.resp, 0, sizeof(struct vgpu_hdr));
        struct virtq_sg sg[2] = {
            { .addr = g->panic.req_dma,
              .len = k == 0 ? sizeof(struct vgpu_transfer) : sizeof(struct vgpu_flush) },
            { .addr = g->panic.resp_dma, .len = sizeof(struct vgpu_hdr) },
        };
        if (virtq_add_panic(g->vq, sg, 1, 1, &g->panic) != 0)
            goto out;
        virtq_kick(g->vq);
        /* Not clock_deadline_ns: nothing migrates in a panic, and the
         * machine-wide tick it falls back to may not run. */
        uint64_t deadline = clock_now_ns() + VGPU_PANIC_POLL_NS;
        bool seen = false;
        while (!seen && clock_now_ns() < deadline) {
            uint32_t len;
            seen = virtq_pop_panic(g->vq, &len) == &g->panic;
        }
        if (!seen) {
            /* Not again: each later panic line would wait as long,
             * and the serial sink after this one with it. */
            g->panic_dead = true;
            goto out;
        }
        if (((struct vgpu_hdr *)g->panic.resp)->type != VGPU_RESP_OK_NODATA)
            goto out;   /* refused: the band stays pending */
    }
    g->panic_flushes++;
    done = true;
out:
    g->panic_busy = false;
    return done;
}

static int vgpu_buf_alloc(struct vgpu *g, struct vgpu_buf *b)
{
    b->req = dma_alloc(&g->vdev->dev, PAGE_SIZE, &b->req_dma, DMA_ZERO);
    b->resp = dma_alloc(&g->vdev->dev, PAGE_SIZE, &b->resp_dma, DMA_ZERO);
    return b->req && b->resp ? 0 : -ENOMEM;
}

static void vgpu_buf_free(struct vgpu *g, struct vgpu_buf *b)
{
    if (b->req)
        dma_free(&g->vdev->dev, PAGE_SIZE, b->req, b->req_dma);
    if (b->resp)
        dma_free(&g->vdev->dev, PAGE_SIZE, b->resp, b->resp_dma);
}

/* False when `g` must outlive this: a panic has begun, and its writer,
 * which takes no lock, may still draw on the frame and flush it through
 * g's queue (fbcon.h). Then nothing is torn down. */
static bool vgpu_teardown(struct vgpu *g)
{
    if (g->frame) {
        fbcon_detach(&g->frame->surface);
        if (console_in_panic_mode())
            return false;
        mutex_lock(&g->cmd_lock);
        (void)vgpu_set_scanout(g, 0, 0, 0);   /* resource 0: scanout off */
        vgpu_frame_free(g, g->frame);
        g->frame = NULL;
        mutex_unlock(&g->cmd_lock);
    }
    virtio_device_reset(g->vdev);
    /* Until the reset takes (status reads 0) the device may still read the
     * queue and, after a timeout or a refused detach, the kept buffers:
     * none of it is freed, and no driver binds the device again (U14).
     * The queue's interrupt goes all the same: vgpu_done must not run
     * once `g` is freed, nor after the module is unloaded. */
    if (g->vdev->tr->get_status(g->vdev) != 0) {
        kwarn("virtio-gpu: %s: the reset was not acknowledged: its queue and buffers are kept",
              g->vdev->dev.name);
        if (g->vq)
            virtq_quiesce(g->vq);
        device_retain_dma(&g->vdev->dev);
        return true;
    }
    if (g->vq)
        virtq_free(g->vq);
    while (g->retained != NULL) {
        struct vgpu_retained *r = g->retained;
        g->retained = r->next;
        dma_free(&g->vdev->dev, r->len, r->va, r->dma);
        kfree(r);
    }
    vgpu_buf_free(g, &g->cmd);
    vgpu_buf_free(g, &g->panic);
    return true;
}

static int vgpu_probe(struct virtio_device *vdev)
{
    struct vgpu *g = kzalloc(sizeof(*g));
    if (g == NULL)
        return -ENOMEM;
    g->vdev = vdev;
    vdev->priv = g;
    mutex_init(&g->cmd_lock, "virtio-gpu-cmd");
    completion_init(&g->done, "vgpu-cmd");

    int rc = virtio_device_init(vdev, 0);
    if (rc)
        goto fail;
    rc = vgpu_buf_alloc(g, &g->cmd);
    if (rc == 0)
        rc = vgpu_buf_alloc(g, &g->panic);
    if (rc)
        goto fail;
    rc = virtq_alloc(vdev, 0, 0, vgpu_done, &g->vq);
    if (rc)
        goto fail;
    virtio_device_ready(vdev);

    uint32_t w, h;
    mutex_lock(&g->cmd_lock);
    /* Cleared before the size is read: a change after the read raises
     * the event again, and the thread follows it. */
    virtio_write_config32(vdev, VGPU_CFG_EVENTS_CLEAR, VGPU_EVENT_DISPLAY);
    rc = vgpu_display_size(g, &w, &h);
    if (rc == 0)
        rc = vgpu_show(g, w, h);
    mutex_unlock(&g->cmd_lock);
    if (rc)
        goto fail;

    g->thread = thread_create(vgpu_thread, g, "vgpu", SCHED_PRIO_DEFAULT);
    if (g->thread == NULL) {
        rc = -ENOMEM;
        goto fail;
    }
    kinfo("virtio-gpu: %s: %u scanout(s); the console is on scanout 0", vdev->dev.name,
          virtio_read_config32(vdev, VGPU_CFG_NUM_SCANOUTS));
    return 0;

fail:
    kwarn("virtio-gpu: %s: not used (%d)", vdev->dev.name, rc);
    if (vgpu_teardown(g)) {
        vdev->priv = NULL;
        kfree(g);
    }
    return rc;
}

static void vgpu_remove(struct virtio_device *vdev)
{
    struct vgpu *g = vdev->priv;
    __atomic_store_n(&g->stopping, true, __ATOMIC_RELEASE);
    thread_join(g->thread);
    if (!vgpu_teardown(g))
        return;   /* a panic is drawing through it */
    kinfo("virtio-gpu: %s: removed after %llu flushes, %llu resizes", vdev->dev.name,
          (unsigned long long)g->flushes, (unsigned long long)g->resizes);
    vdev->priv = NULL;
    kfree(g);
}

static const uint32_t vgpu_ids[] = { VIRTIO_ID_GPU, 0 };

static struct virtio_driver vgpu_driver = {
    .drv = { .name = "virtio_gpu" },
    .ids = vgpu_ids,
    .probe = vgpu_probe,
    .remove = vgpu_remove,
};

static int vgpu_module_init(void)
{
    return virtio_register_driver(&vgpu_driver);
}

static void vgpu_module_shutdown(void)
{
    virtio_unregister_driver(&vgpu_driver);
}

COSMO_MODULE("virtio_gpu", "1.0", vgpu_module_init, vgpu_module_shutdown, "virtio", MODULE_CAP_DRIVER);
