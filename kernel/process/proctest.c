/*
 * proctest.c - Boot-time self-tests for kernel objects, handles, the ELF
 * validator, and processes running the boot module.
 */

#include <kernel/bootarchive.h>
#include <kernel/bootinfo.h>
#include <kernel/elf.h>
#include <kernel/errno.h>
#include <kernel/handle.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/object.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>

#include <uapi/cosmo/syscall.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

/* --- objects and handles --- */

struct counted {
    struct kobject obj;
    int *released;
};

static void counted_release(struct kobject *obj)
{
    struct counted *c = container_of(obj, struct counted, obj);
    (*c->released)++;
}

static const struct kobject_type counted_type = { .name = "counted", .release = counted_release };

bool selftest_objects(const char **reason)
{
    int released = 0;
    struct counted c = { .released = &released };
    kobject_init(&c.obj, &counted_type);
    CHECK(kobject_refcount(&c.obj) == 1);
    kobject_get(&c.obj);
    CHECK(kobject_refcount(&c.obj) == 2);
    kobject_put(&c.obj);
    CHECK(released == 0);
    kobject_put(&c.obj);
    CHECK(released == 1);

    /* Handle table: install, rights, lookup, close, full, destroy. */
    struct handle_table *t = kmalloc(sizeof(*t), KMEM_ZERO);
    CHECK(t != NULL);
    handle_table_init(t);
    struct counted d = { .released = &released };
    released = 0;
    kobject_init(&d.obj, &counted_type);

    int h = handle_install(t, &d.obj, HANDLE_RIGHT_READ);
    CHECK(h == 0);
    CHECK(kobject_refcount(&d.obj) == 2);
    CHECK(handle_table_count(t) == 1);

    struct kobject *o = handle_lookup(t, h, HANDLE_RIGHT_READ);
    CHECK(o == &d.obj);
    CHECK(kobject_refcount(&d.obj) == 3);
    kobject_put(o);
    CHECK(handle_lookup(t, h, HANDLE_RIGHT_WRITE) == NULL);   /* lacks the right */
    CHECK(handle_lookup(t, 5, HANDLE_RIGHT_READ) == NULL);    /* empty */
    CHECK(handle_lookup(t, -1, 0) == NULL);
    CHECK(handle_lookup(t, HANDLE_TABLE_SIZE, 0) == NULL);

    CHECK(handle_install_at(t, 3, &d.obj, HANDLE_RIGHT_ALL) == 3);
    CHECK(handle_install_at(t, 3, &d.obj, HANDLE_RIGHT_ALL) == -EBUSY);
    CHECK(handle_install_at(t, 99, &d.obj, HANDLE_RIGHT_ALL) == -EBADF);
    CHECK(handle_close(t, 3) == 0);
    CHECK(handle_close(t, 3) == -EBADF);

    /* Fill it. */
    int installed = 0;
    for (;;) {
        int r = handle_install(t, &d.obj, HANDLE_RIGHT_READ);
        if (r < 0) {
            CHECK(r == -EMFILE);
            break;
        }
        installed++;
    }
    CHECK(installed == HANDLE_TABLE_SIZE - 1);
    CHECK(handle_table_count(t) == HANDLE_TABLE_SIZE);

    handle_table_destroy(t);
    CHECK(handle_table_count(t) == 0);
    CHECK(kobject_refcount(&d.obj) == 1);
    kobject_put(&d.obj);
    CHECK(released == 1);
    kfree(t);
    return true;
}

/* --- ELF validator on crafted images --- */

struct tiny_elf {
    uint8_t ehdr[64];
    uint8_t phdr[56];
};

static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

