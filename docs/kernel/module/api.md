# Kernel modules: API

Three audiences: module authors (the module ABI), kernel code (the
loader and its helpers), and build engineers (the tools and make
knobs). Each entry follows constitution section 52. Unless stated,
functions are thread context only, may not be called from interrupt
context, and are **ABI-internal** (kernel API, may change). The module
ABI section is the exception and is versioned.

## Module ABI (`kernel/include/kernel/module.h`)

**ABI stability: versioned.** `COSMO_MODULE_ABI_VERSION` is 1. It is
bumped on any incompatible change to the exported symbol set, to a
structure an exported function takes, to `struct cosmo_module_info`,
or to `struct ksym`. The loader compares the number exactly; a module
built against another version is refused (`-ENOEXEC`, "module ABI
version mismatch"). This is a separate contract from the kernel's
internal API (section 24): nothing that is not `EXPORT_SYMBOL`ed is
reachable by name.

### `COSMO_MODULE(name, version, init, shutdown, deps, caps)`

Purpose: declare the module. Exactly one per module. Expands to a
`const struct cosmo_module_info __cosmo_module_info` in section
`.cosmo.module`.
Inputs: `name` string literal, at most 31 characters from
`[A-Za-z0-9_-]`; `version` string literal, at most 15 characters;
`init` of type `int (*)(void)`, returns 0 or a negative errno;
`shutdown` of type `void (*)(void)`; `deps` string literal, comma
separated module names (at most `MODULE_MAX_DEPS` = 8, each a valid
name, no empty items), `""` for none; `caps` a bitmask of
`MODULE_CAP_*`.
Contract for `init`: runs in thread context with interrupts enabled
while the loader holds its mutex, so it must not call `module_load`,
`module_unload`, `module_find`, `module_symbol_lookup`, or
`module_count`. It may allocate, sleep, and use every exported symbol.
Returning nonzero aborts the load, the value becomes `module_load`'s
result, and `shutdown` is not called.
Contract for `shutdown`: same context; it must undo everything `init`
did; after it returns the module's memory is freed.

### `struct cosmo_module_info`

240 bytes, `STATIC_ASSERT`ed. `magic[8]` = `"COSMOMOD"` (no NUL;
initialise with `COSMO_MODULE_MAGIC_INIT`), `abi_version`,
`capabilities`, `name[32]`, `version[16]`, `deps[128]`, `init`,
`shutdown`, `reserved[4]` (zero). Only `init`/`shutdown` are pointers;
the loader reads every other field straight from the file before
allocating anything.

### Capability flags

`MODULE_CAP_NONE` 0, `MODULE_CAP_DRIVER` bit 0, `MODULE_CAP_FS` bit 1,
`MODULE_CAP_NET` bit 2, `MODULE_CAP_TEST` bit 31 (self-test fixture;
never listed under `modules/` in the archive). Phase 5 records and logs
them (`module_dump`); later phases refuse registrations a module did
not declare.

### `EXPORT_SYMBOL(sym)`

Purpose: make a function or object visible to modules by name. Emits a
`static const struct ksym { const char *name; uintptr_t addr; }` into
section `.ksymtab`. In the kernel the linker script keeps the section
between `__ksymtab_start`/`__ksymtab_end` in the read-only segment; in
a module it becomes that module's export table. On non-ELF hosts (the
macOS host tests) the macro degrades to a `STATIC_ASSERT`.
Rules: a module's export may not repeat and may not shadow a kernel
export (`-EEXIST`); the kernel panics at `ksym_init` on a duplicate.

### Module ABI v1: the exported symbols

The kernel image exports 117 symbols (Phase 5's 43, the Phase 6
device, PCI, DMA, block, entropy and console-sink interfaces, and the
Phase 8 mbuf and network-interface surface a NIC driver needs). Adding
one is compatible; removing or changing one bumps the version.

