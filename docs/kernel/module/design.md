# Kernel modules: design

## Data structures

### The boot archive (`kernel/core/bootarchive.c`)

A plain ustar archive as `scripts/mkbootarchive.py` writes it: 512-byte
headers, file data padded to 512, two zero blocks at the end, regular
files only, names of at most 100 bytes, no `prefix` field. The kernel
walks it once at `bootarchive_init()` and refuses the whole archive on
the first malformed header (bad checksum, non-octal size, size past the
end, name without NUL, typeflag other than `'0'` or `'\0'`). Entries are
recorded in a fixed table (`BOOTARCHIVE_MAX_ENTRIES` = 64) as
`{name, data, size}` pointers into the archive's `COSMOBOOT_MEM_ARCHIVE`
memory, which the PMM keeps reserved for the life of the kernel. Lookup
is by exact name (`init`, `modules/hello.ko`, `tests/cosmotest.ko`;
since Phase 9 also `bin/sh`, `sbin/ps`, `etc/rc`, which the ramfs
places at `/bin`, `/sbin`, `/etc`, and since Phase 10 `sbin/pkg`,
`etc/pkg/repos.conf`, `etc/pkg/keys/cosmo-dev.pub` and the package
repository as `repo/INDEX`, `repo/hello-1.1.cpk`, ...; nested names are
allowed, the ramfs creates the intermediate directories,
`docs/kernel-services/vfs/api.md`).

### The module ABI (`kernel/include/kernel/module.h`)

```c
#define COSMO_MODULE_MAGIC        "COSMOMOD"     /* 8 bytes */
#define COSMO_MODULE_ABI_VERSION  1

struct cosmo_module_info {                 /* one per module, in .cosmo.module */
    char     magic[8];
    uint32_t abi_version;                  /* COSMO_MODULE_ABI_VERSION at build */
    uint32_t capabilities;                 /* MODULE_CAP_* bits */
    char     name[MODULE_NAME_MAX];        /* 32, NUL terminated, [A-Za-z0-9_-] */
    char     version[MODULE_VERSION_MAX];  /* 16, free text, NUL terminated */
    char     deps[MODULE_DEPS_MAX];        /* 128, comma separated module names */
    int    (*init)(void);                  /* required */
    void   (*shutdown)(void);              /* required */
    uint64_t reserved[4];                  /* zero */
};
```

Every field a decision depends on before memory is allocated (magic,
ABI version, name, dependencies) is an inline array readable straight
from the file. Only `init` and `shutdown` are pointers; they are fixed up
by ordinary `R_X86_64_64` relocations in `.rela.cosmo.module` and read
only after relocation. `COSMO_MODULE(name, version, init, shutdown,
deps, caps)` expands to the definition of a `const` instance placed in
`.cosmo.module`, so the section carries exactly `sizeof(struct
cosmo_module_info)` bytes; anything else is rejected.

Exports use the same record in the kernel and in modules:

```c
struct ksym { const char *name; uintptr_t addr; };
#define EXPORT_SYMBOL(sym) \
    static const struct ksym __ksym_##sym __section(".ksymtab") __always_used = { #sym, (uintptr_t)&(sym) }
```

The kernel linker script keeps `.ksymtab` (with `KEEP`) between
`__ksymtab_start` and `__ksymtab_end` inside the read-only segment.
`ksym_init()` builds a sorted pointer index over it (heap sort; the
table itself is read-only) for binary search. A module's `.ksymtab`
section, if present, becomes that module's export table after
relocation.

### The signature trailer (`kernel/include/kernel/modsig.h`)

```c
struct modsig_trailer {                    /* 88 bytes, appended to the ELF */
    uint8_t  sig[64];                      /* Ed25519 signature over bytes [0, file_size - 88) */
    uint8_t  key_id[8];                    /* first 8 bytes of SHA-512(public key) */
    uint32_t version;                      /* 1 */
    uint32_t algo;                         /* MODSIG_ALGO_ED25519 = 1 */
    uint8_t  magic[8];                     /* "COSMOSIG", the last 8 bytes of the file */
};
```

