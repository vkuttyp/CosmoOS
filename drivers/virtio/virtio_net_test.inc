/* Included only in debug self-test builds. A deterministic device peer over
 * real DMA allocations, split queues, irq_poll and vnet_remove. The transport
 * holds RX and TX completions; in the late case it schedules both queues at
 * the reset status write, before the reset acknowledgement is read, where
 * a pending interrupt can still arrive before its vector is masked. No hardware timing or host packet is needed. */
static unsigned vnet_peer_size(struct virtio_device *dev, unsigned index)
{
    (void)dev;
    return index < 2 ? 64 : 0;
}

static int vnet_peer_setup(struct virtio_device *dev, struct virtqueue *vq)
{
    (void)dev;
    (void)vq;
    return 0;
}

static void vnet_peer_irq(struct virtqueue *vq)
{
    struct vnet_test_state *t = vq->vdev->tr_priv;
    arch_irq_state_t s = spin_lock_irqsave(&t->irq_lock);
    irq_poll_sched(&vq->poll);
    spin_unlock_irqrestore(&t->irq_lock, s);
}

static void vnet_peer_teardown(struct virtio_device *dev, struct virtqueue *vq)
{
    (void)dev;
    (void)vq;
}

static void vnet_peer_notify(struct virtio_device *dev, struct virtqueue *vq)
{
    struct vnet_test_state *t = dev->tr_priv;
    /* A kick with no new buffers is not a submission after reset. */
    if (t->reset && vq->avail->idx != 0)
        t->posts_after_reset++;
}

static void vnet_peer_status(struct virtio_device *dev, uint8_t status)
{
    struct vnet_test_state *t = dev->tr_priv;
    KASSERT(status == 0);
    t->reset = true;
    /* Reset discards all outstanding device work, and the device forgets
     * avail indices. Ring memory, including finished used entries, remains
     * host memory until freed, as on the PCI transport. */
    for (unsigned q = 0; q < 2; q++)
        if (dev->vq[q])
            dev->vq[q]->avail->idx = 0;
    if (t->late) {
        t->hold = false;
        vnet_peer_irq(dev->vq[0]);
        vnet_peer_irq(dev->vq[1]);
    }
}

static uint8_t vnet_peer_get_status(struct virtio_device *dev)
{
    (void)dev;
    return 0;
}

static const struct virtio_transport vnet_peer = {
    .name = "vnet-remove-peer", .queue_max_size = vnet_peer_size,
    .setup_queue = vnet_peer_setup, .teardown_queue = vnet_peer_teardown,
    .notify = vnet_peer_notify, .set_status = vnet_peer_status,
    .get_status = vnet_peer_get_status,
};

static void vnet_peer_finish(struct virtqueue *vq, unsigned avail, uint32_t len)
{
    uint16_t idx = vq->used->idx;
    unsigned head = vq->avail->ring[avail % vq->size];
    vq->used->ring[idx % vq->size] = (struct virtq_used_elem){ head, len };
    __atomic_thread_fence(__ATOMIC_RELEASE);
    vq->used->idx = (uint16_t)(idx + 1);
}

/* Called through module_symbol_lookup by devtest.c; no production ABI or
 * shared structure changes. The module stays loaded throughout the call. */