| Area | Symbols |
|---|---|
| Logging | `klog`, `kvlog`, `kprintf`, `kvprintf`, `ksnprintf`, `kvsnprintf`, `panic` |
| Heap | `kmalloc`, `kzalloc`, `krealloc`, `kfree`, `kmem_cache_create`, `kmem_cache_destroy`, `kmem_cache_alloc`, `kmem_cache_free` |
| Strings | `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `strlen`, `strnlen`, `strcmp`, `strncmp`, `strchr`, `strstr`, `strlcpy` |
| Locks | `spinlock_init`, `spin_lock`, `spin_unlock`, `spin_trylock`, `spin_lock_irqsave`, `spin_unlock_irqrestore`, `mutex_init`, `mutex_lock`, `mutex_trylock`, `mutex_unlock` |
| Scheduling and time | `thread_create`, `thread_sleep_ns`, `sched_yield`, `clock_now_ns`, `ndelay`, `udelay` |
| Device model (`kernel/device.h`) | `bus_register`, `bus_find`, `device_setup`, `device_add_resource`, `device_resource`, `device_register`, `device_unregister`, `driver_register`, `driver_unregister`, `device_map_mmio`, `device_unmap_mmio`, `device_find`, `device_for_each`, `device_count` |
| DMA (`kernel/dma.h`) | `dma_alloc`, `dma_free`, `dma_map`, `dma_unmap`, `dma_sync_for_device`, `dma_sync_for_cpu`, `dma_set_mask` |
| PCI (`drivers/pci.h`) | `pci_bus`, `pci_register_driver`, `pci_unregister_driver`, `pci_cfg_read8/16/32`, `pci_cfg_write8/16/32`, `pci_enable_device`, `pci_map_bar`, `pci_find_capability`, `pci_msix_enable`, `pci_msix_request`, `pci_msix_release`, `pci_msix_disable`, `pci_msi_enable`, `pci_msi_disable`, `pci_device_count`, `pci_device_at`, `pci_find_device` |
| Block (`kernel/blk.h`) | `blk_register`, `blk_unregister`, `blk_submit`, `bio_complete`, `blk_read`, `blk_write`, `blk_flush`, `blk_find` |
| Entropy (`kernel/random.h`) | `random_add_entropy`, `random_get_bytes`, `random_u64`, `random_entropy_bits` |
| Console (`kernel/console.h`) | `console_register`, `console_unregister` |
| Packet buffers (`kernel/mbuf.h`) | `m_get`, `m_getcl`, `m_free`, `m_freem`, `m_prepend`, `m_pullup`, `m_adj`, `m_copydata`, `m_append`, `m_length`, `m_copypacket` |
| Network interfaces (`kernel/netif.h`) | `netif_register`, `netif_unregister`, `netif_rx`, `netif_set_ipv4`, `netif_set_up` |

Modules export too: the `virtio` module provides 15 symbols
(`virtio_bus`, `virtio_register_driver`, `virtio_unregister_driver`,
`virtio_device_init`, `virtio_device_ready`, `virtio_device_reset`,
`virtio_read_config`, `virtio_read_config32`, `virtio_read_config64`,
`virtq_alloc`, `virtq_free`, `virtq_add`, `virtq_kick`, `virtq_pop`,
`virtq_free_count`) that `virtio_blk`, `virtio_rng`, `virtio_console`
and `virtio_net` resolve by declaring `deps = "virtio"`; the loader
resolves a foreign symbol only from a declared dependency (invariant
M3), and a module cannot be unloaded while a dependant holds it. In
total the tree carries 132 `EXPORT_SYMBOL` records.

Semantics are those of the headers the symbols come from
(`docs/kernel/diagnostics/api.md`, `memory/api.md`, `scheduler/api.md`,
`timer/api.md`, `device/api.md`, `docs/drivers/pci/api.md`,
`docs/drivers/virtio/api.md`, `docs/kernel-services/network/api.md`). `EXPORT_SYMBOL` sites sit at the end of
the defining `.c` files.

## Loader API (`kernel/include/kernel/module.h`, `kernel/module/module.c`)

Lock: `g_lock` (mutex `"modules"`) serialises everything below; it
nests inside nothing and takes `kernel_space.lock`, `kmem_cache.lock`,
and `pmm_zone.lock` beneath it during allocation. Every function here
may sleep.

### `void module_init(void)`

Purpose: initialise the mutex, build the kernel export index
(`ksym_init`), log the signing policy and key count.
Lifetime: once, from `kernel_main` after `process_init()` (needs
`kmalloc` and the scheduler's mutex). Panics on a malformed or
duplicate export.

### `int module_load(const void *file, size_t size, const char *origin, struct module **out)`

Purpose: the pipeline of `design.md`: signature, ELF and architecture,
ABI, name and dependencies, allocate, copy, resolve symbols, relocate,
index exports, W^X, `init()`, register.
Inputs: `file`/`size` the complete module file (ELF plus signature
trailer), untrusted; `origin` a string for log lines (archive entry
name); `out` may be NULL.
Outputs: `0` and `*out` a borrowed pointer valid until the module is
unloaded; on failure `*out` is untouched. Errors: `-ENOKEY` (no trailer,
unknown key; with `MODULE_SIG_ENFORCE=0` this becomes a warning and a
taint instead), `-EKEYREJECTED` (bad signature, algorithm, or version;
refused in every build), `-ENOEXEC` (any ELF, layout, metadata, or
`init`/`shutdown` pointer rule, or an unsupported relocation type),
`-ENOENT` (a dependency not loaded, or an unresolved symbol),
`-EEXIST` (name already loaded, an export shadowing a kernel symbol, a
duplicate export), `-ERANGE` (32-bit relocation overflow), `-EINVAL`
(relocation offset or symbol index out of range), `-ENOMEM`, or
`init()`'s own negative value. Every failure logs one `module: <origin>:
...` line and leaves no state.
Ownership: `file` is borrowed and never written; the bytes are copied
out. The module's three regions and record belong to the loader.
Blocking: allocations, the mutex, a TLB shootdown per protected region
(interrupts must be enabled), and `init()` itself.

### `int module_unload(const char *name)`

Purpose: `shutdown()` and free a live module.
Outputs: `0`; `-ENOENT` (not live); `-EBUSY` (another live module
declared it as a dependency; logged). On success dependency reference
counts drop and every region is freed.

### `struct module *module_find(const char *name)`

Borrowed pointer to a live module or NULL. Valid until an unload; the
loader has no reference counting for lookups, so callers use it
promptly and never across a call that could unload.

### `uintptr_t module_symbol_lookup(const char *name, const struct module **owner)`

Purpose: address of a symbol exported by the kernel or any live module.
Outputs: the address or 0; `*owner` (if non-NULL) is NULL for a kernel
symbol, else the owning module. Kernel lookups are lock-free
(`ksym_lookup`); module lookups take the mutex.

### `unsigned module_load_boot(void)`

Loads every archive entry whose name starts with `modules/`, in archive
order, and logs a summary. Returns the failure count; `kernel_main`
counts failures into the boot verdict.

### `unsigned module_count(void)`, `void module_dump(void)`

Live module count; a `kprintf` listing (name, version, region bases,
refs, capabilities, unsigned flag). Both take the mutex.

### `struct module`

Public fields (read-only for callers): `name`, `version`,
`capabilities`, `flags` (`MODULE_FLAG_UNSIGNED`), `state`
(`MODULE_LOADING`/`LIVE`/`GOING`), `text`/`rodata`/`data` bases (0 when
that group is empty) and sizes, `info` (the relocated metadata, inside
rodata), `exports`/`nr_exports`, `deps[]`/`nr_deps`, `refs`. The
private continuation `struct module_priv` holds the sorted export
index.

## Export table (`kernel/include/kernel/ksym.h`, `kernel/module/ksym.c`)

### `void ksym_init(void)`

Builds the sorted pointer index over `.ksymtab` (heap sort; the table
is read-only). Allocates `count * 8` bytes. Panics on a NULL name, a
zero address, or a duplicate name. Called by `module_init`.

### `uintptr_t ksym_lookup(const char *name)`

Binary search; 0 if absent or before `ksym_init`. Lock-free, no
allocation, safe in interrupt and panic context.

### `size_t ksym_count(void)`, `const struct ksym *ksym_entry(size_t sorted_index)`

Count and the i-th entry in sorted order (NULL past the end).

### `void ksym_sort(const struct ksym **v, size_t n)`, `uintptr_t ksym_search(const struct ksym *const *v, size_t n, const char *name)`

The sort and search shared with module export tables: in-place heap
sort of pointers by name; binary search returning the address or 0.
Pure.

## Signatures (`kernel/include/kernel/modsig.h`, `kernel/module/modsig.c`)

### `struct modsig_trailer`

88 bytes, `STATIC_ASSERT`ed, appended to the ELF: `sig[64]`,
`key_id[8]` (first 8 bytes of SHA-512 over the raw public key),
`version` (`MODSIG_VERSION` = 1), `algo` (`MODSIG_ALGO_ED25519` = 1),
`magic[8]` = `"COSMOSIG"` as the last bytes of the file. The signature
covers `[0, size - 88)`. **Stability: format, versioned by `version`.**

### `int modsig_check(const void *file, size_t size, size_t *payload_size, const char **why)`

Purpose: detect and verify the trailer.
Outputs: `0` with `*payload_size` = ELF length; `-ENOKEY` (file shorter
than a trailer, no magic, key id not in the ring); `-EKEYREJECTED`
(version or algorithm unsupported, signature does not verify). `why`
(optional) gets an immortal string. Pure apart from CPU time (a few
milliseconds per 100 KiB under TCG); any context.

### `bool modsig_enforced(void)`

`CONFIG_MODULE_SIG_ENFORCE != 0` (make `MODULE_SIG_ENFORCE`, default 1).

## ELF validation (`kernel/include/kernel/modelf.h`, `kernel/module/modelf.c`)

Pure functions; compiled on the host with `-DMODELF_HOST_TEST=1`.

### `int modelf_validate(const void *file, size_t size, struct modelf_layout *out, const char **why)`

Purpose: check an `ET_REL` x86-64 image and compute its layout.
Outputs: `0` and `*out` filled, or `-ENOEXEC` with `*why` naming the
rule (the exact strings are asserted by `tests/host/test_modelf.c` and
the `module-reject` self-test). Rules: ELF64 little-endian `ET_REL`
`EM_X86_64` with sane header sizes; section table in bounds; valid
`.shstrtab`; exactly one `SHT_SYMTAB` (entry size 24, valid string
table link); no `SHT_REL`, no `SHT_DYNSYM`; every non-NOBITS section
in the file; allocatable sections only PROGBITS or NOBITS, never
writable and executable, never executable NOBITS, alignment a power of
two ≤ 4096, ≤ 1 GiB each, at most `MODELF_MAX_SECTIONS` = 32; every
`SHT_RELA` with entry size 24, linked to the symbol table, targeting an
in-range section (a NOBITS target is refused; a non-allocatable target
is ignored); `.cosmo.module` present, PROGBITS, allocatable, read-only,
exactly 240 bytes, landing in the RODATA group; `.ksymtab` (if present)
allocatable PROGBITS, a multiple of 16 bytes, not in the TEXT group;
symbol 0 null; every symbol name in bounds; no `SHN_COMMON`, no
`SHN_XINDEX`, no undefined local, section indices in range.
Layout: `group_size[TEXT|RODATA|DATA]` page rounded; `sections[]` with
ELF index, group, `nobits`, offset within the group (packed in file
order at `sh_addralign`), size, file offset, alignment; `symtab`,
`strtab`, `shstrtab`, `info_section`, `ksymtab_section`,
`info_file_off`, `nr_symbols`.

### `int modelf_check_info(const struct cosmo_module_info *info, const char **why)`

Metadata rules: magic, exact ABI version, name terminated, non-empty,
`[A-Za-z0-9_-]`; version and deps terminated; deps items non-empty,
valid characters, shorter than a name, no trailing comma, at most 8;
`reserved` zero. `-ENOEXEC` and `why` on failure.

### `const struct modelf_section *modelf_find_section(const struct modelf_layout *, uint32_t index)`, `const struct elf64_shdr *modelf_shdr(const void *file, uint32_t index)`, `const char *modelf_section_name(const void *file, const struct modelf_layout *, uint32_t index)`

Accessors over a validated file: the allocatable-section record (NULL
if not allocated), a section header, a section name (`""` if out of
range). No checking of their own; only for files `modelf_validate`
accepted.

## Relocation (`kernel/include/arch/module.h`, `kernel/arch/x86_64/modreloc.c`)

### `int arch_module_reloc(vaddr_t target, size_t target_size, const struct elf64_rela *rela, size_t count, const uintptr_t *sym_addr, size_t nr_syms, const char **why)`

Applies one RELA section to a section mapped read-write at `target`.
`sym_addr[i]` is the resolved address of symbol `i`. Types:
`R_X86_64_64`, `PC32`, `PLT32` (as `PC32`: there is no PLT), `32`,
`32S`, `NONE`. Each offset is bounds checked against the section
(`-EINVAL`), each symbol index against `nr_syms` (`-EINVAL`), each
32-bit result against its range (`-ERANGE`); any other type is
`-ENOEXEC`. Pure over the given memory; the caller flips protections
afterwards. An AArch64 implementation is a new file with the same
signature.

## Boot archive (`kernel/include/kernel/bootarchive.h`, `kernel/core/bootarchive.c`)

### `void bootarchive_init(void)`

Parses the archive named by the boot info (`archive_phys`/`archive_size`)
once, right after `bootinfo_init`. No archive: a warning, count 0. A
malformed archive panics (size not a multiple of 512, bad checksum, not
`ustar`, unterminated name, `prefix` used, bad or oversized size field,
non-regular file, empty/absolute/`..` name, more than
`BOOTARCHIVE_MAX_ENTRIES` = 64 entries, duplicate name). No allocation,
no locks.

### `bool bootarchive_find(const char *name, const void **data, size_t *size)`

Exact-name lookup; outputs may be NULL. Pointers are into the archive's
reserved memory and stay valid for the life of the kernel. Lock-free,
any context after init.

### `unsigned bootarchive_count(void)`, `const struct bootarchive_entry *bootarchive_entry(unsigned index)`

Entry count and the i-th entry `{name[101], data, size}` in archive
order (NULL past the end).

## Cryptography and keys

See `docs/kernel/security/api.md` for `sha512_*`, `ed25519_verify`,
`keyring_*`, and `kernel_taint`.

## Tools

### `scripts/modsign.py`

```sh
scripts/modsign.py keygen --out-key K.key --out-pub K.pub   # new Ed25519 pair (hex seed / hex public key)
scripts/modsign.py sign   --key K.key --in m.ko.unsigned --out m.ko
scripts/modsign.py verify --pub K.pub --in m.ko            # exit 0 on success
scripts/modsign.py keyid  --pub K.pub                      # the 8-byte id the kernel matches
```

Pure Python (RFC 8032 reference arithmetic, standard library only);
re-signing an already signed file replaces the trailer. Signing is
deterministic, so builds stay reproducible.

### `scripts/mkbootarchive.py OUTPUT.tar NAME=PATH ...`

Reproducible ustar (mode 0644, uid/gid 0, mtime 0, no prefix, names ≤
100 bytes, regular files only, entries in argument order). Rejects
duplicate, absolute, or `..` names.

### `scripts/gen-keyring.py OUTPUT.c KEY.pub ...`

Emits `keyring_builtin[]`/`keyring_builtin_count` for
`kernel/security/keyring.c`; the key name is the file's basename.
Deterministic.

### `scripts/check-module-elf.py module.ko`

Independent post-build check: ELF64 LE `ET_REL` x86-64, no allocatable
section both writable and executable, `.cosmo.module` present, 240
bytes and read-only, `COSMOSIG` trailer present. Run by `build/module.mk`
after signing.

## Make knobs (`build/config.mk`, `build/module.mk`)

| Knob | Default | Meaning |
|---|---|---|
| `MODULE_SIG_ENFORCE` | `1` | `CONFIG_MODULE_SIG_ENFORCE`; `0` loads unsigned modules with a warning and taints the kernel |
| `MODSIGN_KEY` | `tools/keys/cosmo-dev.key` | seed used to sign every module |
| `MODULES` | `hello cosmotest cosmotest_dep cosmotest_fail` | module names; each needs `MODULE_<name>_SRCS` |
| `MODULE_ARCHIVE_ENTRIES` | see file | `archive-name=path` pairs; `modules/` entries load at boot in this order, `tests/` entries are fixtures. The top-level `Makefile` adds `init=` and `USER_ARCHIVE_ENTRIES` (`bin/`, `sbin/`, `etc/` from `userland/userland.mk`) in front of them |
| `MODULE_CFLAGS` | `KERNEL_CFLAGS -DCOSMO_MODULE_BUILD=1` | the kernel flags (`-mcmodel=kernel`, no red zone, general registers only, freestanding) |
| `MODULE_LDFLAGS` | `-r --no-dynamic-linker -z noexecstack --build-id=none` | relocatable link |

To add a module: put its sources under `modules/<name>/`, add
`MODULE_<name>_SRCS := modules/<name>/<file>.c ...`, append `<name>` to
`MODULES`, add `modules/<name>.ko=$(MODULE_OUT)/<name>.ko` to
`MODULE_ARCHIVE_ENTRIES` after any module it depends on, and declare it
with `COSMO_MODULE(...)`. `make modules` builds it, `make test` loads it
at boot. Relocation types are limited to those `-mcmodel=kernel`
non-PIC code emits; do not add `-fPIC`.
