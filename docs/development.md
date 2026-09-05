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
  `/opt/homebrew/share/qemu/edk2-x86_64-code.fd` and, for `ARCH=aarch64`,
  `edk2-aarch64-code.fd` (`scripts/find-firmware.sh <arch>` searches the
  usual locations; `OVMF_CODE` overrides).

## Make targets

| Target | Effect |
|---|---|
| `all` | Build the kernel ELF, the UEFI loader, the C library, the user programs, `pkg` and the ports repository, the Linux test programs, the virtualization guest images, and the kernel modules (default) |
| `kernel` | `out/<arch>-<build>/kernel/kernel.elf` plus `kernel.map` |
| `boot` | `out/<arch>-<build>/boot/BOOTX64.EFI` (`BOOTAA64.EFI` for `ARCH=aarch64`) |
| `libc` | `out/<arch>-<build>/libc/libc.a` and `libc/src/crt0.o`: the native C library (`libc/libc.mk`, `docs/libc/`) |
| `userland` | `out/<arch>-<build>/userland/*.elf`: init, the shell, the coreutils and system tools (`userland/userland.mk`, `docs/userland/`) |
| `pkg` | `out/<arch>-<build>/userland/pkg.elf`: the package manager, from `pkg/*.c` plus the kernel's SHA-512 and Ed25519 sources compiled for user mode (`pkg/pkg.mk`, `docs/pkg/`) |
| `ports` | `out/<arch>-<build>/pkg/repo/`: every recipe under `ports/*/port` cross-compiled by `tools/pkgbuild.py` into a signed `.cpk`, plus the signed `INDEX`; signed with `PKGSIGN_KEY` (default: the module signing key, see `SIGNING`); `SELFTEST=1` adds the `badsig` and `badsum` fixtures |
| `linux-tests` | `out/<arch>-<build>/tests/linux/`: `lxhello.elf` and `lxtest.elf` (freestanding raw-Linux-ABI programs, no crt0/libc, so no CosmoOS note) and, when `musl-gcc` is found or `MUSL_GCC=` names a compiler, `hello_musl` (`tests/linux/linux.mk`, `docs/compat/linux/testing.md`); they ride in the boot archive as `tests/linux/*` |
| `hv-guests` | `out/<arch>-<build>/tests/hv/*.bin`: the seven flat guest images for the virtualization tests (`tests/hv/*.S` assembled and linked with `--image-base=0 -Ttext=0x1000 --oformat=binary`, `tests/hv/hv.mk`, `docs/kernel-services/virtualization/testing.md`); in the boot archive as `tests/hv/*.bin` |
| `modules` | `out/<arch>-<build>/modules/*.ko`: signed `ET_REL` kernel modules (`build/module.mk`, see `docs/kernel/module/`) |
| `image` | FAT32 disk image `out/<arch>-<build>/cosmoos.img` via `scripts/mkimage.sh`: loader, `\cosmo\kernel.elf`, `\cosmo\boot.tar` (the boot archive: `init`, `bin/*`, `sbin/*` including `sbin/pkg`, `etc/*` including `etc/pkg/repos.conf` and `etc/pkg/keys/<key>.pub` (the first trusted public key, `PKG_TRUST_PUB`), `repo/*` (the ports repository, visible at `/boot/repo`) and the modules, built by `scripts/mkbootarchive.py` into `out/<arch>-<build>/boot.tar`) |
| `run` | Boot the image in QEMU with serial on the terminal (`scripts/qemu-run.sh`). Since Phase 6 the machine also carries a virtio-blk scratch disk (`QEMU_TESTDISK`, default `out/<arch>-<build>/testdisk.img`, created as 8 MiB of zeros), a virtio-rng, and a virtio console whose output goes to `QEMU_VCON` (default `out/<arch>-<build>/vcon.log`); since Phase 8 a virtio-net NIC on QEMU user-mode networking (`eth0` is `10.0.2.15`, gateway `10.0.2.2`) |
| `test` | Automated boot test: `tests/boot/run_boot_test.py`, PASS/FAIL exit status. Since Phase 8 it also runs the network harness (`tests/boot/nettest.py`): host port forwards to the guest's echo services and a guest-initiated connection back, see `docs/kernel-services/network/testing.md`. Since Phase 9 it types commands at the shell prompt through QEMU's serial stdin (`tests/boot/shelltest.py`) and requires their output; the boot ends when the harness types `exit 0` |
| `test-crash` | Build with `CRASH_TEST=1` and verify the harness detects a deliberate panic |
| `host-test` | Compile the memory, crypto, module-validation, cosmofs-layout, libc, package-parser, Linux-conversion, nested-page-table and AArch64-relocation algorithms natively with ASan/UBSan and run `tests/host/` (see below) |
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
against synthetic module images, built with `-DMODELF_HOST_TEST=1`), and
`test_cosmofs` (the cosmofs on-disk layout header: structure sizes,
inode-map arithmetic, extent mapping), and `test_libc` (the C library's
`printf.c`, `malloc.c` over a fake `mmap`, and `conv.c`, included with
renamed symbols so they coexist with the host libc; `docs/libc/testing.md`),
and `test_pkg` (the package manager's `manifest.c`, `version.c` and
`tar.c`: formats, constraints, path confinement, the ustar reader;
`docs/pkg/testing.md`), and `test_linux` (the Linux personality's pure
conversions in `compat/linux/convert.c`: open flags, `struct stat`,
wait status, sockaddr, `getdents64` records, `PROT_*`;
`docs/compat/linux/testing.md`), and `test_hv` (the SVM backend's pure
parts: the nested page table builder `kernel/arch/x86_64/svm_npt.c` over
the harness arena, the I/O exit decoder, the VMCB and UAPI layouts;
`docs/kernel-services/virtualization/testing.md`); `test_crypto` also
checks CRC32C. The architecture headers are replaced by `tests/host/shim/arch/*.h`; every
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
with `MODSIGN_KEY`, and checked by `scripts/check-module-elf.py`. No
private key is in the repository: with `SIGNING=dev` (the default) the
key is a per-machine pair that `scripts/devkey.sh` creates on first use
in `COSMO_KEYDIR` (`$HOME/.config/cosmoos/keys`), and only kernels built
on that machine trust it; `SIGNING=release` takes `MODSIGN_KEY` and
`KEYRING_PUBS` explicitly and generates nothing (`tools/keys/README.md`).
`scripts/check-secrets.sh` (run by `make check-tools`) fails if a key
file or a revoked public key is ever tracked. The results go into the
boot archive at the names listed in `MODULE_ARCHIVE_ENTRIES`:
`modules/<name>.ko` entries are loaded by the kernel at boot in archive
order (list dependencies first), `tests/<name>.ko` entries are fixtures
loaded only by the self-tests. The kernel's trusted key ring is
generated from `KEYRING_PUBS` (the developer public key plus any
`tools/keys/*.pub`) into `out/<arch>-<build>/gen/keyring_builtin.c`
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

