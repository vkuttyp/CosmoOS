# modules

Kernel modules loaded at boot from the boot archive (`modules/<name>.ko`
entries, in the order `build/module.mk` lists them). Each module is an
`ET_REL` object built with the kernel flags, declares itself with
`COSMO_MODULE()` from `kernel/include/kernel/module.h`, may `EXPORT_SYMBOL()`
for other modules, and is signed at build time. `hello/` is the smallest
possible module and proves the pipeline at every boot (the boot test
requires its log line). Drivers arrive here as modules from Phase 6.
Documentation: `docs/kernel/module/`.