bool vnet_test_remove(unsigned mode, const char **reason);
bool vnet_test_remove(unsigned mode, const char **reason)
{
    struct virtio_device *dev = kzalloc(sizeof(*dev));
    struct vnet *v = kzalloc(sizeof(*v));
    struct vnet_test_state *t = kzalloc(sizeof(*t));
    bool registered = false, removed = false, ok = false;
    if (dev == NULL || v == NULL || t == NULL) {
        *reason = "vnet-remove: fixture allocation failed";
        goto out;
    }
    dev->dev.dma_mask = UINT64_MAX;
    strlcpy(dev->dev.name, "vnet-remove-peer", sizeof(dev->dev.name));
    dev->tr = &vnet_peer;
    dev->tr_priv = t;
    dev->priv = v;
    v->vdev = dev;
    t->v = v;
    t->hold = true;
    t->late = mode == 1;
    spinlock_init(&v->lock, "virtio-net");
    spinlock_init(&t->irq_lock, "vnet-peer-irq");
    strlcpy(v->nif.name, "vnrace", sizeof(v->nif.name));
    v->nif.mtu = 1500;
    v->nif.ops = &vnet_ops;
    v->nif.priv = v;
    v->nif.flags = NETIF_NODEFAULT | NETIF_NOARP;
    __atomic_store_n(&g_vnet_test, t, __ATOMIC_RELEASE);
    if (virtq_alloc(dev, 0, 64, vnet_rx_done, &v->rx) ||
        virtq_alloc(dev, 1, 64, vnet_tx_done, &v->tx) || netif_register(&v->nif)) {
        *reason = "vnet-remove: fixture queue/interface initialization failed";
        goto out;
    }
    registered = true;
    netif_get(&v->nif);   /* inspect accounting after remove drops its creator reference */
    vnet_post_rx(v);
    /* Two TX buffers in one chain check that every segment is unmapped. */
    struct mbuf *m = m_getcl(), *tail = m_getcl();
    if (m == NULL || tail == NULL) {
        m_freem(m);
        m_freem(tail);
        *reason = "vnet-remove: TX allocation failed";
        goto out;
    }
    memset(m->data, 0, 64);
    memset(tail->data, 0, 64);
    m->len = tail->len = 64;
    m->pkt.len = 128;
    m->next = tail;
    if (vnet_transmit(&v->nif, m)) {
        *reason = "vnet-remove: TX submission failed";
        goto out;
    }
    /* One completed RX, 31 still device-owned RX, a completed TX chain. */
    vnet_peer_finish(v->rx, 0, VNET_HDR_LEN + 14);
    vnet_peer_finish(v->tx, 0, 0);
    vnet_peer_irq(v->rx);
    vnet_peer_irq(v->tx);
    if (t->parked[0] == 0 || t->parked[1] == 0 || v->rx_posted != VNET_RX_BUFS) {
        *reason = "vnet-remove: RX/TX completions were not parked";
        goto out;
    }
    /* They remain held through unregister; the late case releases them
     * at the reset write before its acknowledgement. */
    vnet_remove(dev);
    removed = true;
    kinfo("selftest: vnet-remove: mode %u: parked RX %u TX %u; maps %u unmaps %u; buffers %u freed %u; rx_posted %u; late callbacks %u posts %u; accepted RX %llu",
          mode, t->parked[0], t->parked[1], t->maps, t->unmaps,
          t->records, t->freed, v->rx_posted, t->callbacks_after_reset,
          t->posts_after_reset, (unsigned long long)v->nif.stats.rx_packets);
    if (t->posts_after_reset) {
        *reason = "vnet-remove: receive buffers reposted after reset";
    } else if (t->maps != t->unmaps) {
        *reason = "vnet-remove: mapped buffers not unmapped";
    } else if (t->records != t->freed) {
        *reason = "vnet-remove: device-owned buffers leaked";
    } else if (v->rx_posted != 0) {
        *reason = "vnet-remove: rx_posted did not reach zero";
    } else if (v->nif.stats.rx_packets != 0) {
        *reason = "vnet-remove: frame accepted after unregister";
    } else {
        ok = true;
    }
out:
    if (registered && !removed) {
        vnet_remove(dev);
    }
    if (!registered && dev) {
        if (v && v->rx)
            virtq_free(v->rx);
        if (v && v->tx)
            virtq_free(v->tx);
    }
    __atomic_store_n(&g_vnet_test, NULL, __ATOMIC_RELEASE);
    /* Old-behavior runs must leave no leaked fixture for later tests. The
     * ledger retains missing maps even when remove freed their buffers. */
    if (t && dev) {
        for (unsigned i = 0; i < t->records; i++) {
            struct vnet_test_record *r = &t->record[i];
            if (r->mapped)
                dma_unmap(&dev->dev, r->dma, r->len, r->dir);
            if (r->m)   /* free a segment, not its already-recorded tail twice */
                m_free(r->m);
        }
    }
    if (registered)
        netif_put(&v->nif);
    else
        kfree(v);
    kfree(dev);
    kfree(t);
    return ok;
}
EXPORT_SYMBOL(vnet_test_remove);
