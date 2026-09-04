# modules

Kernel modules loaded at boot from the boot archive (`modules/<name>.ko`
entries, in the order `build/module.mk` lists them). Each module is an
`ET_REL` object built with the kernel flags, declares itself with
`COSMO_MODULE()` from `kernel/include/kernel/module.h`, may `EXPORT_SYMBOL()`
for other modules, and is signed at build time. `hello/` is the smallest
possible module and proves the pipeline at every boot (the boot test
requires its log line). It is the only module that lives here: the
driver modules (`virtio`, `virtio_blk`, `virtio_rng`, `virtio_console`)
live with their subsystem under `drivers/virtio/` and are listed in
`build/module.mk` like any other. Documentation: `docs/kernel/module/`.
