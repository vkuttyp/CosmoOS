/*
 * module.c - The kernel module loader.
 *
 * Pipeline (constitution section 23): validate ELF -> validate
 * architecture -> validate ABI -> verify signature -> resolve
 * dependencies -> allocate -> relocate (resolving symbols) -> enforce
 * W^X -> initialise -> register. The signature is checked before the
 * ELF is parsed at all, since parsing is the larger attack surface;
 * nothing from the file is executed before its text is read-only and
 * executable. See docs/kernel/module/design.md.
 */

#include <kernel/bootarchive.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/ksym.h>
#include <kernel/log.h>
#include <kernel/modelf.h>
#include <kernel/modsig.h>
#include <kernel/module.h>
#include <kernel/mutex.h>
#include <kernel/panic.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

#include <arch/module.h>

static struct mutex g_lock;
static LIST_HEAD(g_modules);
static unsigned g_count;

/* Per-module data the public struct does not expose. */
struct module_priv {
    struct module m;
    const struct ksym **export_index;   /* sorted pointers into m.exports */
};

static struct module *find_locked(const char *name)
{
    struct module *m;
    list_for_each_entry(m, &g_modules, link) {
        if (m->state == MODULE_LIVE && strcmp(m->name, name) == 0)
            return m;
    }
    return NULL;
}

static bool in_module(const struct module *m, uintptr_t addr, size_t len)
{
    struct {
        vaddr_t base;
        size_t size;
    } r[3] = { { m->text, m->text_size }, { m->rodata, m->rodata_size }, { m->data, m->data_size } };
    for (unsigned i = 0; i < 3; i++) {
        if (r[i].base && addr >= r[i].base && addr - r[i].base < r[i].size && r[i].size - (addr - r[i].base) >= len)
            return true;
    }
    return false;
}

static void free_module(struct module_priv *p)
{
    struct module *m = &p->m;
    if (m->text)
        vm_kernel_free(m->text);
    if (m->rodata)
        vm_kernel_free(m->rodata);
    if (m->data)
        vm_kernel_free(m->data);
    kfree(p->export_index);
    kfree(p);
}

/* Split the comma-separated dependency list and look each up. */
static int resolve_deps(struct module *m, const struct cosmo_module_info *info, const char **why)
{
    const char *p = info->deps;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char name[MODULE_NAME_MAX];
        memcpy(name, p, len);
        name[len] = '\0';
        struct module *dep = find_locked(name);
        if (dep == NULL) {
            *why = "dependency not loaded";
            kerror("module: %s depends on %s, which is not loaded", info->name, name);
            return -ENOENT;
        }
        KASSERT(m->nr_deps < MODULE_MAX_DEPS);
        m->deps[m->nr_deps++] = dep;
        p = end ? end + 1 : p + len;
    }
    return 0;
}

static uintptr_t lookup_in_deps(const struct module *m, const char *name)
{
    for (unsigned i = 0; i < m->nr_deps; i++) {
        const struct module_priv *dp = (const struct module_priv *)m->deps[i];
        if (dp->export_index == NULL)
            continue;
        uintptr_t a = ksym_search(dp->export_index, dp->m.nr_exports, name);
        if (a)
            return a;
    }
    return 0;
}

static vaddr_t group_base(const struct module *m, unsigned group)
{
    switch (group) {
    case MODELF_TEXT:   return m->text;
    case MODELF_RODATA: return m->rodata;
    default:            return m->data;
    }
}

/* Every symbol's address: sym_addr[i], 0 for unused ones. */
static int resolve_symbols(struct module *m, const void *file, const struct modelf_layout *l,
                           uintptr_t *sym_addr, const char **why)
{
    const struct elf64_shdr *symtab = modelf_shdr(file, l->symtab);
    const struct elf64_shdr *strtab = modelf_shdr(file, l->strtab);
    const struct elf64_sym *syms = (const struct elf64_sym *)((const uint8_t *)file + symtab->sh_offset);
    const char *names = (const char *)file + strtab->sh_offset;

    sym_addr[0] = 0;
    for (uint32_t i = 1; i < l->nr_symbols; i++) {
        const struct elf64_sym *s = &syms[i];
        const char *name = names + s->st_name;   /* bounded by modelf_validate */

        if (s->st_shndx == SHN_UNDEF) {
            uintptr_t a = ksym_lookup(name);
            if (a == 0)
                a = lookup_in_deps(m, name);
            if (a == 0 && ELF64_ST_BIND(s->st_info) == STB_WEAK) {
                sym_addr[i] = 0;
                continue;
            }
            if (a == 0) {
                *why = "unresolved symbol";
                kerror("module: %s: unresolved symbol '%s' (not exported by the kernel or a declared dependency)",
                       m->name, name);
                return -ENOENT;
            }
            sym_addr[i] = a;
            continue;
        }
        if (s->st_shndx == SHN_ABS) {
            sym_addr[i] = (uintptr_t)s->st_value;
            continue;
        }
        const struct modelf_section *ms = modelf_find_section(l, s->st_shndx);
        if (ms == NULL) {
            sym_addr[i] = 0;   /* symbol in a non-allocated (debug) section */
            continue;
        }
        if (s->st_value > ms->size) {
            *why = "symbol value outside its section";
            return -ENOEXEC;
        }
        sym_addr[i] = group_base(m, ms->group) + ms->offset + s->st_value;
    }
    return 0;
}

