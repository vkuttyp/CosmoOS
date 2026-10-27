# ports/tcc

The C compiler on the target (roadmap 1.2 A1): TinyCC, a static program
built from source for the architecture in hand by the same ports path as
BusyBox (`ports/busybox/`), with musl's headers, start files and libraries
installed beside it, so a program is compiled, assembled and linked on
CosmoOS itself. tcc assembles and links on its own: no binutils.

| Source | Version | SHA-256 |
|---|---|---|
| TinyCC | the `mob` branch at `e23dabed154a8495f3ac1ff3cc9d04fa1c207893` (0.9.28rc: 0.9.27 and the AArch64 fixes since; `tinycc-e23dabed….tar.gz`) | `d2470fdeed862ddb18f4ec4e6a405fa984580c3d85c38492516749c61931c30e` (GitHub's archive of that commit, fetched 2026-10-27) |

`scripts/fetch-pinned.py` fetches it once into `$(PORTS_CACHE)`, from
TinyCC's GitHub mirror (`github.com/TinyCC/tinycc`, which carries the same
`mob` commit as repo.or.cz: `git ls-remote` of both, 2026-10-27) and then
from the repository's `pinned-sources` release. repo.or.cz serves its
snapshot links behind a bot check, so they are not used. The roadmap names
0.9.27 plus the `mob` fixes for AArch64; the `mob` head is both.

## The build (`tcc.mk`)

Against the BusyBox port's musl sysroot and compiler-rt builtins, through
its `cc` wrapper, so nothing is built twice:

- `configure --cpu=<x86_64|arm64> --targetos=Linux --config-musl
  --config-bcheck=no --config-backtrace=no --prefix=/usr
  --sysincludepaths=/usr/lib/tcc/include:/usr/include
  --libpaths=/usr/lib/tcc:/usr/lib --crtprefix=/usr/lib
  --elfinterp=/lib/ld-musl-<arch>.so.1`: the installed system's paths,
  musl's loader as the interpreter of what it links dynamically. The
  bounds checker (`-b`) and backtraces (`-bt`, `-run`'s) need runtime
  support of tcc's own that is not built.
- The build's one host tool, `c2str` (it turns `include/tccdefs.h` into a C
  string compiled into tcc), is built first by the host's `$(CC)`; left to
  make it would be built for the target.
- `make tcc`, then `make libtcc1.a <cpu>-libtcc1-usegcc=yes`: tcc's
  runtime (`libtcc1.a`: long-double and 128-bit helpers, `alloca`,
  `va_list` on x86-64, atomics) built by the target's clang, tcc's own
  switch for doing so, since the tcc just built cannot run on the build
  host.
- One patch, `armflush-clang.patch`: AArch64's `__clear_cache`
  (`lib/armflush.c`) calls `__arm64_clear_cache`, a builtin only tcc has;
  built by clang it cleans the data cache and invalidates the instruction
  cache by line itself, the line sizes from `CTR_EL0`, as tcc's builtin
  does. That runs at EL0, which the kernel allows since A1
  (`SCTLR_EL1.UCT` and `UCI`, as Linux sets them).
- `tcc` stripped with `llvm-objcopy`; `-ffile-prefix-map` keeps the build
  directory out of it.

## On the system

| Path | What |
|---|---|
| `/usr/bin/tcc`, `/usr/bin/cc` (a link) | the compiler |
| `/usr/lib/tcc/libtcc1.a`, `/usr/lib/tcc/include/` | tcc's runtime and its own headers (`stdarg.h`, `stddef.h`, `float.h`, ...) |
| `/usr/include/` | musl's headers |
| `/usr/lib/crt1.o`, `crti.o`, `crtn.o`, `Scrt1.o`, `rcrt1.o` | musl's start files |
| `/usr/lib/libc.a` | musl, for `-static` |
| `/usr/lib/libc.so` | a link to `/lib/libc.so`, the system's, for a dynamic link (its interpreter is `/lib/ld-musl-<arch>.so.1`) |
| `/usr/lib/libm.a`, `libpthread.a`, `libdl.a`, ... | musl's empty stand-ins, so `-lm` and the rest find what libc already holds |

The boot archive carries the header trees as directory entries
(`scripts/mkbootarchive.py`'s `NAME/=DIR`), every file in sorted order.
About 4 MB in all, most of it `libc.a` and the headers.

## Tests

- **The debug boot's shell test** (`userland/etc/rc.test`): `cc` builds
  `tests/tcc/hello.c` statically and dynamically, and both run
  (`cc-hello: static ...`, `cc-hello: dynamic ...`, required by the boot
  test).
- **`make test-selfhost`** (A1's acceptance, to come): on the installed
  system, static, dynamic and `dlopen` programs; tcc's `tests/tests2`
  suite with its exclusions listed; tcc rebuilding itself, and the rebuilt
  compiler building the same `hello` byte for byte.
