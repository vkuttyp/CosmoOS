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

The script installs `clang lld llvm libclang-rt-dev make git mtools
musl-tools qemu-system-x86 ovmf qemu-system-arm qemu-efi-aarch64 python3`
with apt, the same set CI installs. Any Linux distribution works if the same tools are
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
| `run` | Boot the image in QEMU with serial on the terminal (`scripts/qemu-run.sh`). Since Phase 6 the machine also carries a virtio-blk scratch disk (`QEMU_TESTDISK`, default `out/<arch>-<build>/testdisk.img`, created as 8 MiB of zeros), a virtio-rng, and a virtio console whose output goes to `QEMU_VCON` (default `out/<arch>-<build>/vcon.log`); since Phase 8 a virtio-net NIC on QEMU user-mode networking (`eth0` is `10.0.2.15`, gateway `10.0.2.2`); and since the virtio-removal unit a second, 4 MiB virtio-blk that exists to be removed (`QEMU_RMDISK`) |
| `test` | Automated boot test: `tests/boot/run_boot_test.py`, PASS/FAIL exit status. Since Phase 8 it also runs the network harness (`tests/boot/nettest.py`): host port forwards to the guest's echo services and a guest-initiated connection back, see `docs/kernel-services/network/testing.md`. Since Phase 9 it types commands at the shell prompt through QEMU's serial stdin (`tests/boot/shelltest.py`) and requires their output; the boot ends when the harness types `exit 0` |
| `test-entropy` | Milestone M1's acceptance boots (docs/kernel/security/design.md §6), one entropy source each: without the virtio-rng (`QEMU_RNG=0`) on the default CPU model with `random.jitter=off` on the command line (no source: the pool must stay unseeded, `getrandom(GRND_NONBLOCK)` is `-EAGAIN`, an encrypted pool is refused, the boot completes); on AArch64 the same without `random.jitter=off` (seeded by the jitter source alone); on `HWRNG_CPU` (`qemu64,+rdrand,+rdseed`; `neoverse-v1`) with `QEMU_HWRNG=1` and `random.firmware=off` (seeded by the CPU alone); and with the virtio-rng and `random.firmware=off random.jitter=off` (seeded by the device alone). Logs `boot-test-noentropy.log`, `boot-test-jitter.log` (AArch64), `boot-test-cpurng.log`, `boot-test-devrng.log` |
| `test-platforms` | AArch64 platform discovery (`tests/boot/platform_test.py`; `docs/kernel/arch/aarch64/testing.md`, "make test-platforms"): the image under six QEMU configurations that differ from virt's defaults (`gicv2`, `gicv3`, `lowmem` with `highmem=off`, `rndr` on `neoverse-v1`, `nornd` on `cortex-a72`, both without virtio-rng, and `noserial`: `-serial none` and `serial=none` on a re-signed command line); each must reach the userland and print the platform line with what that machine has. Logs in `$(OUT)/test-platforms/<variant>/`. Prints that it is AArch64 only and succeeds on x86-64. CI job `platforms` (`BUILD=release`) |
| `test-crash` | Build with `CRASH_TEST=1` and verify the harness detects a deliberate panic |
| `host-test` | Compile the memory, crypto, module-validation, cosmofs-layout, libc, package-parser, Linux-conversion, nested-page-table and AArch64-relocation algorithms natively with ASan/UBSan and run `tests/host/` (see below) |
| `host-test-fetch` | `cosmo-fetch` (the HTTPS client `pkg` and `cosmo-upgrade` run) built for the host on the target's mbedTLS release and configuration, with ASan/UBSan, and run against local TLS servers: certificate, pin, plain-HTTP, redirect, range and size cases (`tests/host/test_fetch.py`; `ports/mbedtls/README.md`, "Tested by"). Needs the pinned mbedTLS archive and OpenSSL 3.4 or later |
| `fuzz` | Build the host fuzz targets in `tests/fuzz/` (module ELF, user ELF, package formats, Linux ABI conversions, virtqueue, cosmofs images) under ASan/UBSan and run each over its seeds and `FUZZ_RUNS` (default 20000) seeded mutations; `FUZZ_ENGINE=libfuzzer` links libFuzzer instead; `fuzz-build` builds only (`docs/verification/`) |
| `analyze` | clang static analyzer over every target source; fails on any report |
| `reproducible` | Build twice into `out/repro-a` and `out/repro-b`, compare binaries, the image and the release set |
| `release` | The signed release set for `ARCH` (always `BUILD=release`) in `out/release/<arch>/`, with `SHA256SUMS` ("Releases" below) |
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
`-mcmodel=kernel`, no `-mno-red-zone`, `-fno-pic -fno-pie` and **no**
`-mgeneral-regs-only` (user code uses the floating-point and vector
registers; the kernel saves them per thread and still abstains itself,
which `scripts/check-fpregs.sh` enforces), `-I libc/include -I kernel/include` (the
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
| `VERIFY_BOOT` | `1` | verified boot (docs/boot/design.md): the UEFI loader refuses to boot unless `\cosmo\manifest` is signed by a key in its ring and the kernel, archive and command line match it. `0` builds a loader that checks nothing, in its own tree `out/<arch>-<build>-noverify`; the kernel then says `UNVERIFIED BOOT`. No runtime switch |
| `CMDLINE` | `root=boot-disk` | the text of `\cosmo\cmdline`, signed with the kernel: changing it means building the image again |
| `SIGNING` | `dev` | `dev`: sign with a per-machine developer key created on first use outside the tree; `release`: `MODSIGN_KEY` and `KEYRING_PUBS` must be given, nothing is generated (`tools/keys/README.md`) |
| `COSMO_KEYDIR` | `$HOME/.config/cosmoos/keys` | Where `SIGNING=dev` keeps `dev.key` (0600) and `dev.pub`; never inside the repository |
| `MODSIGN_KEY` | `$(COSMO_KEYDIR)/dev.key` | Ed25519 seed used by `build/module.mk` to sign modules |
| `KEYRING_PUBS` | `$(COSMO_KEYDIR)/dev.pub` + `tools/keys/*.pub` | Public keys compiled into the kernel's trusted ring |
| `PKGSIGN_KEY` | `$(MODSIGN_KEY)` | Key that signs packages and the `INDEX` |
| `PKG_TRUST_PUB` | first of `KEYRING_PUBS` | Public key shipped as `/etc/pkg/keys/<name>.pub` for `pkg` |
| `LLVM_PREFIX` | empty | Directory prefix (with trailing `/`) for `clang`, `ld.lld`, `lld-link`, `llvm-objcopy`, `llvm-nm`, `llvm-objdump` |
| `QEMU_MEM` | `256M` | Guest RAM |
| `MUSL_GCC` | `musl-gcc` if found | A musl C compiler used to build `tests/linux/hello_musl`; empty skips it and sets `HAVE_MUSL=0` for the harness |
| `QEMU_SMP` | `4` | Guest CPU count; `QEMU_SMP=1 make test` runs the suite on one CPU (the SMP tests then check their single-CPU behaviour); `make test-smp2` runs it on two, as CI does |
| `BOOT_TIMEOUT` | `300` on AArch64 (240 until 2026-10-15), `240` on x86-64 (180 until 2026-10-07) | Whole-boot timeout in seconds for `make test`, `test-harness-retry`, `test-chaos` (its own 240 s until 2026-10-17), and `test-gic`/`test-guard`/`test-smp2` (through `test`): a hang guard, not a performance budget. `test-crash` and `test-wxn` keep the runner's 180 s. Rationale and CI measurements are in the Makefile comment |
| `QEMU_ACCEL` | `tcg` | QEMU accelerator; `tcg` is the deterministic default, `kvm`/`hvf` are faster where available |
| `QEMU_CPU` | `qemu64,+nx,+svm,+npt` (x86-64), `cortex-a72` (aarch64) | QEMU CPU model; the x86-64 default gives TCG guests AMD-V with nested paging for the virtualization tests. Use `host` with `kvm`/`hvf` (nested virtualization then depends on the host). On aarch64 `max` adds PAN and is also supported |
| `QEMU_EXTRA` | empty | Extra QEMU arguments appended verbatim (for example `-fw_cfg name=opt/cosmo/ipv4,string=10.0.2.20/24,10.0.2.2` to give `eth0` a static address) |
| `QEMU_WRAP` | empty | A command QEMU is run through, word-split: `QEMU_WRAP="taskpolicy -b" make test` pins the whole QEMU process to a macOS host's efficiency cores, which the host-modes investigation used to test where the two benchmark modes come from ("Benchmark runs" below) |
| `QEMU_SERIAL` | `stdio` | The `-serial` backend on both architectures; `none` leaves the machine with no serial port attached (`make test-platforms`, variant `noserial`) |
| `QEMU_VIRT_OPTS` | empty | AArch64: appended to the `-machine virt,...` options, for example `,highmem=off` (`make test-platforms`, variant `lowmem`) |
| `QEMU_TESTDISK` | `<image dir>/testdisk.img` | Raw backing file of the virtio-blk scratch disk (`vda`); created as 8 MiB of zeros when missing. The boot test always uses a fresh `boot-test.log.testdisk.img` |
| `QEMU_RMDISK` | `<image dir>/rmdisk.img` | Raw backing file of the second virtio-blk, the disk `virtio-remove-inflight` removes and re-probes (`vdb` on q35, `vdc` on `virt`); created as 4 MiB of zeros when missing. `0` leaves the device out and the test skips. The boot test uses a fresh `boot-test.log.rmdisk.img` |
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
USERTEST: section fs 128 ms
...
USERTEST: section svc 1386 ms
USERTEST: section syscalls 8 ms
USERTEST: sections 10, total 3610 ms
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
user programs through the `write` system call on handle 1 -- the
`USERTEST: section ...` lines are the user-mode suite timing itself,
which the harness turns into its `user-mode suite ... slowest: ...`
line (invariant **F13**); the `cosmo$ `
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

A boot whose log shows no loader banner (`cosmoboot-uefi`) and whose QEMU
has not exited within 30 s is a firmware-handover stall: every harness
boot kills it and starts it once more, says "firmware stall, retried", and
fails on a second stall (`tests/boot/firmware_stall.py`;
`docs/testing/flakes.md`, "firmware handover"). Once the banner is out,
nothing is retried.

A panic prints `KERNEL PANIC: <reason>`, the CPU, a register dump when a
trap frame is available, and a frame-pointer stack trace. Resolve the
addresses against `out/<arch>-<build>/kernel/kernel.map`, or with
`llvm-symbolizer --obj=out/<arch>-<build>/kernel/kernel.elf <addr>`.
A failing `make test` does this itself: its `---- symbols` table resolves
every stack-trace and `pc` address in the log. An address in a module's
text (marked `(outside kernel text)` in a trace) resolves against the
module's own object in `out/<arch>-<build>/modules/`, through the
`module: base <name> text <addr> size <n> ...` line the loader prints for
each module it loads. It is shown as `vblk_done [virtio_blk]` with its
source line. To resolve one by hand, subtract the base and give the
offset to `llvm-symbolizer --obj=out/<arch>-<build>/modules/<name>.ko.unsigned`.
That is exact for a module with one text section; for more than one, see
`placed_object` in `tests/boot/run_boot_test.py`.

A self-test that stops making progress for its budget (8 s unless the
boot's `SELFTEST: budgets` line says otherwise) triggers the scheduler
hang watchdog. The runner arms it afresh before each test, so the dump
belongs to the test that went quiet, and a passing boot has none. It prints `[WATCHDOG] no progress ...` followed by
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

AArch64 carries the same values through semihosting (`SYS_EXIT_EXTENDED`,
so QEMU exits 33 or 35 there too). Where semihosting is absent -- a QEMU
without `-semihosting-config` (`QEMU_SEMIHOSTING=0`), Parallels Desktop,
other firmware -- the kernel powers off through PSCI `SYSTEM_OFF` and
QEMU exits **0** whatever the boot's result, so a harness without
semihosting reads the outcome from the log (`shutdown: exit status N`;
`make test-platforms`' `psci-exit` and `psci-poweroff`).

## Benchmark runs

A debug boot's benchmark lines (`net-nicbench`, `irqrestore-bench`,
`fpu-bench`, `net-bench`, the lockdep benches) are a draw from the host as
much as a measurement of the kernel, and on a macOS host with performance
and efficiency cores two things are known to move them
(`docs/audit/2026-10-06-flake-triage-report.md`, §§1 and 7):

- **QEMU demoted below the host's other work.** Under the background
  policy (`taskpolicy -b`) every number is about four times slower and
  the tick-rate self-tests fail. Under the utility QoS clamp *together
  with* a lowered priority (`taskpolicy -c utility` with QEMU at nice 5,
  which is what zsh's default `BG_NICE` does to a job started with `&`)
  a third of the NIC rounds read `net-nicbench`'s "ARP normal, UDP ten
  times slower" mode, with the e1000e driver share at 75 % instead of
  40 %: QEMU's synchronous host transmit running on a slow core. Either
  alone did not reproduce it in ten boots. Before a measurement, check
  the QEMU process is at its default priority --
  `ps -o pid,pri,nice,comm -p $(pgrep -f qemu-system)` should read pri 31
  nice 0; `taskpolicy -b` shows as pri 4 nice 5, the clamp as pri 20, a
  zsh background job as nice 5 -- and start boot loops with
  `setopt NO_BG_NICE` or from the harness rather than with `&`. To
  reproduce the slow mode deliberately:
  `QEMU_WRAP="taskpolicy -c utility nice -n 5" make test`.
- **Lockdep.** A debug boot has `LOCKDEP=1` by default, and it costs about
  half of every benchmark number (x86-64 `net-nicbench` UDP 12.7-13 k
  sends/s against 28-36 k at `LOCKDEP=0`; the irqrestore pair 400 against
  170-215 ns). Compare like with like, and say which in the report; a
  `LOCKDEP=0` build goes in its own `OUT`.
- **The two-mode x86-64 and AArch64 benchmark readings** (`irqrestore-bench`
  at about 330 or about 450 ns a pair, `fpu-bench` following;
  `docs/testing/flakes.md`) are *not* core placement: the clamp did not
  select the mode and the certain demotion moves the number fourfold, not
  1.3x. Alternate enough boots of each tree to see both modes.

A CPU-heavy foreground job beside QEMU (four busy processes on eight
cores) halves every number together, ARP with UDP, and puts the suite
past its whole-boot budget: a loaded host, not a mode (report §7.3).

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request on the
GitHub-hosted `ubuntu-24.04` (x86-64) runner. Its first job, `image`,
makes sure the project's toolchain image exists: `ci/Dockerfile` (Debian
trixie, pinned by digest, with QEMU 10, `libclang-rt-dev` for the
sanitizers, `qemu-system-arm` and `qemu-efi-aarch64` for the AArch64
target) is built and pushed to `ghcr.io/vkuttyp/cosmoos-ci` by
`scripts/ci-image.sh` only when the Dockerfile's SHA-256 is not already
a tag there, and the job outputs the image's digest reference. Every
other job runs in that image, pulled from this repository's GitHub
Container Registry by digest (roadmap M6: pulling Debian from Docker Hub,
then from ECR Public, on every job failed whole runs at "Initialize
containers" on rate limits). The litmus job uses the Dockerfile's
`litmus` target (Debian sid with herdtools7) the same way. To change the
toolchain, edit `ci/Dockerfile`: the next run builds the new image, and
its packages are listed in the image at
`/usr/share/cosmoos-ci/packages.txt`.

The `build-and-boot` job is a matrix over `arch: [x86_64, aarch64]`. Each
runs `check-tools`, a debug build with `test`, a release build with
`test`, `host-test`, `analyze`, `reproducible`, and `test-crash` with
`ARCH=<arch>`; the x86-64 job also runs `fuzz` (the targets are host
binaries, the same on either job), and each uploads serial logs and images per architecture.
The `platforms` job builds the AArch64 release image and runs `make
ARCH=aarch64 BUILD=release test-platforms`, uploading the boots' logs on
failure. Both targets are
cross-compiled and emulated under TCG on the x86-64 runner: the toolchain
is host-agnostic, which is the point of using clang, but CI does not
exercise an ARM64 host. GitHub's hosted ARM64 runners are not enabled
for this private repository; when they are, the same matrix can add the
host dimension.

## Releases

`make release` (roadmap M6) builds one architecture's release set with
`BUILD=release` and copies it, with `scripts/mkrelease.py`, to
`out/release/<arch>/` (`RELEASE_DIR` overrides): `cosmoos-<arch>.img`
(the signed live image), `cosmoos-<arch>-BOOTX64.EFI` (`BOOTAA64.EFI`),
`cosmoos-<arch>-kernel.elf`, `cosmoos-<arch>-boot.tar`,
`cosmoos-<arch>-cmdline`, `cosmoos-<arch>-manifest`,
`cosmoos-<arch>-cosmo-upgrade` (the release's `cosmo-upgrade`, which an
installed system runs to move to it; shell completeness S7),
`cosmoos-<arch>-cosmo-upgrade.cpk` (a package of `cosmo-upgrade` and
`cosmo-verify` signed with `PKGSIGN_KEY`, which v1.0.3's `pkg install -n`
checks; version from the build id), `SHA256SUMS` (`sha256sum -c` format)
and `SHA256SUMS.sig` (its detached signature with `MODSIGN_KEY`: the
`COSMOSIG` trailer alone, `scripts/modsign.py sign-detached`). It is signed with whatever `SIGNING` selects:
the developer key by default; the release workflow passes
`SIGNING=release MODSIGN_KEY=<the release key> KEYRING_PUBS=tools/keys/<release>.pub`.

Every file of the set is deterministic: `make reproducible` builds the
tree twice and compares the kernel, loader, archive, modules, programs,
packages, the command line, the manifest, the image and the set's
`SHA256SUMS`; CI's `release-set` job runs it with `BUILD=release` and
uploads one `make release`. The release workflow builds each
architecture twice more, from the tag, in two fresh containers, and
requires the two `SHA256SUMS` to be equal before it publishes.

**The release workflow** (`.github/workflows/release.yml`) runs on a tag
`v*`:

1. `image`: the CI images for the tag's `ci/Dockerfile` (as in CI).
2. `build` (x86-64 and AArch64, copies `a` and `b`, four jobs): the
   release key from the secret `COSMOOS_RELEASE_KEY` into a 0600 file
   (`scripts/release-key.sh`, which refuses a key whose public half is
   not the one `tools/keys/*.pub`), then `make SIGNING=release
   MODSIGN_KEY=<it> KEYRING_PUBS=tools/keys/<release>.pub release`.
   Before building, `scripts/release-tag.sh` fetches the tag's own ref:
   `actions/checkout` fetches a pushed tag by its commit, which leaves an
   annotated tag lightweight in the clone, and `git describe` (the
   build id, `build/config.mk`) skips lightweight tags -- v1.0.4's kernel
   said `v1.0.3-123-g1ec37307` and its package `1.0.3-123`. The script
   refuses a lightweight tag and a `git describe` that is not the tag.
   After building, `scripts/check-release-version.py` requires the
   kernel's build id (every describe-shaped string in `kernel.elf`) to be
   the tag, the set's file names to be the set's, and the
   `cosmo-upgrade` package's version to be the tag's
   (`package_version`). The acceptance jobs restore the tag the same way.
   Both scripts are tested on synthetic sets and a local repository by
   `tests/host/test_release_version.py` (`make host-test`).
3. `reproducible`: each architecture's two sets compared file by file
   (`scripts/compare-release-sets.sh`).
4. `acceptance` (each architecture × `test-entropy`, `test-install`,
   `test-busybox`, `test-net-login`, `test-security`, the guide): the job
   builds again and requires its `SHA256SUMS` to equal the published
   set's, so the images tested are the bytes published; then runs the
   test with `BUILD=release`. `guide-macos` runs the guide on macOS
   against the published set.
5. `publish`: one `SHA256SUMS` for both architectures, signed with the
   release key (`SHA256SUMS.sig`, checked against `tools/keys/*.pub`
   before publishing), the notes (`docs/releases/<tag>.md` first when the
   tag has one), then the generated part
   (`scripts/release-notes.py`: the release key and its id, the CI image
   digests and the compiler and QEMU versions in them, BusyBox, musl,
   compiler-rt, dropbear and OpenSSH portable (sftp-server) with their
   pinned SHA-256, the
   reproducibility lines, every file's SHA-256, the guide's known
   limitations), and `gh release create`; a tag with a hyphen is a
   pre-release. Before `gh release create`, `check-release-version.py
   --notes` requires the notes to say "CosmoOS <version>" and no other
   "CosmoOS X.Y.Z", and checks both sets again.

A release (a tag without a hyphen) must point at a commit on main (job
`guard`); a pre-release may be any head. The key itself is a repository
secret, which any workflow pushed by someone with write access can read;
moving it to a GitHub environment with required reviewers would close
that, and is the owner's to set up (inventory 2.9).

The release key's public half is the only `tools/keys/*.pub`
(`tools/keys/README.md`); developer builds keep their per-machine key
and trust the release key too.

## Workflow for a change

Constitution section 66 applies to every subsystem change. In practice:

1. Read the affected subsystem's `docs/<subsystem>/` files, especially
   `invariants.md`.
2. Work on a feature branch and open a pull request; Greptile reviews every
   PR on the repository.
3. Before pushing: `make test`, `make BUILD=release test`, `make host-test`,
   `make host-test-fetch`, `make fuzz`, `make analyze`, `make reproducible`, `make test-crash`.
4. Update the subsystem documentation in the same PR.

The virtualization tests need QEMU 9.2 or newer (TCG nested paging for
paging-off guests); with an older QEMU the kernel disables the backend at
boot and the boot test fails on the skipped guest tests. CI runs in a
Debian trixie container for this reason.
