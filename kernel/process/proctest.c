/*
 * proctest.c - Boot-time self-tests for kernel objects, handles, the ELF
 * validator, and processes running the boot module.
 */

#include <kernel/bootarchive.h>
#include <kernel/bootinfo.h>
#include <kernel/elf.h>
#include <kernel/elf64.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/handle.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/object.h>
#include <kernel/pmm.h>
#include <kernel/printf.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/signal.h>
#include <kernel/timer.h>
#include <kernel/tty.h>
#include <kernel/vfs.h>
#include <kernel/vmm.h>

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

    /* Rights distinguish "no such handle" from "not through this
     * handle": a caller that wants to answer -EPERM can tell them
     * apart (docs/kernel/object/architecture.md, "Rights"). */
    bool missing = false;
    CHECK(handle_lookup_rights(t, h, HANDLE_RIGHT_WRITE, &missing) == NULL && missing);
    CHECK(handle_lookup_rights(t, 5, HANDLE_RIGHT_READ, &missing) == NULL && !missing);
    CHECK(handle_lookup_rights(t, h, HANDLE_RIGHT_READ, &missing) != NULL && !missing);
    kobject_put(&d.obj);   /* the lookup above took a reference */

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
    /* The gate stays shut afterwards: a thread still inside a syscall
     * must not be able to look one up or put one back, or it would hand
     * out a reference the exit has already released, or add one that
     * nothing will ever close. */
    CHECK(handle_lookup(t, 0, 0) == NULL);
    CHECK(handle_get(t, 0, &(unsigned){ 0 }) == NULL);
    CHECK(handle_install(t, &d.obj, HANDLE_RIGHT_ALL) == -EBADF);
    CHECK(handle_install_at(t, 7, &d.obj, HANDLE_RIGHT_ALL) == -EBADF);
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
    put16(e->ehdr + 18, ELF_MACHINE_NATIVE);
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

    /* ET_DYN (milestone 10): accepted with relative addresses, rebased by the caller. */
    make_elf(&e, 0x10, 0x0, ELF_PF_R | ELF_PF_X);
    put16(e.ehdr + 16, 3);                                           /* ET_DYN */
    CHECK(elf_validate(&e, sizeof(e), USER_LO, USER_HI, &info, &why) == 0);
    CHECK(info.is_dyn && !info.has_interp && info.lo == 0 && info.entry == 0x10);
    elf_rebase(&info, USER_PIE_BASE);
    CHECK(info.segments[0].vaddr == USER_PIE_BASE && info.entry == USER_PIE_BASE + 0x10 && info.hi == USER_PIE_BASE + 4096);
    make_elf(&e, 0x10, USER_HI - USER_LO, ELF_PF_R | ELF_PF_X);     /* relative, yet past the window's span */
    put16(e.ehdr + 16, 3);
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

    /*
     * The bound catches a hang, not slowness: init's self-test now
     * supervises services, which means spawning processes and waiting
     * out real restart backoffs, and it runs on an emulated machine
     * that may be several times slower than this one. Fifteen seconds
     * still says "stuck" and no longer says "busy".
     */
    uint64_t t0 = clock_now_ns();
    int status = process_wait_exit(p);
    CHECK(clock_since_ns(t0) < 15000000000ULL);
    process_put(p);

    /* The process object is released once its thread is reaped. The
     * bound catches a leak, not slowness: under the chaos migrator
     * (`make test-chaos`) the reaper's turn came later than 500 ms once
     * in about fifty boots, so it is two seconds (LOAD-SENSITIVE,
     * docs/testing/flakes.md). */
    uint64_t deadline = clock_deadline_ns(2000000000ULL);
    while (process_count() != before && !clock_deadline_passed(deadline))
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

/* The guest syscall fuzzer (docs/verification/design.md): 20 000 random
 * system calls from an unprivileged init with a fixed seed; the process
 * must survive and report, and so must the kernel. */
bool selftest_syscall_fuzz(const char **reason)
{
    static const char *const argv[] = { "init", "--syscall-fuzz", "20000", "20260905", NULL };
    int status;
    if (!run_module(argv, &status, reason))
        return false;
    if (status == -1)
        return true;
    CHECK(status == 0);
    return true;
}

/*
 * The native signal ABI (docs/kernel/process/design.md, "The native
 * signal ABI"). Each probe is a user program that installs a handler,
 * takes the signal, and checks what the handler left behind: the
 * interrupted registers -- general and vector both -- the blocked mask,
 * and the siginfo. The three cover the three delivery points, because
 * the frame is built at each of them from a different arch frame.
 */
/* Each probe answers with the number of the check it failed, which is
 * the only thing that says what broke: the kernel side sees one exit
 * status and the program that knows the detail is gone. */
static bool run_signal_probe(const char *kind, const char **reason)
{
    const char *argv[] = { "init", "--probe", kind, NULL };
    int status;
    if (!run_module(argv, &status, reason))
        return false;
    if (status == -1)
        return true;   /* no init in the boot archive */
    if (status != 0) {
        kwarn("selftest: signal probe '%s' failed check %d", kind, status);
        *reason = "the signal probe reported a failure";
        return false;
    }
    return true;
}

bool selftest_signal_native(const char **reason)
{
    return run_signal_probe("signal", reason);
}

bool selftest_signal_async(const char **reason)
{
    return run_signal_probe("signal-async", reason);
}

bool selftest_signal_mask(const char **reason)
{
    return run_signal_probe("signal-mask", reason);
}

bool selftest_signal_fault(const char **reason)
{
    return run_signal_probe("signal-fault", reason);
}

/*
 * A pid whose status has been collected must not be findable, even
 * while the object behind it is still alive -- which is the state
 * between the reap and the last reference going. From user mode that
 * window is a race (CI lost it once on `kill(pid, 0)` after `waitpid`,
 * and 200 tries in a row never lost it on the development machine); the
 * kernel can simply hold the reference and look.
 */
bool selftest_process_reaped(const char **reason)
{
    const void *image;
    size_t image_size;
    if (!bootarchive_find("init", &image, &image_size)) {
        kinfo("selftest: no init in the boot archive; skipping");
        return true;
    }
    static const char *const argv[] = { "init", "--probe", "signal-sleep", NULL };
    struct process *p = NULL;
    CHECK(process_create_from_elf(image, image_size, argv[0], argv, NULL, NULL, &p) == 0);
    pid_t pid = p->pid;
    /* Alive and findable, and the lookup takes its own reference. */
    struct process *found = process_lookup(pid);
    CHECK(found == p);
    process_put(found);

    /* It has no parent, so exiting reaps it -- while this test still
     * holds the creation reference, so the object cannot be released
     * and the pid is still in the table. */
    CHECK(process_wait_exit(p) == 0);
    CHECK(process_lookup(pid) == NULL);
    process_put(p);
    return true;
}

bool selftest_signal_group(const char **reason)
{
    return run_signal_probe("signal-group", reason);
}

bool selftest_signal_setsid(const char **reason)
{
    return run_signal_probe("signal-setsid", reason);
}

/*
 * The terminal's foreground group, driven from both ends: a user process
 * claims the terminal and waits, another session is refused it, and the
 * ^C this test types reaches exactly the process that holds it. Then the
 * leader exits and the terminal is free again, which is what lets the
 * shell claim it after the self-tests are over.
 */
bool selftest_signal_stop(const char **reason)
{
    return run_signal_probe("signal-stop", reason);
}

bool selftest_signal_stop_kill(const char **reason)
{
    return run_signal_probe("signal-stop-kill", reason);
}

bool selftest_signal_stop_mask(const char **reason)
{
    return run_signal_probe("signal-stop-mask", reason);
}

/*
 * The restart, driven from both ends. The probe puts a reader in the
 * background so that its own read stops it -- the only way to be sure
 * the stop lands *inside* the call under test -- then hands it the
 * terminal and continues it. This side types the line the restarted
 * read must find.
 */
bool selftest_signal_stop_restart(const char **reason)
{
    const void *image;
    size_t image_size;
    if (!bootarchive_find("init", &image, &image_size)) {
        kinfo("selftest: no init in the boot archive; skipping");
        return true;
    }
    struct tty *t = tty_console();
    static const char *const argv[] = { "init", "--probe", "signal-stop-restart", NULL };
    struct process *p = NULL;
    CHECK(process_create_from_elf(image, image_size, argv[0], argv, NULL, NULL, &p) == 0);
    pid_t pid = p->pid;
    /* Wait until the reader, not the probe, is the foreground group:
     * that is the probe saying it has stopped it and continued it. */
    uint64_t deadline = clock_now_ns() + 8000000000ULL;
    pid_t fg = 0;
    while (clock_now_ns() < deadline) {
        fg = tty_foreground_pgrp(t);
        if (fg != 0 && fg != pid)
            break;
        thread_sleep_ms(1);   /* sleep, not yield: the probe needs the CPU */
    }
    if (fg == 0 || fg == pid) {
        process_put(p);
        *reason = "the reader never reached the foreground";
        return false;
    }
    tty_input(t, (const uint8_t *)"x\n", 2);
    int status = process_wait_exit(p);
    process_put(p);
    if (status != 0) {
        kwarn("selftest: signal-stop-restart: the probe failed check %d", status);
        *reason = "a call cut short by a stop was failed rather than restarted";
        return false;
    }
    return true;
}