static int apply_relocations(struct module *m, const void *file, const struct modelf_layout *l,
                             const uintptr_t *sym_addr, const char **why)
{
    const struct elf64_ehdr *eh = file;
    for (uint32_t i = 1; i < eh->e_shnum; i++) {
        const struct elf64_shdr *sh = modelf_shdr(file, i);
        if (sh->sh_type != SHT_RELA)
            continue;
        const struct modelf_section *target = modelf_find_section(l, sh->sh_info);
        if (target == NULL)
            continue;   /* relocations for a non-allocated section */
        const struct elf64_rela *rela = (const struct elf64_rela *)((const uint8_t *)file + sh->sh_offset);
        size_t count = sh->sh_size / sizeof(*rela);
        int rc = arch_module_reloc(group_base(m, target->group) + target->offset, (size_t)target->size, rela,
                                   count, sym_addr, l->nr_symbols, why);
        if (rc) {
            kerror("module: %s: relocation failed in section %s: %s", m->name,
                   modelf_section_name(file, l, sh->sh_info), *why);
            return rc;
        }
    }
    return 0;
}

/* The module's export table lives in its own memory; every name and
 * address must point inside the module, and no name may shadow a
 * kernel export or repeat. */
static int index_exports(struct module_priv *p, const struct modelf_layout *l, const char **why)
{
    struct module *m = &p->m;
    if (l->ksymtab_section == 0)
        return 0;
    const struct modelf_section *ks = modelf_find_section(l, l->ksymtab_section);
    KASSERT(ks != NULL);
    m->exports = (const struct ksym *)(group_base(m, ks->group) + ks->offset);
    m->nr_exports = (size_t)(ks->size / sizeof(struct ksym));
    if (m->nr_exports == 0)
        return 0;

    const struct ksym **idx = kmalloc(m->nr_exports * sizeof(*idx), 0);
    if (idx == NULL)
        return -ENOMEM;
    for (size_t i = 0; i < m->nr_exports; i++) {
        const struct ksym *e = &m->exports[i];
        if (!in_module(m, (uintptr_t)e->name, 1) || !in_module(m, e->addr, 1)) {
            kfree(idx);
            *why = "export record points outside the module";
            return -ENOEXEC;
        }
        /* The name must terminate inside the module. */
        size_t max = 0;
        while (in_module(m, (uintptr_t)e->name + max, 1) && e->name[max] != '\0' && max < MODULE_NAME_MAX * 4)
            max++;
        if (!in_module(m, (uintptr_t)e->name + max, 1) || e->name[max] != '\0') {
            kfree(idx);
            *why = "export name is not terminated inside the module";
            return -ENOEXEC;
        }
        if (ksym_lookup(e->name) != 0) {
            kfree(idx);
            kerror("module: %s exports '%s', which the kernel already exports", m->name, e->name);
            *why = "export shadows a kernel symbol";
            return -EEXIST;
        }
        idx[i] = e;
    }
    ksym_sort(idx, m->nr_exports);
    for (size_t i = 1; i < m->nr_exports; i++) {
        if (strcmp(idx[i - 1]->name, idx[i]->name) == 0) {
            kfree(idx);
            *why = "duplicate export";
            return -EEXIST;
        }
    }
    p->export_index = idx;
    return 0;
}

