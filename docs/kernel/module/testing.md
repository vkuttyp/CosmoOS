# Kernel modules: testing

Three layers: host unit tests on the pure pieces (crypto, ELF
validation), target self-tests that load, call, and unload real
fixture modules under QEMU, and build-time checks on every module the
build produces.

## Host unit tests (`make host-test`)

`tests/host/test_crypto` (`kernel/security/sha512.c`, `ed25519.c`):

- `sha512-vectors`: FIPS 180-4 examples for `""`, `"abc"`, the 896-bit
  two-block message, and one million `'a'` fed through `sha512_update`
  in irregular chunk sizes (exercises buffering and the length
  encoding).
- `ed25519-vectors`: RFC 8032 section 7.1 TEST 1, 2, 3 and the
  SHA(abc) vector verify; for each, a sweep of single-bit flips across
  the signature, a flipped message bit, and a flipped public-key bit
  all fail.
- `ed25519-malleability`: `S + L` (the same scalar mod L) is rejected;
  a non-canonical public key encoding is rejected.

`tests/host/test_modelf` (`kernel/module/modelf.c` under
`-DMODELF_HOST_TEST=1`): a builder assembles a well-formed synthetic
`ET_REL` (text, rodata, data, bss, `.cosmo.module`, `.ksymtab`, symbol
and string tables, one RELA section), then:

- `modelf-good`: the image validates; layout indices, group sizes (three
  4 KiB groups), the bss offset inside DATA, the metadata offset inside
  RODATA, section names, and `modelf_check_info` are all checked.
- `modelf-header`: bad magic, `ET_EXEC`, AArch64, ELF32, section table
  past the end, bad `e_shstrndx`, a 10-byte file: each fails on its
  exact rule string.
- `modelf-sections`: W+X text, a section past the end, alignment 8192,
  alignment 12, wrong metadata size, writable metadata, missing
  metadata, missing symbol table, bad symbol entry size, relocations
  linked to a foreign table, relocations against NOBITS, `SHT_REL`,
  executable NOBITS, `.ksymtab` in the text group.
- `modelf-symbols`: `SHN_COMMON`, `SHN_XINDEX`, section index out of
  range, name outside the string table, undefined local.
- `modelf-info`: ABI version 2, bad magic, unterminated name, empty
  name, a space in the name, nine dependencies, trailing comma, leading
  comma, nonzero reserved word.

Both binaries run under ASan and UBSan; a single out-of-bounds read in
the validator fails the run.

## Target self-tests (`make test`, debug builds)

Fixtures in the boot archive under `tests/` (built by `build/module.mk`
from `tests/modules/`): `cosmotest` (exports `cosmotest_answer`,
`cosmotest_table`, `cosmotest_counter`; has text, rodata, data, and bss;
its `init` uses `kmalloc`, `memset`, `strlen` across the module ABI and
checks bss is zero), `cosmotest_dep` (depends on `cosmotest`, calls its
function, reads its table, mutates its counter), `cosmotest_fail`
(`init` returns `-EIO`). Every test skips with a log line when its
fixture is absent from the archive.

