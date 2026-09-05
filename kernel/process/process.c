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
#include <kernel/random.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/vfs.h>
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
    if (p->cwd)
        vnode_put(p->cwd);
    if (p->parent)
        process_put(p->parent);
    if (p->linux)
        linux_process_release(p);

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

#define INITIAL_STACK_PAGES 2u
#define INITIAL_STRINGS_MAX 300u

/*
 * Lay out argc/argv/envp/auxv and the strings at the top of the user
 * stack, writing through the direct map into the populated top pages.
 * Native processes get the CosmoOS auxiliary vector, Linux processes the
 * Linux one (compat/linux). Returns the initial user rsp, or 0 if the
 * frame does not fit.
 */
static uint64_t build_initial_stack(struct process *p, const struct elf_info *info, uint64_t stack_top,
                                    const char *const argv[], const char *const envp[])
{
    unsigned argc = 0, envc = 0;
    size_t strings = 0;
    for (; argv && argv[argc]; argc++)
        strings += strlen(argv[argc]) + 1;
    for (; envp && envp[envc]; envc++)
        strings += strlen(envp[envc]) + 1;
    if (argc + envc > INITIAL_STRINGS_MAX)
        return 0;
    const size_t span = INITIAL_STACK_PAGES * PAGE_SIZE;
    /* words: argc, argv[argc+1], envp[envc+1], auxv (up to 20 pairs) */
    size_t words = 1 + (argc + 1) + (envc + 1) + 40;
    size_t need = strings + 16 + words * 8 + 32;
    if (need > span - 64)
        return 0;
    uint64_t base_va = stack_top - span;
    /* The populated pages need not be contiguous in the direct map: every
     * byte is written through the page it lands in. */
    uint8_t *pages[INITIAL_STACK_PAGES];
    for (unsigned i = 0; i < INITIAL_STACK_PAGES; i++) {
        paddr_t pa;
        if (!arch_mmu_query(&p->space->mmu, (vaddr_t)(base_va + i * PAGE_SIZE), &pa, NULL, NULL, NULL))
            return 0;
        pages[i] = phys_to_virt(pa);
    }
#define AT(va) (pages[((va) - base_va) / PAGE_SIZE] + (((va) - base_va) % PAGE_SIZE))
    /* Strings grow down from the top; a copy may straddle the page boundary. */
    uint64_t sp = stack_top;
    uint64_t str_addrs[INITIAL_STRINGS_MAX];
    for (unsigned i = 0; i < argc + envc; i++) {
        const char *str = i < argc ? argv[i] : envp[i - argc];
        size_t n = strlen(str) + 1;
        sp -= n;
        for (size_t k = 0; k < n; k++)
            *AT(sp + k) = (uint8_t)str[k];
        str_addrs[i] = sp;
    }
    /* 16 random bytes for AT_RANDOM (Linux); harmless for native. */
    sp -= 16;
    sp &= ~0xFULL;
    uint64_t random_addr = sp;
    {
        uint8_t rnd[16];
        random_get_bytes(rnd, sizeof(rnd));
        for (size_t k = 0; k < 16; k++)
            *AT(sp + k) = rnd[k];
    }
    /* Word area, 16-byte aligned at the final rsp; built in a kernel
     * array then copied, since it may straddle the boundary too. */
    uint64_t w[1 + INITIAL_STRINGS_MAX + 2 + 40];
    unsigned k = 0;
    w[k++] = argc;
    for (unsigned i = 0; i < argc; i++)
        w[k++] = str_addrs[i];
    w[k++] = 0;
    for (unsigned i = 0; i < envc; i++)
        w[k++] = str_addrs[argc + i];
    w[k++] = 0;
    if (p->pers == &personality_linux) {
        k += linux_auxv(p, info, random_addr, w + k, 40);
    } else {
        w[k++] = COSMO_AT_PAGESZ; w[k++] = PAGE_SIZE;
        w[k++] = COSMO_AT_ENTRY;  w[k++] = info->entry;
        w[k++] = COSMO_AT_NULL;   w[k++] = 0;
    }
    words = k;
    sp &= ~0xFULL;
    if (((words * 8) & 0xF) != 0)
        sp -= 8;
    sp -= words * 8;
    sp &= ~0xFULL;
    for (unsigned i = 0; i < words; i++) {
        uint64_t va = sp + (uint64_t)i * 8;
        for (unsigned b = 0; b < 8; b++)
            *AT(va + b) = (uint8_t)(w[i] >> (8 * b));
    }
#undef AT
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

/* Install the child's handles from the parent's table (docs: spawn). */
static int install_handles(struct process *p, const struct process_spawn_attr *attr)
{
    struct process *parent = attr->parent;
    if (attr->handles == NULL || attr->nr_handles == 0) {
        for (int h = 0; h < 3; h++) {
            unsigned rights;
            struct kobject *obj = handle_get(&parent->handles, h, &rights);
            if (obj == NULL)
                continue;
            int rc = handle_install_at(&p->handles, h, obj, rights);
            kobject_put(obj);
            if (rc < 0)
                return rc;
        }
        return 0;
    }
    for (unsigned i = 0; i < attr->nr_handles; i++) {
        unsigned rights;
        struct kobject *obj = handle_get(&parent->handles, attr->handles[i].parent, &rights);
        if (obj == NULL)
            return -EBADF;
        int rc = handle_install_at(&p->handles, attr->handles[i].child, obj, rights);
        kobject_put(obj);
        if (rc < 0)
            return rc;
    }
    return 0;
}

int process_create_from_elf(const void *image, size_t size, const char *name, const char *const argv[],
                            const char *const envp[], const struct process_spawn_attr *attr, struct process **out)
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
    list_init(&p->children);
    list_init(&p->sibling);
    waitqueue_init(&p->child_wq, "children");
    handle_table_init(&p->handles);
    spinlock_init(&p->lock, "process");
    completion_init(&p->exited, "process-exit");
    p->state = PROCESS_RUNNING;
    p->parent_pid = 0;

    struct process *parent = attr ? attr->parent : NULL;
    /* Personality: the CosmoOS note selects native; kernel-created processes are always native. */
    p->pers = (info.cosmo_note || parent == NULL) ? &personality_native : &personality_linux;
    if (parent) {
        p->parent_pid = parent->pid;
        p->cred = parent->cred;
    }
    /* Working directory: the request's, else the parent's, else the root. */
    if (attr && attr->cwd) {
        vnode_get(attr->cwd);
        p->cwd = attr->cwd;
        strlcpy(p->cwd_path, attr->cwd_path, sizeof(p->cwd_path));
    } else if (parent) {
        arch_irq_state_t ps = spin_lock_irqsave(&parent->lock);
        p->cwd = parent->cwd;
        if (p->cwd)
            vnode_get(p->cwd);
        strlcpy(p->cwd_path, parent->cwd_path, sizeof(p->cwd_path));
        spin_unlock_irqrestore(&parent->lock, ps);
    } else {
        p->cwd = vfs_root();
        strlcpy(p->cwd_path, "/", sizeof(p->cwd_path));
    }

    rc = vm_space_create_user(&p->space);
    if (rc)
        goto fail;

    rc = elf_load_into(p->space, image, &info);
    if (rc) {
        kwarn("process: '%s' load failed (%d)", p->name, rc);
        goto fail;
    }
    p->image_end = info.hi;
    if (p->pers == &personality_linux) {
        rc = linux_process_init(p, &info);
        if (rc)
            goto fail;
    }

    /* Stack: lazily populated except the top pages, with a guard below. */
    rc = vm_user_map_anon(p->space, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE, VM_PROT_RW,
                          VM_REGION_GUARD_BELOW, "stack");
    if (rc)
        goto fail;
    for (unsigned i = 1; i <= INITIAL_STACK_PAGES; i++) {
        /* Populate the top pages so the initial frame can be written. */
        struct page *pg = pmm_alloc_page(PMM_FLAGS_ZERO);
        if (pg == NULL) {
            rc = -ENOMEM;
            goto fail;
        }
        rc = arch_mmu_map(&p->space->mmu, (vaddr_t)(USER_STACK_TOP - i * PAGE_SIZE), page_to_phys(pg), PAGE_SIZE,
                          VM_PROT_RW, VM_CACHE_WB, ARCH_MMU_MAP_USER);
        if (rc) {
            pmm_free_page(pg);
            goto fail;
        }
        p->space->anon_pages++;
    }

    uint64_t sp = build_initial_stack(p, &info, USER_STACK_TOP, argv, envp);
    if (sp == 0) {
        rc = -EINVAL; /* argument/environment strings do not fit the initial pages */
        goto fail;
    }

    /* Handles: exactly what the parent maps; kernel-created processes
     * get the console as 0, 1, 2. */
    if (parent) {
        rc = install_handles(p, attr);
        if (rc)
            goto fail;
    } else {
        struct kobject *con = console_object();
        handle_install_at(&p->handles, COSMO_STDIN, con, HANDLE_RIGHT_READ);
        handle_install_at(&p->handles, COSMO_STDOUT, con, HANDLE_RIGHT_WRITE);
        handle_install_at(&p->handles, COSMO_STDERR, con, HANDLE_RIGHT_WRITE);
    }

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

    if (parent) {
        process_get(parent);
        p->parent = parent;
        s = spin_lock_irqsave(&parent->lock);
        list_push_back(&parent->children, &p->sibling);
        spin_unlock_irqrestore(&parent->lock, s);
    }

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
    if (p->cwd)
        vnode_put(p->cwd);
    if (p->linux)
        linux_process_release(p);
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

static struct process *g_init;   /* referenced; set by process_set_init */

void process_set_init(struct process *p)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_process_table_lock);
    if (g_init)
        process_put(g_init);
    g_init = p;
    if (p)
        process_get(p);
    spin_unlock_irqrestore(&g_process_table_lock, s);
}

