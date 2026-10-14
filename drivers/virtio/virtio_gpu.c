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

/* One frame: a host resource and the guest memory behind it. */
struct vgpu_frame {
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
    struct vgpu_set_scanout *s = g->cmd.req;
    hdr_init(s, VGPU_CMD_SET_SCANOUT);
    s->r = (struct vgpu_rect){ 0, 0, w, h };
    s->scanout_id = 0;
    s->resource_id = resource_id;
    return vgpu_cmd(g, sizeof(*s), sizeof(struct vgpu_hdr), VGPU_RESP_OK_NODATA);
}

/* Drop a frame: its host resource (detached first, as the spec asks
 * before the memory goes), its view and its memory. Not attached to fbcon. */
static void vgpu_frame_free(struct vgpu *g, struct vgpu_frame *f)
{
    if (f->host_resource && !g->broken) {
        struct vgpu_unref *u = g->cmd.req;
        hdr_init(u, VGPU_CMD_RESOURCE_DETACH_BACKING);
        u->resource_id = f->resource_id;
        u->padding = 0;
        (void)vgpu_cmd(g, sizeof(*u), sizeof(struct vgpu_hdr), VGPU_RESP_OK_NODATA);
        hdr_init(u, VGPU_CMD_RESOURCE_UNREF);
        u->resource_id = f->resource_id;
        (void)vgpu_cmd(g, sizeof(*u), sizeof(struct vgpu_hdr), VGPU_RESP_OK_NODATA);
    }
    if (f->view)
        vm_unmap_phys(f->view);
    /* A command that timed out may leave the device holding the memory:
     * keep it rather than hand the device someone else's pages. */
    if (!g->broken)
        for (unsigned i = 0; i < f->nchunks; i++)
            dma_free(&g->vdev->dev, f->chunk_len[i], f->chunk[i], f->chunk_dma[i]);
    kfree(f);
}

static void vgpu_panic_flush(void *arg, uint32_t y0, uint32_t y1);

/* A frame of w x h on the device and in memory, shown on scanout 0 with
 * its whole area flushed. Not yet attached to fbcon. */
static struct vgpu_frame *vgpu_frame_new(struct vgpu *g, uint32_t w, uint32_t h, int *rc_out)
{
    int rc = -ENOMEM;
    struct vgpu_frame *f = kzalloc(sizeof(*f));
    if (f == NULL)
        goto fail;
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
    if (!g->broken)   /* a command that timed out may leave it the device's */
        dma_free(&g->vdev->dev, elen, ent, edma);
    if (rc)
        goto fail;
    rc = vgpu_set_scanout(g, f->resource_id, w, h);
    if (rc)
        goto fail;
    rc = vgpu_transfer_flush(g, f, 0, h);
    if (rc)
        goto fail;

    f->surface = (struct fbcon_surface){
        .base = (void *)f->view, .width = w, .height = h, .pitch = f->pitch, .bpp = 32,
        .red_shift = 16, .red_bits = 8, .green_shift = 8, .green_bits = 8, .blue_shift = 0, .blue_bits = 8,
        .panic_flush = vgpu_panic_flush, .arg = g,
    };
    return f;

fail:
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
        if (g->frame)
            (void)vgpu_set_scanout(g, g->frame->resource_id, g->frame->width, g->frame->height);
        vgpu_frame_free(g, f);
        return rc;
    }
    struct vgpu_frame *old = g->frame;
    g->frame = f;
    if (old)
        vgpu_frame_free(g, old);
    /* The console's first screen, drawn by fbcon_attach. */
    uint32_t y0, y1;
    if (fbcon_take_damage(&f->surface, &y0, &y1))
        (void)vgpu_transfer_flush(g, f, y0, y1);
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
        if (virtio_read_config32(g->vdev, VGPU_CFG_EVENTS_READ) & VGPU_EVENT_DISPLAY) {
            virtio_write_config32(g->vdev, VGPU_CFG_EVENTS_CLEAR, VGPU_EVENT_DISPLAY);
            uint32_t w, h;
            if (vgpu_display_size(g, &w, &h) == 0 && (w != g->frame->width || h != g->frame->height)) {
                if (vgpu_show(g, w, h) == 0)
                    g->resizes++;
                else
                    kwarn("virtio-gpu: %s: cannot change to %ux%u; staying at %ux%u", g->vdev->dev.name, w, h,
                          g->frame->width, g->frame->height);
            }
        }
        uint32_t y0, y1;
        if (fbcon_take_damage(&g->frame->surface, &y0, &y1))
            (void)vgpu_transfer_flush(g, g->frame, y0, y1);
        mutex_unlock(&g->cmd_lock);
    }
}