### The C library and the user programs

`libc/` is the native C library (`docs/libc/`): `libc/include/` holds
the standard headers (`stdio.h`, `stdlib.h`, `string.h`, `unistd.h`,
`spawn.h`, `sys/wait.h`, ...) and `libc/src/` the implementation;
`libc/libc.mk` builds `libc.a` and `crt0.o` with the user flags
(`USER_CFLAGS`, defined there): the kernel target triple, no
`-mcmodel=kernel`, no `-mno-red-zone`, `-fno-pic -fno-pie
-mgeneral-regs-only` (no floating point: the kernel does not save FPU
state for user threads yet), `-I libc/include -I kernel/include` (the
UAPI header). `string.c` is compiled with `-fno-builtin`.

`userland/` holds the programs (`docs/userland/`): `init/`, `shell/`
(`sh`), `coreutils/` (echo cat ls cp mv rm mkdir rmdir pwd true false
sleep), `system/` (mount umount ps kill dmesg sysctl), `etc/` (`rc`,
`rc.test`). `userland/userland.mk` links every program as `crt0.o
<name>.o libc.a` at 4 MiB through `userland/user.ld` with `-z
noexecstack -z separate-code` (separate r-x, r--, rw- segments; the
kernel refuses W+X segments and executable stacks) and generates the
boot archive entries: `init=` (the kernel starts that entry),
`bin/<name>` and `sbin/<name>` (executable, mode 0755 in the ramfs),
`etc/rc`, and `etc/rc.test` only when `SELFTEST=1`. The kernel's ramfs
places them at `/bin`, `/sbin`, `/etc`, keeps everything else under
`/boot` (`/boot/init`, `/boot/modules/*.ko`), and creates `/tmp`, `/mnt`,
`/dev` (`docs/kernel-services/vfs/api.md`). The scratch virtio disk can
carry a cosmofs (`mount vda /mnt cosmofs`); the kernel formats it during
the self-tests.

