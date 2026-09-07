# drivers/network

NIC drivers producing and consuming mbufs. The network stack never depends on a specific NIC (Invariant 5).

That invariant had one implementation (virtio-net, `drivers/virtio/`) until `e1000e.c`, the Intel 82574L driver, which added nothing to the kernel's interface to work (`docs/drivers/e1000e/`). `QEMU_NIC=e1000e gmake test` runs the whole network suite over it; the default boot has both and `net-second-nic` makes the second take over from the first.
