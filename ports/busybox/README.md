# ports/busybox

Static musl BusyBox, built from source for the architecture in hand
(roadmap M3, decision 6). Every image carries it: `/bin/busybox` and a
symbolic link per applet of `applets` (docs/userland/design.md,
"BusyBox").

| Source | Version | SHA-256 |
|---|---|---|
| BusyBox | 1.37.0 (`busybox-1.37.0.tar.bz2`) | `3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4` (busybox.net's published `.sha256`) |
| musl | 1.2.5 (`musl-1.2.5.tar.gz`) | `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4` |
| compiler-rt | 19.1.7 (`compiler-rt-19.1.7.src.tar.xz`) | `c12b6e764202c615c1a3af9a13d477846878757ae0e29e5f8979215a6958fffc` |

`scripts/fetch-pinned.py` downloads each once into `$(PORTS_CACHE)`
(`.cache/ports` by default; CI caches it) and refuses a file whose digest
is not the pinned one. That is the only network access of the build.

## The build (`busybox.mk`)

1. **musl**: `configure --target=<arch>-linux-musl --disable-shared` with
   the project's clang, `llvm-ar`; installed into `$(BB_DIR)/sysroot` (`BB_DIR`: `out/busybox-<arch>` beside the build trees, one per architecture, which every build variant shares).
2. **compiler-rt builtins**: every `lib/builtins/*.c` compiled for the
   target against the musl headers into `libclang_rt.builtins.a`, except,
   on AArch64, the twelve x87 80-bit `long double` routines (`*xf*`,
   `divxc3`, `mulxc3`, `powixf2`), which are x86-only. AArch64 needs the
   128-bit `long double` routines (`__addtf3`, `__multf3`,
   `__trunctfdf2`, ...): musl's printf and strtod use them, and the
   host's clang has no Linux builtins to offer.
3. **BusyBox** with `busybox.config`, through `cc` (from `cc.in`): clang
   for the target with the musl headers; a link is static, with musl's
   `crt1.o crti.o ... -lc builtins crtn.o` in that order, and the image
   at 4 MiB (`--image-base=0x400000`: lld's default 2 MiB is below the
   kernel's user window). `oldconfig` with every answer the default must
   leave the configuration unchanged, or the build stops: the tree
   records the whole configuration. `KCONFIG_NOTIMESTAMP` keeps the
   build time out, and `-ffile-prefix-map` the build directory, so the
   binary is reproducible (`make reproducible`). The binary is stripped with
   `llvm-objcopy` (`SKIP_STRIP=y`: BusyBox wants `strip`).
4. **The testsuite**: BusyBox's `testsuite/` packed by
   `pack-testsuite.py` into a reproducible `testsuite.tgz` for
   `make test-busybox`.

## The configuration

`busybox.config` is generated, then checked in. To regenerate it, from a
BusyBox source tree:

```sh
make O=/tmp/bbcfg defconfig
python3 ports/busybox/mkconfig.py . /tmp/bbcfg/.config
yes "" | make O=/tmp/bbcfg oldconfig
grep -v '^# [A-Z][a-z][a-z] [A-Z][a-z][a-z] .* [0-9][0-9][0-9][0-9]$' /tmp/bbcfg/.config > ports/busybox/busybox.config
```

`mkconfig.py` starts from defconfig, turns off every applet outside the
list (an applet is a symbol named by some `//applet:IF_<SYMBOL>(` line),
and forces a few options, each with its reason in the script: static;
no ash job control; ash as `sh`; no setuid; no SHA hardware paths (x86
only, and one configuration serves both architectures); `ls` plain on a
terminal; and `-DBB_GLOBAL_CONST=` -- BusyBox's "const pointer to
globals" trick assumes a const pointer is reloaded after an asm barrier,
clang on AArch64 does not, and every applet with a globals struct
faulted at address 4.