At boot init runs `/etc/rc` and then `sh` on the console; the boot ends
when that shell exits (`exit`). `make run` gives an interactive
`cosmo$ ` prompt on the terminal; `make test` types a scripted session
into it.

To write a new program: add `userland/<family>/<name>.c` with a `main`,
add `<name>` to `USER_BIN_PROGRAMS` or `USER_SBIN_PROGRAMS` and a
`PROG_DIR_<name> := <family>` line in `userland/userland.mk`; the link
rule and the archive entry follow. The library's interface is in
`docs/libc/api.md`, the system calls in `docs/kernel/syscall/api.md`.

### Packages

`ports/<dir>/port` is a recipe (`docs/pkg/api.md`); `make ports` builds
every recipe into `out/<arch>-<build>/pkg/repo/` with
`tools/pkgbuild.py` and signs packages and `INDEX` with `PKGSIGN_KEY`.
The repository rides in the boot archive as `repo/` and appears at
`/boot/repo`, which `/etc/pkg/repos.conf` names; `/etc/pkg/keys/`
holds the accepted public keys. On the target: `pkg update`, `pkg
install fortune`, `pkg list`, `pkg remove fortune`. In self-test builds
`/etc/rc.test` runs the whole flow, including the refused `badsig` and
`badsum` fixtures and an upgrade of `hello` from 1.0 to 1.1; the boot
harness requires the resulting lines (`PKGTEST_MARKERS` in
`tests/boot/run_boot_test.py`) and types `pkg install hello && hello &&
pkg list` at the prompt in every build. To add a package: a directory
under `ports/` with a `port` file and its sources; `make ports` picks it
up and `make reproducible` should still say yes.

### Virtual machines

The kernel is a hypervisor since Phase 12 (`kernel-services/virtualization/`,
`docs/kernel-services/virtualization/`), on AMD-V with nested paging.
QEMU's default CPU model in `scripts/qemu-run.sh` is
`qemu64,+nx,+svm,+npt` so TCG emulates the extension (`QEMU_CPU`
overrides it; `host` with `kvm`/`hvf` needs nested virtualization on the
host). The boot archive carries `tests/hv/*.bin`; eight self-tests run
guests from them, and `/etc/rc.test` runs `vmctl probe`, `vmctl run
/boot/tests/hv/guest_pio.bin` and `vmctl info`, printing `HVTEST: PASS`,
which the harness requires on x86-64; `selftest: hv: skipped` and
`HVTEST: skipped` (what a CPU model without SVM produces) are forbidden
markers there. On `ARCH=aarch64` there is no backend yet: the guest
images are not built, `vmctl probe` fails and the harness requires
`HVTEST: skipped` instead. To try a
guest by hand: write a flat real-mode program that talks to port 0xE9
and halts, add it to `HV_GUESTS` in `tests/hv/hv.mk` (or copy the binary
onto the cosmofs disk), boot, and `vmctl run /path/to/image`.

### Linux programs

A static x86-64 ELF without the CosmoOS ABI note (which `crt0.S` emits
for every native program) runs under the Linux personality
(`compat/linux/`, `docs/compat/linux/`). The boot archive carries
`tests/linux/lxhello` and `lxtest` (built by the project toolchain
against the raw Linux ABI) and, when a musl compiler is available,
`hello_musl` (`musl-gcc -static`); `/etc/rc.test` runs them and the
harness requires `hello from linux abi`, `LINUXTEST: PASS` and, when
the build had musl (`HAVE_MUSL=1`, passed by `make test`), `hello from
musl on Linux x86_64 (pid N)`. CI installs `musl-tools`; on macOS use a
wrapper that compiles in an Alpine container: `make
MUSL_GCC=/path/to/musl-gcc-docker.sh test` (recipe in
`docs/compat/linux/testing.md`). The Linux test programs are x86-64
only; on `ARCH=aarch64` the personality has no system-call table yet,
`rc.test` prints `LINUXTEST: skipped` and the harness requires that
line. To try a Linux program by hand: build
it `-static` with musl, add a `tests/linux/<name>=<path>` archive entry
(or copy it onto the cosmofs disk), boot, and run it from the shell;
unimplemented calls show up as `linux: pid N: unimplemented system call
NR` in the debug log.

## Variables

Set on the command line (`make BUILD=release test`) or in the environment.

