# Kernel modules: architecture

Constitution sections 23 (module architecture), 24 (kernel ABI versus
module ABI), 15 (W^X applies to modules), 13 (core stays small), and the
Phase 5 roadmap entry: ELF module loader, symbol resolution, relocations,
module dependencies, module signing.

## Where it sits

```text
    boot archive (boot.tar, untrusted bytes)      kernel/core/bootarchive.c
                    │
                    ▼
    kernel/module/          modsig.c   signature trailer, key lookup, Ed25519
                            modelf.c   ET_REL validation and layout (pure, host-testable)
                            module.c   load pipeline, symbol tables, dependencies, unload
                            ksym.c     the kernel's exported symbol table
                    │                         │
                    ▼                         ▼
    kernel/security/        sha512.c, ed25519.c, keyring.c (built-in trusted keys)
    kernel/memory/          vm_kernel_alloc(VM_KALLOC_NEAR_KERNEL), vm_kernel_protect
```

The module subsystem is core infrastructure (section 13 lists "kernel
module infrastructure" in the core) but it is a consumer of the memory
and security layers, never something they call back into. The security
directory gains its first code in this phase: the hash and signature
primitives and the compiled-in key ring. They know nothing about modules.

## Purpose

Let ring-0 code that is not part of the kernel image be loaded at run
time from an ELF container, bound to a deliberately small and versioned
module ABI, verified against a compiled-in public key before a single
byte of it is mapped, placed under W^X, initialised, tracked, and
unloaded. Everything that arrives later as a module (drivers in Phase 6,
filesystems and network protocols afterwards) goes through this path.

## Responsibilities

- The **boot archive**: a ustar archive delivered by the loader (boot
  protocol v3 field `archive_phys`/`archive_size`, memory type
  `COSMOBOOT_MEM_ARCHIVE`). The kernel validates every header before it
  trusts a byte and exposes name-based lookup. `init` and the boot-time
  modules come from it. This replaces the single v2 module.
- The **module ABI** (`kernel/include/kernel/module.h`): the metadata
  structure every module carries in its `.cosmo.module` section, the
  `COSMO_MODULE()` declaration macro, `EXPORT_SYMBOL()`, the ABI version
  constant, and the capability flags. It is a separate contract from the
  kernel's internal API (section 24): only symbols marked with
  `EXPORT_SYMBOL` are visible to modules, and the ABI version changes
  whenever the exported set or any exported structure changes
  incompatibly.
- The **load pipeline** in the order the constitution prescribes:
  validate ELF, validate architecture, validate ABI, verify signature,
  resolve dependencies, allocate memory, apply relocations (which
  resolves symbols), enforce W^X, initialise, register.
- **Symbol tables**: the kernel's exported table (`.ksymtab`, sorted at
  boot for binary search), each loaded module's exports, lookup by name
  for the loader and for diagnostics.
- **Dependencies and lifetime**: a module names the modules it needs;
  they must already be loaded; the loader only resolves a foreign symbol
  from a declared dependency; a module with dependants cannot be
  unloaded (`-EBUSY`).
- **Signing policy**: signatures are mandatory by default
  (`CONFIG_MODULE_SIG_ENFORCE=1`). An unsigned or badly signed module is
  refused before allocation. A build with enforcement off is a
  development build; such modules load with a warning and the kernel
  records itself as tainted in its panic report.

## Non-responsibilities

- Dynamic linking of user programs (Phase 4 loads static executables;
  `ld.so` is a later userland phase).
- Driver, filesystem, or network registration. A module's `init()` calls
  those subsystems' registration functions once they exist (Phase 6+).
  The `capabilities` field says which it intends to use; Phase 5 records
  it, later phases enforce it.
- Loading from a filesystem or from user space. No VFS exists; the only
  source is the boot archive. A `sys_module_load` call is deliberately
  absent: it needs credentials and capability checks that arrive with
  the security phase.
- Secure Boot integration and key provisioning from firmware. The key
  ring is compiled in from the `.pub` files in `tools/keys/`. The signature format
  carries a key id so a firmware-provisioned ring can be added without a
  format change.
- Versioned symbol CRCs per symbol (Linux `modversions`). The whole ABI
  is versioned as one number; finer granularity is future work.
- AArch64 relocations. The relocation applier is the only
  architecture-specific piece and lives behind `arch_module_reloc()` so
  an AArch64 implementation adds a file, not a rewrite.

## Interfaces at a glance

| Interface | Header | Used by |
|---|---|---|
| `bootarchive_init/find/entry/count` | `kernel/bootarchive.h` | `kernel_main` (init, boot modules), self-tests |
| `COSMO_MODULE`, `EXPORT_SYMBOL`, `struct cosmo_module_info` | `kernel/module.h` | every module, kernel export sites |
| `module_init`, `module_load`, `module_unload`, `module_find`, `module_symbol_lookup`, `module_dump`, `module_load_boot` | `kernel/module.h` | `kernel_main`, self-tests, diagnostics |
| `ksym_lookup`, `ksym_count` | `kernel/ksym.h` | loader, diagnostics |
| `modelf_validate`, `modelf_check_info`, `struct modelf_layout` | `kernel/modelf.h` | loader, host tests, self-tests |
| `modsig_check` | `kernel/modsig.h` | loader, self-tests |
| `sha512_*`, `ed25519_verify` | `kernel/crypto.h` | modsig, host tests |
| `keyring_find` | `kernel/keyring.h` | modsig |
| `vm_kernel_alloc(VM_KALLOC_NEAR_KERNEL)`, `vm_kernel_protect` | `kernel/vmm.h` | loader |
| `arch_module_reloc` | `arch/module.h` | loader |

Tools: `scripts/mkbootarchive.py` (reproducible ustar), `scripts/modsign.py`
(key generation, sign, verify, pure Python Ed25519), `scripts/gen-keyring.py`
(compiles the `.pub` files in `tools/keys/` into the kernel), `build/module.mk` (module
build rules: compile with the kernel flags, `ld.lld -r`, sign).
