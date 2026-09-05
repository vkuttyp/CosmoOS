# Kernel modules: invariants

Each invariant names how it is checked: build (compile-time or link),
assert (KASSERT/panic at runtime), test (a host test or a self-test
name), or review. Changing any of them means revising this document and
the code together.

## Trust and code execution

**M1. No byte of a module executes before its signature has verified
and its text is read-only.** `module_load` runs `modsig_check` before
parsing a single ELF field; text and rodata are copied and relocated
through RW mappings, then `vm_kernel_protect` flips them to RX and R
(with a TLB shootdown) before `init()` is called. The only calls into
module memory are `init` and `shutdown` (and exported symbols, after
registration). Check: test `module-load` (`vm_query` reports RX/R/RW on
the three regions before the first exported call), review of the
pipeline order in `load_locked`.

**M2. A bad signature is refused in every build.** `-EKEYREJECTED` is
never downgraded; only `-ENOKEY` (no trailer, unknown key) is turned
into a warning, and only when `CONFIG_MODULE_SIG_ENFORCE=0`, which
also sets `TAINT_UNSIGNED_MODULE`. Check: test `modsig` (flipped
payload byte, flipped signature byte, wrong version, wrong algorithm,
shifted payload all `-EKEYREJECTED`), the `MODULE_SIG_ENFORCE=0` boot
(`testing.md`), review.

**M3. The signature covers every byte the loader will read.** The
trailer is the last 88 bytes and the signature is over `[0, size-88)`;
the ELF is validated on exactly that prefix (`elf_size`). Check: test
`modsig` (trailer accepted for its own payload only), review.

**M4. The module ABI version matches exactly.** `modelf_check_info`
compares `abi_version` with `COSMO_MODULE_ABI_VERSION`; there is no
range. Check: host `modelf-info`, test `module-reject`.

**M5. Nothing is trusted from the file before it is bounds-checked.**
Every section offset, size, name index, symbol index, relocation
offset, and the metadata block are validated by `modelf_validate` /
`modelf_check_info` before `module.c` dereferences them; the loader
never re-derives a bound from file data. Check: host `test_modelf` (36
rule assertions on synthetic images under ASan/UBSan), test
`module-reject` (six crafted variants of a real module), review.

## Memory and W^X

**M6. A module occupies at most three regions, one per protection, and
none is writable and executable.** Groups are TEXT (RX), RODATA (R),
DATA (RW); a section that is both writable and executable is refused
at validation; `vm_kernel_alloc` and `vm_kernel_protect` refuse W+X.
Check: host `modelf-sections`, test `module-load`,
`scripts/check-module-elf.py` on every built module, review.

**M7. Module memory lies in the near arena inside the top 2 GiB above
the image.** `VM_KALLOC_NEAR_KERNEL` selects
`[0xFFFFFFFF88000000, 0xFFFFFFFFFF000000)`; `vmm_init` panics if the
image reaches it. This is what makes `-mcmodel=kernel` relocations
(`32S`, `PC32`) valid for module code. Check: assert (`vmm_init`), test
`module-load` (a `PC32`-relocated call into the module returns 42),
memory invariant M30.

**M8. The metadata block ends up read-only.** `.cosmo.module` must be
non-writable and land in the RODATA group; `m->info` therefore points
into the R region after the flip. Check: host `modelf-sections`, test
`module-load` (`prot_is(info, READ)`).

**M9. Relocations write only inside their target section and only
while it is RW.** `arch_module_reloc` bounds-checks each offset against
the section; all relocation happens before `vm_kernel_protect`. Check:
review; a violation would fault (RX region) and panic.

**M10. Every 32-bit relocation result is range checked.** `PC32`,
`PLT32`, `32S` must fit a signed 32-bit value, `32` an unsigned one;
overflow is `-ERANGE`, never truncation. Check: review (the near arena
makes overflow impossible for correct inputs; the check exists for
hostile ones).

**M11. The boot archive is never written and never freed.** Its memory
is `COSMOBOOT_MEM_ARCHIVE`, reserved by map type; every consumer copies
out of it (`elf_load_into`, `module_load`). Check: review (`const` all
the way from `bootarchive_find`), memory invariant on reserved types.

## Symbols and dependencies

