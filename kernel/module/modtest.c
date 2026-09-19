/*
 * modtest.c - Self-tests for the boot archive, the export table, the
 * signature check, and the module loader, using the fixtures under
 * tests/ in the boot archive (cosmotest, cosmotest_dep, cosmotest_fail).
 */

#include <kernel/bootarchive.h>
#include <kernel/bootinfo.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/ksym.h>
#include <kernel/log.h>
#include <kernel/modelf.h>
#include <kernel/modsig.h>
#include <kernel/module.h>
#include <kernel/object.h>
#include <kernel/printf.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/vmm.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

/* Fixture bytes, or NULL (no archive: tests skip). */
static const void *fixture(const char *name, size_t *size)
{
    const void *data;
    if (!bootarchive_find(name, &data, size))
        return NULL;
    return data;
}

bool selftest_bootarchive(const char **reason)
{
    const struct cosmoboot_info *info = bootinfo_get();
    if (info->archive_size == 0) {
        kinfo("selftest: no boot archive; skipping");
        return true;
    }
    CHECK(bootarchive_count() > 0);
    const void *data;
    size_t size;
    CHECK(bootarchive_find("init", &data, &size));
    CHECK(size > 0);
    CHECK(!bootarchive_find("no/such/file", &data, &size));
    CHECK(!bootarchive_find("", &data, &size));

    const uint8_t *lo = bootinfo_phys_to_virt(info->archive_phys);
    const uint8_t *hi = lo + info->archive_size;
    for (unsigned i = 0; i < bootarchive_count(); i++) {
        const struct bootarchive_entry *e = bootarchive_entry(i);
        CHECK(e != NULL);
        CHECK(e->name[0] != '\0');
        CHECK((const uint8_t *)e->data >= lo && (const uint8_t *)e->data + e->size <= hi);
        CHECK(((uintptr_t)e->data - (uintptr_t)lo) % 512 == 0);
    }
    CHECK(bootarchive_entry(bootarchive_count()) == NULL);
    return true;
}

bool selftest_ksym(const char **reason)
{
    CHECK(ksym_count() >= 30);
    CHECK(ksym_lookup("kprintf") == (uintptr_t)kprintf);
    CHECK(ksym_lookup("kmalloc") == (uintptr_t)kmalloc);
    CHECK(ksym_lookup("memcpy") == (uintptr_t)memcpy);
    CHECK(ksym_lookup("no_such_symbol_") == 0);
    CHECK(ksym_lookup("") == 0);
    /* Internals must not be exported. */
    CHECK(ksym_lookup("pmm_alloc_page") == 0);
    CHECK(ksym_lookup("schedule") == 0);
    for (size_t i = 1; i < ksym_count(); i++)
        CHECK(strcmp(ksym_entry(i - 1)->name, ksym_entry(i)->name) < 0);
    const struct module *owner = (const struct module *)1;
    CHECK(module_symbol_lookup("kfree", &owner) == (uintptr_t)kfree && owner == NULL);
    return true;
}