/* Table lock held: init if it is alive and not `except`. */
static struct process *find_init_locked(struct process *except)
{
    struct process *q = g_init;
    if (q && q != except && q->state == PROCESS_RUNNING)
        return q;
    return NULL;
}

/*
 * Reaper context, once every thread of `p` is gone. Children are handed
 * to init or to the kernel; `p` becomes a zombie its parent must collect,
 * or is dropped at once when it has no parent.
 */
void process_last_thread_gone(struct process *p)
{
    LIST_HEAD(orphans);
    LIST_HEAD(to_drop);   /* exited, unreaped children with no one left to wait */
    arch_irq_state_t ts = spin_lock_irqsave(&g_process_table_lock);
    struct process *init = find_init_locked(p);

    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    p->state = PROCESS_EXITED;
    int status = p->exit_status;
    /* Detach the children first; their locks are taken after ours only
     * one at a time, and init's lock never while ours is held. */
    struct process *c, *tmp;
    list_for_each_entry_safe(c, tmp, &p->children, sibling) {
        list_remove(&c->sibling);
        list_push_back(&orphans, &c->sibling);
    }
    spin_unlock_irqrestore(&p->lock, s);

    list_for_each_entry_safe(c, tmp, &orphans, sibling) {
        arch_irq_state_t cs = spin_lock_irqsave(&c->lock);
        process_put(c->parent);   /* was p; p is alive (thread and table refs) */
        c->parent = NULL;
        bool zombie = c->state == PROCESS_EXITED && !c->reaped;
        if (init) {
            process_get(init);
            c->parent = init;
            c->parent_pid = init->pid;
        } else {
            c->parent_pid = 0;
            if (zombie)
                c->reaped = true;
        }
        spin_unlock_irqrestore(&c->lock, cs);
        list_remove(&c->sibling);
        if (init) {
            arch_irq_state_t is = spin_lock_irqsave(&init->lock);
            list_push_back(&init->children, &c->sibling);
            spin_unlock_irqrestore(&init->lock, is);
        } else if (zombie) {
            list_push_back(&to_drop, &c->sibling);
        } else {
            list_init(&c->sibling);
        }
    }
    struct process *parent = p->parent;
    bool zombie = parent != NULL;
    if (!zombie)
        p->reaped = true;
    spin_unlock_irqrestore(&g_process_table_lock, ts);

    /* Handles close at exit, not at reaping: a pipe whose writer has
     * exited must deliver EOF while the reader has yet to wait for it.
     * The zombie keeps only its identity and status. */
    handle_table_destroy(&p->handles);
    if (p->cwd) {
        vnode_put(p->cwd);
        p->cwd = NULL;
    }

    if (init)
        waitqueue_wake_all(&init->child_wq);
    list_for_each_entry_safe(c, tmp, &to_drop, sibling) {
        list_remove(&c->sibling);
        list_init(&c->sibling);
        process_put(c);   /* the table's reference */
    }

    kinfo("process: pid %u '%s' exited with status %d (%llu syscalls)", p->pid, p->name, status,
          (unsigned long long)p->syscalls);
    complete(&p->exited);
    if (zombie)
        waitqueue_wake_all(&parent->child_wq);
    else
        process_put(p); /* the table's reference */
}