**M12. A module resolves an undefined symbol only from the kernel's
export table or from a module it declared as a dependency.**
`resolve_symbols` consults `ksym_lookup` and then `lookup_in_deps`
over `m->deps[]` only; any other live module's exports are invisible.
Check: test `module-reject` (`cosmotest_dep` without `cosmotest` is
`-ENOENT` before allocation), review.

**M13. The symbol namespace is flat.** A module may not export a name
the kernel exports (`-EEXIST`) or export the same name twice; the
kernel panics at `ksym_init` on a duplicate export. Check: assert
(`ksym_init`), review (`index_exports`).

**M14. A module with live dependants cannot be unloaded.** `refs` counts
dependants; `module_unload` returns `-EBUSY` while it is nonzero; refs
change only under `g_lock`. Check: test `module-load` (`-EBUSY`, then
success after the dependant is gone).

**M15. Dependencies outlive dependants, so `deps[]` pointers never
dangle.** Follows from M14 and from dependencies being resolved before
registration. Check: review.

**M16. Weak undefined symbols resolve to 0; everything else undefined
must resolve.** Check: review (`resolve_symbols`); a host test for a
weak symbol is a gap.

## Lifetime and concurrency

**M17. A failed load leaves nothing behind.** Regions, the record, the
export index, and the symbol scratch array are freed on every failure
path; dependency refs are only incremented at registration. Check:
test `module-fail` and `module-load` (`vm_stats.regions` and
`anon_pages` equal before and after; `module_count` unchanged).

**M18. `g_lock` is held for the whole of `module_load` and
`module_unload`, and `init`/`shutdown` run under it.** Consequently a
module's `init`/`shutdown` must not call `module_load`, `module_unload`,
`module_find`, `module_symbol_lookup`, or `module_count` (deadlock).
`g_lock` nests inside nothing; the VMM and PMM locks are taken beneath
it. Check: review; the mutex would report recursion in debug builds.

**M19. The kernel export index is immutable after `ksym_init` and
`ksym_lookup` is lock-free.** Safe from the panic path. Check: review.

**M20. A live module's `info`, `exports`, and every symbol address
handed out are valid until `module_unload` returns.** There is no
reference counting for lookups; callers of `module_find` and
`module_symbol_lookup` may not hold results across a possible unload.
Check: review.

**M22. A module's memory is freed only after `shutdown()`, one grace
period, and `live_objects == 0`.** A kobject whose release code lives in
the module keeps it mapped; the unloader waits up to the unload timeout
and otherwise leaves a zombie that a later unload reaps
(`docs/kernel/quiesce/invariants.md` Q15–Q16). Check: test
`module-unload-busy` (`-EBUSY` after the timeout, the release runs from
the zombie's text, the second unload frees it).

**M23. `module_owner_of` never returns a module that can be freed before
the caller's increment is seen.** The lookup and the increment happen in
one `quiesce_read_lock` section; unload unpublishes the slot, then waits
a grace period, then reads the count. Check: `module-unload-busy`
(owner lookup of the fixture's export raises the count; a kernel address
gives NULL); review of the ordering.

**M21. `module_load_boot` loads `modules/` entries in archive order and
only those.** Order is the build's dependency order
(`MODULE_ARCHIVE_ENTRIES`); `tests/` entries are never loaded at boot.
Check: boot test marker (`module: loaded hello 1.0`), test
`module-reject` (`cosmotest` is not live when the self-tests start).

## Format

**M22. `struct cosmo_module_info` is 240 bytes and `struct
modsig_trailer` is 88 bytes.** Both are `STATIC_ASSERT`ed; the section
size and the trailer position are checked against them. Check: build.

**M23. Only the relocation types non-PIC `-mcmodel=kernel` code emits
are accepted.** `64`, `PC32`, `PLT32`, `32`, `32S`, `NONE`; GOT forms
and everything else are `-ENOEXEC`. Check: review; `llvm-objdump -r`
on the built modules shows only these types.

## Gaps

- No host test for weak undefined symbols or for `arch_module_reloc`
  overflow paths (M10, M16): both are review-checked. Planned:
  `test_modreloc` on the host with crafted RELA tables.
- SMAP is not enabled, so a module writing to its own rodata is caught
  by the page tables (a fault, then a panic), not by a test. Planned
  with the security phase.
- `capabilities` are recorded, not enforced (later phases).