bool selftest_modsig(const char **reason)
{
    size_t size;
    const void *file = fixture("tests/cosmotest.ko", &size);
    if (file == NULL) {
        kinfo("selftest: no tests/cosmotest.ko in the boot archive; skipping");
        return true;
    }
    CHECK(keyring_count() >= 1);
    const char *why = "";
    size_t payload = 0;
    CHECK(modsig_check(file, size, &payload, &why) == 0);
    CHECK(payload == size - sizeof(struct modsig_trailer));

    uint8_t *copy = kmalloc(size, 0);
    CHECK(copy != NULL);
    bool ok = true;

    /* Flip one byte of the ELF: signature must fail. */
    memcpy(copy, file, size);
    copy[payload / 2] ^= 0x01;
    ok = ok && modsig_check(copy, size, NULL, &why) == -EKEYREJECTED;

    /* Flip one byte of the signature itself. */
    memcpy(copy, file, size);
    copy[payload + 3] ^= 0x80;
    ok = ok && modsig_check(copy, size, NULL, &why) == -EKEYREJECTED;

    /* Unknown key id. */
    memcpy(copy, file, size);
    copy[payload + ED25519_SIGNATURE_SIZE] ^= 0xff;
    ok = ok && modsig_check(copy, size, NULL, &why) == -ENOKEY;

    /* Wrong algorithm and version. */
    memcpy(copy, file, size);
    copy[payload + ED25519_SIGNATURE_SIZE + KEYRING_ID_SIZE + 4] = 9;
    ok = ok && modsig_check(copy, size, NULL, &why) == -EKEYREJECTED;
    memcpy(copy, file, size);
    copy[payload + ED25519_SIGNATURE_SIZE + KEYRING_ID_SIZE] = 2;
    ok = ok && modsig_check(copy, size, NULL, &why) == -EKEYREJECTED;

    /* No trailer at all (truncated by one byte), and a tiny file. */
    ok = ok && modsig_check(file, size - 1, NULL, &why) == -ENOKEY;
    ok = ok && modsig_check(file, 10, NULL, &why) == -ENOKEY;

    /* The trailer must never be accepted for a different payload. */
    memcpy(copy, file, size);
    memmove(copy + 1, copy, size - 1 - sizeof(struct modsig_trailer));
    ok = ok && modsig_check(copy, size, NULL, &why) == -EKEYREJECTED;

    kfree(copy);
    CHECK(ok);
    return true;
}

static bool validate_variant(const void *file, size_t size, void (*mutate)(uint8_t *elf, size_t n),
                             const char *expect_why)
{
    uint8_t *copy = kmalloc(size, 0);
    if (copy == NULL)
        return false;
    memcpy(copy, file, size);
    mutate(copy, size);
    struct modelf_layout *l = kzalloc(sizeof(*l));
    if (l == NULL) {
        kfree(copy);
        return false;
    }
    const char *why = "";
    int rc = modelf_validate(copy, size, l, &why);
    bool ok = rc == -ENOEXEC && strcmp(why, expect_why) == 0;
    if (!ok)
        kerror("selftest: modelf variant '%s' gave rc %d why '%s'", expect_why, rc, why);
    kfree(l);
    kfree(copy);
    return ok;
}

static void mut_type(uint8_t *elf, size_t n)
{
    (void)n;
    elf[16] = ET_EXEC;
}

static void mut_machine(uint8_t *elf, size_t n)
{
    (void)n;
    elf[18] = 0x34;   /* no such machine: rejected on every target */
    elf[19] = 0x12;
}

static void mut_magic(uint8_t *elf, size_t n)
{
    (void)n;
    elf[1] = 'X';
}

/* Make .text writable as well as executable. */
static void mut_wx(uint8_t *elf, size_t n)
{
    (void)n;
    struct elf64_ehdr *eh = (struct elf64_ehdr *)elf;
    struct elf64_shdr *sh = (struct elf64_shdr *)(elf + eh->e_shoff);
    for (unsigned i = 1; i < eh->e_shnum; i++) {
        if (sh[i].sh_flags & SHF_EXECINSTR) {
            sh[i].sh_flags |= SHF_WRITE;
            return;
        }
    }
}

/* Push one section past the end of the file. */
static void mut_oob(uint8_t *elf, size_t n)
{
    struct elf64_ehdr *eh = (struct elf64_ehdr *)elf;
    struct elf64_shdr *sh = (struct elf64_shdr *)(elf + eh->e_shoff);
    for (unsigned i = 1; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_PROGBITS && sh[i].sh_size > 0) {
            sh[i].sh_offset = (uint64_t)n - sh[i].sh_size + 1;
            return;
        }
    }
}

static void mut_info_size(uint8_t *elf, size_t n)
{
    (void)n;
    struct elf64_ehdr *eh = (struct elf64_ehdr *)elf;
    struct elf64_shdr *sh = (struct elf64_shdr *)(elf + eh->e_shoff);
    const struct elf64_shdr *names = &sh[eh->e_shstrndx];
    for (unsigned i = 1; i < eh->e_shnum; i++) {
        const char *nm = (const char *)elf + names->sh_offset + sh[i].sh_name;
        if (strcmp(nm, ".cosmo.module") == 0) {
            sh[i].sh_size -= 8;
            return;
        }
    }
}