static int load_locked(const void *file, size_t size, const char *origin, struct module **out)
{
    const char *why = "";
    int rc;

    /* 1. Signature, before any parsing. */
    size_t elf_size = size;
    rc = modsig_check(file, size, &elf_size, &why);
    bool unsigned_ok = false;
    if (rc == -ENOKEY && !modsig_enforced()) {
        kwarn("module: %s: %s; loading anyway (MODULE_SIG_ENFORCE=0), kernel tainted", origin, why);
        kernel_taint(TAINT_UNSIGNED_MODULE);
        unsigned_ok = true;
        rc = 0;
        /* Without a trailer the whole file is the ELF; with an unknown
         * key the trailer is still there. */
        if (size >= sizeof(struct modsig_trailer) &&
            memcmp((const uint8_t *)file + size - 8, MODSIG_MAGIC, 8) == 0)
            elf_size = size - sizeof(struct modsig_trailer);
    }
    if (rc) {
        kerror("module: %s: rejected: %s", origin, why);
        return rc;
    }

    /* 2-3. ELF, architecture, ABI. */
    struct modelf_layout *l = kzalloc(sizeof(*l));
    if (l == NULL)
        return -ENOMEM;
    rc = modelf_validate(file, elf_size, l, &why);
    if (rc) {
        kerror("module: %s: rejected: %s", origin, why);
        kfree(l);
        return rc;
    }
    const struct cosmo_module_info *finfo =
        (const struct cosmo_module_info *)((const uint8_t *)file + l->info_file_off);
    rc = modelf_check_info(finfo, &why);
    if (rc) {
        kerror("module: %s: rejected: %s (module ABI v%u, kernel v%u)", origin, why, finfo->abi_version,
               COSMO_MODULE_ABI_VERSION);
        kfree(l);
        return rc;
    }

    /* 4. Name and dependencies. */
    if (find_locked(finfo->name) != NULL) {
        kerror("module: %s: '%s' is already loaded", origin, finfo->name);
        kfree(l);
        return -EEXIST;
    }
    struct module_priv *p = kzalloc(sizeof(*p));
    if (p == NULL) {
        kfree(l);
        return -ENOMEM;
    }
    struct module *m = &p->m;
    list_init(&m->link);
    strlcpy(m->name, finfo->name, sizeof(m->name));
    strlcpy(m->version, finfo->version, sizeof(m->version));
    m->capabilities = finfo->capabilities;
    m->flags = unsigned_ok ? MODULE_FLAG_UNSIGNED : 0;
    m->state = MODULE_LOADING;
    rc = resolve_deps(m, finfo, &why);
    if (rc)
        goto fail;

    /* 5. Memory: three RW regions near the kernel image. */
    unsigned aflags = VM_KALLOC_POPULATE | VM_KALLOC_GUARD | VM_KALLOC_NEAR_KERNEL;
    m->text_size = (size_t)l->group_size[MODELF_TEXT];
    m->rodata_size = (size_t)l->group_size[MODELF_RODATA];
    m->data_size = (size_t)l->group_size[MODELF_DATA];
    if (m->text_size && (m->text = vm_kernel_alloc(m->text_size, aflags, VM_PROT_RW)) == 0)
        goto nomem;
    if (m->rodata_size && (m->rodata = vm_kernel_alloc(m->rodata_size, aflags, VM_PROT_RW)) == 0)
        goto nomem;
    if (m->data_size && (m->data = vm_kernel_alloc(m->data_size, aflags, VM_PROT_RW)) == 0)
        goto nomem;

    for (unsigned i = 0; i < l->nr_sections; i++) {
        const struct modelf_section *ms = &l->sections[i];
        if (ms->nobits || ms->size == 0)
            continue;
        memcpy((void *)(group_base(m, ms->group) + ms->offset), (const uint8_t *)file + ms->file_off,
               (size_t)ms->size);
    }

    /* 6. Symbols and relocations. */
    uintptr_t *sym_addr = kmalloc((size_t)l->nr_symbols * sizeof(*sym_addr), 0);
    if (sym_addr == NULL)
        goto nomem;
    rc = resolve_symbols(m, file, l, sym_addr, &why);
    if (rc == 0)
        rc = apply_relocations(m, file, l, sym_addr, &why);
    kfree(sym_addr);
    if (rc) {
        kerror("module: %s: rejected: %s", origin, why);
        goto fail;
    }

    const struct modelf_section *is = modelf_find_section(l, l->info_section);
    KASSERT(is != NULL && is->group == MODELF_RODATA);
    m->info = (const struct cosmo_module_info *)(m->rodata + is->offset);
    if (m->info->init == NULL || m->info->shutdown == NULL ||
        !in_module(m, (uintptr_t)m->info->init, 1) || !in_module(m, (uintptr_t)m->info->shutdown, 1)) {
        kerror("module: %s: init/shutdown do not point into the module", origin);
        rc = -ENOEXEC;
        goto fail;
    }
    rc = index_exports(p, l, &why);
    if (rc) {
        kerror("module: %s: rejected: %s", origin, why);
        goto fail;
    }

    /* 7. W^X. */
    if (m->text && (rc = vm_kernel_protect(m->text, VM_PROT_RX)) != 0)
        goto fail;
    if (m->rodata && (rc = vm_kernel_protect(m->rodata, VM_PROT_READ)) != 0)
        goto fail;

    /* 8. Initialise. */
    rc = m->info->init();
    if (rc) {
        kerror("module: %s: init failed (%d)", m->name, rc);
        goto fail;
    }

    /* 9. Register. */
    m->state = MODULE_LIVE;
    list_push_back(&g_modules, &m->link);
    g_count++;
    for (unsigned i = 0; i < m->nr_deps; i++)
        m->deps[i]->refs++;
    kinfo("module: loaded %s %s (text %zu KiB, rodata %zu KiB, data %zu KiB, %zu exports%s)", m->name,
          m->version, m->text_size >> 10, m->rodata_size >> 10, m->data_size >> 10, m->nr_exports,
          (m->flags & MODULE_FLAG_UNSIGNED) ? ", UNSIGNED" : "");
    kfree(l);
    if (out)
        *out = m;
    return 0;

nomem:
    rc = -ENOMEM;
    kerror("module: %s: out of memory", origin);
fail:
    free_module(p);
    kfree(l);
    return rc;
}