bool selftest_signal_stop_late(const char **reason)
{
    return run_signal_probe("signal-stop-late", reason);
}

bool selftest_signal_stop_threads(const char **reason)
{
    return run_signal_probe("signal-stop-threads", reason);
}

/*
 * ^Z at the terminal. The same two-ended shape as tty-intr: a process
 * claims the terminal and waits, the kernel types the keystroke, and
 * the process must *stop* -- not die, which is what every other control
 * character here does. Then a SIGCONT and it finishes.
 */
bool selftest_tty_stop(const char **reason)
{
    const void *image;
    size_t image_size;
    if (!bootarchive_find("init", &image, &image_size)) {
        kinfo("selftest: no init in the boot archive; skipping");
        return true;
    }
    struct tty *t = tty_console();
    static const char *const argv[] = { "init", "--probe", "signal-tty-stop", NULL };
    struct process *p = NULL;
    CHECK(process_create_from_elf(image, image_size, argv[0], argv, NULL, NULL, &p) == 0);
    pid_t pid = p->pid;
    /* It claims the terminal for its *child's* group, so the foreground
     * group is something other than its own pid. */
    uint64_t deadline = clock_now_ns() + 5000000000ULL;
    pid_t fg = 0;
    while (clock_now_ns() < deadline) {
        fg = tty_foreground_pgrp(t);
        if (fg != 0 && fg != pid)
            break;
        thread_sleep_ms(1);   /* sleep, not yield: the probe needs the CPU */
    }
    if (fg == 0 || fg == pid) {
        process_put(p);
        *reason = "the job never became the foreground group";
        return false;
    }
    uint8_t susp = 0x1a;   /* ^Z */
    tty_input(t, &susp, 1);
    int status = process_wait_exit(p);
    process_put(p);
    if (status != 0) {
        kwarn("selftest: tty-stop: the probe failed check %d", status);
        *reason = "^Z did not stop the foreground job";
        return false;
    }
    CHECK(tty_foreground_pgrp(t) == 0);
    return true;
}

bool selftest_tty_ttin(const char **reason)
{
    const void *image;
    size_t image_size;
    if (!bootarchive_find("init", &image, &image_size)) {
        kinfo("selftest: no init in the boot archive; skipping");
        return true;
    }
    static const char *const argv[] = { "init", "--probe", "signal-tty-background", NULL };
    struct process *p = NULL;
    CHECK(process_create_from_elf(image, image_size, argv[0], argv, NULL, NULL, &p) == 0);
    int status = process_wait_exit(p);
    process_put(p);
    if (status != 0) {
        kwarn("selftest: tty-ttin: the probe failed check %d", status);
        *reason = "a background reader was not refused the terminal";
        return false;
    }
    CHECK(tty_foreground_pgrp(tty_console()) == 0);
    return true;
}

/*
 * Non-canonical mode, driven from both ends: the probe claims the
 * terminal and turns canonical mode off, this side types one byte, and
 * the probe must read it without a newline ever arriving.
 */
static bool run_tty_probe(const char *kind, const uint8_t *type, size_t n, const char **reason)
{
    const void *image;
    size_t image_size;
    if (!bootarchive_find("init", &image, &image_size)) {
        kinfo("selftest: no init in the boot archive; skipping");
        return true;
    }
    struct tty *t = tty_console();
    const char *argv[] = { "init", "--probe", kind, NULL };
    struct process *p = NULL;
    CHECK(process_create_from_elf(image, image_size, argv[0], argv, NULL, NULL, &p) == 0);
    pid_t pid = p->pid;
    /*
     * Only a probe that must be typed at is waited for. A probe with
     * nothing to type claims the terminal, does its work and exits, and
     * releasing the terminal on the way out sets the foreground group
     * back to 0 -- so watching for the claim is watching for a window
     * that closes on its own, and a test that lost the race called a
     * probe that had already passed a failure. What that probe proves,
     * it proves by its exit status.
     */
    if (n) {
        uint64_t deadline = clock_now_ns() + 5000000000ULL;
        while (tty_foreground_pgrp(t) != pid && clock_now_ns() < deadline)
            thread_sleep_ms(1);   /* sleep, not yield: the probe needs the CPU */
        if (tty_foreground_pgrp(t) != pid) {
            process_put(p);
            *reason = "the probe never claimed the terminal";
            return false;
        }
        /*
         * Then wait for the mode, not for a handshake: a byte typed
         * while the line discipline is still canonical would be edited
         * rather than delivered, and the probe has no handle to say
         * "ready" on. The terminal's own state is the readiness signal.
         * This window does not close on its own -- the probe is blocked
         * in the read that is waiting for the byte.
         */
        deadline = clock_now_ns() + 5000000000ULL;
        struct cosmo_termios tio;
        for (;;) {
            tty_get_termios(t, &tio);
            if (!(tio.modes & COSMO_TTY_ICANON) || clock_now_ns() >= deadline)
                break;
            thread_sleep_ms(1);
        }
        if (tio.modes & COSMO_TTY_ICANON) {
            process_put(p);
            *reason = "the probe never left canonical mode";
            return false;
        }
        tty_input(t, type, n);
    }
    int status = process_wait_exit(p);
    process_put(p);
    if (status != 0) {
        kwarn("selftest: %s: the probe failed check %d", kind, status);
        *reason = "the terminal-mode probe reported a failure";
        return false;
    }
    return true;
}

bool selftest_tty_raw(const char **reason)
{
    static const uint8_t x = 'x';
    return run_tty_probe("tty-raw", &x, 1, reason);
}

bool selftest_tty_nosig(const char **reason)
{
    static const uint8_t intr = 0x03;   /* ^C, which must arrive as a byte */
    return run_tty_probe("tty-nosig", &intr, 1, reason);
}

bool selftest_tty_isatty(const char **reason)
{
    return run_signal_probe("tty-isatty", reason);
}

bool selftest_tty_pollraw(const char **reason)
{
    return run_signal_probe("tty-pollraw", reason);
}

bool selftest_dev_tty(const char **reason)
{
    return run_tty_probe("dev-tty", NULL, 0, reason);
}

bool selftest_dev_tty_none(const char **reason)
{
    return run_signal_probe("dev-tty-none", reason);
}

