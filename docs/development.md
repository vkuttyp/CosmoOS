# Development environment and workflow

## Host versus target

Everything in this repository is cross-compiled. The **host** is the machine
running the compiler; the **target** is the machine the output runs on.
Nothing built for the target ever runs on the host, and no host property
(pointer size, endianness, ABI, libc) is assumed while building target
code. The build system encodes this split: `build/toolchain.mk` records
`HOST_OS`/`HOST_ARCH` for diagnostics only, and every compiler invocation
carries an explicit `--target=` triple.

```
HOST                      TARGET
MacBook (Apple Silicon)   x86-64 kernel + UEFI loader
  └─ Parallels              └─ QEMU (deterministic test platform)
       └─ ARM64 Linux VM         └─ real hardware (later)
            └─ clang/lld
```

## Primary host: ARM64 Linux VM under Parallels

Run once, inside the VM:

```sh
./scripts/setup-dev-linux.sh
make check-tools
```

The script installs `clang lld llvm make git mtools qemu-system-x86 ovmf
python3` with apt. Any Linux distribution works if the same tools are
present; only Debian/Ubuntu is scripted.

## Secondary host: macOS with Homebrew

Convenience only; the constitution names the Linux VM as primary.

```sh
./scripts/setup-dev-macos.sh
export PATH="/opt/homebrew/opt/make/libexec/gnubin:/opt/homebrew/bin:$PATH"
make check-tools
```

Notes:

- The build needs GNU make 4. Apple ships 3.81, which is why the gnubin
  path goes first (or call `gmake`).
- Apple's clang 21 can target both `x86_64-unknown-none-elf` and
  `x86_64-unknown-windows`. It does not ship `ld.lld`/`lld-link`; the
  Swift toolchain (`~/.swiftly/bin`) or Homebrew `llvm` provides them. To
  use Homebrew's complete toolchain set
  `LLVM_PREFIX="$(brew --prefix llvm)/bin/"`.
- UEFI firmware comes from Homebrew's QEMU:
  `/opt/homebrew/share/qemu/edk2-x86_64-code.fd`.

## Make targets

| Target | Effect |
|---|---|
| `all` | Build the kernel ELF, the UEFI loader, the user programs, and the kernel modules (default) |
| `kernel` | `out/<arch>-<build>/kernel/kernel.elf` plus `kernel.map` |
| `boot` | `out/<arch>-<build>/boot/BOOTX64.EFI` |
| `userland` | `out/<arch>-<build>/userland/init.elf`, the first user program (`userland/userland.mk`) |
| `modules` | `out/<arch>-<build>/modules/*.ko`: signed `ET_REL` kernel modules (`build/module.mk`, see `docs/kernel/module/`) |
| `image` | FAT32 disk image `out/<arch>-<build>/cosmoos.img` via `scripts/mkimage.sh`: loader, `\cosmo\kernel.elf`, `\cosmo\boot.tar` (the boot archive: `init` plus the modules, built by `scripts/mkbootarchive.py` into `out/<arch>-<build>/boot.tar`) |
| `run` | Boot the image in QEMU with serial on the terminal (`scripts/qemu-run.sh`). Since Phase 6 the machine also carries a virtio-blk scratch disk (`QEMU_TESTDISK`, default `out/<arch>-<build>/testdisk.img`, created as 8 MiB of zeros), a virtio-rng, and a virtio console whose output goes to `QEMU_VCON` (default `out/<arch>-<build>/vcon.log`) |
| `test` | Automated boot test: `tests/boot/run_boot_test.py`, PASS/FAIL exit status |
| `test-crash` | Build with `CRASH_TEST=1` and verify the harness detects a deliberate panic |
| `host-test` | Compile the memory, crypto, and module-validation algorithms natively with ASan/UBSan and run `tests/host/` (see below) |
| `analyze` | clang static analyzer over every target source; fails on any report |
| `reproducible` | Build twice into `out/repro-a` and `out/repro-b`, compare binaries |
| `check-tools` | Verify toolchain, image tools, QEMU, firmware, and both compiler targets |
| `compile-commands` | Write `compile_commands.json` for clangd using the real cross flags |
| `clean` | Remove `$(OUT)` |
| `help` | Print this table and the effective configuration |

### Host unit tests