static void make_elf(struct tiny_elf *e, uint64_t entry, uint64_t vaddr, uint32_t flags)
{
    memset(e, 0, sizeof(*e));
    memcpy(e->ehdr, "\177ELF", 4);
    e->ehdr[4] = 2;   /* ELFCLASS64 */
    e->ehdr[5] = 1;   /* little-endian */
    e->ehdr[6] = 1;   /* EV_CURRENT */
    put16(e->ehdr + 16, 2);   /* ET_EXEC */
    put16(e->ehdr + 18, 62);  /* EM_X86_64 */
    put32(e->ehdr + 20, 1);
    put64(e->ehdr + 24, entry);
    put64(e->ehdr + 32, 64);  /* e_phoff */
    put16(e->ehdr + 52, 64);  /* e_ehsize */
    put16(e->ehdr + 54, 56);  /* e_phentsize */
    put16(e->ehdr + 56, 1);   /* e_phnum */
    put32(e->phdr + 0, 1);    /* PT_LOAD */
    put32(e->phdr + 4, flags);
    put64(e->phdr + 8, 0);    /* p_offset */
    put64(e->phdr + 16, vaddr);
    put64(e->phdr + 32, 120); /* p_filesz */
    put64(e->phdr + 40, 4096);/* p_memsz */
    put64(e->phdr + 48, 4096);
}

bool selftest_elf(const char **reason)
{
    struct tiny_elf e;
    struct elf_info info;
    const char *why;

    make_elf(&e, 0x400010, 0x400000, ELF_PF_R | ELF_PF_X);
    CHECK(elf_validate(&e, sizeof(e), USER_LO, USER_HI, &info, &why) == 0);
    CHECK(info.nr_segments == 1 && info.entry == 0x400010);
    CHECK(info.segments[0].vaddr == 0x400000 && info.segments[0].memsz == 4096);

    make_elf(&e, 0x400010, 0x400000, ELF_PF_R | ELF_PF_W | ELF_PF_X);
    CHECK(elf_validate(&e, sizeof(e), USER_LO, USER_HI, &info, &why) == -ENOEXEC);
    CHECK(strcmp(why, "PT_LOAD is writable and executable (W^X)") == 0);

    make_elf(&e, 0x400010, 0x400000, ELF_PF_R | ELF_PF_W);
    CHECK(elf_validate(&e, sizeof(e), USER_LO, USER_HI, &info, &why) == -ENOEXEC);
    CHECK(strcmp(why, "entry point is not inside an executable segment") == 0);

    make_elf(&e, 0x400010, 0x1000, ELF_PF_R | ELF_PF_X);            /* below the window */
    CHECK(elf_validate(&e, sizeof(e), USER_LO, USER_HI, &info, &why) == -ENOEXEC);

    make_elf(&e, 0x400010, 0x400000, ELF_PF_R | ELF_PF_X);
    put64(e.phdr + 32, 100000);                                      /* filesz beyond the file */
    CHECK(elf_validate(&e, sizeof(e), USER_LO, USER_HI, &info, &why) == -ENOEXEC);

    make_elf(&e, 0x400010, 0x400000, ELF_PF_R | ELF_PF_X);
    e.ehdr[0] = 'X';
    CHECK(elf_validate(&e, sizeof(e), USER_LO, USER_HI, &info, &why) == -ENOEXEC);

    make_elf(&e, 0x400010, 0x400000, ELF_PF_R | ELF_PF_X);
    put16(e.ehdr + 16, 3);                                           /* ET_DYN */
    CHECK(elf_validate(&e, sizeof(e), USER_LO, USER_HI, &info, &why) == -ENOEXEC);

    CHECK(elf_validate(&e, 10, USER_LO, USER_HI, &info, &why) == -ENOEXEC);
    return true;
}

/* --- run the boot module --- */

static bool run_module(const char *const argv[], int *status_out, const char **reason)
{
    const void *image;
    size_t image_size;
    if (!bootarchive_find("init", &image, &image_size)) {
        kinfo("selftest: no init in the boot archive; skipping");
        *status_out = -1;
        return true;
    }
    struct process *p = NULL;
    unsigned before = process_count();
    int rc = process_create_from_elf(image, image_size, argv[0], argv, NULL, NULL, &p);
    CHECK(rc == 0);
    CHECK(p != NULL && p->pid > 0);

    uint64_t t0 = clock_now_ns();
    int status = process_wait_exit(p);
    CHECK(clock_now_ns() - t0 < 5000000000ULL);
    process_put(p);

    /* The process object is released once its thread is reaped. */
    uint64_t deadline = clock_now_ns() + 500000000ULL;
    while (process_count() != before && clock_now_ns() < deadline)
        sched_yield();
    CHECK(process_count() == before);
    *status_out = status;
    return true;
}