The magic sits at the very end so detection is a fixed-offset compare.
`modsig_check(file, size, &elf_size)` finds the trailer, looks the key up
in the ring, verifies, and returns the ELF length without the trailer.
The signing tool never alters the ELF; verification of an unsigned
module is impossible by construction (no trailer means `-ENOKEY`).

### The key ring (`kernel/security/keyring.c`)

`scripts/gen-keyring.py` turns the `.pub` files in `tools/keys/` (32-byte hex) into a
generated C array of `{key_id[8], pub[32], name}` at build time. The
repository ships one development key pair, `cosmo-dev`, whose secret
half is public by definition: it authenticates nothing outside a
developer's own tree. Production images must be built with a different
ring and the secret key kept off the build host. `keyring_find(id)` is a
linear scan; the ring is immutable after build.

### Loaded module record (`kernel/module/module.c`)

```c
struct module {
    struct list_node link;                 /* g_modules, load order */
    char name[MODULE_NAME_MAX];
    char version[MODULE_VERSION_MAX];
    uint32_t capabilities;
    unsigned flags;                        /* MODULE_FLAG_UNSIGNED */
    enum module_state state;               /* LOADING, LIVE, GOING */
    vaddr_t text, rodata, data;            /* three vm_kernel_alloc regions (0 if empty) */
    size_t text_size, rodata_size, data_size;
    const struct cosmo_module_info *info;  /* inside rodata after relocation */
    const struct ksym *exports;            /* inside module memory, or NULL */
    size_t nr_exports;
    struct module *deps[MODULE_MAX_DEPS];  /* 8, referenced while this module lives */
    unsigned nr_deps;
    unsigned refs;                         /* live dependants */
};
```

`module.c` embeds this in a private `struct module_priv` that adds
`export_index`, the sorted pointer array over `exports` (built with
`ksym_sort`, searched with `ksym_search`); callers only ever see the
public `struct module *`.

### ELF layout description (`kernel/include/kernel/modelf.h`)

`modelf_validate()` is a pure function over the file bytes (compiled on
the host under `MODELF_HOST_TEST`). It fills:

```c
struct modelf_section { uint32_t index; uint8_t group; bool nobits; uint64_t offset, size, file_off, align; };
struct modelf_layout {
    uint64_t group_size[3];                /* TEXT, RODATA, DATA, page rounded */
    unsigned nr_sections;
    struct modelf_section sections[MODELF_MAX_SECTIONS];   /* 32, SHF_ALLOC only */
    uint32_t symtab, strtab, shstrtab;     /* section indices */
    uint32_t info_section;                 /* .cosmo.module */
    uint32_t ksymtab_section;              /* .ksymtab or 0 */
    uint64_t info_file_off;                /* where the metadata sits in the file */
    uint32_t nr_symbols;
};
```

Groups: `SHF_EXECINSTR` sections form TEXT, writable sections
(`SHF_WRITE`, PROGBITS or NOBITS) form DATA, every other allocatable
section forms RODATA. Within a group the sections are packed in file
order at their `sh_addralign` (which must be a power of two no larger
than a page). The three groups become three regions so that the
protection flip needs no page splitting.

## The load pipeline

`module_load(file, size, origin, &mod)` holds `g_modules_lock` (a
mutex) from the first check to the last log line. Steps, each of which
fails without side effects on the module list:

1. **Signature** (`modsig_check`): locate the trailer, find the key,
   `ed25519_verify`. Failures: `-ENOKEY` (no trailer or unknown key id),
   `-EKEYREJECTED` (bad signature, wrong algorithm, wrong version).
   With `CONFIG_MODULE_SIG_ENFORCE=0` a `-ENOKEY` result becomes a
   warning, the module is flagged `MODULE_FLAG_UNSIGNED`, and
   `kernel_taint(TAINT_UNSIGNED_MODULE)` marks the panic report. A bad
   signature is refused in every build: a signed-but-tampered file is
   never a development convenience.