| Test | What it checks |
|---|---|
| `bootarchive` | entries exist; `init` is found; misses miss; every entry's data lies inside the archive at a 512-byte offset; `bootarchive_entry` past the end is NULL |
| `ksym` | ≥ 30 exports; `kprintf`, `kmalloc`, `memcpy` resolve to their addresses; unknown and empty names give 0; internals (`pmm_alloc_page`, `schedule`) are not exported; the index is strictly sorted; `module_symbol_lookup` reports the kernel as owner |
| `modsig` | the real `cosmotest.ko` verifies with the right payload length; a flipped payload byte, a flipped signature byte, a wrong version, a wrong algorithm, and a payload shifted by one byte are `-EKEYREJECTED`; an unknown key id, a truncated trailer, and a 10-byte file are `-ENOKEY` |
| `module-reject` | the genuine payload validates with all three groups and a `.ksymtab`; six in-memory mutations of it (`ET_EXEC`, AArch64, bad magic, W+X text, section past the end, wrong metadata size) fail on their exact rule; metadata rules (ABI version, magic, name syntax, empty dependency, reserved word); 64 bytes of garbage through `module_load` is `-ENOKEY` (enforcing) or `-ENOEXEC`; loading `cosmotest_dep` before `cosmotest` is `-ENOENT` and leaves nothing |
| `module-load` | `hello` is live from boot; `cosmotest` loads: state, refs, flags, capabilities; text RX, rodata R, data RW, metadata R (`vm_query`); `init` points into text; 3 exports; `cosmotest_answer()` called through the export returns 42; the counter is 101 (data relocated and mutated by `init`); the table is 36 and read-only; a second load is `-EEXIST`; `cosmotest_dep` loads with `deps[0] == cosmotest`, `refs == 1`, counter 111, `cosmotest_dep_sum()` == 189; unloading the dependency is `-EBUSY`; unloading the dependant restores the counter and removes its export; unloading `cosmotest` then works, lookups return 0, a second unload is `-ENOENT`; reload and unload; `vm_stats.regions`, `anon_pages`, and `module_count` equal the values before the test |
| `module-fail` | `cosmotest_fail` returns `-EIO`, the out pointer is untouched, nothing is live, region and page counts are unchanged |

The self-test run is 38 tests; the six above run last. The boot test
also requires, in every build type, the boot-loaded module's lines:

```text
[ INFO] module: loaded hello 1.0 (text 4 KiB, rodata 4 KiB, data 4 KiB, 0 exports)
[ INFO] hello: module init (ABI v1, load 1)
```

(`REQUIRED_MARKERS` in `tests/boot/run_boot_test.py`), and
`kernel_main` counts `module_load_boot` failures into the exit verdict.

## Variants

- `QEMU_SMP=1 make test`: the same suite on one CPU (the protect
  shootdown degenerates to a local flush).
- `make BUILD=release test`: no self-tests; `hello` still loads and its
  markers are still required.
- `make MODULE_SIG_ENFORCE=0 OUT=out/x86_64-noenforce test`: the
  development policy. The log shows `module: signature enforcement OFF
  (development build)`, the garbage image in `module-reject` is loaded
  "anyway" with a taint warning and then refused by the ELF validator,
  and every other test passes unchanged.
- `make test-crash`: the panic report; with a taint set it would carry
  a `taint: 0x1 (unsigned module loaded)` line (not exercised by the
  crash build, which loads only signed modules).

## Build-time checks

- `scripts/check-module-elf.py` runs on every `.ko` after signing
  (`build/module.mk`): `ET_REL`, x86-64, no W+X section, `.cosmo.module`
  of 240 bytes and read-only, trailer present. It is independent of the
  kernel's validator so a regression in either is caught by the other.
- `scripts/modsign.py verify --pub tools/keys/cosmo-dev.pub --in out/x86_64-debug/modules/hello.ko`
  checks a module with the Python implementation; the kernel's
  `ed25519.c` and the script were cross-checked against each other and
  the RFC vectors during development.
- `make reproducible` covers the generated key ring and the signatures
  (Ed25519 is deterministic).
- `llvm-objdump -r out/x86_64-debug/modules/*.ko` lists only
  `R_X86_64_64`, `32`, `32S`, `PC32`, `PLT32` (the `32` entries are DWARF
  in non-allocated sections and are never applied).

## Running

```sh
make host-test                                   # test_crypto, test_modelf (and buddy, slab)
make test                                        # debug boot, 38 self-tests, hello markers
QEMU_SMP=1 make test
make BUILD=release test
make MODULE_SIG_ENFORCE=0 OUT=$PWD/out/x86_64-noenforce test
grep -n "module:\|SELFTEST: mod\|hello:" out/x86_64-debug/boot-test.log
```

## Gaps

- No fixture with an unresolvable symbol, a weak undefined symbol, or a
  deliberately overflowing relocation (host `test_modreloc` planned).
- No test that an unsigned module is refused under enforcement with a
  well-formed ELF (the garbage image covers the `-ENOKEY` path; a
  stripped-trailer fixture would be more direct).
- `module_unload` of a module whose `shutdown` misbehaves is untested;
  nothing protects against it (it is ring-0 code).