| Variable | Default | Meaning |
|---|---|---|
| `ARCH` | `x86_64` | Target architecture, `x86_64` or `aarch64`; must match a directory under `kernel/arch/` and a file `build/arch/<ARCH>.mk`. `ARCH=aarch64` builds `BOOTAA64.EFI`, boots QEMU's `virt` machine (`docs/kernel/arch/aarch64/testing.md`) and leaves out the x86-only `tests/linux` and `tests/hv` fixtures |
| `BUILD` | `debug` | `debug` (-O1 -g, `CONFIG_DEBUG=1`) or `release` (-O2 -g, `CONFIG_DEBUG=0`) |
| `OUT` | `out/$(ARCH)-$(BUILD)` | Output tree; never inside the source directories |
| `V` | `0` | `V=1` prints full command lines |
| `SELFTEST` | `1` for debug, `0` for release | Compile boot-time self-tests (`CONFIG_SELFTEST`) |
| `CRASH_TEST` | `0` | Compile a deliberate fault after the banner (`CONFIG_CRASH_TEST`) to exercise the panic path |
| `MODULE_SIG_ENFORCE` | `1` | `CONFIG_MODULE_SIG_ENFORCE`: refuse a kernel module without a valid signature from a key in the kernel's ring. `0` loads unsigned modules with a warning and taints the kernel; a bad signature is refused either way |
| `SIGNING` | `dev` | `dev`: sign with a per-machine developer key created on first use outside the tree; `release`: `MODSIGN_KEY` and `KEYRING_PUBS` must be given, nothing is generated (`tools/keys/README.md`) |
| `COSMO_KEYDIR` | `$HOME/.config/cosmoos/keys` | Where `SIGNING=dev` keeps `dev.key` (0600) and `dev.pub`; never inside the repository |
| `MODSIGN_KEY` | `$(COSMO_KEYDIR)/dev.key` | Ed25519 seed used by `build/module.mk` to sign modules |
| `KEYRING_PUBS` | `$(COSMO_KEYDIR)/dev.pub` + `tools/keys/*.pub` | Public keys compiled into the kernel's trusted ring |
| `PKGSIGN_KEY` | `$(MODSIGN_KEY)` | Key that signs packages and the `INDEX` |
| `PKG_TRUST_PUB` | first of `KEYRING_PUBS` | Public key shipped as `/etc/pkg/keys/<name>.pub` for `pkg` |
| `LLVM_PREFIX` | empty | Directory prefix (with trailing `/`) for `clang`, `ld.lld`, `lld-link`, `llvm-objcopy`, `llvm-nm`, `llvm-objdump` |
| `QEMU_MEM` | `256M` | Guest RAM |
| `MUSL_GCC` | `musl-gcc` if found | A musl C compiler used to build `tests/linux/hello_musl`; empty skips it and sets `HAVE_MUSL=0` for the harness |
| `QEMU_SMP` | `4` | Guest CPU count; `QEMU_SMP=1 make test` runs the suite on one CPU (the SMP tests then check their single-CPU behaviour) |
| `QEMU_ACCEL` | `tcg` | QEMU accelerator; `tcg` is the deterministic default, `kvm`/`hvf` are faster where available |
| `QEMU_CPU` | `qemu64,+nx,+svm,+npt` (x86-64), `cortex-a72` (aarch64) | QEMU CPU model; the x86-64 default gives TCG guests AMD-V with nested paging for the virtualization tests. Use `host` with `kvm`/`hvf` (nested virtualization then depends on the host). On aarch64 `max` adds PAN and is also supported |
| `QEMU_EXTRA` | empty | Extra QEMU arguments appended verbatim (for example `-fw_cfg name=opt/cosmo/ipv4,string=10.0.2.20/24,10.0.2.2` to give `eth0` a static address) |
| `QEMU_TESTDISK` | `<image dir>/testdisk.img` | Raw backing file of the virtio-blk scratch disk (`vda`); created as 8 MiB of zeros when missing. The boot test always uses a fresh `boot-test.log.testdisk.img` |
| `QEMU_VCON` | `<image dir>/vcon.log` | File the virtio console writes to (truncated at start). The boot test uses `boot-test.log.vcon` and checks it |
| `QEMU_NET_HOSTFWD` | empty | Comma-separated QEMU `hostfwd` rules for the user-mode netdev, e.g. `tcp:127.0.0.1:2007-:7,udp:127.0.0.1:2008-:7`. The boot test sets it to reach the guest's echo services |
| `QEMU_FWCFG_NETTEST` | empty | Value of the fw_cfg item `opt/cosmo/nettest` (`tcp=<hostport>`); its presence makes the `net-harness` self-test run and connect back to the host. The boot test sets it |
| `QEMU_PCAP` | empty | When set, QEMU records every frame on the guest NIC into this pcap file (`filter-dump`), for Wireshark or `tcpdump -r` |
| `OVMF_CODE` | auto | Path to the UEFI firmware image; overrides `scripts/find-firmware.sh <arch>`. On aarch64 the image is padded to the 64 MiB `virt` flash size into `out/<arch>-<build>/firmware-aarch64.fd` |
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
SELFTEST: linux-elf        ... ok
SELFTEST: hv-probe         ... ok
...
SELFTEST: hv-guest-spin    ... ok
SELFTEST: process-fault    ... ok
SELFTEST: PASS (70 tests)
[ INFO] process: pid 8 'init' created, entry 0x400000, 3 segments
init: CosmoOS userland, pid 8
CosmoOS userland ready
...
SHTEST: PASS
init: rc exited with status 0
cosmo$ echo interactive-ok
interactive-ok
...
cosmo$ exit 0
init: shell exited with status 0
[ INFO] process: pid 8 'init' exited with status 0 (61 syscalls)
[ INFO] init exited with status 0
[ INFO] boot complete; nothing more to do in this phase
[ INFO] shutdown: exit status 0
[ INFO] shutdown: halting CPU
```

Lines starting with `init:`, `USERTEST:` and `SHTEST:` are written by
user programs through the `write` system call on handle 1; the `cosmo$ `
prompts come from the shell and the text after them is what the harness
typed, echoed by the console tty; the self-test run
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
markers (loader banner, kernel banner, `init: CosmoOS userland, pid N`,
`CosmoOS userland ready`, `init: rc exited with status 0`,
`interactive-ok`, `init: shell exited with status 0`, `init exited with
status 0`, `boot complete`, the shell harness's per-command patterns
(`tests/boot/shelltest.py`), and, when a `SELFTEST:` line appears,
`SELFTEST: PASS`, `USERTEST: PASS`, `SHTEST: PASS` and the package
markers `pkg: index updated`, `pkg: installing fortunes-1.0`, `hello,
world (hello 1.0)` then `(hello 1.1)`, `pkg: verify: 0 problems`, and
the Linux markers `hello from linux abi`, `LINUXTEST: PASS` and, with
`HAVE_MUSL=1`, the musl line),
and rejects any log containing `KERNEL PANIC`, `BUG:`, `SELFTEST: FAIL`,
or `cosmoboot: FATAL`. A non-zero exit status from `init` makes the
kernel report failure through the same port. On hardware or an emulator without the device the port
write is ignored and the kernel simply halts.

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request on the
GitHub-hosted `ubuntu-24.04` (x86-64) runner, inside a `debian:trixie`
container (QEMU 10, `libclang-rt-dev` for the sanitizers,
`qemu-system-arm` and `qemu-efi-aarch64` for the AArch64 target), as a
matrix over `arch: [x86_64, aarch64]`. Each job runs `check-tools`, a
debug build with `test`, a release build with `test`, `host-test`,
`analyze`, `reproducible`, and `test-crash` with `ARCH=<arch>`, and
uploads serial logs and images per architecture. Both targets are
cross-compiled and emulated under TCG on the x86-64 runner: the toolchain
is host-agnostic, which is the point of using clang, but CI does not
exercise an ARM64 host. GitHub's hosted ARM64 runners are not enabled
for this private repository; when they are, the same matrix can add the
host dimension.

## Workflow for a change

Constitution section 66 applies to every subsystem change. In practice:

1. Read the affected subsystem's `docs/<subsystem>/` files, especially
   `invariants.md`.
2. Work on a feature branch and open a pull request; Greptile reviews every
   PR on the repository.
3. Before pushing: `make test`, `make BUILD=release test`, `make host-test`,
   `make analyze`, `make reproducible`, `make test-crash`.
4. Update the subsystem documentation in the same PR.

The virtualization tests need QEMU 9.2 or newer (TCG nested paging for
paging-off guests); with an older QEMU the kernel disables the backend at
boot and the boot test fails on the skipped guest tests. CI runs in a
Debian trixie container for this reason.
