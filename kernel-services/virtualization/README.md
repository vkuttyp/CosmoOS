# kernel-services/virtualization

The VM manager (Phase 12): `/dev/vmm` and the `hv.*` sysctl values
(`vmm.c`), VMs as kobjects owning guest memory and a debug-console ring
(`vm.c`, `guestmem.c`, `vmdev.c`), vCPUs with the run loop and the
CPUID/MSR emulation (`vcpu.c`), the pending-vector set (`vintr.c`), the
seven system calls (`hvsys.c`), and the self-tests (`hvtest.c`).
Everything here is generic: the hardware lives behind
`kernel/include/arch/hv.h` (x86-64: `kernel/arch/x86_64/svm.c`,
`svm_npt.c`, `svm_run.S`), and VirtIO drivers are unrelated to it.

Documentation: `docs/kernel-services/virtualization/` (architecture,
design, api, invariants, testing).