int process_wait_exit(struct process *p)
{
    wait_for_completion(&p->exited);
    return p->exit_status;
}

/* --- Phase 9: wait, kill, cwd, introspection --- */

/* Parent lock held. */
static struct process *find_reapable_locked(struct process *parent, int pid, bool *matched)
{
    struct process *c;
    *matched = false;
    list_for_each_entry(c, &parent->children, sibling) {
        if (pid > 0 && (int)c->pid != pid)
            continue;
        *matched = true;
        if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == PROCESS_EXITED && !c->reaped)
            return c;
    }
    return NULL;
}

static bool child_reapable(struct process *parent, int pid)
{
    bool matched;
    arch_irq_state_t s = spin_lock_irqsave(&parent->lock);
    struct process *c = find_reapable_locked(parent, pid, &matched);
    spin_unlock_irqrestore(&parent->lock, s);
    return c != NULL || !matched;
}

int process_wait_child(int pid, unsigned flags, pid_t *pid_out, int *status_out)
{
    struct process *cur = process_current();
    for (;;) {
        bool matched;
        arch_irq_state_t s = spin_lock_irqsave(&cur->lock);
        struct process *c = find_reapable_locked(cur, pid, &matched);
        if (c) {
            c->reaped = true;
            list_remove(&c->sibling);
            list_init(&c->sibling);
            spin_unlock_irqrestore(&cur->lock, s);
            *pid_out = c->pid;
            *status_out = c->exit_status;
            process_put(c);   /* the table's reference: the zombie goes */
            return 0;
        }
        spin_unlock_irqrestore(&cur->lock, s);
        if (!matched)
            return -ECHILD;
        if (flags & PROCESS_WAIT_NOHANG) {
            *pid_out = 0;
            return 0;
        }
        int rc = wait_event_killable(&cur->child_wq, child_reapable(cur, pid));
        if (rc)
            return rc;
    }
}

