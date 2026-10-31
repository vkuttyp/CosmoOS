# Driver modules in CeeSafe

Roadmap 2.0 SA4: drivers are written in CeeSafe and built as ordinary
signed modules. The first is `virtio_rng` (`drivers/virtio/virtio_rng.cee`).

## What a CeeSafe module is made of

| Part | Where | What it is |
|---|---|---|
| The driver | `drivers/<bus>/<name>.cee` | probe, remove, the device's state, and two exports: `<name>_module_init` and `<name>_module_shutdown` |
| The device API | `drivers/ceesafe/driver.cee`, `virtio.cee` | the kernel's structs and functions a driver uses, imported from its headers (never redefined), with the safe wrappers over them |
| The kernel library | in ceesafec (`--kernel cosmoos`) | `KernelHeap`, `SpinLock`, `IrqSpinLock`, `Mutex`, `KRef` and the rest (`docs/build/ceesafe.md`) |
| The module entry | generated | `scripts/cee-gen.py` compiles the driver with the device API into one C file and appends its `COSMO_MODULE` line from `MODULES` |
| The runtime | generated | `drivers/ceesafe/ceesafe_rt_core.c`, CeeSafe's runtime, compiled once with the module flags and linked into every CeeSafe module |
| The floor | `drivers/ceesafe/driver_floor.c` | the little C a driver needs and CeeSafe cannot write: log lines (no variadic calls), `CONFIG_SELFTEST`, a device's name, and the module's `ceesafe_panic` |

The floor is the same for every driver; nothing driver specific is
written in C. The generated C lives under `out/cee` like the kernel's
(`build/cee.mk`); `make dist` puts it beside the source.

## The rules a driver follows

- **The kernel's structs are imported, never made.** A static the bus
  keeps (the driver's registration) comes from a header macro
  (`VIRTIO_DRIVER_INIT`); a static's initialiser cannot name a function,
  so `Virtio.register` sets probe and remove before registering.
- **A device's state is one CeeSafe value in a `KernelHeap` box**, made
  by probe and dropped by remove; its address is the device's `priv` in
  between. Probe undoes everything it did when it fails.
- **Callbacks are `@callback` functions**, safe functions with a
  `/// Safety:` comment that says why their raw pointers are valid.
- **State the queue callback shares goes behind a lock** in the state
  (`IrqSpinLock` where an interrupt's thread takes it). Effects make a
  sleep under a spin guard a compile error, as in the kernel.
- **Remove leaves nothing**: reset the device, free the queues, free DMA
  memory, drop the state. `make test-modcycle
  MODCYCLE_ENTRY=modules/<name>.ko` loads and unloads the module 1,000
  times and fails on anything left behind.

## Adding a driver

1. Copy `drivers/ceesafe-template/template.cee` to
   `drivers/<bus>/<name>.cee` and rename `template` everywhere.
2. Add it to `MODULES` in `scripts/cee-gen.py` (name, source, generated
   C, title, version, dependencies, capabilities).
3. In `build/module.mk`, add the name to `MODULES` and `CEE_MODULES`,
   set `MODULE_<name>_SRCS` to the generated C's path, and add the
   archive entry.
4. `make cee-check`, then the boot test and `test-modcycle` on both
   architectures. If the driver replaces a C one, delete the C.

The template is compiled by every `make cee-check`, so it keeps up with
the device API.