/*
 * From fbcon in panic mode: no thread runs and console.c holds no lock.
 * The two commands go on controlq from vgpu_panic's own buffers and are
 * polled for, each for at most VGPU_PANIC_POLL_NS. The queue's lock may be
 * held by a CPU the panic stopped: then nothing is sent (the serial
 * console has the panic either way).
 */
static void vgpu_panic_flush(void *arg, uint32_t y0, uint32_t y1)
{
    struct vgpu *g = arg;
    struct vgpu_frame *f = g->frame;
    if (f == NULL || g->broken || !spin_trylock(&g->vq->lock))
        return;
    spin_unlock(&g->vq->lock);
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
        struct virtq_sg sg[2] = {
            { .addr = g->panic.req_dma,
              .len = k == 0 ? sizeof(struct vgpu_transfer) : sizeof(struct vgpu_flush) },
            { .addr = g->panic.resp_dma, .len = sizeof(struct vgpu_hdr) },
        };
        if (virtq_add(g->vq, sg, 1, 1, &g->panic) != 0)
            return;
        virtq_kick(g->vq);
        uint64_t deadline = clock_now_ns() + VGPU_PANIC_POLL_NS;
        bool seen = false;
        while (!seen && clock_now_ns() < deadline) {
            uint32_t len;
            void *cookie = virtq_pop(g->vq, &len);
            if (cookie == &g->panic)
                seen = true;
            else if (cookie == &g->cmd)
                complete(&g->done);   /* a thread's command, answered meanwhile */
        }
        if (!seen)
            return;
    }
    g->panic_flushes++;
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

static void vgpu_teardown(struct vgpu *g)
{
    if (g->frame) {
        fbcon_detach(&g->frame->surface);
        mutex_lock(&g->cmd_lock);
        (void)vgpu_set_scanout(g, 0, 0, 0);   /* resource 0: scanout off */
        vgpu_frame_free(g, g->frame);
        g->frame = NULL;
        mutex_unlock(&g->cmd_lock);
    }
    virtio_device_reset(g->vdev);
    if (g->vq)
        virtq_free(g->vq);
    if (!g->broken) {
        vgpu_buf_free(g, &g->cmd);
        vgpu_buf_free(g, &g->panic);
    }
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
    rc = vgpu_display_size(g, &w, &h);
    if (rc == 0)
        rc = vgpu_show(g, w, h);
    /* A display change before now is in that size already. */
    virtio_write_config32(vdev, VGPU_CFG_EVENTS_CLEAR, VGPU_EVENT_DISPLAY);
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
    vgpu_teardown(g);
    kfree(g);
    vdev->priv = NULL;
    return rc;
}

static void vgpu_remove(struct virtio_device *vdev)
{
    struct vgpu *g = vdev->priv;
    __atomic_store_n(&g->stopping, true, __ATOMIC_RELEASE);
    thread_join(g->thread);
    vgpu_teardown(g);
    kinfo("virtio-gpu: %s: removed after %llu flushes, %llu resizes", vdev->dev.name,
          (unsigned long long)g->flushes, (unsigned long long)g->resizes);
    kfree(g);
    vdev->priv = NULL;
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