`make host-test` (`tests/host/host.mk`) compiles kernel sources
unchanged with the *host* `clang` (no `--target`), links them with
`tests/host/harness.c` and `tests/host/shim_spinlock.c`, and runs the
resulting binaries under `-fsanitize=address,undefined`: `test_buddy`
and `test_slab` (`kernel/memory/buddy.c`, `slab.c`, `kmalloc.c`),
`test_crypto` (`kernel/security/sha512.c`, `ed25519.c` against the
FIPS and RFC 8032 vectors), and `test_modelf` (`kernel/module/modelf.c`
against synthetic module images, built with `-DMODELF_HOST_TEST=1`). The
architecture headers are replaced by `tests/host/shim/arch/*.h`; every
other header is the real kernel header. This requires a host compiler
with the ASan and UBSan runtimes: Apple's clang on macOS and the `clang`
package on Ubuntu both qualify. Set `HOST_CC` to override the compiler.
The tests live in `out/<arch>-<build>/host/` and can be run directly for a
single binary. See `docs/kernel/memory/testing.md` and
`docs/kernel/module/testing.md` for what they cover.

### Kernel modules

`build/module.mk` builds every module named in `MODULES`: its sources
are compiled with the kernel flags plus `-DCOSMO_MODULE_BUILD=1`, merged
with `ld.lld -r` into an `ET_REL` object, signed by `scripts/modsign.py`
with `MODSIGN_KEY` (default `tools/keys/cosmo-dev.key`, a development
key whose secret half is public), and checked by
`scripts/check-module-elf.py`. The results go into the boot archive at
the names listed in `MODULE_ARCHIVE_ENTRIES`: `modules/<name>.ko`
entries are loaded by the kernel at boot in archive order (list
dependencies first), `tests/<name>.ko` entries are fixtures loaded only
by the self-tests. The kernel's trusted key ring is generated from the
`.pub` files in `tools/keys/` into `out/<arch>-<build>/gen/keyring_builtin.c`
(`scripts/gen-keyring.py`). To add a module, add its sources under
`modules/<name>/` (or with their subsystem under `drivers/`), define
`MODULE_<name>_SRCS`, append `<name>` to `MODULES` and an entry to
`MODULE_ARCHIVE_ENTRIES`; the module itself declares `COSMO_MODULE(...)`
from `kernel/include/kernel/module.h`. Everything is in
`docs/kernel/module/api.md`.

The boot modules today are `hello` (`modules/hello/`) and the VirtIO
stack from `drivers/virtio/`: `virtio` (bus, virtqueues, virtio-pci
transport) followed by `virtio_blk`, `virtio_rng`, `virtio_console`,
which declare `virtio` as a dependency and are listed after it. Driver
headers shared between the kernel and modules live in
`drivers/include/drivers/` (`pci.h`, `virtio.h`); that directory is on
`KERNEL_CFLAGS`, so both kernel code and modules include them as
`<drivers/pci.h>`. See `docs/kernel/device/`, `docs/drivers/pci/`,
`docs/drivers/virtio/`.

### User programs

`userland/` holds programs that run in ring 3; `libc/include/` is the
start of the native C library (only the raw system-call wrappers so
far). They are compiled with the same clang and the kernel target
triple, but with user flags (`USER_CFLAGS` in `userland/userland.mk`):
no `-mcmodel=kernel`, no `-mno-red-zone`, `-fno-pic -fno-pie`, static
link at 4 MiB through `userland/init/user.ld` with
`-z noexecstack -z separate-code` so the ELF has separate r-x, r--,
rw- segments (the kernel refuses W+X segments and executable stacks).
`init` is delivered to the kernel as the `init` entry of the boot
archive (protocol v3, `docs/boot/design.md`); there is no filesystem
yet.

To write a new program: add a directory under `userland/`, start from
`userland/init/crt0.S` (reads `argc`/`argv` from the initial stack,
calls `main`, exits with its return value) and the wrappers in
`libc/include/cosmo/syscall.h`, add a rule modelled on `INIT_ELF` in
`userland.mk`, and add it to `BOOT_ARCHIVE_ENTRIES` in the top-level
`Makefile` (`name=path`). The system calls available are listed in
`docs/kernel/syscall/api.md`; the kernel starts only the archive entry
named `init` today, so a second program is reachable only by replacing
`init`.

## Variables

Set on the command line (`make BUILD=release test`) or in the environment.