bool selftest_module_reject(const char **reason)
{
    size_t size;
    const void *file = fixture("tests/cosmotest.ko", &size);
    if (file == NULL) {
        kinfo("selftest: no tests/cosmotest.ko in the boot archive; skipping");
        return true;
    }
    size_t elf_size = size - sizeof(struct modsig_trailer);

    /* The genuine payload validates. */
    struct modelf_layout *l = kzalloc(sizeof(*l));
    CHECK(l != NULL);
    const char *why = "";
    int rc = modelf_validate(file, elf_size, l, &why);
    bool ok = rc == 0 && l->group_size[MODELF_TEXT] > 0 && l->group_size[MODELF_RODATA] > 0 &&
              l->group_size[MODELF_DATA] > 0 && l->ksymtab_section != 0;
    const struct cosmo_module_info *info =
        (const struct cosmo_module_info *)((const uint8_t *)file + l->info_file_off);
    ok = ok && modelf_check_info(info, &why) == 0 && strcmp(info->name, "cosmotest") == 0;
    kfree(l);
    CHECK(ok);

    /* Crafted variants, each failing on exactly the intended rule. */
    CHECK(validate_variant(file, elf_size, mut_type, "not a relocatable object (ET_REL)"));
    CHECK(validate_variant(file, elf_size, mut_machine, "not a native (" ELF_MACHINE_NATIVE_NAME ") object"));
    CHECK(validate_variant(file, elf_size, mut_magic, "bad ELF magic"));
    CHECK(validate_variant(file, elf_size, mut_wx, "section is writable and executable (W^X)"));
    CHECK(validate_variant(file, elf_size, mut_oob, "section outside the file"));
    CHECK(validate_variant(file, elf_size, mut_info_size, ".cosmo.module has the wrong size"));

    /* Metadata rules. */
    struct cosmo_module_info bad = *info;
    bad.abi_version = COSMO_MODULE_ABI_VERSION + 1;
    CHECK(modelf_check_info(&bad, &why) == -ENOEXEC && strcmp(why, "module ABI version mismatch") == 0);
    bad = *info;
    bad.magic[0] = 'x';
    CHECK(modelf_check_info(&bad, &why) == -ENOEXEC);
    bad = *info;
    strlcpy(bad.name, "bad name!", sizeof(bad.name));
    CHECK(modelf_check_info(&bad, &why) == -ENOEXEC);
    bad = *info;
    strlcpy(bad.deps, "a,,b", sizeof(bad.deps));
    CHECK(modelf_check_info(&bad, &why) == -ENOEXEC);
    bad = *info;
    bad.reserved[2] = 1;
    CHECK(modelf_check_info(&bad, &why) == -ENOEXEC);

    /* Through the public entry point: garbage is refused before parsing,
     * a dependant without its dependency is refused before allocation. */
    static const uint8_t garbage[64] = { 1, 2, 3 };
    CHECK(module_load(garbage, sizeof(garbage), "garbage", NULL) == (modsig_enforced() ? -ENOKEY : -ENOEXEC));
    size_t dep_size;
    const void *dep = fixture("tests/cosmotest_dep.ko", &dep_size);
    CHECK(dep != NULL);
    CHECK(module_find("cosmotest") == NULL);
    CHECK(module_load(dep, dep_size, "tests/cosmotest_dep.ko", NULL) == -ENOENT);
    CHECK(module_find("cosmotest_dep") == NULL);
    return true;
}

static bool prot_is(vaddr_t va, vm_prot_t want)
{
    vm_prot_t prot;
    paddr_t pa;
    if (!vm_query(va, &pa, &prot, NULL, NULL))
        return false;
    return (prot & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC)) == want;
}