void process_kill(struct process *p, int sig)
{
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    if (p->state != PROCESS_RUNNING || p->kill_sig != 0) {
        spin_unlock_irqrestore(&p->lock, s);
        return;
    }
    p->kill_sig = sig;
    p->exit_status = 128 + sig;
    /* Kick every blocked thread; wait_event_killable re-checks the flag. */
    struct thread *t;
    list_for_each_entry(t, &p->threads, proc_link)
        sched_wake(t);
    spin_unlock_irqrestore(&p->lock, s);
}

bool process_kill_pending(void)
{
    struct process *p = process_current();
    return p != NULL && __atomic_load_n(&p->kill_sig, __ATOMIC_ACQUIRE) != 0;
}

void process_check_kill(void)
{
    struct process *p = process_current();
    if (p && __atomic_load_n(&p->kill_sig, __ATOMIC_ACQUIRE) != 0)
        process_exit(p->exit_status);
}

void process_return_to_user(void)
{
    struct process *p = process_current();
    if (p && __atomic_load_n(&p->kill_sig, __ATOMIC_ACQUIRE) != 0) {
        /* The trap tail runs with interrupts off; a user frame had them on. */
        arch_irq_enable();
        process_exit(p->exit_status);
    }
}

/*
 * Join `rel` to `base` (absolute, normalised) resolving "." and "..",
 * into `out` (size n). Returns 0 or -ENAMETOOLONG.
 */