| Variable | Default | Meaning |
|---|---|---|
| `ARCH` | `x86_64` | Target architecture; must match a directory under `kernel/arch/` and a file `build/arch/<ARCH>.mk` |
| `BUILD` | `debug` | `debug` (-O1 -g, `CONFIG_DEBUG=1`) or `release` (-O2 -g, `CONFIG_DEBUG=0`) |
| `OUT` | `out/$(ARCH)-$(BUILD)` | Output tree; never inside the source directories |
| `V` | `0` | `V=1` prints full command lines |
| `SELFTEST` | `1` for debug, `0` for release | Compile boot-time self-tests (`CONFIG_SELFTEST`) |
| `CRASH_TEST` | `0` | Compile a deliberate fault after the banner (`CONFIG_CRASH_TEST`) to exercise the panic path |
| `MODULE_SIG_ENFORCE` | `1` | `CONFIG_MODULE_SIG_ENFORCE`: refuse a kernel module without a valid signature from a key in `tools/keys/`. `0` loads unsigned modules with a warning and taints the kernel; a bad signature is refused either way |
| `MODSIGN_KEY` | `tools/keys/cosmo-dev.key` | Ed25519 seed used by `build/module.mk` to sign modules |
| `LLVM_PREFIX` | empty | Directory prefix (with trailing `/`) for `clang`, `ld.lld`, `lld-link`, `llvm-objcopy`, `llvm-nm`, `llvm-objdump` |
| `QEMU_MEM` | `256M` | Guest RAM |
| `QEMU_SMP` | `4` | Guest CPU count; `QEMU_SMP=1 make test` runs the suite on one CPU (the SMP tests then check their single-CPU behaviour) |
| `QEMU_ACCEL` | `tcg` | QEMU accelerator; `tcg` is the deterministic default, `kvm`/`hvf` are faster where available |
| `QEMU_EXTRA` | empty | Extra QEMU arguments appended verbatim (for example `-device virtio-net-pci` to see the transport bind an undriven device) |
| `QEMU_TESTDISK` | `<image dir>/testdisk.img` | Raw backing file of the virtio-blk scratch disk (`vda`); created as 8 MiB of zeros when missing. The boot test always uses a fresh `boot-test.log.testdisk.img` |
| `QEMU_VCON` | `<image dir>/vcon.log` | File the virtio console writes to (truncated at start). The boot test uses `boot-test.log.vcon` and checks it |
| `OVMF_CODE` | auto | Path to the UEFI firmware image; overrides `scripts/find-firmware.sh` |
| `SOURCE_DATE_EPOCH` | `0` | Exported to the compiler for reproducible builds |

## Running and reading the serial log

`make run` boots the image with `-serial stdio -display none`. All loader
and kernel output arrives on the terminal. `make test` captures the same
stream into `out/<arch>-<build>/boot-test.log`, creates a fresh scratch
disk `boot-test.log.testdisk.img` for the virtio-blk self-test, and
sends the virtio console to `boot-test.log.vcon`, which must contain the
`boot complete` line for the run to pass (the kernel log reaches the
serial port and the virtio console alike).

A successful debug boot looks like this (abridged):

```
cosmoboot-uefi v1
kernel: 114632 bytes read
archive: 240128 bytes read
kernel: virt 0xffffffff80000000-0xffffffff8001e000 -> phys 0xdd0f000, entry 0xffffffff80000000, 3 segments
paging: pml4 at 0xdd01000, 13/24 pool pages used, NX on
exiting boot services
memory map: 32 entries
  ...
jumping to kernel entry 0xffffffff80000000, info at 0xffff80000dcfc000
[DEBUG] x86: console up
...
CosmoOS kernel 0.0.1 (build 47a16b5)
Architecture: x86_64
Build: DEBUG
Boot: UEFI (cosmoboot-uefi v1, protocol v2)
CPU: QEMU Virtual CPU version 2.5+
Memory: 205 MiB usable in 32 regions, RAM ends at 256 MiB
[ INFO] pmm: 256 MiB RAM span, 246 MiB free, 9 MiB reserved, 0 MiB deferred, page array 2048 KiB
[ INFO] kmalloc: 15 size classes up to 8192 bytes, page path up to 4096 KiB
[ INFO] vmm: 246 MiB free after takeover, arena 0xffffc00000000000-0xffffe00000000000
[ INFO] acpi: XSDT rev 2, 6 tables, LAPIC at 0xfee00000, 1 CPUs, 1 IOAPICs, 5 overrides
[ INFO] irq: controllers up, 24 GSIs
[ INFO] timer: tsc at 996.000 MHz, tick 250 Hz
[ INFO] sched: policy 'rr', slice 10 ms, tick 250 Hz
[ INFO] interrupts enabled
[DEBUG] smp: CPU 1 (APIC 1) up
[DEBUG] smp: CPU 2 (APIC 2) up
[DEBUG] smp: CPU 3 (APIC 3) up
[ INFO] smp: 4 CPUs online of 4 reported
SELFTEST: printf           ... ok
...
SELFTEST: irq-route        ... ok
SELFTEST: thread           ... ok
...
SELFTEST: smp-mutex        ... ok
...
USERTEST: PASS
SELFTEST: process-user     ... ok
init: crashing on purpose
SELFTEST: process-fault    ... ok
SELFTEST: PASS (32 tests)
[ INFO] process: pid 3 'init' created, entry 0x400000, 3 segments
init: hello from user mode, pid 3, argc 1
init: nothing to supervise yet; exiting
[ INFO] process: pid 3 'init' exited with status 0 (5 syscalls)
[ INFO] init exited with status 0
[ INFO] boot complete; nothing more to do in this phase
[ INFO] shutdown: exit status 0
[ INFO] shutdown: halting CPU
```