bool selftest_module_load(const char **reason)
{
    size_t size, dep_size;
    const void *file = fixture("tests/cosmotest.ko", &size);
    const void *dep = fixture("tests/cosmotest_dep.ko", &dep_size);
    if (file == NULL || dep == NULL) {
        kinfo("selftest: module fixtures missing from the boot archive; skipping");
        return true;
    }
    CHECK(module_find("hello") != NULL);   /* loaded at boot */

    struct vm_stats before;
    vm_get_stats(&before);
    unsigned count_before = module_count();

    struct module *m = NULL;
    CHECK(module_load(file, size, "tests/cosmotest.ko", &m) == 0);
    CHECK(m != NULL && module_find("cosmotest") == m);
    CHECK(m->state == MODULE_LIVE && m->refs == 0 && m->nr_deps == 0);
    CHECK(m->capabilities == MODULE_CAP_TEST && (m->flags & MODULE_FLAG_UNSIGNED) == 0);
    CHECK(m->text && m->rodata && m->data);
    CHECK(prot_is(m->text, VM_PROT_RX));
    CHECK(prot_is(m->rodata, VM_PROT_READ));
    CHECK(prot_is(m->data, VM_PROT_RW));
    CHECK(prot_is((vaddr_t)m->info, VM_PROT_READ));
    CHECK(m->info->init != NULL && (vaddr_t)m->info->init >= m->text &&
          (vaddr_t)m->info->init < m->text + m->text_size);
    CHECK(m->nr_exports == 5);   /* table, counter, answer, released, object_take */

    /* Call into the module through its export table. */
    const struct module *owner = NULL;
    uintptr_t fn = module_symbol_lookup("cosmotest_answer", &owner);
    CHECK(fn != 0 && owner == m);
    CHECK(prot_is(fn, VM_PROT_RX));
    int (*answer)(void) = (int (*)(void))fn;
    CHECK(answer() == 42);
    int *counter = (int *)module_symbol_lookup("cosmotest_counter", NULL);
    CHECK(counter != NULL && *counter == 101);
    const int *table = (const int *)module_symbol_lookup("cosmotest_table", NULL);
    CHECK(table != NULL && table[3] == 36 && prot_is((vaddr_t)table, VM_PROT_READ));

    /* Second load of the same name, and the dependant. */
    CHECK(module_load(file, size, "tests/cosmotest.ko", NULL) == -EEXIST);
    struct module *d = NULL;
    CHECK(module_load(dep, dep_size, "tests/cosmotest_dep.ko", &d) == 0);
    CHECK(d != NULL && d->nr_deps == 1 && d->deps[0] == m && m->refs == 1);
    CHECK(*counter == 111);
    int (*sum)(void) = (int (*)(void))module_symbol_lookup("cosmotest_dep_sum", NULL);
    CHECK(sum != NULL && sum() == 42 + 36 + 111);
    CHECK(module_count() == count_before + 2);

    /* Unload order is enforced by the reference count. */
    CHECK(module_unload("cosmotest") == -EBUSY);
    CHECK(module_find("cosmotest") == m);
    CHECK(module_unload("cosmotest_dep") == 0);
    CHECK(module_find("cosmotest_dep") == NULL && m->refs == 0);
    CHECK(*counter == 101);
    CHECK(module_symbol_lookup("cosmotest_dep_sum", NULL) == 0);
    CHECK(module_unload("cosmotest") == 0);
    CHECK(module_find("cosmotest") == NULL);
    CHECK(module_symbol_lookup("cosmotest_answer", NULL) == 0);
    CHECK(module_unload("cosmotest") == -ENOENT);

    /* Reload works, and everything is released afterwards. */
    CHECK(module_load(file, size, "tests/cosmotest.ko", &m) == 0);
    CHECK(module_unload("cosmotest") == 0);
    struct vm_stats after;
    vm_get_stats(&after);
    CHECK(after.regions == before.regions);
    CHECK(after.anon_pages == before.anon_pages);
    CHECK(module_count() == count_before);
    return true;
}

bool selftest_module_fail(const char **reason)
{
    size_t size;
    const void *file = fixture("tests/cosmotest_fail.ko", &size);
    if (file == NULL) {
        kinfo("selftest: no tests/cosmotest_fail.ko in the boot archive; skipping");
        return true;
    }
    struct vm_stats before;
    vm_get_stats(&before);
    unsigned count_before = module_count();

    struct module *m = (struct module *)1;
    CHECK(module_load(file, size, "tests/cosmotest_fail.ko", &m) == -EIO);
    CHECK(m == (struct module *)1);   /* untouched on failure */
    CHECK(module_find("cosmotest_fail") == NULL);
    CHECK(module_count() == count_before);

    struct vm_stats after;
    vm_get_stats(&after);
    CHECK(after.regions == before.regions);
    CHECK(after.anon_pages == before.anon_pages);
    return true;
}

