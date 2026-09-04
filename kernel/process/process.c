/*
 * process.c - Process lifecycle.
 */

#include <kernel/elf.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/object.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/vmm.h>

#include <arch/irq.h>
#include <arch/user.h>

#include <uapi/cosmo/syscall.h>

#include "../scheduler/sched_internal.h"

static struct kmem_cache *g_process_cache;
static LIST_HEAD(g_processes);
static spinlock_t g_process_table_lock = SPINLOCK_INIT("process_table");
static pid_t g_next_pid = 1;
static unsigned g_process_count;

/* --- object type --- */

static void process_release(struct kobject *obj)
{
    struct process *p = container_of(obj, struct process, obj);
    KASSERT(p->state == PROCESS_EXITED);
    KASSERT(p->nr_threads == 0);

    handle_table_destroy(&p->handles);
    if (p->space != NULL)
        vm_space_destroy(p->space);

    arch_irq_state_t s = spin_lock_irqsave(&g_process_table_lock);
    list_remove(&p->all_link);
    g_process_count--;
    spin_unlock_irqrestore(&g_process_table_lock, s);

    kdebug("process: pid %u '%s' released", p->pid, p->name);
    kmem_cache_free(g_process_cache, p);
}

static const struct kobject_type process_type = {
    .name = "process",
    .release = process_release,
};

/* --- fault hooks for the VMM --- */

static struct vm_space *hook_current_space(void)
{
    struct process *p = process_current();
    return p ? p->space : NULL;
}

static void hook_fatal(uint64_t addr, unsigned fault_flags, struct arch_trap_frame *frame) __noreturn;
static void hook_fatal(uint64_t addr, unsigned fault_flags, struct arch_trap_frame *frame)
{
    struct process *p = process_current();
    KASSERT(p != NULL);
    kwarn("process: pid %u '%s' fault: %s %s at %p (%s); terminating", p->pid, p->name,
          (fault_flags & VM_FAULT_USER) ? "user" : "kernel",
          (fault_flags & VM_FAULT_EXEC) ? "execute" : (fault_flags & VM_FAULT_WRITE) ? "write" : "read",
          (void *)(uintptr_t)addr, (fault_flags & VM_FAULT_PRESENT) ? "protection" : "not present");
    (void)frame;
    /* The fault arrived with interrupts disabled on this thread's kernel
     * stack; thread_exit needs a preemptible context. A user-mode frame
     * always has IF set, so enabling here restores the state that was
     * in effect before the trap. */
    arch_irq_enable();
    process_exit(COSMO_EXIT_FAULT);
}

static const struct vm_user_hooks g_hooks = {
    .current_space = hook_current_space,
    .fatal = hook_fatal,
};

void process_init(void)
{
    g_process_cache = kmem_cache_create("process", sizeof(struct process), 64);
    if (g_process_cache == NULL)
        panic("process: cannot create process cache");
    vm_set_user_hooks(&g_hooks);
}

/* --- initial user stack --- */

/*
 * Lay out argc/argv/envp/auxv and the strings at the top of the user
 * stack, writing through the direct map into the (populated) top page.
 * Returns the initial user rsp, or 0 if it does not fit in one page.
 */
static uint64_t build_initial_stack(struct vm_space *space, uint64_t stack_top, const char *const argv[],
                                    const char *const envp[])
{
    unsigned argc = 0, envc = 0;
    size_t strings = 0;
    for (; argv && argv[argc]; argc++)
        strings += strlen(argv[argc]) + 1;
    for (; envp && envp[envc]; envc++)
        strings += strlen(envp[envc]) + 1;

    /* words: argc, argv[argc+1], envp[envc+1], auxv (3 pairs) */
    size_t words = 1 + (argc + 1) + (envc + 1) + 6;
    size_t need = strings + words * 8 + 16;
    if (need > PAGE_SIZE - 64)
        return 0;

    uint64_t page_va = stack_top - PAGE_SIZE;
    paddr_t pa;
    if (!arch_mmu_query(&space->mmu, (vaddr_t)page_va, &pa, NULL, NULL, NULL))
        return 0;
    uint8_t *page = phys_to_virt(pa);

    /* Strings grow down from the top of the page. */
    uint64_t sp = stack_top;
    uint64_t str_addrs[64];
    unsigned nstr = 0;
    for (unsigned i = 0; i < argc + envc; i++) {
        const char *s = i < argc ? argv[i] : envp[i - argc];
        size_t n = strlen(s) + 1;
        sp -= n;
        memcpy(page + (sp - page_va), s, n);
        str_addrs[nstr++] = sp;
        if (nstr >= 64)
            return 0;
    }

    /* Word area, 16-byte aligned at the final rsp. */
    sp &= ~0xFULL;
    if (((words * 8) & 0xF) != 0)
        sp -= 8;
    sp -= words * 8;
    sp &= ~0xFULL;

    uint64_t *w = (uint64_t *)(page + (sp - page_va));
    unsigned k = 0;
    w[k++] = argc;
    for (unsigned i = 0; i < argc; i++)
        w[k++] = str_addrs[i];
    w[k++] = 0;
    for (unsigned i = 0; i < envc; i++)
        w[k++] = str_addrs[argc + i];
    w[k++] = 0;
    w[k++] = COSMO_AT_PAGESZ; w[k++] = PAGE_SIZE;
    w[k++] = COSMO_AT_ENTRY;  w[k++] = 0; /* patched by the caller */
    w[k++] = COSMO_AT_NULL;   w[k++] = 0;
    return sp;
}