Lines starting with `init:` and `USERTEST:` are written by the user
program through the `write` system call on handle 1; the self-test run
(`init --selftest`) and the crash run (`init --crash`) happen inside
the `process-user` and `process-fault` self-tests, and the plain run
is the real first process. Release builds skip the self-tests and show
only the plain run.

Lines with a `[LEVEL]` prefix come from `klog()`; unprefixed lines are
`kprintf()` output (banner, self-tests, panic dumps). Everything before
`jumping to kernel entry` is the loader. Before `exiting boot services`
the loader writes through the firmware console, which OVMF mirrors to the
serial port; afterwards it writes the UART directly.

A panic prints `KERNEL PANIC: <reason>`, the CPU, a register dump when a
trap frame is available, and a frame-pointer stack trace. Resolve the
addresses against `out/<arch>-<build>/kernel/kernel.map`, or with
`llvm-symbolizer --obj=out/<arch>-<build>/kernel/kernel.elf <addr>`.

A self-test that stops making progress for 8 s triggers the scheduler
hang watchdog, which prints `[WATCHDOG] no progress ...` followed by
every CPU's run queue (`need_resched`, `preempt`, `irq_depth`, `ticks`)
and every thread with its state and `waiting_on`. All CPUs on `idle`
with empty queues and a thread `blocked` on `-` is a lost wakeup; see
`docs/kernel/smp/testing.md` for the QEMU-monitor steps that go with it.

### Exit-code contract

QEMU is started with `-device isa-debug-exit,iobase=0xf4,iosize=0x04`.
The kernel writes one value to port `0xF4` via `arch_emulator_exit()`;
QEMU then exits with status `(value << 1) | 1`.

| Kernel writes | Meaning | QEMU exit status |
|---|---|---|
| `0x10` (`ARCH_EMULATOR_EXIT_SUCCESS`) | clean shutdown, self-tests passed | 33 |
| `0x11` (`ARCH_EMULATOR_EXIT_FAILURE`) | self-test failure or panic | 35 |

QEMU processes the exit asynchronously, so the kernel still prints
`shutdown: halting CPU` and halts before the emulator terminates. The
harness requires both the correct exit status and the expected log
markers (loader banner, kernel banner, `init: hello from user mode,
pid N`, `init exited with status 0`, `boot complete`, and, when a
`SELFTEST:` line appears, both `SELFTEST: PASS` and `USERTEST: PASS`),
and rejects any log containing `KERNEL PANIC`, `BUG:`, `SELFTEST: FAIL`,
or `cosmoboot: FATAL`. A non-zero exit status from `init` makes the
kernel report failure through the same port. On hardware or an emulator without the device the port
write is ignored and the kernel simply halts.

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request on the
GitHub-hosted `ubuntu-24.04` (x86-64) runner. It installs the same apt
packages as `scripts/setup-dev-linux.sh`, then runs `check-tools`, a
debug build with `test`, a release build with `test`, `host-test`,
`analyze`, `reproducible`, and `test-crash`. The x86-64 runner is a deliberate simplification: the
toolchain is host-agnostic, which is the point of using clang, but it
means CI does not currently exercise an ARM64 host. GitHub's hosted ARM64
runners are not enabled for this private repository; when they are, the
same job can run in a matrix over both.

## Workflow for a change

Constitution section 66 applies to every subsystem change. In practice:

1. Read the affected subsystem's `docs/<subsystem>/` files, especially
   `invariants.md`.
2. Work on a feature branch and open a pull request; Greptile reviews every
   PR on the repository.
3. Before pushing: `make test`, `make BUILD=release test`, `make host-test`,
   `make analyze`, `make reproducible`, `make test-crash`.
4. Update the subsystem documentation in the same PR.