2. **ELF and architecture** (`modelf_validate`): magic, class 64, little
   endian, `ET_REL`, `EM_X86_64`, sane header sizes; every section
   header in bounds; `.shstrtab` valid; exactly one `SHT_SYMTAB` with
   `sh_entsize` 24 and a valid `SHT_STRTAB` link; every allocatable
   section within the file (except NOBITS), never both writable and
   executable; every `SHT_RELA` with `sh_entsize` 24, `sh_link` the
   symbol table, `sh_info` an allocatable section; no `SHT_REL`, no
   `SHT_DYNSYM`, no `SHN_XINDEX`; at most `MODELF_MAX_SECTIONS`
   allocatable sections; `.cosmo.module` present, allocatable, exactly
   `sizeof(struct cosmo_module_info)` bytes, not writable. Rejections
   return `-ENOEXEC` with the rule in `why`.
3. **ABI**: magic, `abi_version == COSMO_MODULE_ABI_VERSION`
   (`-ENOEXEC` "module ABI version mismatch"), name syntax, `reserved`
   zero, `init` relocation present (checked after relocation: a NULL
   `init` is `-ENOEXEC`).
4. **Dependencies**: split `deps`; each must name a `LIVE` module
   (`-ENOENT` "dependency not loaded"); at most `MODULE_MAX_DEPS`; the
   name must not already be loaded (`-EEXIST`).
5. **Allocate**: one `vm_kernel_alloc(size, VM_KALLOC_POPULATE |
   VM_KALLOC_GUARD | VM_KALLOC_NEAR_KERNEL, VM_PROT_RW)` per non-empty
   group. `VM_KALLOC_NEAR_KERNEL` selects the arena
   `[0xFFFFFFFF88000000, 0xFFFFFFFFFF000000)`: inside the top 2 GiB so
   `-mcmodel=kernel` code and `R_X86_64_32S` addresses work, above the
   image so the image can grow to 128 MiB. `PROGBITS` bytes are copied
   through the fresh RW mapping; NOBITS is already zero.
6. **Relocate** (`arch_module_reloc`): for every symbol compute its
   address once: defined symbols are group base + section offset +
   `st_value`; `SHN_ABS` is `st_value`; undefined globals are looked up
   in the kernel export table, then in the export tables of the declared
   dependencies only (`-ENOENT` "unresolved symbol", logged with the
   name); undefined weak symbols resolve to 0; symbols in non-allocated
   (debug) sections get address 0 and are never referenced by an applied
   relocation; `SHN_COMMON` is refused at validation. After relocation
   `init` and `shutdown` must be non-NULL and point into the module
   (`-ENOEXEC`), and the module's `.ksymtab`, if any, is indexed: every
   record's name and address must lie inside the module, the name must
   terminate inside it, no name may repeat, and no name may shadow a
   kernel export (`-EEXIST` "export shadows a kernel symbol"), so the
   symbol namespace stays flat.
   Then every `SHT_RELA` entry is applied: `R_X86_64_64`, `_32`, `_32S`,
   `_PC32`, `_PLT32` (treated as `PC32`, there is no PLT). Offsets are
   bounds checked against the target section, 32-bit results are range
   checked (`-ERANGE` "relocation overflow"). Any other type is
   `-ENOEXEC`. All writes go through the RW mapping; nothing executes
   yet.
7. **W^X**: `vm_kernel_protect(text, VM_PROT_RX)`,
   `vm_kernel_protect(rodata, VM_PROT_READ)`; data stays RW. The
   protect path changes the region's protection, rewrites the leaf
   entries, and runs a TLB shootdown so no CPU keeps a writable
   translation of executable pages.
8. **Initialise**: `info->init()`. Nonzero return: log, free the three
   regions, return that value. `init` runs with `g_modules_lock` held,
   so it must not call `module_load`/`module_unload`.
9. **Register**: state `LIVE`, appended to `g_modules`, each dependency's
   `refs` incremented, `module: loaded <name> <version> (text N KiB,
   rodata N KiB, data N KiB)` logged.

`module_unload(name)`: `-ENOENT` if absent, `-EBUSY` if `refs != 0`,
otherwise state `GOING`, `shutdown()`, unlink, decrement dependency
refs, free regions. `module_load_boot()` loads every archive entry whose
name starts with `modules/` in archive order (the packer writes them in
the order the Makefile lists them, which is dependency order); a failure
is logged and counted but does not stop the boot.

## Ownership and lifetime