/* --- user thread start --- */

static void user_thread_main(void *arg)
{
    struct thread *self = thread_current();
    (void)arg;
    arch_user_enter(self->user_entry, self->user_sp);
}

/* --- creation --- */

int process_create_from_elf(const void *image, size_t size, const char *name, const char *const argv[],
                            const char *const envp[], struct process **out)
{
    struct elf_info info;
    const char *why = NULL;
    int rc = elf_validate(image, size, USER_LO, USER_HI, &info, &why);
    if (rc) {
        kwarn("process: '%s' rejected: %s", name ? name : "?", why ? why : "?");
        return rc;
    }

    struct process *p = kmem_cache_alloc(g_process_cache, KMEM_ZERO);
    if (p == NULL)
        return -ENOMEM;
    kobject_init(&p->obj, &process_type);
    strlcpy(p->name, name ? name : "?", sizeof(p->name));
    list_init(&p->threads);
    list_init(&p->all_link);
    handle_table_init(&p->handles);
    spinlock_init(&p->lock, "process");
    completion_init(&p->exited, "process-exit");
    p->pers = &personality_native;
    p->state = PROCESS_RUNNING;
    p->parent_pid = 0;

    struct process *cur = process_current();
    if (cur)
        p->parent_pid = cur->pid;

    rc = vm_space_create_user(&p->space);
    if (rc)
        goto fail;

    rc = elf_load_into(p->space, image, &info);
    if (rc) {
        kwarn("process: '%s' load failed (%d)", p->name, rc);
        goto fail;
    }

    /* Stack: lazily populated except the top page, with a guard below. */
    rc = vm_user_map_anon(p->space, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE, VM_PROT_RW,
                          VM_REGION_GUARD_BELOW, "stack");
    if (rc)
        goto fail;
    {
        /* Populate the top page so the initial frame can be written. */
        struct page *pg = pmm_alloc_page(PMM_FLAGS_ZERO);
        if (pg == NULL) {
            rc = -ENOMEM;
            goto fail;
        }
        rc = arch_mmu_map(&p->space->mmu, (vaddr_t)(USER_STACK_TOP - PAGE_SIZE), page_to_phys(pg), PAGE_SIZE,
                          VM_PROT_RW, VM_CACHE_WB, ARCH_MMU_MAP_USER);
        if (rc) {
            pmm_free_page(pg);
            goto fail;
        }
        p->space->anon_pages++;
    }

    uint64_t sp = build_initial_stack(p->space, USER_STACK_TOP, argv, envp);
    if (sp == 0) {
        rc = -EINVAL; /* argument/environment strings do not fit the first page */
        goto fail;
    }
    {
        /* Patch AT_ENTRY (second-to-last pair). */
        paddr_t pa;
        arch_mmu_query(&p->space->mmu, (vaddr_t)(USER_STACK_TOP - PAGE_SIZE), &pa, NULL, NULL, NULL);
        uint64_t *w = (uint64_t *)((uint8_t *)phys_to_virt(pa) + (sp - (USER_STACK_TOP - PAGE_SIZE)));
        unsigned argc = (unsigned)w[0];
        unsigned envc = 0;
        for (unsigned i = argc + 2; w[i] != 0; i++)
            envc++;
        unsigned aux = 1 + argc + 1 + envc + 1;
        w[aux + 3] = info.entry; /* AT_ENTRY value */
    }

    /* Standard handles. */
    struct kobject *con = console_object();
    handle_install_at(&p->handles, COSMO_STDIN, con, HANDLE_RIGHT_READ);
    handle_install_at(&p->handles, COSMO_STDOUT, con, HANDLE_RIGHT_WRITE);
    handle_install_at(&p->handles, COSMO_STDERR, con, HANDLE_RIGHT_WRITE);

    /* Register. */
    arch_irq_state_t s = spin_lock_irqsave(&g_process_table_lock);
    p->pid = g_next_pid++;
    list_push_back(&g_processes, &p->all_link);
    g_process_count++;
    spin_unlock_irqrestore(&g_process_table_lock, s);

    /* Main thread: a kernel thread that enters user mode on first run.
     * It holds a process reference; the table holds one; the creator
     * gets the initial one from kobject_init. */
    struct thread *t = thread_prepare(user_thread_main, NULL, p->name, SCHED_PRIO_DEFAULT, 0);
    if (t == NULL) {
        rc = -ENOMEM;
        goto fail_registered;
    }
    t->proc = p;
    process_get(p);
    t->user_entry = (uintptr_t)info.entry;
    t->user_sp = (uintptr_t)sp;
    s = spin_lock_irqsave(&p->lock);
    list_push_back(&p->threads, &t->proc_link);
    p->nr_threads = 1;
    spin_unlock_irqrestore(&p->lock, s);
    thread_put(t); /* the creator's thread reference; the process owns it now */

    kinfo("process: pid %u '%s' created, entry %p, %u segments", p->pid, p->name, (void *)info.entry,
          info.nr_segments);
    process_get(p); /* the table's reference */
    sched_enqueue_new(t);
    *out = p;
    return 0;

fail_registered:
    s = spin_lock_irqsave(&g_process_table_lock);
    list_remove(&p->all_link);
    g_process_count--;
    spin_unlock_irqrestore(&g_process_table_lock, s);
fail:
    handle_table_destroy(&p->handles);
    if (p->space)
        vm_space_destroy(p->space);
    kmem_cache_free(g_process_cache, p);
    return rc;
}

