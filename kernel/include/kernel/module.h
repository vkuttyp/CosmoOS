/*
 * module.h - Kernel module ABI and the module loader API.
 *
 * Two audiences share this header. Module authors use the ABI part:
 * COSMO_MODULE() to declare the module, EXPORT_SYMBOL() to export a
 * symbol, and COSMO_MODULE_ABI_VERSION, which the loader compares
 * exactly. The kernel uses the loader API at the bottom. The ABI part is
 * a contract separate from the kernel's internal API (constitution
 * section 24): a module sees only exported symbols, and any incompatible
 * change to the exported set or to a structure an exported function
 * takes bumps COSMO_MODULE_ABI_VERSION. See docs/kernel/module/.
 */

#ifndef KERNEL_MODULE_H
#define KERNEL_MODULE_H

#include <kernel/compiler.h>
#include <kernel/list.h>
#include <kernel/types.h>

/* --- ABI ------------------------------------------------------------- */

#define COSMO_MODULE_ABI_VERSION 3u   /* 2: struct kobject gained `owner`, mandatory release callbacks; 3: spinlock_t and struct mutex gained `class` */
#define COSMO_MODULE_MAGIC       "COSMOMOD"   /* 8 bytes, compared without a NUL */
#define COSMO_MODULE_MAGIC_INIT  { 'C', 'O', 'S', 'M', 'O', 'M', 'O', 'D' }

#define MODULE_NAME_MAX    32
#define MODULE_VERSION_MAX 16
#define MODULE_DEPS_MAX    128   /* comma separated names, NUL terminated */
#define MODULE_MAX_DEPS    8

/* Capabilities: what a module declares it will register with. Phase 5
 * records and logs them; the subsystems that arrive later enforce them. */
#define MODULE_CAP_NONE   0u
#define MODULE_CAP_DRIVER (1u << 0)
#define MODULE_CAP_FS     (1u << 1)
#define MODULE_CAP_NET    (1u << 2)
#define MODULE_CAP_TEST   (1u << 31)  /* self-test fixture, never loaded at boot */

/* One instance per module, in section .cosmo.module. Every field the
 * loader reads before allocating memory is inline; only init/shutdown
 * are pointers, fixed up by the module's own relocations. */
struct cosmo_module_info {
    char     magic[8];
    uint32_t abi_version;
    uint32_t capabilities;
    char     name[MODULE_NAME_MAX];
    char     version[MODULE_VERSION_MAX];
    char     deps[MODULE_DEPS_MAX];
    int    (*init)(void);
    void   (*shutdown)(void);
    uint64_t reserved[4];
};

STATIC_ASSERT(sizeof(struct cosmo_module_info) == 240, "module ABI v1 metadata is 240 bytes");

/* Declare a module. name, version and deps are string literals; init is
 * int (*)(void) and returns 0 or a negative errno; shutdown is
 * void (*)(void). Exactly one per module. */
#define COSMO_MODULE(mname, mversion, minit, mshutdown, mdeps, mcaps)                     \
    const struct cosmo_module_info __cosmo_module_info __section(".cosmo.module")        \
        __always_used = {                                                                \
            .magic = COSMO_MODULE_MAGIC_INIT,                                            \
            .abi_version = COSMO_MODULE_ABI_VERSION,                                     \
            .capabilities = (mcaps),                                                     \
            .name = (mname),                                                             \
            .version = (mversion),                                                       \
            .deps = (mdeps),                                                             \
            .init = (minit),                                                             \
            .shutdown = (mshutdown),                                                     \
        }

/* Exported symbol record, identical in the kernel image (.ksymtab kept by
 * the linker script) and in modules (.ksymtab section). */
struct ksym {
    const char *name;
    uintptr_t addr;
};

#if defined(__ELF__)
#define EXPORT_SYMBOL(sym)                                                              \
    static const struct ksym __ksym_##sym __section(".ksymtab") __always_used = {       \
        #sym, (uintptr_t)&(sym)                                                          \
    }
#else
/* Host unit tests on non-ELF platforms (Mach-O) compile kernel sources
 * that carry exports; there is no module loader there. */
#define EXPORT_SYMBOL(sym) STATIC_ASSERT(sizeof(&(sym)) > 0, "export")
#endif

/* --- Loader API (kernel side) --------------------------------------- */

#ifndef COSMO_MODULE_BUILD

enum module_state {
    MODULE_LOADING,
    MODULE_LIVE,
    MODULE_GOING,    /* shutdown ran; waiting for the grace period or for live objects (a zombie) */
};

#define MODULE_FLAG_UNSIGNED (1u << 0)   /* loaded without a signature (enforcement off) */

struct module {
    struct list_node link;
    char name[MODULE_NAME_MAX];
    char version[MODULE_VERSION_MAX];
    uint32_t capabilities;
    unsigned flags;
    enum module_state state;
    vaddr_t text, rodata, data;          /* 0 when the group is empty */
    size_t text_size, rodata_size, data_size;
    const struct cosmo_module_info *info;
    const struct ksym *exports;
    size_t nr_exports;
    struct module *deps[MODULE_MAX_DEPS];
    unsigned nr_deps;
    unsigned refs;                       /* live dependants */
    uint32_t live_objects;               /* kobjects whose release code lives in this module */
};

/* Sort the kernel export index. Call once, after kmalloc_init(). */
void module_init(void);

/* Run the pipeline in docs/kernel/module/design.md on a file image.
 * `origin` names the source for logs. Sleeps (allocations, mutex).
 * Returns 0 and a borrowed pointer (valid until unload) or a negative
 * errno: -ENOKEY, -EKEYREJECTED, -ENOEXEC, -ENOENT (dependency or
  * symbol), -EEXIST, -ERANGE, -ENOMEM, or init()'s own value. */
int module_load(const void *file, size_t size, const char *origin, struct module **out);

/* Unload by name (docs/kernel/quiesce/design.md, "Module unload"):
 * GOING -> shutdown() -> one grace period -> wait for live objects -> free.
 * -ENOENT, -EBUSY (dependants, or objects still alive after the timeout:
 * the module becomes a zombie whose memory stays mapped; calling again
 * once the objects are gone frees it and returns 0). Sleeps. */
int module_unload(const char *name);

/* The module whose text, rodata or data contains `addr`, with its
 * live-object count raised, or NULL for a kernel address. The caller
 * balances with module_object_released() when the object dies. Any
 * context (a read-side section inside). */
struct module *module_owner_of(uintptr_t addr);
void module_object_released(struct module *m);

/* How long module_unload waits for live objects before giving up
 * (default 5000 ms). Self-tests shorten it. */
void module_set_unload_timeout_ms(unsigned ms);

/* Borrowed pointer while the module is live, or NULL. Sleeps (mutex). */
struct module *module_find(const char *name);

/* Address of a symbol exported by the kernel or any live module. */
uintptr_t module_symbol_lookup(const char *name, const struct module **owner);

/* Load every "modules/" entry of the boot archive in archive order.
 * Returns the number of failures (each is logged). */
unsigned module_load_boot(void);

unsigned module_count(void);
void module_dump(void);

#endif /* !COSMO_MODULE_BUILD */

#endif /* KERNEL_MODULE_H */