/* --- unload with a live object: zombie, then reaped ------------------------------
 *
 * docs/kernel/quiesce/design.md, "Module unload": a kobject whose release
 * code lives in the module keeps the module's memory mapped past unload;
 * the release runs from module text after the unload returned -EBUSY,
 * and a second unload frees the zombie.
 */
/* --- the zombie nobody comes back for ------------------------------------- *
 *
 * selftest_module_unload_busy proves the reap works when you ask for
 * it: build a zombie, let its object go, unload the name again, freed.
 * Everything it asserts is true, and it is why this went unnoticed --
 * the mechanism was tested and the policy was that nothing invoked it.
 *
 * These two ask the question that one does not: who asks?
 * (docs/audit/next-subsystem-module-zombie-reap.md)
 */
bool selftest_module_zombie_swept(const char **reason)
{
    size_t size;
    const void *file = fixture("tests/cosmotest.ko", &size);
    if (file == NULL) {
        kinfo("selftest: module fixture missing from the boot archive; skipping");
        return true;
    }
    struct module *m = NULL;
    CHECK(module_load(file, size, "tests/cosmotest.ko", &m) == 0);
    struct kobject *(*take)(void) = (struct kobject * (*)(void)) module_symbol_lookup("cosmotest_object_take", NULL);
    CHECK(take != NULL);
    struct kobject *obj = take();
    CHECK(obj != NULL);

    /* Restore the GLOBAL timeout before checking: a CHECK returns
     * immediately, and leaving 50 ms behind would make every later
     * test that unloads a module look broken. */
    module_set_unload_timeout_ms(50);
    int rc = module_unload("cosmotest");
    module_set_unload_timeout_ms(5000);
    CHECK(rc == -EBUSY);                           /* a zombie now */
    kobject_put(obj);                              /* its last object goes */

    /*
     * THE CLAIM. Nothing here unloads "cosmotest" again -- that is the
     * request nobody makes. An unrelated load is enough, and after it
     * the zombie is gone: a later unload of the name finds nothing
     * rather than finding a zombie to free.
     */
    struct module *other = NULL;
    CHECK(module_load(file, size, "tests/cosmotest.ko", &other) == 0);
    CHECK(module_unload("cosmotest") == 0);        /* this unloads OTHER, the live one */
    CHECK(module_unload("cosmotest") == -ENOENT);  /* and no zombie was left behind */

    kinfo("selftest: module-zombie-swept: a zombie whose objects died was collected by an "
          "unrelated load, with no unload of its own name");
    return true;
}

bool selftest_module_zombie_name_reused(const char **reason)
{
    size_t size;
    const void *file = fixture("tests/cosmotest.ko", &size);
    if (file == NULL) {
        kinfo("selftest: module fixture missing from the boot archive; skipping");
        return true;
    }
    struct module *m = NULL;
    CHECK(module_load(file, size, "tests/cosmotest.ko", &m) == 0);
    struct kobject *(*take)(void) = (struct kobject * (*)(void)) module_symbol_lookup("cosmotest_object_take", NULL);
    CHECK(take != NULL);
    struct kobject *obj = take();
    CHECK(obj != NULL);

    module_set_unload_timeout_ms(50);
    int rc = module_unload("cosmotest");
    module_set_unload_timeout_ms(5000);
    CHECK(rc == -EBUSY);

    /*
     * The name is reused while the zombie is still holding its object.
     * Before the sweep this was the case that hid a zombie for good:
     * module_unload finds the LIVE module of that name and never looks
     * at the zombie list, so the only reaper could not reach it.
     */
    struct module *repl = NULL;
    CHECK(module_load(file, size, "tests/cosmotest.ko", &repl) == 0);
    CHECK(repl != m);
    CHECK(module_find("cosmotest") == repl);

    kobject_put(obj);                              /* the zombie is now collectable */

    /* Unloading the replacement sweeps the zombie too, and the sweep is
     * by identity: it does not care that they share a name. */
    CHECK(module_unload("cosmotest") == 0);        /* the replacement */
    CHECK(module_unload("cosmotest") == -ENOENT);  /* nothing left, zombie included */

    kinfo("selftest: module-zombie-name-reused: a zombie hidden behind a replacement of its "
          "own name was still collected");
    return true;
}