/* --- exit --- */

void process_exit(int status)
{
    struct thread *self = thread_current();
    struct process *p = self->proc;
    KASSERT(p != NULL);

    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    if (p->state == PROCESS_RUNNING) {
        p->state = PROCESS_EXITING;
        p->exit_status = status;
    }
    spin_unlock_irqrestore(&p->lock, s);

    /* Only one thread per process in this phase; when there are more,
     * the others are signalled here. */
    thread_exit(status);
}

void process_last_thread_gone(struct process *p)
{
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    p->state = PROCESS_EXITED;
    int status = p->exit_status;
    spin_unlock_irqrestore(&p->lock, s);

    kinfo("process: pid %u '%s' exited with status %d (%llu syscalls)", p->pid, p->name, status,
          (unsigned long long)p->syscalls);
    complete(&p->exited);
    process_put(p); /* the table's reference */
}

int process_wait_exit(struct process *p)
{
    wait_for_completion(&p->exited);
    return p->exit_status;
}

struct process *process_current(void)
{
    struct thread *t = this_cpu()->current;
    return t ? t->proc : NULL;
}

struct process *process_lookup(pid_t pid)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_process_table_lock);
    struct process *p;
    list_for_each_entry(p, &g_processes, all_link) {
        if (p->pid == pid) {
            process_get(p);
            spin_unlock_irqrestore(&g_process_table_lock, s);
            return p;
        }
    }
    spin_unlock_irqrestore(&g_process_table_lock, s);
    return NULL;
}

unsigned process_count(void)
{
    return g_process_count;
}

void process_dump_all(void)
{
    static const char *const states[] = { "running", "exiting", "exited" };
    arch_irq_state_t s = spin_lock_irqsave(&g_process_table_lock);
    struct process *p;
    kprintf("%4s %-20s %-8s %7s %8s\n", "pid", "name", "state", "threads", "syscalls");
    list_for_each_entry(p, &g_processes, all_link) {
        kprintf("%4u %-20s %-8s %7u %8llu\n", p->pid, p->name, states[p->state], p->nr_threads,
                (unsigned long long)p->syscalls);
    }
    spin_unlock_irqrestore(&g_process_table_lock, s);
}