The archive bytes belong to the boot memory map and are never freed or
written. A module's three regions belong to its `struct module` and are
freed only by `module_unload` or a failed load. `struct module` itself
is `kzalloc`ed and freed at unload. Dependencies hold no pointer to
their dependants; a dependant holds `deps[]` pointers that stay valid
because `refs` forbids unloading the dependency first. Exported symbol
addresses handed to callers of `module_symbol_lookup` are valid only
while the owning module is `LIVE`; diagnostics callers accept that.

## Concurrency

- `g_modules_lock` (mutex, may sleep) serialises every list mutation
  and the entire load/unload pipeline. Lock order: it is taken with no
  other lock held and it nests inside nothing; the VMM's
  `kernel_space.lock` is taken and released beneath it during
  allocation and protection.
- `module_find` and `module_symbol_lookup` take the mutex, so they are
  thread context only. The kernel export index is immutable after
  `ksym_init()` and `ksym_lookup` is lock-free, usable from the panic
  path.
- Module code runs on whichever thread calls it; the loader imposes no
  threading model beyond that `init` and `shutdown` run in thread
  context with interrupts enabled.

## Memory

Per module: three page-rounded regions with guard pages, one
`struct module` (~400 bytes), a transient `nr_symbols * 8` byte address
array during relocation. The archive is typically a few hundred KiB of
reserved boot memory. The kernel export index is `ksym_count * 8` bytes
of `kmalloc` memory.

## Error handling

Every rejection is a negative errno plus one log line naming the module
origin and the rule. `modelf_validate` reports the rule through `why`
so host tests can assert on it exactly. No partial state survives a
failed load: regions freed, `struct module` freed, dependency refs
untouched. `KASSERT` guards internal invariants (group sizes, section
indices) that validation has already established; a firing assertion
there is a loader bug, not bad input.

## Performance

Loading is O(sections + symbols + relocations) with a binary search per
undefined symbol and a linear scan of each dependency's export table.
Ed25519 verification of a 100 KiB module costs a few milliseconds under
TCG; it is a boot-time cost paid once per module.

## Security

- Nothing from the file is trusted before validation, and no byte of it
  is executable before the signature has been verified and the text
  region has been flipped to RX with a shootdown.
- The ABI version is a hard gate: a module built against another
  version is refused, never "probably compatible".
- A module reaches only exported symbols and only its declared
  dependencies' exports. Reaching a kernel internal by address is
  possible in ring 0 by definition; the export table is a discipline
  boundary (section 24), not an isolation boundary.
- The signing key in the tree is a development key. Enforcement is on
  by default in both build types; turning it off is a build-time
  decision (`MODULE_SIG_ENFORCE=0`) that taints the kernel visibly.
- The Ed25519 implementation is verification-only and variable-time:
  every input to verification is public, so timing leaks nothing.

## Testing strategy

Host: SHA-512 and Ed25519 against RFC 8032 test vectors (positive and
bit-flipped negatives) under ASan/UBSan; `modelf_validate` against
crafted headers (bad magic, `ET_EXEC`, wrong machine, section past end,
W+X section, missing metadata, wrong metadata size, oversized
alignment, bad relocation link). Target: self-tests for the archive,
the export index, the signature check on the real test modules (bad
key id, flipped byte, truncated trailer), loading a module that exports
a function, loading a dependant that calls it, refusing to unload a
dependency (`-EBUSY`), unload order, reload, a module whose `init`
fails, and a `vm_query` check that module text is RX and rodata is
read-only. The boot test requires the log line for the boot-loaded
`hello` module. Details in `testing.md`.

## Future extensibility

- AArch64: `arch_module_reloc` for `R_AARCH64_*`, a near arena inside
  the image's ±128 MiB adrp reach, no generic change.
- A firmware-provisioned key ring: `keyring_add()` at boot from a
  UEFI variable; the trailer's `key_id` already selects the key.
- Per-symbol versions: an optional `.ksymvers` section checked when
  present.
- User-initiated loading (`sys_module_load`) once credentials exist,
  and module parameters through the metadata `reserved` words.
- Loading from the VFS: the pipeline takes a byte buffer, so the source
  is the only thing that changes.