int path_normalize(const char *base, const char *rel, char *out, size_t n)
{
    size_t len = 0;
    if (rel[0] != '/') {
        len = strlen(base);
        if (len >= n)
            return -ENAMETOOLONG;
        memcpy(out, base, len);
        while (len > 0 && out[len - 1] == '/')
            len--;   /* the root becomes empty; components add their slash */
    }
    const char *s = rel;
    while (*s) {
        while (*s == '/')
            s++;
        const char *e = s;
        while (*e && *e != '/')
            e++;
        size_t cl = (size_t)(e - s);
        if (cl == 0)
            break;
        if (cl == 1 && s[0] == '.') {
            /* nothing */
        } else if (cl == 2 && s[0] == '.' && s[1] == '.') {
            while (len > 0 && out[len - 1] != '/')
                len--;
            if (len > 0)
                len--;   /* drop the slash before the component */
        } else {
            if (len + 1 + cl >= n)
                return -ENAMETOOLONG;
            out[len++] = '/';
            memcpy(out + len, s, cl);
            len += cl;
        }
        s = e;
    }
    if (len == 0)
        out[len++] = '/';
    out[len] = '\0';
    return 0;
}

int process_chdir(const char *path)
{
    struct process *cur = process_current();
    KASSERT(cur != NULL);   /* a system call: always on a process */
    char newpath[sizeof(cur->cwd_path)];
    int rc = path_normalize(cur->cwd_path, path, newpath, sizeof(newpath));
    if (rc)
        return rc;
    struct vnode *vn;
    rc = vfs_lookup(cur->cwd, path, &vn);
    if (rc)
        return rc;
    if (vn->type != VNODE_DIR) {
        vnode_put(vn);
        return -ENOTDIR;
    }
    arch_irq_state_t s = spin_lock_irqsave(&cur->lock);
    struct vnode *old = cur->cwd;
    cur->cwd = vn;
    strlcpy(cur->cwd_path, newpath, sizeof(cur->cwd_path));
    spin_unlock_irqrestore(&cur->lock, s);
    if (old)
        vnode_put(old);
    return 0;
}

unsigned process_info(struct cosmo_procinfo *buf, unsigned count)
{
    unsigned total = 0;
    arch_irq_state_t ts = spin_lock_irqsave(&g_process_table_lock);
    struct process *p;
    list_for_each_entry(p, &g_processes, all_link) {
        if (total < count) {
            struct cosmo_procinfo *pi = &buf[total];
            memset(pi, 0, sizeof(*pi));
            pi->pid = p->pid;
            pi->ppid = p->parent_pid;
            pi->uid = p->cred.uid;
            pi->gid = p->cred.gid;
            pi->state = (uint32_t)p->state;
            pi->syscalls = p->syscalls;
            strlcpy(pi->name, p->name, sizeof(pi->name));
            arch_irq_state_t s = spin_lock_irqsave(&p->lock);
            pi->nr_threads = p->nr_threads;
            struct thread *t;
            list_for_each_entry(t, &p->threads, proc_link)
                pi->run_ns += t->run_time_ns;
            spin_unlock_irqrestore(&p->lock, s);
        }
        total++;
    }
    spin_unlock_irqrestore(&g_process_table_lock, ts);
    return total;
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
