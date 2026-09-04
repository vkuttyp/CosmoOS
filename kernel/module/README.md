# kernel/module

Kernel module loader (Phase 5). `ksym.c` is the kernel's exported symbol
table (`EXPORT_SYMBOL`, sorted index, binary search); `modsig.c` verifies
the Ed25519 signature trailer against `kernel/security`'s key ring;
`modelf.c` validates an `ET_REL` image and computes its text/rodata/data
layout (pure, host-tested); `module.c` runs the load pipeline (signature,
ELF, ABI, dependencies, allocate, relocate, W^X, init, register), unload,
and lookups; `modtest.c` holds the self-tests. The architecture-specific
relocation applier is `kernel/arch/<arch>/modreloc.c`. Module sources
live under `modules/` (boot) and `tests/modules/` (fixtures); build rules
in `build/module.mk`. Documentation: `docs/kernel/module/`.