int module_load(const void *file, size_t size, const char *origin, struct module **out)
{
    mutex_lock(&g_lock);
    int rc = load_locked(file, size, origin, out);
    mutex_unlock(&g_lock);
    return rc;
}

int module_unload(const char *name)
{
    mutex_lock(&g_lock);
    struct module *m = find_locked(name);
    if (m == NULL) {
        mutex_unlock(&g_lock);
        return -ENOENT;
    }
    if (m->refs != 0) {
        kwarn("module: %s has %u dependant(s), not unloading", name, m->refs);
        mutex_unlock(&g_lock);
        return -EBUSY;
    }
    m->state = MODULE_GOING;
    m->info->shutdown();
    list_remove(&m->link);
    g_count--;
    for (unsigned i = 0; i < m->nr_deps; i++) {
        KASSERT(m->deps[i]->refs > 0);
        m->deps[i]->refs--;
    }
    kinfo("module: unloaded %s", m->name);
    free_module((struct module_priv *)m);
    mutex_unlock(&g_lock);
    return 0;
}

struct module *module_find(const char *name)
{
    mutex_lock(&g_lock);
    struct module *m = find_locked(name);
    mutex_unlock(&g_lock);
    return m;
}

uintptr_t module_symbol_lookup(const char *name, const struct module **owner)
{
    uintptr_t a = ksym_lookup(name);
    if (a) {
        if (owner)
            *owner = NULL;
        return a;
    }
    mutex_lock(&g_lock);
    struct module *m;
    list_for_each_entry(m, &g_modules, link) {
        const struct module_priv *p = (const struct module_priv *)m;
        if (m->state != MODULE_LIVE || p->export_index == NULL)
            continue;
        a = ksym_search(p->export_index, m->nr_exports, name);
        if (a) {
            if (owner)
                *owner = m;
            break;
        }
    }
    mutex_unlock(&g_lock);
    return a;
}

unsigned module_load_boot(void)
{
    unsigned failed = 0, loaded = 0;
    for (unsigned i = 0; i < bootarchive_count(); i++) {
        const struct bootarchive_entry *e = bootarchive_entry(i);
        if (strncmp(e->name, "modules/", 8) != 0)
            continue;
        int rc = module_load(e->data, e->size, e->name, NULL);
        if (rc)
            failed++;
        else
            loaded++;
    }
    kinfo("module: %u boot module(s) loaded, %u failed", loaded, failed);
    return failed;
}

unsigned module_count(void)
{
    mutex_lock(&g_lock);
    unsigned n = g_count;
    mutex_unlock(&g_lock);
    return n;
}

void module_dump(void)
{
    mutex_lock(&g_lock);
    kprintf("modules (%u):\n", g_count);
    struct module *m;
    list_for_each_entry(m, &g_modules, link) {
        kprintf("  %-16s %-8s text %p rodata %p data %p refs %u caps 0x%x%s\n", m->name, m->version,
                (void *)m->text, (void *)m->rodata, (void *)m->data, m->refs, m->capabilities,
                (m->flags & MODULE_FLAG_UNSIGNED) ? " UNSIGNED" : "");
    }
    mutex_unlock(&g_lock);
}

void module_init(void)
{
    mutex_init(&g_lock, "modules");
    ksym_init();
    if (modsig_enforced())
        kinfo("module: signatures enforced, %u trusted key(s)", keyring_count());
    else
        kwarn("module: signature enforcement OFF (development build)");
}