bool selftest_module_zombie_two_of_a_name(const char **reason)
{
    size_t size;
    const void *file = fixture("tests/cosmotest.ko", &size);
    if (file == NULL) {
        kinfo("selftest: module fixture missing from the boot archive; skipping");
        return true;
    }
    struct kobject *(*take)(void);
    struct module *m1 = NULL, *m2 = NULL;
    struct kobject *o1, *o2;

    /* Two zombies of one name, made the only way they can be: load,
     * strand an object, unload, and do it again under the same name. */
    CHECK(module_load(file, size, "tests/cosmotest.ko", &m1) == 0);
    take = (struct kobject * (*)(void)) module_symbol_lookup("cosmotest_object_take", NULL);
    CHECK(take != NULL);
    o1 = take();
    CHECK(o1 != NULL);
    module_set_unload_timeout_ms(50);
    int rc1 = module_unload("cosmotest");
    if (rc1 != -EBUSY) {
        module_set_unload_timeout_ms(5000);
        *reason = "the first zombie was not created";
        return false;
    }

    CHECK(module_load(file, size, "tests/cosmotest.ko", &m2) == 0);
    CHECK(m2 != m1);
    take = (struct kobject * (*)(void)) module_symbol_lookup("cosmotest_object_take", NULL);
    CHECK(take != NULL);
    o2 = take();
    CHECK(o2 != NULL);
    int rc2 = module_unload("cosmotest");
    module_set_unload_timeout_ms(5000);
    CHECK(rc2 == -EBUSY);

    /* Both collectable now. */
    kobject_put(o1);
    kobject_put(o2);

    /*
     * ONE sweep takes both. The name lookup cannot: it finds the first
     * match, so before this unit each zombie needed its own
     * otherwise-unmotivated call -- and the report's first draft wrongly
     * said the second could never be found at all. It can; it just
     * needed a second request nobody had a reason to make.
     */
    struct module *live = NULL;
    CHECK(module_load(file, size, "tests/cosmotest.ko", &live) == 0);
    CHECK(module_unload("cosmotest") == 0);        /* the live one */
    CHECK(module_unload("cosmotest") == -ENOENT);  /* BOTH zombies gone */

    kinfo("selftest: module-zombie-two-of-a-name: two zombies sharing a name were collected "
          "by one sweep, which the name lookup takes two calls to do");
    return true;
}

bool selftest_module_slots_enospc(const char **reason)
{
#if !CONFIG_DEBUG
    /* The cap that makes -ENOSPC reachable is a debug-only seam. */
    (void)reason;
    kinfo("selftest: module-slots-enospc: needs the debug slot cap; skipping");
    return true;
#else
    size_t size;
    const void *file = fixture("tests/cosmotest.ko", &size);
    if (file == NULL) {
        kinfo("selftest: module fixture missing from the boot archive; skipping");
        return true;
    }
    /*
     * Reaching MODULE_MAX_LIVE honestly would need thirty-two distinct
     * modules -- duplicate names are refused -- so the bound is capped
     * instead. The SEARCH and the error return are the real ones; only
     * the number changes.
     *
     * What this pins is not just the errno. Before the slot was
     * reserved ahead of `init()`, failing here would have left an
     * initialised, linked, counted module behind, so the load that
     * follows is the assertion that matters: the loader is still
     * usable, which a botched unwind would not leave it.
     */
    module_set_max_live_for_test(1);      /* every slot past the first is unreachable */
    struct module *m = NULL;
    int rc = module_load(file, size, "tests/cosmotest.ko", &m);
    module_set_max_live_for_test(0);      /* the real bound back */

    CHECK(rc == -ENOSPC);
    CHECK(m == NULL);
    CHECK(module_find("cosmotest") == NULL);   /* nothing was published */

    /* And the loader still works, which is what a bad unwind would break. */
    CHECK(module_load(file, size, "tests/cosmotest.ko", &m) == 0);
    CHECK(module_unload("cosmotest") == 0);

    kinfo("selftest: module-slots-enospc: a full slot array refuses with -ENOSPC, publishes "
          "nothing, and leaves the loader usable");
    return true;
#endif
}