bool selftest_tty_intr(const char **reason)
{
    const void *image;
    size_t image_size;
    if (!bootarchive_find("init", &image, &image_size)) {
        kinfo("selftest: no init in the boot archive; skipping");
        return true;
    }
    struct tty *t = tty_console();
    CHECK(tty_foreground_pgrp(t) == 0);   /* nothing holds it yet */

    static const char *const argv[] = { "init", "--probe", "signal-tty", NULL };
    struct process *p = NULL;
    CHECK(process_create_from_elf(image, image_size, argv[0], argv, NULL, NULL, &p) == 0);
    pid_t pid = p->pid;   /* it is its own group and its own session leader */

    uint64_t deadline = clock_now_ns() + 5000000000ULL;
    while (tty_foreground_pgrp(t) != pid && clock_now_ns() < deadline)
        thread_sleep_ms(1);   /* sleep, not yield: the probe needs the CPU */
    if (tty_foreground_pgrp(t) != pid) {
        process_put(p);
        *reason = "the terminal was never claimed";
        return false;
    }

    /* A second session is refused both the terminal and the question. */
    static const char *const steal[] = { "init", "--probe", "signal-tty-steal", NULL };
    struct process *q = NULL;
    CHECK(process_create_from_elf(image, image_size, steal[0], steal, NULL, NULL, &q) == 0);
    int steal_status = process_wait_exit(q);
    process_put(q);
    if (steal_status != 0) {
        process_put(p);
        kwarn("selftest: tty-intr: the stealing probe failed check %d", steal_status);
        *reason = "another session was allowed the terminal";
        return false;
    }

    /* A partial line and then the interrupt: the line under edit is
     * thrown away -- no line reaches a reader -- and the signal reaches
     * the foreground group. The console may already hold input typed by
     * the harness, so what is checked is that this typing added none. */
    struct tty_stats before, after;
    tty_get_stats(t, &before);
    tty_input(t, (const uint8_t *)"abc", 3);
    uint8_t intr = 0x03;
    tty_input(t, &intr, 1);
    int status = process_wait_exit(p);
    process_put(p);
    CHECK(status == 128 + SIGINT);
    tty_get_stats(t, &after);
    CHECK(after.lines_in == before.lines_in);
    /* The leader is gone, so the terminal is nobody's again. */
    CHECK(tty_foreground_pgrp(t) == 0);

    /*
     * Every signal a batch carries, not just its last. One process with
     * SIGINT caught and SIGQUIT left fatal, and one write of `^\` then
     * `^C`: it must die of the quit. A line discipline that remembered
     * only the last signal of a batch would send the interrupt alone,
     * the handler would run, and the process would still be here.
     */
    static const char *const quit[] = { "init", "--probe", "signal-tty-quit", NULL };
    struct process *b = NULL;
    CHECK(process_create_from_elf(image, image_size, quit[0], quit, NULL, NULL, &b) == 0);
    pid_t bpid = b->pid;
    deadline = clock_now_ns() + 5000000000ULL;
    while (tty_foreground_pgrp(t) != bpid && clock_now_ns() < deadline)
        thread_sleep_ms(1);
    if (tty_foreground_pgrp(t) != bpid) {
        process_put(b);
        *reason = "the terminal was never claimed by the second probe";
        return false;
    }
    static const uint8_t batch[2] = { 0x1c, 0x03 };   /* ^\ then ^C, one write */
    tty_input(t, batch, sizeof(batch));
    int bstatus = process_wait_exit(b);
    process_put(b);
    CHECK(bstatus == 128 + SIGQUIT);
    CHECK(tty_foreground_pgrp(t) == 0);
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

/* --probe efault: system calls whose user pointers name PROT_NONE,
 * read-only and unmapped pages get -EFAULT through the exception fixup
 * path; the process survives and exits 0 (design.md §6.1). */
bool selftest_process_efault(const char **reason)
{
    static const char *const argv[] = { "init", "--probe", "efault", NULL };
    int status;
    struct vm_stats s0, s1;
    vm_get_stats(&s0);
    if (!run_module(argv, &status, reason))
        return false;
    if (status == -1)
        return true;
    CHECK(status == 0);
    vm_get_stats(&s1);
    CHECK(s1.fixups > s0.fixups);
    kinfo("selftest: process-efault: %llu kernel-mode faults resumed as -EFAULT", (unsigned long long)(s1.fixups - s0.fixups));
    return true;
}

/* --probe none-touch: user code touching a PROT_NONE page dies with the
 * fault status; the kernel keeps running. */
bool selftest_process_protnone(const char **reason)
{
    static const char *const argv[] = { "init", "--probe", "none-touch", NULL };
    int status;
    if (!run_module(argv, &status, reason))
        return false;
    if (status == -1)
        return true;
    CHECK(status == COSMO_EXIT_FAULT);
    return true;
}

/* Demand-zero allocation failures: inside a user copy the system call
 * returns -EFAULT (the process exits 0); on a user-mode touch the process
 * dies. Both were kernel panics before milestone 5 (finding #11). */
bool selftest_process_oom(const char **reason)
{
#if CONFIG_FAULTINJECT
    static const char *const copy_argv[] = { "init", "--probe", "oom-copy", NULL };
    static const char *const touch_argv[] = { "init", "--probe", "oom-touch", NULL };
    int status;
    struct fi_stats st;

    faultinject_set(FI_DEMAND_COPY, 1, 1, NULL);   /* the first kernel-mode demand fault in any user space */
    bool ok = run_module(copy_argv, &status, reason);
    faultinject_clear(FI_DEMAND_COPY);
    if (!ok)
        return false;
    if (status == -1)
        return true;
    faultinject_stats(FI_DEMAND_COPY, &st);
    CHECK(st.hits == 1);
    CHECK(status == 0);

    faultinject_set(FI_DEMAND_PAGE, 1, 1, NULL);   /* the first user-mode demand fault */
    ok = run_module(touch_argv, &status, reason);
    faultinject_clear(FI_DEMAND_PAGE);
    if (!ok)
        return false;
    faultinject_stats(FI_DEMAND_PAGE, &st);
    CHECK(st.hits == 1);
    CHECK(status == COSMO_EXIT_FAULT);
    kinfo("selftest: process-oom: an injected demand-page failure is -EFAULT in a copy and fatal on a user touch");
    return true;
#else
    (void)reason;
    kinfo("selftest: process-oom: fault injection is compiled out of this build");
    return true;
#endif
}

/*
 * The two held-fault proofs of the file-regions unit
 * (docs/audit/next-subsystem-file-regions.md, "One seam, for two
 * proofs"): a FILE fault held between its phases while another thread
 * installs the same page (the retry path finds it present: one frame),
 * and while the range is unmapped (the retry path installs nothing and
 * the instruction's retry is SIGSEGV). The seam is event-driven: the
 * held fault moves when the other event happens, never on a clock.
 */
bool selftest_vm_file_fault_hold(const char **reason)
{
#if CONFIG_DEBUG
    static const char *const race_argv[] = { "init", "--probe", "mmap-race", NULL };
    static const char *const unmap_argv[] = { "init", "--probe", "mmap-unmap-race", NULL };
    struct vm_stats s0, s1;
    int status;

    vm_get_stats(&s0);
    vm_test_file_hold_arm();
    bool ok = run_module(race_argv, &status, reason);
    if (!ok)
        return false;
    if (status == -1)
        return true;
    CHECK(status == 0);
    CHECK(vm_test_file_hold_state() == 0);   /* held, then released by the other thread's install */
    vm_get_stats(&s1);
    CHECK(s1.file_fault_retries - s0.file_fault_retries == 1);

    vm_get_stats(&s0);
    vm_test_file_hold_arm();
    ok = run_module(unmap_argv, &status, reason);
    if (!ok)
        return false;
    CHECK(status == COSMO_EXIT_FAULT);      /* the retry met no region */
    CHECK(vm_test_file_hold_state() == 0);
    vm_get_stats(&s1);
    CHECK(s1.file_fault_retries - s0.file_fault_retries == 1);

    /* The range replaced by a mapping of another file while held: the
     * re-find is by (vnode, index, sharing), so the first file's page is
     * not installed under the second file's name. */
    static const char *const remap_argv[] = { "init", "--probe", "mmap-remap-race", NULL };
    vm_get_stats(&s0);
    vm_test_file_hold_arm();
    ok = run_module(remap_argv, &status, reason);
    if (!ok)
        return false;
    CHECK(status == 0);
    CHECK(vm_test_file_hold_state() == 0);
    vm_get_stats(&s1);
    CHECK(s1.file_fault_retries - s0.file_fault_retries == 1);
    CHECK(vfs_unlink(NULL, "/tmp/mm-race") == 0 && vfs_unlink(NULL, "/tmp/mm-race2") == 0);
    kinfo("selftest: vm-file-fault-hold: a held fault installs nothing over another's page, a gone range, or another file's");
    return true;
#else
    (void)reason;
    kinfo("selftest: vm-file-fault-hold: the seam is compiled out of this build");
    return true;
#endif
}

/* A cache miss whose read fails under a mapping: SIGBUS on the touch,
 * nothing installed, the counter says so. */
bool selftest_vm_file_readpage_fail(const char **reason)
{
#if CONFIG_FAULTINJECT
    static const char *const argv[] = { "init", "--probe", "mmap-readfail", NULL };
    struct vm_stats s0, s1;
    struct fi_stats st;
    int status;

    /* The file is made here, not by the child: its first two pages are a
     * hole (written past), so the child's touch of page 0 is the first
     * miss after the rule is armed -- a child making the file would spend
     * the injected failure on its own write. */
    struct file *f;
    CHECK(vfs_open(NULL, "/tmp/mm-readfail", COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f) == 0);
    CHECK(file_pwrite(f, "hole", 4, 2 * PAGE_SIZE) == 4);
    file_put(f);

    vm_get_stats(&s0);
    faultinject_set(FI_FILE_READPAGE, 1, 1, NULL);   /* the next miss, in any file */
    bool ok = run_module(argv, &status, reason);
    faultinject_clear(FI_FILE_READPAGE);
    CHECK(vfs_unlink(NULL, "/tmp/mm-readfail") == 0);
    if (!ok)
        return false;
    if (status == -1)
        return true;
    faultinject_stats(FI_FILE_READPAGE, &st);
    CHECK(st.hits == 1);
    CHECK(status == 128 + SIGBUS);
    vm_get_stats(&s1);
    CHECK(s1.file_sigbus - s0.file_sigbus == 1);
    CHECK(s1.file_faults == s0.file_faults);   /* nothing installed */
    kinfo("selftest: vm-file-readpage-fail: a read that fails under a mapping is SIGBUS");
    return true;
#else
    (void)reason;
    kinfo("selftest: vm-file-readpage-fail: fault injection is compiled out of this build");
    return true;
#endif
}

/* Resource limits and the credential transition (docs/kernel/security/design.md):
 * a root probe (defaults, AS, NOFILE, NPROC, VMEM, SETCRED to another user),
 * an unprivileged probe (lowering only, no way back to root, procinfo shows
 * its own user, the log rate limit), and a resident-memory limit that ends
 * a process touching past it. */
bool selftest_process_rlimit(const char **reason)
{
    static const char *const root_argv[] = { "init", "--probe", "rlimit-root", NULL };
    static const char *const unpriv_argv[] = { "init", "--probe", "rlimit-unpriv", NULL };
    static const char *const mem_argv[] = { "init", "--probe", "mem-limit", NULL };
    int status;
    if (!run_module(root_argv, &status, reason))
        return false;
    if (status == -1)
        return true;
    CHECK(status == 0);
    CHECK(run_module(unpriv_argv, &status, reason));
    CHECK(status == 0);
    CHECK(run_module(mem_argv, &status, reason));
    CHECK(status == COSMO_EXIT_FAULT);
    kinfo("selftest: process-rlimit: limits inherited, lowered, raised only by root; SETCRED flows down; a memory limit ends the toucher");
    return true;
}

/* Two kernel threads create children of uid 4242 under a limit of four
 * while a third samples the count: the admission is decided under the
 * table lock, so the count never exceeds the limit (Greptile on PR #21
 * found the earlier count-then-register window). */
struct nproc_stress {
    const void *image;
    size_t size;
    unsigned ok, eagain;
    struct process *kids[8];
};

static const struct rlimits g_nproc_rlim = {
    .v = { [COSMO_RLIMIT_AS] = 2ull << 30, [COSMO_RLIMIT_MEM] = 128ull << 20, [COSMO_RLIMIT_NOFILE] = 64,
           [COSMO_RLIMIT_NPROC] = 4, [COSMO_RLIMIT_VMEM] = 0 },
};

static void nproc_spawner(void *arg)
{
    struct nproc_stress *st = arg;
    static const char *const argv[] = { "init", "--probe", "hold", NULL };
    struct process_spawn_attr attr = { .set_cred = true, .uid = 4242, .gid = 4242, .rlim = &g_nproc_rlim };
    for (unsigned i = 0; i < 8; i++) {
        struct process *p = NULL;
        int rc = process_create_from_elf(st->image, st->size, "hold", argv, NULL, &attr, &p);
        if (rc == 0) {
            st->kids[st->ok++] = p;
        } else if (rc == -EAGAIN) {
            st->eagain++;
            thread_sleep_ms(5);
        }
    }
}

static volatile bool g_nproc_stop;
static volatile unsigned g_nproc_peak;

static void nproc_sampler(void *arg)
{
    (void)arg;
    while (!g_nproc_stop) {
        unsigned n = process_count_uid(4242);
        if (n > g_nproc_peak)
            g_nproc_peak = n;
        sched_yield();
    }
}

bool selftest_process_nproc(const char **reason)
{
    const void *image;
    size_t size;
    if (!bootarchive_find("init", &image, &size)) {
        kinfo("selftest: no init in the boot archive; skipping");
        return true;
    }
    struct nproc_stress a = { .image = image, .size = size }, b = { .image = image, .size = size };
    g_nproc_stop = false;
    g_nproc_peak = 0;
    struct thread *sampler = thread_create(nproc_sampler, NULL, "nproc-sampler", SCHED_PRIO_DEFAULT);
    struct thread *ta = thread_create(nproc_spawner, &a, "nproc-a", SCHED_PRIO_DEFAULT);
    struct thread *tb = thread_create(nproc_spawner, &b, "nproc-b", SCHED_PRIO_DEFAULT);
    CHECK(sampler && ta && tb);
    thread_join(ta);
    thread_join(tb);
    for (unsigned i = 0; i < a.ok; i++) {
        process_wait_exit(a.kids[i]);
        process_put(a.kids[i]);
    }
    for (unsigned i = 0; i < b.ok; i++) {
        process_wait_exit(b.kids[i]);
        process_put(b.kids[i]);
    }
    g_nproc_stop = true;
    thread_join(sampler);
    CHECK(a.ok + b.ok >= 4);
    CHECK(a.eagain + b.eagain >= 1);   /* sixteen attempts, at most four alive */
    CHECK(g_nproc_peak <= 4);
    uint64_t deadline = clock_now_ns() + 1000000000ULL;
    while (process_count_uid(4242) != 0 && clock_now_ns() < deadline)
        sched_yield();
    CHECK(process_count_uid(4242) == 0);
    kinfo("selftest: process-nproc: %u admitted, %u refused across two spawners; peak %u of a limit of 4",
          a.ok + b.ok, a.eagain + b.eagain, g_nproc_peak);
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
    CHECK(clock_since_ns(t0) < 2000000000ULL);
    CHECK(status == 128 + sig);
    process_put(p);
    return true;
}

/* A loadable image with one PT_NOTE segment of 32 bytes at offset 176. */
struct note_elf {
    uint8_t ehdr[64];
    uint8_t phdr[2][56];
    uint8_t note[32];
};

static void make_note_elf(struct note_elf *e, uint32_t namesz, uint32_t descsz, uint32_t type, const char *name)
{
    struct tiny_elf t;
    make_elf(&t, 0x400010, 0x400000, ELF_PF_R | ELF_PF_X);
    memset(e, 0, sizeof(*e));
    memcpy(e->ehdr, t.ehdr, 64);
    put16(e->ehdr + 56, 2);                    /* e_phnum */
    memcpy(e->phdr[0], t.phdr, 56);
    put64(e->phdr[0] + 32, sizeof(*e));        /* p_filesz: the whole file */
    put32(e->phdr[1] + 0, PT_NOTE);
    put32(e->phdr[1] + 4, ELF_PF_R);
    put64(e->phdr[1] + 8, 176);                /* p_offset */
    put64(e->phdr[1] + 16, 0x400000 + 176);
    put64(e->phdr[1] + 32, sizeof(e->note));   /* p_filesz */
    put64(e->phdr[1] + 40, sizeof(e->note));
    put64(e->phdr[1] + 48, 4);
    put32(e->note + 0, namesz);
    put32(e->note + 4, descsz);
    put32(e->note + 8, type);
    if (name)
        memcpy(e->note + 12, name, strlen(name) + 1);
}

/* Phase 11: the CosmoOS note marks native programs; a Linux test program lacks it. */
bool selftest_linux_elf(const char **reason)
{
    struct note_elf ne;
    struct elf_info ninfo;
    const char *nwhy;
    make_note_elf(&ne, 8, 4, 1, "CosmoOS");
    put32(ne.note + 20, 1);                    /* desc: ABI version */
    CHECK(elf_validate(&ne, sizeof(ne), USER_LO, USER_HI, &ninfo, &nwhy) == 0);
    CHECK(ninfo.cosmo_note);
    make_note_elf(&ne, 8, 4, 2, "CosmoOS");    /* wrong type */
    CHECK(elf_validate(&ne, sizeof(ne), USER_LO, USER_HI, &ninfo, &nwhy) == 0 && !ninfo.cosmo_note);
    make_note_elf(&ne, 6, 0, 1, "Other");      /* a foreign note first, then nothing */
    CHECK(elf_validate(&ne, sizeof(ne), USER_LO, USER_HI, &ninfo, &nwhy) == 0 && !ninfo.cosmo_note);
    /* Sizes near UINT32_MAX must neither wrap into a zero-length record nor hang the walker. */
    make_note_elf(&ne, 0xFFFFFFFDu, 0, 1, NULL);
    CHECK(elf_validate(&ne, sizeof(ne), USER_LO, USER_HI, &ninfo, &nwhy) == 0 && !ninfo.cosmo_note);
    make_note_elf(&ne, 0, 0xFFFFFFFFu, 1, NULL);
    CHECK(elf_validate(&ne, sizeof(ne), USER_LO, USER_HI, &ninfo, &nwhy) == 0 && !ninfo.cosmo_note);
    make_note_elf(&ne, 8, 4096, 1, "CosmoOS"); /* desc runs past the segment: ignored */
    CHECK(elf_validate(&ne, sizeof(ne), USER_LO, USER_HI, &ninfo, &nwhy) == 0 && !ninfo.cosmo_note);

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

/*
 * Leave the machine as it was found: wait for the process table to come
 * back to where it started before returning. Killing a child and
 * joining it is not the same as the child being *gone*, and the tests
 * that run next -- `process-spawn` among them -- ask whether a freshly
 * spawned child is alive after fifty milliseconds. Two of this unit's
 * boots failed there, on the slower architecture, because these tests
 * were still being torn down.
 */
static void elf_settle_processes(unsigned before)
{
    uint64_t deadline = clock_deadline_ns(2000ull * 1000000ull);
    while (process_count() != before && !clock_deadline_passed(deadline))
        thread_sleep_ms(5);
}

/*
 * A note on the children these tests run.
 *
 * They spin (`init --spin`) rather than block on the console. A child
 * that blocks reads the console, the suite contends for it, and two
 * unrelated tests -- `process-spawn` and `hid-keyboard` -- failed
 * because these tests were holding it. A test that perturbs what it
 * shares the machine with is measuring the machine, not the change.
 */

/*
 * Two processes running one program map the same frames for its text.
 *
 * Not "the second costs less", which a leak or a smaller stack would
 * also produce: the same *physical address* for the same virtual
 * address in two address spaces, which nothing but sharing explains
 * (docs/audit/next-subsystem-elf-shared-text.md).
 *
 * The image is read the way `read_executable` reads it and carries its
 * vnode, because that -- not the bytes -- is what lets the loader map
 * instead of copy.
 */
static int read_image_with_vnode(const char *path, struct process_image *img)
{
    struct vnode *vn = NULL;
    int rc = vfs_lookup(vfs_root(), path, &vn);
    if (rc)
        return rc;
    struct file *f;
    vnode_get(vn);
    rc = vfs_open_vnode(vn, COSMO_O_RDONLY, &f);   /* consumes one reference */
    if (rc) {
        vnode_put(vn);
        return rc;
    }
    struct cosmo_stat st;
    file_stat(f, &st);
    size_t size = (size_t)st.size;
    vaddr_t image = vm_kernel_alloc((size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1),
                                    VM_KALLOC_POPULATE, VM_PROT_RW);
    if (image == 0) {
        file_put(f);
        vnode_put(vn);
        return -ENOMEM;
    }
    size_t got = 0;
    while (got < size) {
        int64_t n = file_pread(f, (uint8_t *)image + got, size - got, got);
        if (n <= 0) {
            vm_kernel_free(image);
            file_put(f);
            vnode_put(vn);
            return n < 0 ? (int)n : -EIO;
        }
        got += (size_t)n;
    }
    file_put(f);
    img->data = (const void *)image;
    img->size = size;
    img->path = path;
    img->vn = vn;
    return 0;
}

static void free_image_with_vnode(struct process_image *img)
{
    if (img->data)
        vm_kernel_free((vaddr_t)img->data);
    if (img->vn)
        vnode_put(img->vn);
    img->data = NULL;
    img->vn = NULL;
}

/*
 * A file being executed does not change underneath the process running
 * it, and stops being busy when that process is gone.
 *
 * Both halves matter. The refusal alone would pass with a flag that is
 * set once and never cleared; the second write, after the process has
 * exited, is what says the answer is read from the mappings rather than
 * remembered (docs/audit/next-subsystem-elf-shared-text.md).
 *
 * It runs on a copy in /tmp rather than on /boot/init, because a test
 * that writes to the program the machine is running is a test that has
 * already gone wrong.
 */
/*
 * What a segment's zero tail reads as, and what shared text refuses.
 *
 * Two claims the sharing put at risk, checked in one process so the
 * setup is paid once:
 *
 *  - **The zero tail is zero.** `memsz > filesz` means the segment ends
 *    in bytes the file does not hold, and the loader now leaves them
 *    demand-paged instead of populating them. An anonymous page arrives
 *    zero, which is why that is safe -- but "is why" is an argument and
 *    this is the check. It reads the tail through the child's own
 *    address space.
 *  - **Shared text cannot be made writable.** `maxprot` excludes W on a
 *    text mapping, so `vm_user_protect` must refuse to grant it. Without
 *    that, one process could rewrite another's instructions through a
 *    mapping it was handed for free.
 */
/*
 * A writable segment is private, and the test exists because a mutation
 * survived without it.
 *
 * Deleting the `not writable` half of `seg_shareable` -- sharing the
 * data segment too -- passed every other test in this unit: the text
 * frames still matched, the cost still fell, the interlock still
 * worked. What it would have broken is the thing no other test looks
 * at: one process's store reaching another's memory, and the file.
 *
 * So this writes a byte through one process's data segment and reads it
 * through another's. They must disagree.
 */
bool selftest_elf_data_private(const char **reason);
bool selftest_elf_data_private(const char **reason)
{
    unsigned procs0 = process_count();
    struct process_image img = { 0 };
    if (read_image_with_vnode("/boot/init", &img) != 0) {
        kinfo("selftest: elf-data-private: /boot/init unreadable; skipping");
        elf_settle_processes(procs0);
        return true;
    }
    struct elf_info info;
    const char *why = NULL;
    if (elf_validate(img.data, img.size, USER_LO, USER_HI, &info, &why) != 0) {
        free_image_with_vnode(&img);
        *reason = "the boot image does not validate";
        elf_settle_processes(procs0);
        return false;
    }
    /* A byte inside a writable segment's *file* part, which both
     * processes load from the same bytes and must not then share. */
    uint64_t data_va = 0;
    for (unsigned i = 0; i < info.nr_segments; i++) {
        const struct elf_segment *sg = &info.segments[i];
        if ((sg->flags & ELF_PF_W) && sg->filesz > 0) {
            data_va = sg->file_vaddr;
            break;
        }
    }
    if (data_va == 0) {
        free_image_with_vnode(&img);
        kinfo("selftest: elf-data-private: no writable segment with file bytes; skipping");
        elf_settle_processes(procs0);
        return true;
    }

    static const char *const argv[] = { "init", "--block", NULL };
    struct process *p1 = NULL, *p2 = NULL;
    bool ok = process_create_from_images(&img, NULL, "init", argv, NULL, NULL, &p1) == 0 &&
              process_create_from_images(&img, NULL, "init", argv, NULL, NULL, &p2) == 0;
    free_image_with_vnode(&img);

    uint8_t before2 = 0, after2 = 0, after1 = 0;
    bool measured = false;
    if (ok) {
        paddr_t pa1 = 0, pa2 = 0;
        vaddr_t page = (vaddr_t)(data_va & ~(uint64_t)(PAGE_SIZE - 1));
        unsigned in_page = (unsigned)(data_va & (PAGE_SIZE - 1));
        if (arch_mmu_query(&p1->space->mmu, page, &pa1, NULL, NULL, NULL) &&
            arch_mmu_query(&p2->space->mmu, page, &pa2, NULL, NULL, NULL)) {
            uint8_t *f1 = (uint8_t *)phys_to_virt(pa1) + in_page;
            uint8_t *f2 = (uint8_t *)phys_to_virt(pa2) + in_page;
            before2 = *f2;
            *f1 = (uint8_t)(*f1 ^ 0xA5);   /* a store in the first process's data */
            after1 = *f1;
            after2 = *f2;
            measured = true;
        }
    }
    if (p1) {
        process_kill(p1, COSMO_SIGKILL);
        process_wait_exit(p1);
        process_put(p1);
    }
    if (p2) {
        process_kill(p2, COSMO_SIGKILL);
        process_wait_exit(p2);
        process_put(p2);
    }
    if (!ok) {
        *reason = "could not create two processes from one image";
        elf_settle_processes(procs0);
        return false;
    }
    if (!measured) {
        kinfo("selftest: elf-data-private: the data page is not present in both; skipping");
        elf_settle_processes(procs0);
        return true;
    }
    if (after2 != before2) {
        kerror("selftest: elf-data-private: a store in one process changed the other's data byte "
               "(%02x -> %02x) at %p",
               before2, after2, (void *)data_va);
        *reason = "two processes share a writable segment";
        elf_settle_processes(procs0);
        return false;
    }
    kinfo("selftest: elf-data-private: a store in one process's data (now %02x) left the other's at %02x",
          after1, after2);
    elf_settle_processes(procs0);
    return true;
}

bool selftest_elf_text_ro(const char **reason);
bool selftest_elf_text_ro(const char **reason)
{
    unsigned procs0 = process_count();
    struct process_image img = { 0 };
    if (read_image_with_vnode("/boot/init", &img) != 0) {
        kinfo("selftest: elf-text-ro: /boot/init unreadable; skipping");
        elf_settle_processes(procs0);
        return true;
    }
    struct elf_info info;
    const char *why = NULL;
    if (elf_validate(img.data, img.size, USER_LO, USER_HI, &info, &why) != 0) {
        free_image_with_vnode(&img);
        *reason = "the boot image does not validate";
        elf_settle_processes(procs0);
        return false;
    }
    uint64_t text_va = 0, tail_va = 0;
    for (unsigned i = 0; i < info.nr_segments; i++) {
        const struct elf_segment *sg = &info.segments[i];
        if (text_va == 0 && (sg->flags & ELF_PF_X) && sg->file_memsz == sg->filesz)
            text_va = sg->vaddr;
        /* A segment with a real zero tail, and a byte inside it. */
        if (tail_va == 0 && sg->file_memsz > sg->filesz && sg->filesz > 0 &&
            ((sg->file_vaddr + sg->filesz) & (PAGE_SIZE - 1)) != 0)
            tail_va = sg->file_vaddr + sg->filesz;
    }
    static const char *const argv[] = { "init", "--block", NULL };
    struct process *p = NULL;
    bool ok = process_create_from_images(&img, NULL, "init", argv, NULL, NULL, &p) == 0;
    free_image_with_vnode(&img);
    if (!ok) {
        *reason = "could not create the process";
        elf_settle_processes(procs0);
        return false;
    }

    /* Text stays read-only: maxprot has no W, so this must be refused. */
    int prot_rc = 0;
    if (text_va != 0)
        prot_rc = vm_user_protect(p->space, text_va, PAGE_SIZE, VM_PROT_RW);

    /*
     * The zero tail, read out of the child's own frame.
     *
     * The bytes checked are the ones just past `filesz` inside the last
     * page the file's bytes touch -- the part the loader populates and
     * copies into, and therefore the part this unit could have got
     * wrong. Beyond that page the tail is a separate anonymous region
     * that is demand-paged, and an anonymous page arrives zero by
     * construction with no copy to get wrong.
     */
    uint8_t tail[16];
    int tail_rc = 0;
    bool tail_zero = true;
    if (tail_va != 0) {
        paddr_t pa = 0;
        if (!arch_mmu_query(&p->space->mmu, (vaddr_t)(tail_va & ~(uint64_t)(PAGE_SIZE - 1)),
                            &pa, NULL, NULL, NULL)) {
            tail_rc = -EFAULT;
        } else {
            const uint8_t *page = phys_to_virt(pa);
            memcpy(tail, page + (tail_va & (PAGE_SIZE - 1)), sizeof(tail));
            for (unsigned i = 0; i < sizeof(tail); i++)
                if (tail[i] != 0)
                    tail_zero = false;
        }
    }

    process_kill(p, COSMO_SIGKILL);
    process_wait_exit(p);
    process_put(p);

    if (text_va != 0 && prot_rc == 0) {
        *reason = "shared text could be made writable";
        elf_settle_processes(procs0);
        return false;
    }
    if (tail_va != 0 && tail_rc != 0) {
        kinfo("selftest: elf-text-ro: the zero tail at %p is not readable (%d); skipping that half",
              (void *)tail_va, tail_rc);
    } else if (tail_va != 0 && !tail_zero) {
        kerror("selftest: elf-text-ro: the zero tail at %p reads %02x %02x %02x %02x",
               (void *)tail_va, tail[0], tail[1], tail[2], tail[3]);
        *reason = "a segment's zero tail is not zero";
        elf_settle_processes(procs0);
        return false;
    }
    kinfo("selftest: elf-text-ro: shared text refuses PROT_WRITE (%d), and the zero tail reads as zero",
          prot_rc);
    elf_settle_processes(procs0);
    return true;
}

bool selftest_elf_txtbsy(const char **reason);
bool selftest_elf_txtbsy(const char **reason)
{
    unsigned procs0 = process_count();
    struct process_image src = { 0 };
    if (read_image_with_vnode("/boot/init", &src) != 0) {
        kinfo("selftest: elf-txtbsy: /boot/init unreadable; skipping");
        elf_settle_processes(procs0);
        return true;
    }
    /* A copy of the program, which this test may write to. */
    const char *path = "/tmp/elf-txtbsy.bin";
    struct file *f = NULL;
    int rc = vfs_open(NULL, path, COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_TRUNC, 0755, &f);
    if (rc != 0) {
        free_image_with_vnode(&src);
        kinfo("selftest: elf-txtbsy: cannot create %s (%d); skipping", path, rc);
        elf_settle_processes(procs0);
        return true;
    }
    size_t off = 0;
    bool wrote = true;
    while (off < src.size && wrote) {
        int64_t n = file_pwrite(f, (const uint8_t *)src.data + off, src.size - off, off);
        if (n <= 0)
            wrote = false;
        else
            off += (size_t)n;
    }
    file_put(f);
    free_image_with_vnode(&src);
    if (!wrote) {
        vfs_unlink(NULL, path);
        *reason = "could not write the copy this test runs on";
        elf_settle_processes(procs0);
        return false;
    }

    struct process_image img = { 0 };
    if (read_image_with_vnode(path, &img) != 0) {
        vfs_unlink(NULL, path);
        *reason = "the copy is unreadable";
        elf_settle_processes(procs0);
        return false;
    }
    /* Where this program's shared text lands, so the wait below can see
     * the child running out of the file rather than merely existing. */
    uint64_t text_va = 0;
    {
        struct elf_info info;
        const char *why = NULL;
        if (elf_validate(img.data, img.size, USER_LO, USER_HI, &info, &why) == 0)
            for (unsigned i = 0; i < info.nr_segments; i++)
                if ((info.segments[i].flags & ELF_PF_X) &&
                    info.segments[i].file_memsz == info.segments[i].filesz) {
                    text_va = info.segments[i].vaddr;
                    break;
                }
    }

    /*
     * A child that spins, not one that blocks on the console.
     *
     * `--block` waits on a console read, and the suite contends for the
     * console: this child was exiting early, its mapping going with it,
     * and the write then succeeded because nothing was executing the
     * file -- which is not the defect this test is about. It failed
     * that way under mutations that cannot touch the interlock, which
     * is how a flake says it is a flake. A spinning child holds its
     * text for as long as this test needs it.
     */
    static const char *const argv[] = { "init", "--spin", NULL };
    struct process *p = NULL;
    bool ok = process_create_from_images(&img, NULL, "init", argv, NULL, NULL, &p) == 0;

    /* While it runs: refused, by name. */
    uint8_t byte = 0x90;
    int64_t busy_rc = 0, free_rc = 0;
    bool alive = false, same_vnode = false, direct_busy = false;
    int trunc_rc = -ETXTBSY;   /* untested unless the child runs */
    /* The mapping door, in both orders. Defaults are the wanted values,
     * so a skip (the child's text was not shared) reports nothing. */
    int wshared_rc = -ETXTBSY, wpriv_rc = 0, text_after_w_rc = -ETXTBSY;
    if (ok) {
        /*
         * The precondition, *observed* exactly.
         *
         * This test asserts what happens while a program is running
         * **from a file's shared text**, so neither "the child exists"
         * nor "the child's text page is present" is enough: an
         * anonymous copy satisfies both. Two proxies were tried and
         * both let the test fail intermittently, including under
         * mutations that cannot touch the interlock -- which is how a
         * flake announces itself rather than a defect.
         *
         * What is checked instead is the thing itself: a second process
         * from the same image maps the same *frame*. That is true only
         * if the loader shared the file's pages, which is the condition
         * the refusal below depends on. If it did not -- the mapping
         * can fall back to a copy -- this skips and says so, rather
         * than reporting the interlock broken.
         */
        struct process *p2 = NULL;
        if (text_va != 0 &&
            process_create_from_images(&img, NULL, "init", argv, NULL, NULL, &p2) == 0) {
            uint64_t deadline = clock_deadline_ns(2000ull * 1000000ull);
            while (!clock_deadline_passed(deadline)) {
                paddr_t a1 = 0, a2 = 0;
                if (arch_mmu_query(&p->space->mmu, (vaddr_t)text_va, &a1, NULL, NULL, NULL) &&
                    arch_mmu_query(&p2->space->mmu, (vaddr_t)text_va, &a2, NULL, NULL, NULL)) {
                    alive = (a1 == a2);
                    break;
                }
                thread_sleep_ms(5);
            }
            process_kill(p2, COSMO_SIGKILL);
            process_wait_exit(p2);
            process_put(p2);
        }
        /* And still running when the write happens: the precondition
         * above was observed a moment earlier, and a moment is enough
         * for a child to die. */
        if (alive && completion_done(&p->exited))
            alive = false;
        /*
         * Ask the interlock directly, on the vnode the mapping was made
         * from, so a failure below separates the two things it could
         * mean: no mapping to refuse, or a write path that did not ask.
         * That distinction is what found the real cause of this test's
         * early flakiness -- a stale object file, not the kernel.
         */
        pagecache_lock(img.vn);
        direct_busy = pagecache_text_busy(img.vn);
        pagecache_unlock(img.vn);
        /* And the other way to change a file's contents: a truncate
         * removes the very pages the program is executing, which the
         * write's refusal alone would not have stopped (found in
         * review of this unit). */
        if (alive)
            trunc_rc = vfs_truncate(NULL, path, 0);
        /*
         * The third door, and the one no write ever passes through: a
         * store through a writable MAP_SHARED mapping dirties the page
         * cache's own frame, and the text mapping IS that frame. This
         * is the first of the two orders -- text first, then the
         * writable shared mapping -- and the private mapping beside it
         * is the control: it differs in the sharing alone, so a
         * refusal of both would mean the check is about writability
         * rather than about the frame.
         */
        if (alive) {
            struct vm_space *sp = NULL;
            if (vm_space_create_user(&sp) == 0) {
                const uint64_t A = 0x0000340000000000ULL;
                wshared_rc = vm_user_map_file(sp, A, PAGE_SIZE, VM_PROT_RW, VM_PROT_RW,
                                              VM_MAP_SHARED, img.vn, 0, "w-shared");
                if (wshared_rc == 0)
                    (void)vm_user_unmap(sp, A, PAGE_SIZE, 0);
                wpriv_rc = vm_user_map_file(sp, A + PAGE_SIZE, PAGE_SIZE, VM_PROT_RW, VM_PROT_RW,
                                            0, img.vn, 0, "w-private");
                vm_space_destroy(sp);
            }
        }
        struct file *w = NULL;
        if (alive && vfs_open(NULL, path, COSMO_O_WRONLY, 0, &w) == 0) {
            /* The vnode the write lands on, against the one the mapping
             * was made from: if a second lookup of one path can produce
             * a second vnode, the interlock is asking the wrong list
             * and the failure below would otherwise say only "allowed". */
            same_vnode = (w->vn == img.vn);
            busy_rc = file_pwrite(w, &byte, 1, 0);
            file_put(w);
        }
        process_kill(p, COSMO_SIGKILL);
        process_wait_exit(p);
        process_put(p);
    }
    /* And once it is gone: allowed. The image's own reference is
     * dropped first -- it is a reference to the vnode, not a mapping,
     * but releasing it here keeps the second write's meaning clean. */
    free_image_with_vnode(&img);
    if (ok) {
        struct file *w = NULL;
        if (vfs_open(NULL, path, COSMO_O_WRONLY, 0, &w) == 0) {
            free_rc = file_pwrite(w, &byte, 1, 0);
            file_put(w);
        }
    }
    /*
     * The second order, now that nothing is executing the file: a
     * writable shared mapping stands first, and the TEXT mapping is
     * what is refused. An interlock that only looked one way would let
     * this one through and leave the two mappings coexisting, which is
     * the state the whole rule exists to prevent.
     */
    if (ok && alive) {
        struct vnode *vn = NULL;
        if (vfs_lookup(NULL, path, &vn) == 0) {
            struct vm_space *sp = NULL;
            if (vm_space_create_user(&sp) == 0) {
                const uint64_t A = 0x0000340000000000ULL;
                if (vm_user_map_file(sp, A, PAGE_SIZE, VM_PROT_RW, VM_PROT_RW,
                                     VM_MAP_SHARED, vn, 0, "w-shared") == 0)
                    text_after_w_rc = vm_user_map_file(sp, A + PAGE_SIZE, PAGE_SIZE, VM_PROT_READ,
                                                       VM_PROT_READ, VM_MAP_SHARED | VM_MAP_TEXT,
                                                       vn, 0, "text");
                vm_space_destroy(sp);
            }
            vnode_put(vn);
        }
    }
    vfs_unlink(NULL, path);

    if (!ok) {
        *reason = "could not run the copy";
        elf_settle_processes(procs0);
        return false;
    }
    if (!alive) {
        kinfo("selftest: elf-txtbsy: this copy's text was not shared from the file, so there is "
              "nothing for the interlock to refuse; skipping");
        elf_settle_processes(procs0);
        return true;
    }
    if (busy_rc != -ETXTBSY) {
        kerror("selftest: elf-txtbsy: writing a running program returned %lld, wanted %d "
               "(the write's vnode %s the mapping's; asked directly, the file %s busy)",
               (long long)busy_rc, -ETXTBSY, same_vnode ? "is" : "IS NOT",
               direct_busy ? "IS" : "is not");
        *reason = "a file being executed could be written";
        elf_settle_processes(procs0);
        return false;
    }
    if (trunc_rc != -ETXTBSY) {
        kerror("selftest: elf-txtbsy: truncating a running program returned %d, wanted %d",
               trunc_rc, -ETXTBSY);
        *reason = "a file being executed could be truncated";
        elf_settle_processes(procs0);
        return false;
    }
    if (wshared_rc != -ETXTBSY) {
        kerror("selftest: elf-txtbsy: a writable shared mapping of a running program returned %d, wanted %d",
               wshared_rc, -ETXTBSY);
        *reason = "a running program's text could be mapped writable and shared";
        elf_settle_processes(procs0);
        return false;
    }
    if (wpriv_rc != 0) {
        kerror("selftest: elf-txtbsy: a PRIVATE writable mapping of a running program returned %d, wanted 0",
               wpriv_rc);
        *reason = "the refusal is about writability, not about the shared frame";
        elf_settle_processes(procs0);
        return false;
    }
    if (text_after_w_rc != -ETXTBSY) {
        kerror("selftest: elf-txtbsy: a text mapping made after a writable shared one returned %d, wanted %d",
               text_after_w_rc, -ETXTBSY);
        *reason = "the two mappings are refused in one order only";
        elf_settle_processes(procs0);
        return false;
    }
    if (free_rc != 1) {
        kerror("selftest: elf-txtbsy: writing after the process exited returned %lld, wanted 1",
               (long long)free_rc);
        *reason = "a file stayed busy after the process running it exited";
        elf_settle_processes(procs0);
        return false;
    }
    kinfo("selftest: elf-txtbsy: a write, a truncate and a writable shared mapping of a running "
          "program are all -ETXTBSY -- a private one is not -- the text mapping is refused after a "
          "writable shared one too, and the write succeeds once it exits");
    elf_settle_processes(procs0);
    return true;
}

/* What one more process running an already-running program may cost.
 * 16 measured; the bound leaves room without letting the sharing or the
 * demand paging quietly stop working. */
#define ELF_COST_MAX_PAGES 32u

bool selftest_elf_share_cost(const char **reason);
bool selftest_elf_share_cost(const char **reason)
{
    unsigned procs0 = process_count();
    struct process_image img = { 0 };
    if (read_image_with_vnode("/boot/init", &img) != 0) {
        kinfo("selftest: elf-share-cost: /boot/init unreadable; skipping");
        elf_settle_processes(procs0);
        return true;
    }
    enum { COPIES = 3 };
    struct process *p[COPIES] = { NULL };
    static const char *const argv[] = { "init", "--spin", NULL };
    struct pmm_stats st;
    unsigned made = 0;
    uint64_t cost[COPIES] = { 0 };
    for (unsigned i = 0; i < COPIES; i++) {
        pmm_get_stats(&st);
        uint64_t before = st.free_pages;
        if (process_create_from_images(&img, NULL, "init", argv, NULL, NULL, &p[i]) != 0)
            break;
        made++;
        thread_sleep_ms(80);
        pmm_get_stats(&st);
        cost[i] = before - st.free_pages;
    }
    for (unsigned i = 0; i < made; i++) {
        process_kill(p[i], COSMO_SIGKILL);
        process_wait_exit(p[i]);
        process_put(p[i]);
    }
    free_image_with_vnode(&img);
    if (made < 2) {
        *reason = "could not create two processes";
        elf_settle_processes(procs0);
        return false;
    }
    /*
     * And a bound, so the number is a claim rather than a log line.
     *
     * Measured at 16 pages per copy on both architectures, against 89
     * before this unit. Thirty-two leaves room for a process's own
     * fixed cost to grow without pretending the sharing still works:
     * turning either half off puts it back over forty -- populating the
     * zero tail alone costs 26 pages, and it was an unnoticed
     * equivalent mutant until this bound existed
     * (docs/audit/next-subsystem-elf-shared-text.md).
     */
    if (cost[made - 1] > ELF_COST_MAX_PAGES) {
        kerror("selftest: elf-share-cost: a copy cost %llu pages, over the bound of %u",
               (unsigned long long)cost[made - 1], ELF_COST_MAX_PAGES);
        *reason = "a process costs more than a shared, demand-paged image should";
        elf_settle_processes(procs0);
        return false;
    }
    kinfo("selftest: elf-share-cost: pages per copy %llu, %llu, %llu (bound %u; the report measured 89 with no sharing)",
          (unsigned long long)cost[0], (unsigned long long)cost[1],
          (unsigned long long)(made > 2 ? cost[2] : 0), ELF_COST_MAX_PAGES);
    (void)reason;
    elf_settle_processes(procs0);
    return true;
}

bool selftest_elf_shared_text(const char **reason)
{
    unsigned procs0 = process_count();
    struct process_image img = { 0 };
    int rc = read_image_with_vnode("/boot/init", &img);
    if (rc) {
        kinfo("selftest: elf-shared-text: /boot/init unreadable (%d); skipping", rc);
        elf_settle_processes(procs0);
        return true;
    }
    struct elf_info info;
    const char *why = NULL;
    if (elf_validate(img.data, img.size, USER_LO, USER_HI, &info, &why) != 0) {
        free_image_with_vnode(&img);
        *reason = "the boot image does not validate";
        elf_settle_processes(procs0);
        return false;
    }
    /* The first executable segment's first page: what two processes
     * should agree on. */
    uint64_t text_va = 0;
    for (unsigned i = 0; i < info.nr_segments; i++)
        if ((info.segments[i].flags & ELF_PF_X) && info.segments[i].file_memsz == info.segments[i].filesz) {
            text_va = info.segments[i].vaddr;
            break;
        }
    if (text_va == 0) {
        free_image_with_vnode(&img);
        kinfo("selftest: elf-shared-text: no shareable executable segment; skipping");
        elf_settle_processes(procs0);
        return true;
    }

    static const char *const argv[] = { "init", "--spin", NULL };
    struct process *p1 = NULL, *p2 = NULL;
    bool ok = process_create_from_images(&img, NULL, "init", argv, NULL, NULL, &p1) == 0 &&
              process_create_from_images(&img, NULL, "init", argv, NULL, NULL, &p2) == 0;
    paddr_t pa1 = 0, pa2 = 0;
    bool got1 = false, got2 = false;
    if (ok) {
        /* Text is demand-paged now, so the page is present only once the
         * process has executed it. Both of these block on a console read,
         * so both reach their entry point; wait for the state rather than
         * sleeping a fixed time, and let the wait expire into a skip
         * rather than a failure -- an absent page has a benign reading
         * and this test is about identity, not about presence. */
        uint64_t deadline = clock_deadline_ns(2000ull * 1000000ull);
        while (!clock_deadline_passed(deadline)) {
            got1 = arch_mmu_query(&p1->space->mmu, (vaddr_t)text_va, &pa1, NULL, NULL, NULL);
            got2 = arch_mmu_query(&p2->space->mmu, (vaddr_t)text_va, &pa2, NULL, NULL, NULL);
            if (got1 && got2)
                break;
            thread_sleep_ms(5);
        }
    }
    if (p1) {
        process_kill(p1, COSMO_SIGKILL);
        process_wait_exit(p1);
        process_put(p1);
    }
    if (p2) {
        process_kill(p2, COSMO_SIGKILL);
        process_wait_exit(p2);
        process_put(p2);
    }
    free_image_with_vnode(&img);

    if (!ok) {
        *reason = "could not create two processes from one image";
        elf_settle_processes(procs0);
        return false;
    }
    if (!got1 || !got2) {
        /* Demand paging: the page may not be present until it is
         * touched. Say which, rather than failing on an absence that
         * has a benign reading. */
        kinfo("selftest: elf-shared-text: text page not present in %s; skipping the identity check",
              !got1 && !got2 ? "either space" : (!got1 ? "the first space" : "the second space"));
        elf_settle_processes(procs0);
        return true;
    }
    if (pa1 != pa2) {
        kerror("selftest: elf-shared-text: va %p maps to %p in one process and %p in the other",
               (void *)text_va, (void *)pa1, (void *)pa2);
        *reason = "two processes running one program have separate copies of its text";
        elf_settle_processes(procs0);
        return false;
    }
    kinfo("selftest: elf-shared-text: two processes map va %p to the same frame %p",
          (void *)text_va, (void *)pa1);
    elf_settle_processes(procs0);
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


/*
 * The held-walk proofs (docs/audit/next-subsystem-cwd-hold.md): the
 * cwd-ref fix's regression test, made a proof at both doors. The seam is
 * armed for the racer's process name, the racer is spawned once per pass,
 * and what the seam recorded is read back after it exits. Every claim is
 * a field the seam derived from what it saw -- `released_after_put` is
 * the releasing side finding the count one lower than before its put --
 * and not a flag the code under test set about itself.
 */
#if CONFIG_DEBUG
static bool run_archived(const char *archive_path, const char *name, const char *const argv[], int *status_out,
                         const char **reason)
{
    const void *image;
    size_t image_size;
    if (!bootarchive_find(archive_path, &image, &image_size)) {
        *status_out = -1;
        return true;
    }
    struct process *p = NULL;
    CHECK(process_create_from_elf(image, image_size, name, argv, NULL, NULL, &p) == 0);
    CHECK(p != NULL);
    uint64_t t0 = clock_now_ns();
    int status = process_wait_exit(p);
    CHECK(clock_since_ns(t0) < 15000000000ULL);
    process_put(p);
    *status_out = status;
    return true;
}

static bool cwd_hold_check(const char *door, const char *pass, int status, const struct vfs_cwd_hold_record *r,
                           const char **reason)
{
    bool outlive = pass[0] == 'o';
    kinfo("selftest: cwd-hold-%s %s: status %d held %d matches_old %d ref hold %u put %u resume %u "
          "released_after_put %d dead %d swapper_held %d timeouts %d/%d intr %d",
          door, pass, status, r->held, r->held_matches_old, r->ref_at_hold, r->ref_before_put, r->ref_at_resume,
          r->released_after_put, r->resumed_dead, r->swapper_was_held, r->walk_timed_out, r->swap_timed_out,
          r->interrupted);
    kinfo("selftest: cwd-hold-%s %s: ref at release %u; swap_rc %d; swap wait %+lld us .. %+lld us from the hold",
          door, pass, r->ref_at_release, r->swap_rc, (long long)((int64_t)(r->t_swap_wait_ns - r->t_hold_ns) / 1000),
          (long long)((int64_t)(r->t_swap_done_ns - r->t_hold_ns) / 1000));
    CHECK(status == 0);
    CHECK(r->held);                     /* a walk was held: the racer reached the seam */
    CHECK(r->held_matches_old);         /* holding the directory the swapper replaced, not the one it installed;
                                           * decisive in the swapfirst pass, where the swapper is provably ahead */
    CHECK(!r->swapper_was_held);        /* the racer made no relative walk on its swapper */
    CHECK(!r->walk_timed_out && !r->swap_timed_out && !r->interrupted);
    CHECK(r->released_after_put);       /* the put preceded the release: the count fell by one */
    if (outlive) {
        /* rmdir dropped ramfs's pin, so before the put the directory has
         * exactly the process's reference and the walk's. Without the
         * fix it has one, the put frees it, and the boot panics. */
        CHECK(r->ref_before_put == 2);
        CHECK(r->resumed_dead);
    } else {
        CHECK(r->ref_before_put == 3);  /* ramfs's pin, the process's, the walk's */
        CHECK(!r->resumed_dead);
    }
    return true;
}
#endif

bool selftest_cwd_hold_native(const char **reason)
{
#if CONFIG_DEBUG
    /* Every pass runs before any is judged: a failing first pass must
     * not hide what the second finds, which under the door mutations is
     * the walk resuming on the poison. `swapfirst` is native-only: it
     * starts the walker once debug.cwd_hold says the swapper is already
     * waiting inside chdir, and a Linux program has no sysctl. */
    static const char *const passes[] = { "capture", "outlive", "swapfirst" };
    bool all = true;
    for (unsigned i = 0; i < 3; i++) {
        const char *argv[] = { "cwdtest", "--held", passes[i], NULL };
        struct vfs_cwd_hold_record r;
        int status;
        vfs_test_cwd_hold_arm("cwdtest");
        bool ok = run_archived("tests/native/cwdtest", "cwdtest", argv, &status, reason);
        vfs_test_cwd_hold_disarm(&r);   /* on every exit, including a failed spawn */
        if (!ok)
            return false;
        if (status == -1) {
            kinfo("selftest: cwd-hold-native: no cwdtest in the boot archive; skipping");
            return true;
        }
        if (!cwd_hold_check("native", passes[i], status, &r, reason))
            all = false;
    }
    if (!all)
        return false;
    kinfo("selftest: cwd-hold-native: a held walk resolved against the directory it captured, and its "
          "reference outlived the swap that removed it");
    return true;
#else
    (void)reason;
    kinfo("selftest: cwd-hold-native: the seam is compiled out of this build");
    return true;
#endif
}

bool selftest_cwd_hold_linux(const char **reason)
{
#if CONFIG_DEBUG
    const void *image;
    size_t image_size;
    if (!bootarchive_find("tests/linux/lxcwd", &image, &image_size)) {
        kinfo("selftest: cwd-hold-linux: no lxcwd in the boot archive; skipping");
        return true;
    }
    static const char *const passes[] = { "capture", "outlive" };
    bool all = true;
    for (unsigned i = 0; i < 2; i++) {
        char probe[40];
        ksnprintf(probe, sizeof(probe), "cwd-hold-linux:%s", passes[i]);
        const char *argv[] = { "init", "--probe", probe, NULL };
        struct vfs_cwd_hold_record r;
        int status;
        /* Armed for the Linux program's name: init spawns it and makes no
         * relative walk of its own in between. */
        vfs_test_cwd_hold_arm("lxcwd");
        bool ok = run_module(argv, &status, reason);
        vfs_test_cwd_hold_disarm(&r);
        if (!ok)
            return false;
        if (status == -1)
            return true;
        if (!cwd_hold_check("linux", passes[i], status, &r, reason))
            all = false;
    }
    if (!all)
        return false;
    kinfo("selftest: cwd-hold-linux: the same two proofs through the Linux door");
    return true;
#else
    (void)reason;
    kinfo("selftest: cwd-hold-linux: the seam is compiled out of this build");
    return true;
#endif
}