bool selftest_process_selftest(const char **reason)
{
    static const char *const argv[] = { "init", "--selftest", NULL };
    int status;
    if (!run_module(argv, &status, reason))
        return false;
    if (status == -1)
        return true;
    CHECK(status == 0);
    return true;
}

bool selftest_process_fault(const char **reason)
{
    static const char *const argv[] = { "init", "--crash", NULL };
    int status;
    if (!run_module(argv, &status, reason))
        return false;
    if (status == -1)
        return true;
    CHECK(status == COSMO_EXIT_FAULT);
    return true;
}

bool selftest_process_reject(const char **reason)
{
    /* A kernel image is a valid ELF but not a user executable. */
    const struct cosmoboot_info *info = bootinfo_get();
    struct process *p = NULL;
    static const char *const argv[] = { "bogus", NULL };
    char junk[128];
    memset(junk, 0, sizeof(junk));
    CHECK(process_create_from_elf(junk, sizeof(junk), "junk", argv, NULL, NULL, &p) == -ENOEXEC);
    CHECK(p == NULL);
    (void)info;
    return true;
}

/* --- Phase 9: kill delivery and path normalisation --- */

/* Run the boot module with `argv` and kill it with `sig` once it has had
 * time to block or spin; the exit status must be 128 + sig. */
static bool kill_module(const char *const argv[], int sig, const char **reason)
{
    const void *image;
    size_t image_size;
    if (!bootarchive_find("init", &image, &image_size))
        return true;
    struct process *p = NULL;
    CHECK(process_create_from_elf(image, image_size, argv[0], argv, NULL, NULL, &p) == 0);
    thread_sleep_ms(50);
    CHECK(!completion_done(&p->exited));
    process_kill(p, sig);
    uint64_t t0 = clock_now_ns();
    int status = process_wait_exit(p);
    CHECK(clock_now_ns() - t0 < 2000000000ULL);
    CHECK(status == 128 + sig);
    process_put(p);
    return true;
}

/* Phase 11: the CosmoOS note marks native programs; a Linux test program lacks it. */
bool selftest_linux_elf(const char **reason)
{
    const void *image;
    size_t image_size;
    struct elf_info info;
    const char *why;
    if (bootarchive_find("init", &image, &image_size)) {
        CHECK(elf_validate(image, image_size, USER_LO, USER_HI, &info, &why) == 0);
        CHECK(info.cosmo_note);
        CHECK(info.phdr_vaddr != 0 && info.phnum > 0 && info.phent == 56);
    }
    if (bootarchive_find("tests/linux/lxhello", &image, &image_size)) {
        CHECK(elf_validate(image, image_size, USER_LO, USER_HI, &info, &why) == 0);
        CHECK(!info.cosmo_note);
        CHECK(info.phdr_vaddr == 0x400040);
    }
    return true;
}

bool selftest_process_spawn(const char **reason)
{
    char out[64];
    CHECK(path_normalize("/", "usr/bin", out, sizeof(out)) == 0 && strcmp(out, "/usr/bin") == 0);
    CHECK(path_normalize("/usr/bin", "..", out, sizeof(out)) == 0 && strcmp(out, "/usr") == 0);
    CHECK(path_normalize("/usr/bin", "../../..", out, sizeof(out)) == 0 && strcmp(out, "/") == 0);
    CHECK(path_normalize("/a", "./b//c/./d", out, sizeof(out)) == 0 && strcmp(out, "/a/b/c/d") == 0);
    CHECK(path_normalize("/a/b", "/x/../y", out, sizeof(out)) == 0 && strcmp(out, "/y") == 0);
    CHECK(path_normalize("/", ".", out, sizeof(out)) == 0 && strcmp(out, "/") == 0);
    CHECK(path_normalize("/a", "b", out, 4) == -ENAMETOOLONG);

    /* A process blocked in a console read dies from a kill (killable
     * wait); a spinning one dies at its next return to user mode. */
    static const char *const block_argv[] = { "init", "--block", NULL };
    static const char *const spin_argv[] = { "init", "--spin", NULL };
    if (!kill_module(block_argv, COSMO_SIGTERM, reason))
        return false;
    if (!kill_module(spin_argv, COSMO_SIGKILL, reason))
        return false;
    return true;
}