bool selftest_module_unload_busy(const char **reason)
{
    size_t size;
    const void *file = fixture("tests/cosmotest.ko", &size);
    if (file == NULL) {
        kinfo("selftest: module fixture missing from the boot archive; skipping");
        return true;
    }
    struct module *m = NULL;
    CHECK(module_load(file, size, "tests/cosmotest.ko", &m) == 0);
    struct kobject *(*take)(void) = (struct kobject * (*)(void)) module_symbol_lookup("cosmotest_object_take", NULL);
    int *released = (int *)module_symbol_lookup("cosmotest_released", NULL);
    CHECK(take != NULL && released != NULL);

    /* A kernel address has no module owner; the fixture's does. */
    CHECK(module_owner_of((uintptr_t)&selftest_module_unload_busy) == NULL);
    struct module *owner = module_owner_of((uintptr_t)take);
    CHECK(owner == m);
    module_object_released(owner);
    CHECK(m->live_objects == 0);

    struct kobject *obj = take();
    CHECK(obj != NULL && obj->owner == m && m->live_objects == 1);

    module_set_unload_timeout_ms(50);
    uint64_t t0 = clock_now_ns();
    int rc = module_unload("cosmotest");
    uint64_t waited = clock_since_ns(t0);
    module_set_unload_timeout_ms(5000);
    CHECK(rc == -EBUSY);
    CHECK(waited >= 50000000ULL);                 /* it waited the timeout for the object */
    CHECK(module_find("cosmotest") == NULL);      /* not live */
    CHECK(module_symbol_lookup("cosmotest_object_take", NULL) == 0);
    CHECK(*released == 0);

    /* The release runs from the zombie's text. */
    kobject_put(obj);
    CHECK(*released == 1);
    CHECK(m->live_objects == 0);

    /* Reaping frees it; a third unload finds nothing. */
    CHECK(module_unload("cosmotest") == 0);
    CHECK(module_unload("cosmotest") == -ENOENT);

    /* The name is usable again, and a clean unload still works. */
    CHECK(module_load(file, size, "tests/cosmotest.ko", &m) == 0);
    CHECK(module_unload("cosmotest") == 0);

    /* A zombie keeps its dependencies pinned: cosmotest_dep's release
     * calls cosmotest_answer(), so cosmotest must stay mapped until the
     * zombie's object is gone. */
    size_t dep_size;
    const void *dep = fixture("tests/cosmotest_dep.ko", &dep_size);
    CHECK(dep != NULL);
    struct module *d = NULL;
    CHECK(module_load(file, size, "tests/cosmotest.ko", &m) == 0);
    CHECK(module_load(dep, dep_size, "tests/cosmotest_dep.ko", &d) == 0);
    CHECK(m->refs == 1);
    struct kobject *(*dtake)(void) = (struct kobject * (*)(void)) module_symbol_lookup("cosmotest_dep_object_take", NULL);
    int *dreleased = (int *)module_symbol_lookup("cosmotest_dep_released", NULL);
    CHECK(dtake != NULL && dreleased != NULL);
    struct kobject *dobj = dtake();
    CHECK(dobj != NULL && dobj->owner == d);
    module_set_unload_timeout_ms(50);
    rc = module_unload("cosmotest_dep");
    module_set_unload_timeout_ms(5000);
    CHECK(rc == -EBUSY);
    CHECK(m->refs == 1);                            /* still pinned by the zombie */
    CHECK(module_unload("cosmotest") == -EBUSY);    /* so the dependency cannot go */
    kobject_put(dobj);                              /* release runs, calling into cosmotest */
    CHECK(*dreleased == 42);
    CHECK(module_unload("cosmotest_dep") == 0);     /* reap: the pin drops here */
    CHECK(m->refs == 0);
    CHECK(module_unload("cosmotest") == 0);
    return true;
}
