# compat/linux

The Linux process personality (docs/compat/linux/): a static x86-64 ELF
without the CosmoOS ABI note runs here. `syscalls.c` is the Linux
system-call table translating onto the native kernel services;
`convert.c` holds the pure structure and flag conversions (also compiled
by the host test); `linux_abi.h` writes out the Linux ABI the personality
needs. Nothing native depends on this directory (invariant 7).

Documentation: `docs/compat/linux/architecture.md`, `design.md`,
`api.md` (every translated call and its deviations), `invariants.md`
(L1 to L12), `testing.md` (`tests/host/test_linux.c`, `tests/linux/`,
the musl program, the boot markers).
